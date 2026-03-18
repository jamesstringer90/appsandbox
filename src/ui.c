/*
 * ui.c — Main window + WebView2 UI bridge.
 *
 * All presentation is handled by the HTML/JS frontend (web/).
 * This file manages: window creation, WebView2 hosting, JSON message
 * dispatch, VM lifecycle operations, persistence, and state callbacks.
 */

#include "ui.h"
#include "resource.h"
#include "prereq.h"
#include "hcs_vm.h"
#include "gpu_enum.h"
#include "hcn_network.h"
#include "disk_util.h"
#include "snapshot.h"
#include "vmms_cert.h"
#include "vm_display.h"
#include "vm_display_idd.h"
#include "vm_agent.h"
#include "webview2_bridge.h"
#include <dwmapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <shlobj.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* DWMWA_USE_IMMERSIVE_DARK_MODE */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* ---- Globals ---- */

static HWND g_hwnd_main = NULL;
static HINSTANCE g_hInstance = NULL;

/* GPU list (cached at startup) */
static GpuList g_gpu_list;

/* VM storage */
#define MAX_VMS 32
static VmInstance g_vms[MAX_VMS];
static int g_vm_count = 0;
static int g_selected_vm = -1;

/* Last-used ISO path */
static wchar_t g_last_iso_path[MAX_PATH] = { 0 };

/* Suppress minimize-to-tray warning */
static BOOL g_suppress_tray_warn = FALSE;

/* Minimum window size (set by JS after measuring content) */
static int g_min_width = 0;
static int g_min_height = 0;

/* Template storage */
#define MAX_TEMPLATES 32
typedef struct {
    wchar_t name[256];
    wchar_t os_type[32];
    wchar_t image_path[MAX_PATH];
    wchar_t vhdx_path[MAX_PATH];
} TemplateInfo;
static TemplateInfo g_templates[MAX_TEMPLATES];
static int g_template_count = 0;

/* Snapshot storage per VM */
static SnapshotList g_snap_lists[MAX_VMS];

/* Display windows (RDP and IDD) */
static VmDisplay *g_displays[MAX_VMS];
static VmDisplayIdd *g_idd_displays[MAX_VMS];

/* Custom window messages */
#define WM_VM_STATE_CHANGED    (WM_APP + 1)
#define WM_VM_AGENT_STATUS     (WM_APP + 2)
#define WM_VM_AGENT_SHUTDOWN   (WM_APP + 3)
#define WM_VM_AGENT_GPUCOPY    (WM_APP + 4)
#define WM_VM_DISPLAY_CLOSED   (WM_APP + 5)
#define WM_VM_MONITOR_DETECTED (WM_APP + 6)
#define WM_HCS_OP_DONE         (WM_APP + 7)
#define WM_VM_SHUTDOWN_TIMEOUT (WM_APP + 9)
#define WM_WEBVIEW2_LOG        (WM_APP + 10)
#define WM_TRAYICON            (WM_APP + 11)
#define WM_VM_HYPERV_VIDEO_OFF (WM_APP + 12)
#define WM_VHDX_PROGRESS       (WM_APP + 13)
#define WM_VHDX_DONE           (WM_APP + 14)

/* Tray menu command ID ranges */
#define TRAY_CMD_SHOW          1
#define TRAY_CMD_EXIT          2
#define TRAY_CMD_CONNECT_BASE  100
#define TRAY_CMD_SHUTDOWN_BASE 200
#define TRAY_CMD_STOP_BASE     300

/* System tray icon */
static NOTIFYICONDATAW g_nid;

/* Async HCS operation types */
#define HCS_OP_STOP       1
#define HCS_OP_SNAP_TAKE  2
#define HCS_OP_SNAP_REVERT 3

typedef struct {
    int         op_type;
    int         vm_index;
    VmInstance  *vm;
    HRESULT     result;
    int         snap_index;
    wchar_t     snap_name[128];
} HcsAsyncOp;

/* Args for background VHDX creation thread */
typedef struct {
    VmConfig config;
    int      vm_index;
    wchar_t  vhdx_dir[MAX_PATH];
    wchar_t  endpoint_guid[64];
    GUID     network_id;
    GUID     endpoint_id;
    BOOL     has_network;
    wchar_t  net_adapter[256];
    HRESULT  result;
    wchar_t  error_msg[512];
    VmInstance *vm_inst;    /* heap-allocated VmInstance on success, NULL on failure */
} VhdxCreateArgs;

/* ---- Forward declarations ---- */

static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void on_create_vm(const wchar_t *json);
static void do_start_vm(int idx);
static void do_shutdown_vm(int idx);
static void do_stop_vm(int idx);
static void do_connect_rdp(int idx);
static void do_connect_idd(int idx);
static void on_snap_take(void);
static void on_snap_revert(int snap_idx);
static void on_snap_delete(int snap_idx);
static void save_vm_list(void);
static void load_vm_list(void);
static void scan_templates(void);
static void destroy_vm_at(int idx);

/* ---- Safe display teardown (nulls global BEFORE destroy to prevent re-entrancy) ---- */

static void safe_destroy_rdp(int idx)
{
    if (g_displays[idx]) {
        VmDisplay *d = g_displays[idx];
        g_displays[idx] = NULL;
        vm_display_disconnect(d);
        vm_display_destroy(d);
    }
}

static void safe_destroy_idd(int idx)
{
    if (g_idd_displays[idx]) {
        VmDisplayIdd *d = g_idd_displays[idx];
        g_idd_displays[idx] = NULL;
        vm_display_idd_destroy(d);
    }
}

/* ---- JSON state helpers ---- */

static void build_host_info_json(JsonBuilder *jb);
static void send_vm_list(void);
static void send_snap_list(void);
static void send_host_info(void);
static void send_full_state(void);
static void send_adapters(void);
static void send_templates(void);

/* ---- Per-VM state JSON (beside disk.vhdx) ---- */

/* Derive the state JSON path from the VHDX path: <dir>\vm_state.json */
static void get_state_json_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_len)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_len, L"%s\\vm_state.json", dir);
}

void vm_save_state_json(const wchar_t *vhdx_path, BOOL install_complete)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    get_state_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"installComplete\":%d}\n", install_complete ? 1 : 0);
    fclose(f);
}

BOOL vm_load_state_json(const wchar_t *vhdx_path)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    BOOL result = FALSE;
    get_state_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return FALSE;
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"installComplete\":1"))
            result = TRUE;
    }
    fclose(f);
    return result;
}

/* ---- Config file path ---- */

static void get_config_path(wchar_t *out, size_t out_len)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, out_len, L"%s\\AppSandbox\\vms.cfg", base);
}

/* ---- Persistence: save VM list ---- */

static void save_vm_list(void)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    int i;

    get_config_path(path, MAX_PATH);

    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;

    if (g_last_iso_path[0] != L'\0' || g_suppress_tray_warn) {
        fwprintf(f, L"[Settings]\n");
        if (g_last_iso_path[0] != L'\0')
            fwprintf(f, L"LastIsoPath=%s\n", g_last_iso_path);
        if (g_suppress_tray_warn)
            fwprintf(f, L"SuppressTrayWarn=1\n");
        fwprintf(f, L"\n");
    }

    for (i = 0; i < g_vm_count; i++) {
        /* Don't persist VMs that are still being built (VHDX creation in progress) */
        if (g_vms[i].building_vhdx) continue;
        fwprintf(f, L"[VM]\n");
        fwprintf(f, L"Name=%s\n", g_vms[i].name);
        fwprintf(f, L"OsType=%s\n", g_vms[i].os_type);
        fwprintf(f, L"ImagePath=%s\n", g_vms[i].image_path);
        fwprintf(f, L"VhdxPath=%s\n", g_vms[i].vhdx_path);
        fwprintf(f, L"RamMB=%lu\n", g_vms[i].ram_mb);
        fwprintf(f, L"HddGB=%lu\n", g_vms[i].hdd_gb);
        fwprintf(f, L"CpuCores=%lu\n", g_vms[i].cpu_cores);
        fwprintf(f, L"GpuMode=%d\n", g_vms[i].gpu_mode);
        fwprintf(f, L"GpuName=%s\n", g_vms[i].gpu_name);
        fwprintf(f, L"NetworkMode=%d\n", g_vms[i].network_mode);
        if (g_vms[i].net_adapter[0] != L'\0')
            fwprintf(f, L"NetAdapter=%s\n", g_vms[i].net_adapter);
        if (g_vms[i].resources_iso_path[0] != L'\0')
            fwprintf(f, L"ResourcesIso=%s\n", g_vms[i].resources_iso_path);
        if (g_vms[i].is_template)
            fwprintf(f, L"IsTemplate=1\n");
        if (g_vms[i].test_mode)
            fwprintf(f, L"TestMode=1\n");
        if (g_vms[i].install_complete)
            fwprintf(f, L"InstallComplete=1\n");
        fwprintf(f, L"\n");
    }

    fclose(f);
}

/* ---- Persistence: load VM list ---- */

static void load_vm_list(void)
{
    wchar_t path[MAX_PATH];
    wchar_t line[1024];
    FILE *f;
    VmInstance *vm = NULL;
    BOOL in_settings = FALSE;

    get_config_path(path, MAX_PATH);

    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;

    while (fgetws(line, 1024, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r'))
            line[--len] = L'\0';

        if (wcscmp(line, L"[Settings]") == 0) {
            in_settings = TRUE;
            vm = NULL;
            continue;
        }

        if (wcscmp(line, L"[VM]") == 0) {
            in_settings = FALSE;
            if (g_vm_count >= MAX_VMS) break;
            vm = &g_vms[g_vm_count];
            ZeroMemory(vm, sizeof(VmInstance));
            g_vm_count++;
            continue;
        }

        if (in_settings) {
            if (wcsncmp(line, L"LastIsoPath=", 12) == 0)
                wcscpy_s(g_last_iso_path, MAX_PATH, line + 12);
            else if (wcsncmp(line, L"SuppressTrayWarn=", 17) == 0)
                g_suppress_tray_warn = (_wtoi(line + 17) != 0);
            continue;
        }

        if (!vm) continue;

        if (wcsncmp(line, L"Name=", 5) == 0)
            wcscpy_s(vm->name, 256, line + 5);
        else if (wcsncmp(line, L"OsType=", 7) == 0)
            wcscpy_s(vm->os_type, 32, line + 7);
        else if (wcsncmp(line, L"ImagePath=", 10) == 0)
            wcscpy_s(vm->image_path, MAX_PATH, line + 10);
        else if (wcsncmp(line, L"VhdxPath=", 9) == 0)
            wcscpy_s(vm->vhdx_path, MAX_PATH, line + 9);
        else if (wcsncmp(line, L"RamMB=", 6) == 0)
            vm->ram_mb = (DWORD)_wtoi(line + 6);
        else if (wcsncmp(line, L"HddGB=", 6) == 0)
            vm->hdd_gb = (DWORD)_wtoi(line + 6);
        else if (wcsncmp(line, L"CpuCores=", 9) == 0)
            vm->cpu_cores = (DWORD)_wtoi(line + 9);
        else if (wcsncmp(line, L"GpuMode=", 8) == 0)
            vm->gpu_mode = _wtoi(line + 8);
        else if (wcsncmp(line, L"GpuName=", 8) == 0)
            wcscpy_s(vm->gpu_name, 256, line + 8);
        else if (wcsncmp(line, L"GpuDevicePath=", 14) == 0)
            { /* ignored — backwards compat */ }
        else if (wcsncmp(line, L"NetworkMode=", 12) == 0)
            vm->network_mode = _wtoi(line + 12);
        else if (wcsncmp(line, L"NetAdapter=", 11) == 0)
            wcscpy_s(vm->net_adapter, 256, line + 11);
        else if (wcsncmp(line, L"ResourcesIso=", 13) == 0)
            wcscpy_s(vm->resources_iso_path, MAX_PATH, line + 13);
        else if (wcsncmp(line, L"IsTemplate=", 11) == 0)
            vm->is_template = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"TestMode=", 9) == 0)
            vm->test_mode = (_wtoi(line + 9) != 0);
        else if (wcsncmp(line, L"InstallComplete=", 16) == 0)
            vm->install_complete = (_wtoi(line + 16) != 0);
    }

    fclose(f);

    /* Initialize snapshot lists, reset runtime state, load per-VM state */
    {
        int i;
        for (i = 0; i < g_vm_count; i++) {
            wchar_t snap_dir[MAX_PATH];
            wchar_t *last_slash;
            g_vms[i].handle = NULL;
            g_vms[i].running = FALSE;
            /* Load per-VM state JSON (may update install_complete) */
            if (vm_load_state_json(g_vms[i].vhdx_path))
                g_vms[i].install_complete = TRUE;
            wcscpy_s(snap_dir, MAX_PATH, g_vms[i].vhdx_path);
            last_slash = wcsrchr(snap_dir, L'\\');
            if (last_slash) *last_slash = L'\0';
            wcscat_s(snap_dir, MAX_PATH, L"\\snapshots");
            snapshot_init(&g_snap_lists[i], snap_dir);
        }
    }
}

/* ---- JSON state builders ---- */

static void build_host_info_json(JsonBuilder *jb)
{
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    DWORD host_cores, host_ram_mb;
    DWORD vm_cores = 0, vm_ram_mb = 0, vm_hdd_gb = 0;
    wchar_t base_dir[MAX_PATH];
    ULARGE_INTEGER free_bytes;
    DWORD free_gb = 0;
    int i;

    GetSystemInfo(&si);
    host_cores = si.dwNumberOfProcessors;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    host_ram_mb = (DWORD)(ms.ullTotalPhys / (1024 * 1024));

    for (i = 0; i < g_vm_count; i++) {
        if (g_vms[i].running) {
            vm_cores += g_vms[i].cpu_cores;
            vm_ram_mb += g_vms[i].ram_mb;
        }
        vm_hdd_gb += g_vms[i].hdd_gb;
    }

    if (!GetEnvironmentVariableW(L"ProgramData", base_dir, MAX_PATH))
        wcscpy_s(base_dir, MAX_PATH, L"C:\\ProgramData");
    if (GetDiskFreeSpaceExW(base_dir, &free_bytes, NULL, NULL))
        free_gb = (DWORD)(free_bytes.QuadPart / (1024ULL * 1024 * 1024));

    jb_int(jb, L"hostCores", (int)host_cores);
    jb_int(jb, L"hostRamMb", (int)host_ram_mb);
    jb_int(jb, L"vmCores", (int)vm_cores);
    jb_int(jb, L"vmRamMb", (int)vm_ram_mb);
    jb_int(jb, L"freeGb", (int)free_gb);
    jb_int(jb, L"vmHddGb", (int)vm_hdd_gb);
}

static void build_vm_json(JsonBuilder *jb, int i)
{
    jb_object_begin(jb);
    jb_string(jb, L"name", g_vms[i].name);
    jb_string(jb, L"osType", g_vms[i].os_type);
    jb_bool(jb, L"running", g_vms[i].running);
    jb_bool(jb, L"shuttingDown", g_vms[i].shutdown_requested);
    jb_bool(jb, L"agentOnline", g_vms[i].agent_online);
    jb_int(jb, L"ramMb", (int)g_vms[i].ram_mb);
    jb_int(jb, L"hddGb", (int)g_vms[i].hdd_gb);
    jb_int(jb, L"cpuCores", (int)g_vms[i].cpu_cores);
    jb_int(jb, L"gpuMode", g_vms[i].gpu_mode);
    jb_string(jb, L"gpuName", g_vms[i].gpu_name);
    jb_int(jb, L"networkMode", g_vms[i].network_mode);
    jb_string(jb, L"netAdapter", g_vms[i].net_adapter);
    jb_bool(jb, L"isTemplate", g_vms[i].is_template);
    jb_bool(jb, L"hypervVideoOff", g_vms[i].hyperv_video_off);
    jb_bool(jb, L"buildingVhdx", g_vms[i].building_vhdx);
    jb_bool(jb, L"vhdxStaging", g_vms[i].vhdx_staging);
    jb_int(jb, L"vhdxProgress", g_vms[i].vhdx_progress);
    jb_bool(jb, L"installComplete", g_vms[i].install_complete);
    jb_object_end(jb);
}

static void send_vm_list(void)
{
    wchar_t buf[16384];
    JsonBuilder jb;
    int i;

    jb_init(&jb, buf, 16384);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"vmListChanged");

    jb_array_begin(&jb, L"vms");
    for (i = 0; i < g_vm_count; i++) {
        if (i > 0) jb_append(&jb, L",");
        build_vm_json(&jb, i);
    }
    jb_array_end(&jb);

    /* Include host info with every VM list update */
    jb_string(&jb, L"_hi", L"");  /* dummy to keep comma logic */
    /* Nest hostInfo */
    {
        /* We'll just add the host info fields at the top level of a hostInfo object */
        wchar_t hi_buf[512];
        JsonBuilder hi;
        jb_init(&hi, hi_buf, 512);
        jb_object_begin(&hi);
        build_host_info_json(&hi);
        jb_object_end(&hi);

        if (jb.count > 0) jb_append(&jb, L",");
        jb_append(&jb, L"\"hostInfo\":");
        jb_append(&jb, hi_buf);
        jb.count++;
    }

    jb_object_end(&jb);
    webview2_post(buf);
}

static void send_snap_list(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    int i;
    SnapshotList *slist;

    if (g_selected_vm < 0 || g_selected_vm >= g_vm_count) return;
    slist = &g_snap_lists[g_selected_vm];

    jb_init(&jb, buf, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"snapListChanged");

    jb_array_begin(&jb, L"snapshots");
    for (i = 0; i < slist->count; i++) {
        FILETIME local_ft;
        SYSTEMTIME st;
        wchar_t date_buf[64];

        if (!slist->snapshots[i].valid) continue;

        FileTimeToLocalFileTime(&slist->snapshots[i].timestamp, &local_ft);
        FileTimeToSystemTime(&local_ft, &st);
        swprintf_s(date_buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        if (i > 0) jb_append(&jb, L",");
        jb_object_begin(&jb);
        jb_string(&jb, L"name", slist->snapshots[i].name);
        jb_string(&jb, L"date", date_buf);
        jb_string(&jb, L"parentVhdx", slist->snapshots[i].parent_vhdx);
        jb_object_end(&jb);
    }
    jb_array_end(&jb);

    jb_object_end(&jb);
    webview2_post(buf);
}

static void send_host_info(void)
{
    wchar_t buf[512];
    JsonBuilder jb;
    jb_init(&jb, buf, 512);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"hostInfo");
    build_host_info_json(&jb);
    jb_object_end(&jb);
    webview2_post(buf);
}

/* IF_TYPE constants */
#ifndef IF_TYPE_ETHERNET_CSMACD
#define IF_TYPE_ETHERNET_CSMACD 6
#endif
#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
#endif

typedef struct {
    wchar_t names[32][256];
    int count;
    int first_eth;
    int first_wifi;
} AdapterList;

static void adapter_enum_cb(const wchar_t *name, int if_type, void *ctx)
{
    AdapterList *al = (AdapterList *)ctx;
    if (al->count >= 32) return;
    wcscpy_s(al->names[al->count], 256, name);
    if (if_type == IF_TYPE_ETHERNET_CSMACD && al->first_eth < 0)
        al->first_eth = al->count + 1; /* +1 because (Auto) is index 0 */
    else if (if_type == IF_TYPE_IEEE80211 && al->first_wifi < 0)
        al->first_wifi = al->count + 1;
    al->count++;
}

static void send_adapters(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    AdapterList al;
    int def_idx, i;

    al.count = 0;
    al.first_eth = -1;
    al.first_wifi = -1;
    hcn_enum_adapters(adapter_enum_cb, &al);

    def_idx = (al.first_eth >= 0) ? al.first_eth :
              (al.first_wifi >= 0) ? al.first_wifi : 0;

    jb_init(&jb, buf, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"adapters");
    jb_array_begin(&jb, L"adapters");
    for (i = 0; i < al.count; i++) {
        if (i > 0) jb_append(&jb, L",");
        jb_append(&jb, L"\"");
        jb_append_escaped(&jb, al.names[i]);
        jb_append(&jb, L"\"");
    }
    jb_array_end(&jb);
    jb_int(&jb, L"defaultIndex", def_idx);
    jb_object_end(&jb);
    webview2_post(buf);
}

static void send_templates(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    int i;

    jb_init(&jb, buf, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"templates");
    jb_array_begin(&jb, L"templates");
    for (i = 0; i < g_template_count; i++) {
        if (i > 0) jb_append(&jb, L",");
        jb_object_begin(&jb);
        jb_string(&jb, L"name", g_templates[i].name);
        jb_string(&jb, L"osType", g_templates[i].os_type);
        jb_object_end(&jb);
    }
    jb_array_end(&jb);
    jb_object_end(&jb);
    webview2_post(buf);
}

static void send_full_state(void)
{
    wchar_t buf[32768];
    JsonBuilder jb;
    int i;

    jb_init(&jb, buf, 32768);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"fullState");

    /* VMs */
    jb_array_begin(&jb, L"vms");
    for (i = 0; i < g_vm_count; i++) {
        if (i > 0) jb_append(&jb, L",");
        build_vm_json(&jb, i);
    }
    jb_array_end(&jb);

    /* Host info */
    {
        wchar_t hi[512];
        JsonBuilder hj;
        jb_init(&hj, hi, 512);
        jb_object_begin(&hj);
        build_host_info_json(&hj);
        jb_object_end(&hj);
        if (jb.count > 0) jb_append(&jb, L",");
        jb_append(&jb, L"\"hostInfo\":");
        jb_append(&jb, hi);
        jb.count++;
    }

    /* Adapters */
    {
        AdapterList al;
        int def_idx;
        al.count = 0; al.first_eth = -1; al.first_wifi = -1;
        hcn_enum_adapters(adapter_enum_cb, &al);
        def_idx = (al.first_eth >= 0) ? al.first_eth :
                  (al.first_wifi >= 0) ? al.first_wifi : 0;

        jb_array_begin(&jb, L"adapters");
        for (i = 0; i < al.count; i++) {
            if (i > 0) jb_append(&jb, L",");
            jb_append(&jb, L"\"");
            jb_append_escaped(&jb, al.names[i]);
            jb_append(&jb, L"\"");
        }
        jb_array_end(&jb);
        jb_int(&jb, L"defaultAdapter", def_idx);
    }

    /* Templates */
    jb_array_begin(&jb, L"templates");
    for (i = 0; i < g_template_count; i++) {
        if (i > 0) jb_append(&jb, L",");
        jb_object_begin(&jb);
        jb_string(&jb, L"name", g_templates[i].name);
        jb_string(&jb, L"osType", g_templates[i].os_type);
        jb_object_end(&jb);
    }
    jb_array_end(&jb);

    jb_object_end(&jb);
    webview2_post(buf);
}

/* ---- Log ---- */

static DWORD g_ui_thread_id;

static void ui_log_post(const wchar_t *msg)
{
    wchar_t json[8192];
    JsonBuilder jb;
    jb_init(&jb, json, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"log");
    jb_string(&jb, L"message", msg);
    jb_object_end(&jb);
    webview2_post(json);
}

void ui_log(const wchar_t *fmt, ...)
{
    wchar_t buf[4096];
    va_list args;

    va_start(args, fmt);
    _vsnwprintf_s(buf, 4096, _TRUNCATE, fmt, args);
    va_end(args);

    /* Write to log file */
    {
        FILE *lf = NULL;
        if (_wfopen_s(&lf, L"appsandbox.log", L"a") == 0 && lf) {
            fwprintf(lf, L"%s\n", buf);
            fclose(lf);
        }
    }

    /* WebView2 COM is STA — must post from the UI thread */
    if (GetCurrentThreadId() == g_ui_thread_id) {
        ui_log_post(buf);
    } else if (g_hwnd_main) {
        size_t len = wcslen(buf) + 1;
        wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
        if (copy) {
            wcscpy_s(copy, len, buf);
            PostMessageW(g_hwnd_main, WM_WEBVIEW2_LOG, 0, (LPARAM)copy);
        }
    }
}

HINSTANCE ui_get_instance(void)
{
    return g_hInstance;
}


/* ---- Template scanning ---- */

static void scan_templates(void)
{
    wchar_t tpl_base[MAX_PATH];
    wchar_t pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    g_template_count = 0;

    {
        wchar_t base_dir[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ProgramData", base_dir, MAX_PATH))
            wcscpy_s(base_dir, MAX_PATH, L"C:\\ProgramData");
        swprintf_s(tpl_base, MAX_PATH, L"%s\\AppSandbox\\templates", base_dir);
    }

    swprintf_s(pattern, MAX_PATH, L"%s\\*", tpl_base);
    hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        wchar_t json_path[MAX_PATH];
        wchar_t vhdx_path[MAX_PATH];
        FILE *jf;
        wchar_t line[1024];

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (g_template_count >= MAX_TEMPLATES) break;

        swprintf_s(json_path, MAX_PATH, L"%s\\%s\\%s.json", tpl_base, fd.cFileName, fd.cFileName);
        swprintf_s(vhdx_path, MAX_PATH, L"%s\\%s\\disk.vhdx", tpl_base, fd.cFileName);

        if (GetFileAttributesW(json_path) == INVALID_FILE_ATTRIBUTES) continue;
        if (GetFileAttributesW(vhdx_path) == INVALID_FILE_ATTRIBUTES) continue;

        {
            TemplateInfo *ti = &g_templates[g_template_count];
            wcscpy_s(ti->name, 256, fd.cFileName);
            wcscpy_s(ti->vhdx_path, MAX_PATH, vhdx_path);
            ti->os_type[0] = L'\0';
            ti->image_path[0] = L'\0';

            if (_wfopen_s(&jf, json_path, L"r") == 0 && jf) {
                while (fgetws(line, 1024, jf)) {
                    wchar_t *p;
                    if ((p = wcsstr(line, L"\"os_type\"")) != NULL) {
                        p = wcschr(p + 9, L':');
                        if (p) { p = wcschr(p, L'"'); if (p) { wchar_t *end; p++; end = wcschr(p, L'"');
                            if (end) { *end = L'\0'; wcscpy_s(ti->os_type, 32, p); }
                        }}
                    } else if ((p = wcsstr(line, L"\"image_path\"")) != NULL) {
                        p = wcschr(p + 12, L':');
                        if (p) { p = wcschr(p, L'"'); if (p) { wchar_t *end; p++; end = wcschr(p, L'"');
                            if (end) { *end = L'\0'; wcscpy_s(ti->image_path, MAX_PATH, p); }
                        }}
                    }
                }
                fclose(jf);
            }

            g_template_count++;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

/* ---- VM operations ---- */

static void destroy_vm_at(int idx)
{
    int i;
    wchar_t dir[MAX_PATH];
    wchar_t *last_slash;

    if (idx < 0 || idx >= g_vm_count) return;

    hcs_stop_monitor(&g_vms[idx]);
    vm_agent_stop(&g_vms[idx]);

    if (g_vms[idx].running)
        hcs_terminate_vm(&g_vms[idx]);

    safe_destroy_rdp(idx);
    safe_destroy_idd(idx);

    if (g_vms[idx].network_mode != NET_NONE && !g_vms[idx].network_cleaned) {
        hcn_delete_endpoint(&g_vms[idx].endpoint_id);
        hcn_delete_network(&g_vms[idx].network_id);
        g_vms[idx].network_cleaned = TRUE;
    }

    hcs_close_vm(&g_vms[idx]);
    hcs_destroy_stale(g_vms[idx].name);

    DeleteFileW(g_vms[idx].vhdx_path);
    {
        wchar_t vmgs[MAX_PATH], vmrs[MAX_PATH], res[MAX_PATH], tmp_dir[MAX_PATH];
        wcscpy_s(tmp_dir, MAX_PATH, g_vms[idx].vhdx_path);
        { wchar_t *s = wcsrchr(tmp_dir, L'\\'); if (s) *s = L'\0'; }
        swprintf_s(vmgs, MAX_PATH, L"%s\\vm.vmgs", tmp_dir);
        swprintf_s(vmrs, MAX_PATH, L"%s\\vm.vmrs", tmp_dir);
        swprintf_s(res, MAX_PATH, L"%s\\resources.iso", tmp_dir);
        DeleteFileW(vmgs); DeleteFileW(vmrs); DeleteFileW(res);
    }
    wcscpy_s(dir, MAX_PATH, g_vms[idx].vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) {
        wchar_t snap_dir[MAX_PATH];
        *last_slash = L'\0';
        swprintf_s(snap_dir, MAX_PATH, L"%s\\snapshots", dir);
        {
            WIN32_FIND_DATAW fd;
            wchar_t pat[MAX_PATH];
            HANDLE hFind;
            swprintf_s(pat, MAX_PATH, L"%s\\*", snap_dir);
            hFind = FindFirstFileW(pat, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        wchar_t fp[MAX_PATH];
                        swprintf_s(fp, MAX_PATH, L"%s\\%s", snap_dir, fd.cFileName);
                        DeleteFileW(fp);
                    }
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
        }
        RemoveDirectoryW(snap_dir);
        RemoveDirectoryW(dir);
    }

    for (i = idx; i < g_vm_count - 1; i++) {
        g_vms[i] = g_vms[i + 1];
        g_snap_lists[i] = g_snap_lists[i + 1];
        g_displays[i] = g_displays[i + 1];
        g_idd_displays[i] = g_idd_displays[i + 1];
    }
    ZeroMemory(&g_vms[g_vm_count - 1], sizeof(VmInstance));
    g_displays[g_vm_count - 1] = NULL;
    g_idd_displays[g_vm_count - 1] = NULL;
    g_vm_count--;
}

/* ---- VHDX-first background creation thread ---- */

/* Remove a directory and all files in it (recursive) */
static void remove_staging_dir_ui(const wchar_t *dir);

static DWORD WINAPI vhdx_create_thread(LPVOID param)
{
    VhdxCreateArgs *args = (VhdxCreateArgs *)param;
    wchar_t staging[MAX_PATH], file_path[MAX_PATH];
    wchar_t manifest[MAX_PATH];
    wchar_t exe_dir[MAX_PATH], res_dir[MAX_PATH];
    wchar_t cmdline[2048];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hReadPipe = INVALID_HANDLE_VALUE, hWritePipe = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    BYTE line_buf[4096];
    int pos = 0;
    DWORD bytes_read, exit_code;
    int manifest_count;
    HRESULT hr;

    /* Create staging directory */
    swprintf_s(staging, MAX_PATH, L"%s\\_vhdx_staging", args->vhdx_dir);
    CreateDirectoryW(staging, NULL);

    /* Generate unattend.xml */
    swprintf_s(file_path, MAX_PATH, L"%s\\unattend.xml", staging);
    if (args->config.is_template) {
        if (!generate_unattend_vhdx_template(file_path, args->config.name,
                                              args->config.test_mode)) {
            args->result = E_FAIL;
            wcscpy_s(args->error_msg, 512, L"Failed to generate template unattend.xml");
            goto done;
        }
    } else {
        if (!generate_unattend_vhdx(file_path, args->config.name, args->config.admin_user,
                                     args->config.admin_pass, args->config.test_mode)) {
            args->result = E_FAIL;
            wcscpy_s(args->error_msg, 512, L"Failed to generate unattend.xml");
            goto done;
        }
    }
    SecureZeroMemory(args->config.admin_pass, sizeof(args->config.admin_pass));

    /* Generate setup.cmd */
    swprintf_s(file_path, MAX_PATH, L"%s\\setup.cmd", staging);
    generate_vhdx_setup_cmd(file_path);

    /* Generate SetupComplete.cmd */
    swprintf_s(file_path, MAX_PATH, L"%s\\SetupComplete.cmd", staging);
    generate_vhdx_setupcomplete(file_path);

    /* Determine res_dir (directory with agent exe and VDD files) */
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\'); if (slash) *slash = L'\0';
    swprintf_s(res_dir, MAX_PATH, L"%s\\resources", exe_dir);
    if (GetFileAttributesW(res_dir) == INVALID_FILE_ATTRIBUTES)
        wcscpy_s(res_dir, MAX_PATH, exe_dir);

    /* Generate manifest */
    swprintf_s(manifest, MAX_PATH, L"%s\\manifest.txt", staging);
    manifest_count = generate_vhdx_manifest(manifest, staging, res_dir,
                                             &args->config.gpu_shares);
    if (manifest_count < 0) {
        args->result = E_FAIL;
        wcscpy_s(args->error_msg, 512, L"Failed to generate staging manifest");
        goto done;
    }

    /* Build iso-patch command line */
    swprintf_s(cmdline, 2048,
        L"\"%s\\iso-patch.exe\" --to-vhdx \"%s\" 1 %lu --output \"%s\" --stage \"%s\"",
        exe_dir, args->config.image_path, args->config.hdd_gb,
        args->config.vhdx_path, manifest);

    /* Create pipe for reading iso-patch output */
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        args->result = HRESULT_FROM_WIN32(GetLastError());
        wcscpy_s(args->error_msg, 512, L"Failed to create pipe");
        goto done;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Launch iso-patch.exe */
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;  /* Merge stderr into same pipe */
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                         CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        args->result = HRESULT_FROM_WIN32(GetLastError());
        swprintf_s(args->error_msg, 512, L"Failed to launch iso-patch.exe (error %lu)", GetLastError());
        goto done;
    }

    CloseHandle(hWritePipe);
    hWritePipe = INVALID_HANDLE_VALUE;

    /* Read pipe output line by line.
       iso-patch uses wprintf() which, when stdout is a pipe, outputs narrow-byte
       characters in the process code page (typically UTF-8 or the system ACP).
       We read as narrow chars and compare narrow prefixes. */
    args->result = E_FAIL;  /* Will be set to S_OK on DONE */
    pos = 0;
    {
        char *cbuf = (char *)line_buf;
        int cbuf_size = (int)sizeof(line_buf);

        while (ReadFile(hReadPipe, cbuf + pos, (DWORD)(cbuf_size - pos - 1), &bytes_read, NULL) && bytes_read > 0) {
            int end = pos + (int)bytes_read;
            int start = 0;
            int ci;
            cbuf[end] = '\0';

            for (ci = start; ci < end; ci++) {
                if (cbuf[ci] == '\n' || cbuf[ci] == '\r') {
                    cbuf[ci] = '\0';
                    if (ci > start) {
                        char *line = cbuf + start;

                        if (strncmp(line, "STATUS:", 7) == 0) {
                            /* Informational log — don't reset progress/staging */
                        } else if (strncmp(line, "PROGRESS:", 9) == 0) {
                            int pct = atoi(line + 9);
                            /* Check if this is the staging phase */
                            int is_staging = (strstr(line + 9, "Staging") != NULL) ? 1 : 0;
                            PostMessageW(g_hwnd_main, WM_VHDX_PROGRESS,
                                         (WPARAM)args->vm_index, MAKELPARAM(pct, is_staging));
                        } else if (strncmp(line, "ERROR:", 6) == 0) {
                            /* Keep first error (the specific one) */
                            if (args->error_msg[0] == L'\0')
                                MultiByteToWideChar(CP_ACP, 0, line + 6, -1,
                                                    args->error_msg, 512);
                            args->result = E_FAIL;
                        } else if (strncmp(line, "DONE:", 5) == 0) {
                            args->result = S_OK;
                        }
                    }
                    start = ci + 1;
                    if (start < end && (cbuf[start] == '\n' || cbuf[start] == '\r'))
                        start++;
                }
            }

            if (start < end) {
                memmove(cbuf, cbuf + start, end - start);
                pos = end - start;
            } else {
                pos = 0;
            }
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0 && args->result != S_OK) {
        if (args->error_msg[0] == L'\0')
            swprintf_s(args->error_msg, 512, L"iso-patch.exe exited with code %lu", exit_code);
        args->result = E_FAIL;
    }

    if (FAILED(args->result))
        goto done;

    /* VHDX created successfully — now create network + HCS VM + start */

    /* Network */
    if (args->config.network_mode != NET_NONE) {
        switch (args->config.network_mode) {
        case NET_NAT:      hr = hcn_create_nat_network(&args->network_id); break;
        case NET_INTERNAL: hr = hcn_create_internal_network(&args->network_id); break;
        case NET_EXTERNAL: hr = hcn_create_external_network(&args->network_id, args->net_adapter); break;
        default:           hr = E_INVALIDARG; break;
        }
        if (SUCCEEDED(hr)) {
            hr = hcn_create_endpoint(&args->network_id, &args->endpoint_id,
                                      args->endpoint_guid, 64);
            if (SUCCEEDED(hr))
                args->has_network = TRUE;
            else
                hcn_delete_network(&args->network_id);
        }
        if (FAILED(hr))
            args->config.network_mode = NET_NONE;
    }

    /* VHDX-first: no install ISO, no resources ISO */
    args->config.image_path[0] = L'\0';
    args->config.resources_iso_path[0] = L'\0';

    /* Create HCS VM */
    {
        VmInstance temp_inst;
        ZeroMemory(&temp_inst, sizeof(temp_inst));

        hr = (args->endpoint_guid[0] != L'\0')
            ? hcs_create_vm_with_endpoint(&args->config, args->endpoint_guid, &temp_inst)
            : hcs_create_vm(&args->config, &temp_inst);
        if (FAILED(hr)) {
            args->result = hr;
            swprintf_s(args->error_msg, 512, L"Failed to create HCS VM (0x%08X)", hr);
            goto done;
        }

        /* Start VM */
        hr = hcs_start_vm(&temp_inst);
        if (FAILED(hr)) {
            args->result = hr;
            swprintf_s(args->error_msg, 512, L"Failed to start VM (0x%08X)", hr);
            hcs_close_vm(&temp_inst);
            goto done;
        }

        /* Pass temp_inst to UI thread via heap allocation (args->vm_inst) */
        {
            VmInstance *heap_inst = (VmInstance *)HeapAlloc(GetProcessHeap(),
                                                            HEAP_ZERO_MEMORY, sizeof(VmInstance));
            if (heap_inst)
                memcpy(heap_inst, &temp_inst, sizeof(VmInstance));
            args->vm_inst = heap_inst;
        }

        args->result = S_OK;
    }

done:
    /* Clean up staging dir */
    {
        wchar_t staging_dir[MAX_PATH];
        swprintf_s(staging_dir, MAX_PATH, L"%s\\_vhdx_staging", args->vhdx_dir);
        remove_staging_dir_ui(staging_dir);
    }

    if (hReadPipe != INVALID_HANDLE_VALUE) CloseHandle(hReadPipe);
    if (hWritePipe != INVALID_HANDLE_VALUE) CloseHandle(hWritePipe);

    PostMessageW(g_hwnd_main, WM_VHDX_DONE, 0, (LPARAM)args);
    return 0;
}

static void remove_staging_dir_ui(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                continue;
            swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_staging_dir_ui(full);
            else
                DeleteFileW(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* ---- Create VM (from JSON message) ---- */

static void on_create_vm(const wchar_t *json)
{
    VmConfig config;
    VmInstance *inst;
    wchar_t vhdx_dir[MAX_PATH];
    wchar_t endpoint_guid_str[64] = { 0 };
    HRESULT hr;
    int existing_idx;
    BOOL is_template_create = FALSE;
    wchar_t template_name[256] = { 0 };
    int template_idx = -1;
    BOOL from_template = FALSE;

    ZeroMemory(&config, sizeof(config));

    /* Parse fields from JSON */
    json_get_string(json, L"name", config.name, 256);
    json_get_string(json, L"osType", config.os_type, 32);
    json_get_string(json, L"imagePath", config.image_path, MAX_PATH);
    json_get_string(json, L"templateName", template_name, 256);
    json_get_bool(json, L"isTemplate", &is_template_create);
    json_get_bool(json, L"testMode", &config.test_mode);
    {
        int val;
        if (json_get_int(json, L"hddGb", &val)) config.hdd_gb = (DWORD)val;
        if (json_get_int(json, L"ramMb", &val)) config.ram_mb = (DWORD)val;
        if (json_get_int(json, L"cpuCores", &val)) config.cpu_cores = (DWORD)val;
        if (json_get_int(json, L"gpuMode", &val)) config.gpu_mode = val;
        if (json_get_int(json, L"networkMode", &val)) config.network_mode = val;
    }
    json_get_string(json, L"adminUser", config.admin_user, 128);
    json_get_string(json, L"adminPass", config.admin_pass, 128);

    /* Defaults */
    if (config.hdd_gb == 0) config.hdd_gb = 64;
    if (config.ram_mb == 0) config.ram_mb = 4096;
    if (config.cpu_cores == 0) config.cpu_cores = 4;

    if (config.name[0] == L'\0') {
        ui_log(L"Error: VM name is required.");
        return;
    }

    /* Resolve template */
    if (template_name[0] != L'\0') {
        int i;
        for (i = 0; i < g_template_count; i++) {
            if (_wcsicmp(g_templates[i].name, template_name) == 0) {
                template_idx = i;
                from_template = TRUE;
                wcscpy_s(config.os_type, 32, g_templates[i].os_type);
                break;
            }
        }
    }

    if (is_template_create && from_template) {
        ui_log(L"Error: Cannot create a template from another template.");
        return;
    }

    if (config.image_path[0] != L'\0')
        wcscpy_s(g_last_iso_path, MAX_PATH, config.image_path);

    /* Check for duplicate name against existing VMs */
    existing_idx = -1;
    {
        int i;
        for (i = 0; i < g_vm_count; i++) {
            if (_wcsicmp(g_vms[i].name, config.name) == 0) {
                existing_idx = i;
                break;
            }
        }
    }

    if (existing_idx >= 0) {
        ui_log(L"Error: A VM named \"%s\" already exists.", config.name);
        return;
    }

    /* Check for duplicate name against existing templates */
    {
        int i;
        for (i = 0; i < g_template_count; i++) {
            if (_wcsicmp(g_templates[i].name, config.name) == 0) {
                ui_log(L"Error: A template named \"%s\" already exists.", config.name);
                return;
            }
        }
    }

    if (g_vm_count >= MAX_VMS) {
        ui_log(L"Maximum VM count reached (%d)", MAX_VMS);
        return;
    }

    inst = &g_vms[g_vm_count];
    ZeroMemory(inst, sizeof(VmInstance));

    /* Net adapter */
    {
        wchar_t adapter[256] = { 0 };
        json_get_string(json, L"netAdapter", adapter, 256);
        if (adapter[0] != L'\0' && wcscmp(adapter, L"(Auto)") != 0)
            wcscpy_s(inst->net_adapter, 256, adapter);
    }

    /* Template flag */
    if (is_template_create) {
        config.gpu_mode = GPU_NONE;
        config.network_mode = NET_NONE;
    }
    config.is_template = is_template_create;

    if (config.admin_user[0] == L'\0')
        wcscpy_s(config.admin_user, 128, L"User");

    /* Create VHDX directory */
    {
        wchar_t base_dir[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ProgramData", base_dir, MAX_PATH))
            wcscpy_s(base_dir, MAX_PATH, L"C:\\ProgramData");
        swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox", base_dir);
        CreateDirectoryW(vhdx_dir, NULL);

        if (is_template_create) {
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\templates", base_dir);
            CreateDirectoryW(vhdx_dir, NULL);
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\templates\\%s", base_dir, config.name);
        } else {
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\%s", base_dir, config.name);
        }
    }
    CreateDirectoryW(vhdx_dir, NULL);
    swprintf_s(config.vhdx_path, MAX_PATH, L"%s\\disk.vhdx", vhdx_dir);

    /* GPU driver shares */
    if (config.gpu_mode == GPU_DEFAULT && !is_template_create)
        gpu_get_driver_shares(&g_gpu_list, &config.gpu_shares);

    /* ---- VHDX-first path for Windows VMs (normal + template) ---- */
    {
        BOOL use_vhdx_first = (!from_template &&
                                _wcsicmp(config.os_type, L"Windows") == 0 &&
                                config.image_path[0] != L'\0' &&
                                (is_template_create || config.admin_pass[0] != L'\0'));

        if (use_vhdx_first) {
            VhdxCreateArgs *args;

            /* Add VM to list early with building_vhdx flag */
            wcscpy_s(inst->name, 256, config.name);
            wcscpy_s(inst->os_type, 32, config.os_type);
            wcscpy_s(inst->vhdx_path, MAX_PATH, config.vhdx_path);
            wcscpy_s(inst->image_path, MAX_PATH, config.image_path);
            inst->ram_mb = config.ram_mb;
            inst->hdd_gb = config.hdd_gb;
            inst->cpu_cores = config.cpu_cores;
            inst->gpu_mode = config.gpu_mode;
            wcscpy_s(inst->gpu_name, 256, (config.gpu_mode == GPU_DEFAULT) ? L"Default GPU" : L"None");
            inst->network_mode = config.network_mode;
            inst->is_template = is_template_create;
            inst->test_mode = config.test_mode;
            inst->building_vhdx = TRUE;
            inst->vhdx_progress = 0;
            memcpy(&inst->gpu_shares, &config.gpu_shares, sizeof(GpuDriverShareList));

            { wchar_t sd[MAX_PATH]; swprintf_s(sd, MAX_PATH, L"%s\\snapshots", vhdx_dir); snapshot_init(&g_snap_lists[g_vm_count], sd); }
            g_vm_count++;

            /* Write initial install state for non-template VMs */
            if (!is_template_create)
                vm_save_state_json(config.vhdx_path, FALSE);

            save_vm_list();
            send_vm_list();

            /* Allocate args and kick off background thread */
            args = (VhdxCreateArgs *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VhdxCreateArgs));
            if (!args) {
                ui_log(L"Error: Out of memory for VHDX create args.");
                g_vm_count--;
                return;
            }
            memcpy(&args->config, &config, sizeof(VmConfig));
            args->vm_index = g_vm_count - 1;
            wcscpy_s(args->vhdx_dir, MAX_PATH, vhdx_dir);
            wcscpy_s(args->net_adapter, 256, inst->net_adapter);

            ui_log(L"Building VHDX for \"%s\" (this may take several minutes)...", config.name);
            CreateThread(NULL, 0, vhdx_create_thread, args, 0, NULL);
            return;
        }
    }

    /* ---- Existing synchronous path (from-template, non-Windows, no-password) ---- */

    /* Create disk */
    if (from_template) {
        ui_log(L"Creating differencing VHDX from template \"%s\"...", g_templates[template_idx].name);
        hr = vhdx_create_differencing(config.vhdx_path, g_templates[template_idx].vhdx_path);
        if (FAILED(hr)) { ui_log(L"Error: Failed to create differencing VHDX (0x%08X)", hr); return; }
        ui_log(L"Differencing VHDX created.");
    } else {
        ui_log(L"Creating VHDX: %s (%lu GB)...", config.vhdx_path, config.hdd_gb);
        hr = vhdx_create(config.vhdx_path, (ULONGLONG)config.hdd_gb);
        if (FAILED(hr)) { ui_log(L"Error: Failed to create VHDX (0x%08X)", hr); return; }
        ui_log(L"VHDX created successfully.");
    }

    /* Resources ISO */
    {
        wchar_t res_iso[MAX_PATH], exe_dir[MAX_PATH], res_dir[MAX_PATH];
        wchar_t *slash;
        swprintf_s(res_iso, MAX_PATH, L"%s\\resources.iso", vhdx_dir);
        GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
        slash = wcsrchr(exe_dir, L'\\'); if (slash) *slash = L'\0';
        swprintf_s(res_dir, MAX_PATH, L"%s\\resources", exe_dir);
        if (GetFileAttributesW(res_dir) == INVALID_FILE_ATTRIBUTES)
            wcscpy_s(res_dir, MAX_PATH, exe_dir);

        if (is_template_create) {
            if (_wcsicmp(config.os_type, L"Windows") == 0 && config.image_path[0] != L'\0') {
                hr = iso_create_resources(res_iso, config.name, config.admin_user, config.admin_pass, res_dir, TRUE, config.test_mode);
                if (SUCCEEDED(hr)) wcscpy_s(config.resources_iso_path, MAX_PATH, res_iso);
                else ui_log(L"Warning: Failed to create template resources ISO (0x%08X).", hr);
            }
        } else if (from_template) {
            if (_wcsicmp(config.os_type, L"Windows") == 0 && config.admin_pass[0] != L'\0') {
                hr = iso_create_instance_resources(res_iso, config.name, config.admin_user, config.admin_pass, res_dir);
                if (SUCCEEDED(hr)) wcscpy_s(config.resources_iso_path, MAX_PATH, res_iso);
                else ui_log(L"Warning: Failed to create instance resources ISO (0x%08X).", hr);
            }
        } else {
            if (_wcsicmp(config.os_type, L"Windows") == 0 && config.image_path[0] != L'\0' && config.admin_pass[0] != L'\0') {
                hr = iso_create_resources(res_iso, config.name, config.admin_user, config.admin_pass, res_dir, FALSE, config.test_mode);
                if (SUCCEEDED(hr)) wcscpy_s(config.resources_iso_path, MAX_PATH, res_iso);
                else ui_log(L"Warning: Failed to create resources ISO (0x%08X).", hr);
            }
        }
    }
    SecureZeroMemory(config.admin_pass, sizeof(config.admin_pass));

    inst->network_cleaned = FALSE;

    /* Networking */
    if (config.network_mode != NET_NONE) {
        const wchar_t *ntn = L"";
        switch (config.network_mode) {
        case NET_NAT: ntn = L"NAT"; break;
        case NET_EXTERNAL: ntn = L"External"; break;
        case NET_INTERNAL: ntn = L"Internal"; break;
        }
        ui_log(L"Creating %s network...", ntn);
        switch (config.network_mode) {
        case NET_NAT:      hr = hcn_create_nat_network(&inst->network_id); break;
        case NET_INTERNAL: hr = hcn_create_internal_network(&inst->network_id); break;
        case NET_EXTERNAL: hr = hcn_create_external_network(&inst->network_id, inst->net_adapter); break;
        default:           hr = E_INVALIDARG; break;
        }
        if (FAILED(hr)) {
            ui_log(L"Warning: Network failed (0x%08X). Continuing without.", hr);
            config.network_mode = NET_NONE;
        } else {
            hr = hcn_create_endpoint(&inst->network_id, &inst->endpoint_id, endpoint_guid_str, 64);
            if (FAILED(hr)) { ui_log(L"Warning: Endpoint failed (0x%08X).", hr); hcn_delete_network(&inst->network_id); config.network_mode = NET_NONE; }
            else ui_log(L"%s network created.", ntn);
        }
    }

    /* Create VM via HCS */
    ui_log(L"Creating %sVM \"%s\"...", is_template_create ? L"template " : L"", config.name);
    hr = (endpoint_guid_str[0] != L'\0')
        ? hcs_create_vm_with_endpoint(&config, endpoint_guid_str, inst)
        : hcs_create_vm(&config, inst);
    if (FAILED(hr)) { ui_log(L"Error: Failed to create VM (0x%08X)", hr); return; }

    wcscpy_s(inst->gpu_name, 256, (config.gpu_mode == GPU_DEFAULT) ? L"Default GPU" : L"None");
    wcscpy_s(inst->resources_iso_path, MAX_PATH, config.resources_iso_path);

    { wchar_t sd[MAX_PATH]; swprintf_s(sd, MAX_PATH, L"%s\\snapshots", vhdx_dir); snapshot_init(&g_snap_lists[g_vm_count], sd); }
    g_vm_count++;

    /* Write initial install state for non-template VMs */
    if (!is_template_create)
        vm_save_state_json(config.vhdx_path, FALSE);

    /* Auto-start */
    ui_log(L"Starting VM \"%s\"...", config.name);
    hr = hcs_start_vm(inst);
    if (SUCCEEDED(hr)) {
        hcs_start_monitor(inst);
        vm_agent_start(inst);
        ui_log(L"VM \"%s\" %s.", config.name, is_template_create ? L"started (template)" : L"created and started");
    } else {
        ui_log(L"VM \"%s\" created but failed to start (0x%08X).", config.name, hr);
    }

    save_vm_list();
    send_vm_list();

    /* Auto-connect display */
    if (SUCCEEDED(hr) && !is_template_create) {
        int vm_idx = g_vm_count - 1;
        g_displays[vm_idx] = vm_display_create(inst, g_hInstance, g_hwnd_main);
        if (!g_displays[vm_idx]) ui_log(L"Warning: Failed to auto-create display window.");
    }
}

/* ---- Start VM ---- */

typedef struct {
    VmInstance *vm;
    VmConfig   config;
    GUID       network_id;
    GUID       endpoint_id;
    int        network_mode;
} StartVmArgs;

static DWORD WINAPI start_vm_thread(LPVOID param)
{
    StartVmArgs *args = (StartVmArgs *)param;
    VmInstance *vm = args->vm;
    HRESULT hr;
    wchar_t endpoint_guid_str[64] = { 0 };

    if (args->network_mode != NET_NONE) {
        switch (args->network_mode) {
        case NET_NAT:      hr = hcn_create_nat_network(&args->network_id); break;
        case NET_INTERNAL: hr = hcn_create_internal_network(&args->network_id); break;
        case NET_EXTERNAL: hr = hcn_create_external_network(&args->network_id, args->vm->net_adapter); break;
        default:           hr = E_INVALIDARG; break;
        }
        if (SUCCEEDED(hr)) {
            hr = hcn_create_endpoint(&args->network_id, &args->endpoint_id, endpoint_guid_str, 64);
            if (FAILED(hr)) {
                ui_log(L"Error: Network endpoint failed (0x%08X).", hr);
                PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, 0, (LPARAM)vm);
                free(args); return 1;
            }
            vm->network_id = args->network_id;
            vm->endpoint_id = args->endpoint_id;
        } else {
            ui_log(L"Error: Network unavailable (0x%08X).", hr);
            PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, 0, (LPARAM)vm);
            free(args); return 1;
        }
    }

    if (args->config.gpu_mode == GPU_DEFAULT)
        gpu_get_driver_shares(&g_gpu_list, &args->config.gpu_shares);

    ui_log(L"Re-creating HCS compute system for \"%s\"...", vm->name);
    hr = (endpoint_guid_str[0] != L'\0')
        ? hcs_create_vm_with_endpoint(&args->config, endpoint_guid_str, vm)
        : hcs_create_vm(&args->config, vm);
    if (FAILED(hr)) {
        ui_log(L"Error: Failed to create compute system (0x%08X)", hr);
        PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, 0, (LPARAM)vm);
        free(args); return 1;
    }

    ui_log(L"Starting VM \"%s\"...", vm->name);
    hr = hcs_start_vm(vm);
    if (FAILED(hr)) {
        ui_log(L"Error: Failed to start VM (0x%08X)", hr);
    } else {
        ui_log(L"VM \"%s\" started.", vm->name);
        vm_agent_start(vm);
        hcs_start_monitor(vm);
    }
    PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, 0, (LPARAM)vm);
    free(args);
    return 0;
}

static void do_start_vm(int idx)
{
    VmInstance *vm;
    if (idx < 0 || idx >= g_vm_count) return;
    vm = &g_vms[idx];

    if (vm->running) { ui_log(L"VM \"%s\" is already running.", vm->name); return; }

    if (!vm->handle) {
        StartVmArgs *args = (StartVmArgs *)calloc(1, sizeof(StartVmArgs));
        if (!args) return;
        args->vm = vm;
        wcscpy_s(args->config.name, 256, vm->name);
        wcscpy_s(args->config.os_type, 32, vm->os_type);
        wcscpy_s(args->config.image_path, MAX_PATH, vm->image_path);
        wcscpy_s(args->config.vhdx_path, MAX_PATH, vm->vhdx_path);
        args->config.ram_mb = vm->ram_mb;
        args->config.hdd_gb = vm->hdd_gb;
        args->config.cpu_cores = vm->cpu_cores;
        args->config.gpu_mode = vm->gpu_mode;
        args->config.network_mode = vm->network_mode;
        args->config.test_mode = vm->test_mode;
        wcscpy_s(args->config.resources_iso_path, MAX_PATH, vm->resources_iso_path);
        args->network_mode = vm->network_mode;
        vm->network_cleaned = FALSE;
        ui_log(L"Starting VM \"%s\" (background)...", vm->name);
        CloseHandle(CreateThread(NULL, 0, start_vm_thread, args, 0, NULL));
    } else {
        HRESULT hr = hcs_start_vm(vm);
        if (hr == (HRESULT)0x80370110L && vm->handle) {
            hcs_terminate_vm(vm); hcs_close_vm(vm);
            do_start_vm(idx); return;
        }
        if (FAILED(hr)) { ui_log(L"Error: Failed to start VM (0x%08X)", hr); }
        else {
            ui_log(L"VM \"%s\" started.", vm->name);
            vm_agent_start(vm);
            hcs_start_monitor(vm);
            if (!g_displays[idx]) {
                g_displays[idx] = vm_display_create(vm, g_hInstance, g_hwnd_main);
                if (!g_displays[idx]) ui_log(L"Warning: Failed to auto-create display window.");
            }
        }
        send_vm_list();
    }
}

static void do_shutdown_vm(int idx)
{
    HRESULT hr;
    if (idx < 0 || idx >= g_vm_count) return;
    if (!g_vms[idx].running) { ui_log(L"VM \"%s\" is not running.", g_vms[idx].name); return; }

    ui_log(L"Sending shutdown signal to \"%s\"...", g_vms[idx].name);
    hr = hcs_stop_vm(&g_vms[idx]);
    if (FAILED(hr)) {
        ui_log(L"Shutdown failed (0x%08X). Use Force Stop.", hr);
        return;
    }

    g_vms[idx].shutdown_requested = TRUE;
    g_vms[idx].shutdown_time = GetTickCount64();

    safe_destroy_rdp(idx);
    safe_destroy_idd(idx);

    ui_log(L"Shutdown signal sent to \"%s\".", g_vms[idx].name);
    send_vm_list();
}

/* ---- Background HCS op thread ---- */

static DWORD WINAPI hcs_op_thread(LPVOID param)
{
    HcsAsyncOp *op = (HcsAsyncOp *)param;
    switch (op->op_type) {
    case HCS_OP_STOP:       op->result = hcs_terminate_vm(op->vm); break;
    case HCS_OP_SNAP_TAKE:  op->result = snapshot_take(&g_snap_lists[op->vm_index], op->vm, op->snap_name); break;
    case HCS_OP_SNAP_REVERT: op->result = snapshot_revert(&g_snap_lists[op->vm_index], op->vm, op->snap_index); break;
    }
    PostMessageW(g_hwnd_main, WM_HCS_OP_DONE, 0, (LPARAM)op);
    return 0;
}

static BOOL launch_hcs_op(HcsAsyncOp *op)
{
    HANDLE t = CreateThread(NULL, 0, hcs_op_thread, op, 0, NULL);
    if (!t) { ui_log(L"Error: Failed to create background thread."); free(op); return FALSE; }
    CloseHandle(t);
    return TRUE;
}

static void do_stop_vm(int idx)
{
    HcsAsyncOp *op;
    if (idx < 0 || idx >= g_vm_count) return;
    if (!g_vms[idx].running) { ui_log(L"VM \"%s\" is not running.", g_vms[idx].name); return; }

    g_vms[idx].running = FALSE;
    g_vms[idx].shutdown_requested = FALSE;
    g_vms[idx].hyperv_video_off = FALSE;
    hcs_stop_monitor(&g_vms[idx]);
    vm_agent_stop(&g_vms[idx]);

    safe_destroy_rdp(idx);
    safe_destroy_idd(idx);

    op = (HcsAsyncOp *)calloc(1, sizeof(HcsAsyncOp));
    if (!op) return;
    op->op_type = HCS_OP_STOP;
    op->vm_index = idx;
    op->vm = &g_vms[idx];
    ui_log(L"Force stopping VM \"%s\"...", g_vms[idx].name);
    launch_hcs_op(op);
    send_vm_list();
}

static void do_connect_rdp(int idx)
{
    if (idx < 0 || idx >= g_vm_count) return;
    if (!g_vms[idx].running) { ui_log(L"VM \"%s\" is not running.", g_vms[idx].name); return; }
    if (g_vms[idx].hyperv_video_off)
        ui_log(L"Warning: Hyper-V Video adapter is disabled for \"%s\" - RDP may not connect.", g_vms[idx].name);
    if (g_displays[idx] && vm_display_is_open(g_displays[idx])) {
        ui_log(L"Display for \"%s\" is already open.", g_vms[idx].name);
        return;
    }
    safe_destroy_rdp(idx);
    ui_log(L"Opening display for \"%s\"...", g_vms[idx].name);
    g_displays[idx] = vm_display_create(&g_vms[idx], g_hInstance, g_hwnd_main);
    if (!g_displays[idx]) ui_log(L"Error: Failed to create display window.");
}

static void do_connect_idd(int idx)
{
    if (idx < 0 || idx >= g_vm_count) return;
    if (!g_vms[idx].running) { ui_log(L"VM \"%s\" is not running.", g_vms[idx].name); return; }
    if (g_idd_displays[idx] && vm_display_idd_is_open(g_idd_displays[idx])) {
        ui_log(L"IDD display for \"%s\" is already open.", g_vms[idx].name);
        return;
    }
    safe_destroy_idd(idx);
    ui_log(L"Opening IDD display for \"%s\"...", g_vms[idx].name);
    g_idd_displays[idx] = vm_display_idd_create(&g_vms[idx], g_hInstance, g_hwnd_main);
    if (!g_idd_displays[idx]) ui_log(L"Error: Failed to create IDD display window.");
}

/* ---- System tray ---- */

static void tray_add(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wcscpy_s(g_nid.szTip, 128, L"App Sandbox");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int i, cmd;

    AppendMenuW(menu, MF_STRING, TRAY_CMD_SHOW, L"Show App Sandbox");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    for (i = 0; i < g_vm_count; i++) {
        wchar_t label[300];
        if (!g_vms[i].running) continue;
        swprintf_s(label, 300, L"%s  ", g_vms[i].name);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, label);
        AppendMenuW(menu, MF_STRING, TRAY_CMD_CONNECT_BASE + i,  L"    \U0001F4FA Connect");
        AppendMenuW(menu, MF_STRING, TRAY_CMD_SHUTDOWN_BASE + i, L"    \u23FB Shutdown");
        AppendMenuW(menu, MF_STRING, TRAY_CMD_STOP_BASE + i,     L"    \u2715 Force Stop");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, TRAY_CMD_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);

    if (cmd == TRAY_CMD_SHOW) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    } else if (cmd == TRAY_CMD_EXIT) {
        /* Real exit — force close even with running VMs */
        DestroyWindow(hwnd);
    } else if (cmd >= TRAY_CMD_STOP_BASE) {
        do_stop_vm(cmd - TRAY_CMD_STOP_BASE);
    } else if (cmd >= TRAY_CMD_SHUTDOWN_BASE) {
        do_shutdown_vm(cmd - TRAY_CMD_SHUTDOWN_BASE);
    } else if (cmd >= TRAY_CMD_CONNECT_BASE) {
        do_connect_idd(cmd - TRAY_CMD_CONNECT_BASE);
    }
}

static void on_snap_take(void)
{
    HcsAsyncOp *op;
    if (g_selected_vm < 0 || g_selected_vm >= g_vm_count) return;
    if (!g_vms[g_selected_vm].running) { ui_log(L"VM must be running to take a snapshot."); return; }

    op = (HcsAsyncOp *)calloc(1, sizeof(HcsAsyncOp));
    if (!op) return;
    op->op_type = HCS_OP_SNAP_TAKE;
    op->vm_index = g_selected_vm;
    op->vm = &g_vms[g_selected_vm];
    swprintf_s(op->snap_name, 128, L"Snapshot %d", g_snap_lists[g_selected_vm].count + 1);
    ui_log(L"Taking snapshot \"%s\" of VM \"%s\"...", op->snap_name, op->vm->name);
    launch_hcs_op(op);
}

static void on_snap_revert(int snap_idx)
{
    HcsAsyncOp *op;
    if (g_selected_vm < 0 || g_selected_vm >= g_vm_count) return;
    if (snap_idx < 0) { ui_log(L"Select a snapshot to revert to."); return; }

    if (g_vms[g_selected_vm].running) {
        hcs_stop_monitor(&g_vms[g_selected_vm]);
        vm_agent_stop(&g_vms[g_selected_vm]);
        safe_destroy_rdp(g_selected_vm);
        safe_destroy_idd(g_selected_vm);
    }

    op = (HcsAsyncOp *)calloc(1, sizeof(HcsAsyncOp));
    if (!op) return;
    op->op_type = HCS_OP_SNAP_REVERT;
    op->vm_index = g_selected_vm;
    op->vm = &g_vms[g_selected_vm];
    op->snap_index = snap_idx;
    ui_log(L"Reverting VM \"%s\" to snapshot %d...", op->vm->name, snap_idx);
    launch_hcs_op(op);
}

static void on_snap_delete(int snap_idx)
{
    HRESULT hr;
    if (g_selected_vm < 0 || g_selected_vm >= g_vm_count) return;
    if (snap_idx < 0) { ui_log(L"Select a snapshot to delete."); return; }

    ui_log(L"Deleting snapshot %d...", snap_idx);
    hr = snapshot_delete(&g_snap_lists[g_selected_vm], snap_idx);
    if (FAILED(hr)) ui_log(L"Error: Failed to delete snapshot (0x%08X)", hr);
    else ui_log(L"Snapshot deleted.");
    send_snap_list();
}

/* ---- Edit VM inline ---- */

static void on_edit_vm(const wchar_t *json)
{
    int idx;
    wchar_t field[64], value[256];
    VmInstance *vm;

    if (!json_get_int(json, L"vmIndex", &idx)) return;
    if (idx < 0 || idx >= g_vm_count) return;
    if (!json_get_string(json, L"field", field, 64)) return;
    if (!json_get_string(json, L"value", value, 256)) return;

    vm = &g_vms[idx];
    if (vm->running) return;

    if (wcscmp(field, L"name") == 0) {
        if (value[0] != L'\0') {
            BOOL dup = FALSE;
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (i != idx && _wcsicmp(g_vms[i].name, value) == 0) { dup = TRUE; break; }
            }
            if (!dup) wcscpy_s(vm->name, 256, value);
            else ui_log(L"Name \"%s\" is already in use.", value);
        }
    } else if (wcscmp(field, L"ramMb") == 0) {
        DWORD ram = (DWORD)_wtoi(value);
        if (ram < 4000) ram = 4000;
        vm->ram_mb = ram;
    } else if (wcscmp(field, L"cpuCores") == 0) {
        DWORD cores = (DWORD)_wtoi(value);
        if (cores < 1) cores = 1;
        vm->cpu_cores = cores;
    } else if (wcscmp(field, L"gpuMode") == 0) {
        int mode = _wtoi(value);
        vm->gpu_mode = mode;
        wcscpy_s(vm->gpu_name, 256, (mode == GPU_DEFAULT) ? L"Default GPU" : L"None");
    } else if (wcscmp(field, L"networkMode") == 0) {
        int mode = _wtoi(value);
        if (mode >= 0 && mode <= 3) vm->network_mode = mode;
    }

    save_vm_list();
    send_vm_list();
}

/* ---- WebView2 message dispatch ---- */

static void on_webview2_message(const wchar_t *json)
{
    wchar_t action[64] = { 0 };

    if (!json_get_string(json, L"action", action, 64))
        return;

    if (wcscmp(action, L"uiReady") == 0) {
        /* Backend init — runs after page is loaded so logs appear in UI */
        ui_log(L"App Sandbox initialized.");
        if (!prereq_check_all())
            ui_log(L"HypervisorPlatform prerequisite not met.");
        ui_log(L"HCS: %s", hcs_init() ? L"Available" : L"NOT available (is Hyper-V enabled?)");
        vmms_cert_ensure();
        ui_log(L"HCN: %s", hcn_init() ? L"Available" : L"NOT available");
        gpu_enumerate(&g_gpu_list);
        {
            int gi;
            ui_log(L"GPUs: %d found, %d driver shares", g_gpu_list.count, g_gpu_list.shares.count);
            for (gi = 0; gi < g_gpu_list.count; gi++) {
                ui_log(L"  [%d] %s (GPU-PV)", gi, g_gpu_list.gpus[gi].name);
                if (g_gpu_list.gpus[gi].driver_store_path[0])
                    ui_log(L"      %s", g_gpu_list.gpus[gi].driver_store_path);
            }
        }
        load_vm_list();
        scan_templates();
        if (g_vm_count > 0) ui_log(L"Loaded %d VM(s) from config.", g_vm_count);
        if (g_template_count > 0) ui_log(L"Found %d template(s).", g_template_count);
        send_full_state();
    } else if (wcscmp(action, L"createVm") == 0) {
        on_create_vm(json);
    } else if (wcscmp(action, L"startVm") == 0) {
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_start_vm(idx);
    } else if (wcscmp(action, L"shutdownVm") == 0) {
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_shutdown_vm(idx);
    } else if (wcscmp(action, L"stopVm") == 0) {
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_stop_vm(idx);
    } else if (wcscmp(action, L"connectVm") == 0) {
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_connect_rdp(idx);
    } else if (wcscmp(action, L"connectIddVm") == 0) {
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_connect_idd(idx);
    } else if (wcscmp(action, L"deleteVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx) && idx >= 0 && idx < g_vm_count) {
            ui_log(L"Deleting VM \"%s\"...", g_vms[idx].name);
            destroy_vm_at(idx);
            g_selected_vm = -1;
            save_vm_list();
            send_vm_list();
            send_snap_list();
            ui_log(L"VM deleted.");
        }
    } else if (wcscmp(action, L"deleteTemplate") == 0) {
        wchar_t tpl_name[256] = { 0 };
        json_get_string(json, L"name", tpl_name, 256);
        if (tpl_name[0] != L'\0') {
            wchar_t base_dir[MAX_PATH], tpl_dir[MAX_PATH];
            wchar_t json_path[MAX_PATH], vhdx_path[MAX_PATH];
            wchar_t vmgs[MAX_PATH], vmrs[MAX_PATH], res[MAX_PATH], snap_dir[MAX_PATH];

            if (!GetEnvironmentVariableW(L"ProgramData", base_dir, MAX_PATH))
                wcscpy_s(base_dir, MAX_PATH, L"C:\\ProgramData");
            swprintf_s(tpl_dir, MAX_PATH, L"%s\\AppSandbox\\templates\\%s", base_dir, tpl_name);

            /* Delete known files */
            swprintf_s(json_path, MAX_PATH, L"%s\\%s.json", tpl_dir, tpl_name);
            swprintf_s(vhdx_path, MAX_PATH, L"%s\\disk.vhdx", tpl_dir);
            swprintf_s(vmgs, MAX_PATH, L"%s\\vm.vmgs", tpl_dir);
            swprintf_s(vmrs, MAX_PATH, L"%s\\vm.vmrs", tpl_dir);
            swprintf_s(res, MAX_PATH, L"%s\\resources.iso", tpl_dir);
            DeleteFileW(json_path);
            DeleteFileW(vhdx_path);
            DeleteFileW(vmgs); DeleteFileW(vmrs); DeleteFileW(res);

            /* Remove empty snapshots dir if present, then template dir */
            swprintf_s(snap_dir, MAX_PATH, L"%s\\snapshots", tpl_dir);
            RemoveDirectoryW(snap_dir);
            RemoveDirectoryW(tpl_dir);

            ui_log(L"Template \"%s\" deleted.", tpl_name);
            scan_templates();
            send_templates();
        }
    } else if (wcscmp(action, L"editVm") == 0) {
        on_edit_vm(json);
    } else if (wcscmp(action, L"selectVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx)) {
            g_selected_vm = idx;
            send_snap_list();
        }
    } else if (wcscmp(action, L"browseImage") == 0) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = { 0 };
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd_main;
        ofn.lpstrFilter = L"ISO Files (*.iso)\0*.iso\0VHDX Files (*.vhdx)\0*.vhdx\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            wchar_t json_buf[2048];
            JsonBuilder jb;
            wcscpy_s(g_last_iso_path, MAX_PATH, file);
            jb_init(&jb, json_buf, 2048);
            jb_object_begin(&jb);
            jb_string(&jb, L"type", L"browseResult");
            jb_string(&jb, L"path", file);
            jb_object_end(&jb);
            webview2_post(json_buf);
        }
    } else if (wcscmp(action, L"snapTake") == 0) {
        on_snap_take();
    } else if (wcscmp(action, L"snapRevert") == 0) {
        int idx; if (json_get_int(json, L"snapIndex", &idx)) on_snap_revert(idx);
    } else if (wcscmp(action, L"snapDelete") == 0) {
        int idx; if (json_get_int(json, L"snapIndex", &idx)) on_snap_delete(idx);
    } else if (wcscmp(action, L"getState") == 0) {
        send_full_state();
    } else if (wcscmp(action, L"setMinSize") == 0) {
        int cw = 0, ch = 0;
        json_get_int(json, L"width", &cw);
        json_get_int(json, L"height", &ch);
        if (cw > 0 && ch > 0) {
            RECT rc = { 0, 0, cw, ch };
            RECT wr;
            AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
            g_min_width = rc.right - rc.left;
            g_min_height = rc.bottom - rc.top;

            /* Resize window if currently smaller than minimum */
            GetWindowRect(g_hwnd_main, &wr);
            {
                int cur_w = wr.right - wr.left;
                int cur_h = wr.bottom - wr.top;
                if (cur_w < g_min_width || cur_h < g_min_height) {
                    SetWindowPos(g_hwnd_main, NULL, 0, 0,
                        cur_w < g_min_width ? g_min_width : cur_w,
                        cur_h < g_min_height ? g_min_height : cur_h,
                        SWP_NOMOVE | SWP_NOZORDER);
                }
            }
        }
    }
}

/* ---- HCS callback ---- */

static void vm_state_changed(VmInstance *instance, DWORD event)
{
    PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, (WPARAM)event, (LPARAM)instance);
}

/* ---- Window creation ---- */

HWND ui_create_main_window(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    g_ui_thread_id = GetCurrentThreadId();
    BOOL dark = TRUE;

    g_hInstance = hInstance;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = main_wnd_proc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = L"AppSandbox_Main";
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPSANDBOX));

    if (!RegisterClassExW(&wc))
        return NULL;

    hwnd = CreateWindowExW(
        0, L"AppSandbox_Main", L"App Sandbox - VM Manager",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1050, 900,
        NULL, NULL, hInstance, NULL);

    if (!hwnd)
        return NULL;

    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    g_hwnd_main = hwnd;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    return hwnd;
}

/* ---- Window procedure ---- */

static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {

    case WM_CREATE:
        /* Start WebView2 first — backend init deferred to uiReady */
        hcs_set_state_callback(vm_state_changed);
        hcs_set_monitor_hwnd(hwnd);
        vm_agent_set_hwnd(hwnd);
        webview2_set_message_callback(on_webview2_message);
        if (!webview2_init(hwnd, g_hInstance)) {
            MessageBoxW(hwnd, L"WebView2 initialization failed.\nPlease install Microsoft Edge WebView2 Runtime.",
                        L"App Sandbox", MB_ICONERROR);
        }
        tray_add(hwnd);
        return 0;

    case WM_SIZE:
        webview2_resize(hwnd);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        if (g_min_width > 0)  mmi->ptMinTrackSize.x = g_min_width;
        if (g_min_height > 0) mmi->ptMinTrackSize.y = g_min_height;
        return 0;
    }

    case WM_VM_STATE_CHANGED:
    {
        DWORD hcs_event = (DWORD)wp;
        VmInstance *inst = (VmInstance *)lp;

        if (inst) {
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (&g_vms[i] != inst) continue;

                if (hcs_event == 0 && inst->running && !g_displays[i]) {
                    /* Only auto-open RDP display if VideoMonitor pipe exists */
                    wchar_t pipe_name[512];
                    swprintf_s(pipe_name, 512, L"\\\\.\\pipe\\%s.BasicSession", inst->name);
                    if (WaitNamedPipeW(pipe_name, 0)) {
                        g_displays[i] = vm_display_create(inst, g_hInstance, g_hwnd_main);
                        if (!g_displays[i]) ui_log(L"Warning: Failed to auto-create display window.");
                    }
                }

                if (hcs_event == 0x00000001) {
                    if (!inst->running) break;
                    inst->running = FALSE;
                    inst->shutdown_requested = FALSE;
                    inst->hyperv_video_off = FALSE;
                    hcs_stop_monitor(inst);
                    vm_agent_stop(inst);
                    ui_log(L"VM \"%s\" exited (event=0x%08X).", inst->name, hcs_event);
                    safe_destroy_rdp(i);
                    safe_destroy_idd(i);
                    if (inst->network_mode != NET_NONE && !inst->network_cleaned) {
                        hcn_delete_endpoint(&inst->endpoint_id);
                        hcn_delete_network(&inst->network_id);
                        inst->network_cleaned = TRUE;
                    }
                    hcs_close_vm(inst);

                    /* Template finalization */
                    if (inst->is_template) {
                        wchar_t tpl_dir[MAX_PATH], tmp[MAX_PATH], jp[MAX_PATH];
                        wchar_t *sl;
                        int j;
                        wcscpy_s(tpl_dir, MAX_PATH, inst->vhdx_path);
                        sl = wcsrchr(tpl_dir, L'\\'); if (sl) *sl = L'\0';
                        swprintf_s(tmp, MAX_PATH, L"%s\\vm.vmgs", tpl_dir); DeleteFileW(tmp);
                        swprintf_s(tmp, MAX_PATH, L"%s\\vm.vmrs", tpl_dir); DeleteFileW(tmp);
                        swprintf_s(tmp, MAX_PATH, L"%s\\resources.iso", tpl_dir); DeleteFileW(tmp);
                        swprintf_s(jp, MAX_PATH, L"%s\\%s.json", tpl_dir, inst->name);
                        { FILE *jf; if (_wfopen_s(&jf, jp, L"w,ccs=UTF-8") == 0 && jf) {
                            fwprintf(jf, L"{\n  \"os_type\": \"%s\",\n  \"image_path\": \"%s\"\n}\n", inst->os_type, inst->image_path);
                            fclose(jf);
                        }}
                        ui_log(L"Template \"%s\" created successfully.", inst->name);
                        for (j = i; j < g_vm_count - 1; j++) { g_vms[j] = g_vms[j+1]; g_snap_lists[j] = g_snap_lists[j+1]; g_displays[j] = g_displays[j+1]; g_idd_displays[j] = g_idd_displays[j+1]; }
                        ZeroMemory(&g_vms[g_vm_count-1], sizeof(VmInstance));
                        g_displays[g_vm_count-1] = NULL;
                        g_idd_displays[g_vm_count-1] = NULL;
                        g_vm_count--;
                        scan_templates();
                        send_templates();
                    }
                } else if (hcs_event == 0x01000000 || hcs_event == 0x02000000) {
                    inst->callbacks_dead = TRUE;
                    ui_log(L"VM \"%s\": HCS ServiceDisconnect.", inst->name);
                }
                break;
            }
        }
        send_vm_list();
        save_vm_list();
        return 0;
    }

    case WM_VM_AGENT_STATUS:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst && inst->agent_online) {
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (&g_vms[i] != inst) continue;
                if (g_displays[i] && vm_display_is_open(g_displays[i])) {
                    ui_log(L"Agent online - switching \"%s\" from RDP to IDD.", inst->name);
                    safe_destroy_rdp(i);
                    do_connect_idd(i);
                }
                break;
            }
        }
        send_vm_list();
        return 0;
    }

    case WM_VM_AGENT_SHUTDOWN:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            inst->shutdown_requested = TRUE;
            inst->shutdown_time = GetTickCount64();
        }
        send_vm_list();
        return 0;
    }

    case WM_VM_HYPERV_VIDEO_OFF:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (&g_vms[i] != inst) continue;
                if (!inst->running) break;
                inst->hyperv_video_off = TRUE;
                send_vm_list();
                if (g_displays[i] && vm_display_is_open(g_displays[i])) {
                    ui_log(L"Hyper-V Video disabled - switching \"%s\" from RDP to IDD.", inst->name);
                    safe_destroy_rdp(i);
                    do_connect_idd(i);
                }
                break;
            }
        }
        return 0;
    }

    case WM_VM_DISPLAY_CLOSED:
    {
        /* wp=0 means RDP display closed, wp=1 means IDD display closed */
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (&g_vms[i] != inst) continue;
                if (wp == 0) safe_destroy_rdp(i);
                if (wp == 1) safe_destroy_idd(i);
                break;
            }
        }
        return 0;
    }

    case WM_VM_MONITOR_DETECTED:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (&g_vms[i] != inst) continue;
                if (!inst->running) break;
                ui_log(L"Monitor detected VM \"%s\" stopped.", inst->name);
                inst->running = FALSE; inst->shutdown_requested = FALSE; inst->hyperv_video_off = FALSE;
                hcs_stop_monitor(inst); vm_agent_stop(inst);
                safe_destroy_rdp(i);
                safe_destroy_idd(i);
                if (inst->network_mode != NET_NONE && !inst->network_cleaned) {
                    hcn_delete_endpoint(&inst->endpoint_id); hcn_delete_network(&inst->network_id); inst->network_cleaned = TRUE;
                }
                hcs_close_vm(inst);
                break;
            }
        }
        send_vm_list(); save_vm_list();
        return 0;
    }

    case WM_VM_SHUTDOWN_TIMEOUT:
    {
        VmInstance *inst = (VmInstance *)lp;
        ULONGLONG elapsed = (ULONGLONG)wp;
        if (inst && inst->running && elapsed % 30 < 3)
            ui_log(L"WARNING: VM \"%s\" still shutting down (%llu seconds).", inst->name, elapsed);
        return 0;
    }

    case WM_HCS_OP_DONE:
    {
        HcsAsyncOp *op = (HcsAsyncOp *)lp;
        if (!op) return 0;
        switch (op->op_type) {
        case HCS_OP_STOP:
            if (FAILED(op->result)) ui_log(L"Error: Terminate failed (0x%08X)", op->result);
            hcs_close_vm(op->vm);
            if (op->vm->network_mode != NET_NONE && !op->vm->network_cleaned) {
                hcn_delete_endpoint(&op->vm->endpoint_id); hcn_delete_network(&op->vm->network_id); op->vm->network_cleaned = TRUE;
            }
            ui_log(L"VM \"%s\" terminated.", op->vm->name);
            send_vm_list(); save_vm_list();
            break;
        case HCS_OP_SNAP_TAKE:
            if (FAILED(op->result)) ui_log(L"Error: Snapshot failed (0x%08X)", op->result);
            else ui_log(L"Snapshot \"%s\" created.", op->snap_name);
            send_snap_list();
            break;
        case HCS_OP_SNAP_REVERT:
            if (FAILED(op->result)) ui_log(L"Error: Revert failed (0x%08X)", op->result);
            else ui_log(L"Reverted. VM is stopped - click Start to boot from snapshot.");
            send_vm_list(); send_snap_list();
            break;
        }
        free(op);
        return 0;
    }

    case WM_VM_AGENT_GPUCOPY:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            int i;
            for (i = 0; i < g_vm_count; i++) {
                if (&g_vms[i] != inst) continue;
                if (g_displays[i]) {
                    safe_destroy_rdp(i);
                    ui_log(L"RDP display torn down for \"%s\" (GPU driver activation).", inst->name);
                }
                break;
            }
        }
        return 0;
    }

    case WM_VHDX_PROGRESS:
    {
        int vm_idx = (int)wp;
        int pct = LOWORD(lp);
        int staging = HIWORD(lp);
        if (vm_idx >= 0 && vm_idx < g_vm_count) {
            g_vms[vm_idx].vhdx_progress = pct;
            g_vms[vm_idx].vhdx_staging = staging;
            send_vm_list();
        }
        return 0;
    }

    case WM_VHDX_DONE:
    {
        VhdxCreateArgs *args = (VhdxCreateArgs *)lp;
        int idx = args->vm_index;

        if (idx >= 0 && idx < g_vm_count) {
            VmInstance *inst = &g_vms[idx];
            inst->building_vhdx = FALSE;

            if (SUCCEEDED(args->result)) {
                /* Copy runtime state from the heap-allocated VmInstance */
                VmInstance *heap_inst = args->vm_inst;
                if (heap_inst) {
                    /* Unregister callback from temp_inst — its context pointer is stale */
                    hcs_unregister_vm_callback(heap_inst);

                    inst->handle = heap_inst->handle;
                    inst->runtime_id = heap_inst->runtime_id;
                    inst->running = TRUE;
                    inst->network_mode = heap_inst->network_mode;
                    inst->network_id = args->network_id;
                    inst->endpoint_id = args->endpoint_id;
                    HeapFree(GetProcessHeap(), 0, heap_inst);

                    /* Re-register callback with the real g_vms[] pointer as context */
                    hcs_register_vm_callback(inst);
                }

                save_vm_list();
                send_vm_list();
                hcs_start_monitor(inst);

                if (inst->is_template) {
                    ui_log(L"Template \"%s\" building (sysprep will shut down when ready).", inst->name);
                } else {
                    vm_agent_start(inst);
                    ui_log(L"VM \"%s\" created and started.", inst->name);

                    /* Auto-connect display */
                    g_displays[idx] = vm_display_create(inst, g_hInstance, g_hwnd_main);
                    if (!g_displays[idx]) ui_log(L"Warning: Failed to auto-create display window.");
                }
            } else {
                ui_log(L"Error creating VM \"%s\": %s", inst->name, args->error_msg);
                /* Clean up: remove VM from list */
                {
                    int j;
                    for (j = idx; j < g_vm_count - 1; j++) {
                        g_vms[j] = g_vms[j + 1];
                        g_snap_lists[j] = g_snap_lists[j + 1];
                        g_displays[j] = g_displays[j + 1];
                        g_idd_displays[j] = g_idd_displays[j + 1];
                    }
                    ZeroMemory(&g_vms[g_vm_count - 1], sizeof(VmInstance));
                    g_displays[g_vm_count - 1] = NULL;
                    g_idd_displays[g_vm_count - 1] = NULL;
                    g_vm_count--;
                }
                save_vm_list();
                send_vm_list();
            }
        }

        HeapFree(GetProcessHeap(), 0, args);
        return 0;
    }

    case WM_WEBVIEW2_LOG:
    {
        wchar_t *log_text = (wchar_t *)lp;
        if (log_text) {
            ui_log_post(log_text);
            free(log_text);
        }
        return 0;
    }

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) {
            tray_show_menu(hwnd);
        }
        return 0;

    case WM_CLOSE:
    {
        int i;
        BOOL has_running = FALSE;
        for (i = 0; i < g_vm_count; i++) {
            if (g_vms[i].running) { has_running = TRUE; break; }
        }
        if (has_running) {
            if (!g_suppress_tray_warn) {
                int btn = 0;
                BOOL checked = FALSE;
                TASKDIALOGCONFIG tdc;
                TASKDIALOG_BUTTON buttons[2];

                buttons[0].nButtonID = IDOK;
                buttons[0].pszButtonText = L"Minimize to Tray";
                buttons[1].nButtonID = IDCANCEL;
                buttons[1].pszButtonText = L"Cancel";

                ZeroMemory(&tdc, sizeof(tdc));
                tdc.cbSize = sizeof(tdc);
                tdc.hwndParent = hwnd;
                tdc.pszWindowTitle = L"App Sandbox";
                tdc.pszMainIcon = TD_INFORMATION_ICON;
                tdc.pszMainInstruction = L"VMs are still running";
                tdc.pszContent = L"App Sandbox will minimize to the system tray.\n"
                                 L"Your VMs will continue running in the background.\n\n"
                                 L"Click the tray icon to manage VMs or exit.";
                tdc.pszVerificationText = L"Don't show this again";
                tdc.cButtons = 2;
                tdc.pButtons = buttons;
                tdc.nDefaultButton = IDOK;

                if (FAILED(TaskDialogIndirect(&tdc, &btn, NULL, &checked)) || btn != IDOK)
                    return 0;

                if (checked) {
                    g_suppress_tray_warn = TRUE;
                    save_vm_list();
                }
            }
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
    {
        int i;
        tray_remove();
        webview2_cleanup();
        for (i = 0; i < g_vm_count; i++) {
            hcs_stop_monitor(&g_vms[i]);
            vm_agent_stop(&g_vms[i]);
            safe_destroy_rdp(i);
            safe_destroy_idd(i);
            if (g_vms[i].running) hcs_terminate_vm(&g_vms[i]);
            hcs_close_vm(&g_vms[i]);
        }
        hcs_cleanup();
        hcn_cleanup();
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
