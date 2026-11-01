/*
 * ui.c - Main window + WebView2 UI bridge.
 *
 * Thin UI shell: WebView2 hosting, JSON message dispatch, display windows,
 * tray icon. All VM orchestration is in asb_core.c (the core library).
 */

/* winsock2 before ui.h/windows.h: hcn_network.h needs the Winsock 2 +
   ws2tcpip include chain (netioapi types); windows.h would otherwise
   pull in Winsock 1.1 first and collide with it. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ui.h"
#include "asb_core.h"
#include "resource.h"
#include "hcs_vm.h"
#include "hcn_network.h"
#include "hcn_private.h"
#include "snapshot.h"
#include "vm_display.h"
#include "vm_display_idd.h"
#include "vm_agent.h"
#include "webview2_bridge.h"
#include "prereq.h"
#include <dwmapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <shlobj.h>
#include <shellapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* ---- UI-specific globals ---- */

static HWND g_hwnd_main = NULL;
static HINSTANCE g_hInstance = NULL;
static int g_selected_vm = -1;

/* Minimum window size */
static int g_min_width = 0;
static int g_min_height = 0;

/* Display windows (indexed parallel to the library's VM array) */
static VmDisplay *g_displays[ASB_MAX_VMS];
static VmDisplayIdd *g_idd_displays[ASB_MAX_VMS];

/* Custom window messages */
#define WM_VM_STATE_CHANGED    (WM_APP + 1)
#define WM_VM_AGENT_STATUS     (WM_APP + 2)
#define WM_VM_AGENT_SHUTDOWN   (WM_APP + 3)
#define WM_VM_AGENT_GPUCOPY    (WM_APP + 4)
#define WM_VM_DISPLAY_CLOSED   (WM_APP + 5)
#define WM_VM_MONITOR_DETECTED (WM_APP + 6)
#define WM_VM_IDD_READY        (WM_APP + 7)
#define WM_VM_HYPERV_VIDEO_OFF (WM_APP + 12)
#define WM_VM_REMOVED          (WM_APP + 16)
#define WM_WEBVIEW2_LOG        (WM_APP + 10)
#define WM_TRAYICON            (WM_APP + 11)
#define WM_SHOW_ALERT          (WM_APP + 15)
#define WM_VM_SHUTDOWN_TIMEOUT (WM_APP + 9)
#define WM_PREREQ_DONE        (WM_APP + 17)
#define WM_DISK_SPACE         (WM_APP + 19)

/* Tray */
#define TRAY_CMD_SHOW          1
#define TRAY_CMD_EXIT          2
#define TRAY_CMD_CONNECT_BASE  100
#define TRAY_CMD_SHUTDOWN_BASE 200
#define TRAY_CMD_STOP_BASE     300

static NOTIFYICONDATAW g_nid;

/* UI thread ID for thread-safe log dispatch */
static DWORD g_ui_thread_id;

/* TRUE on Windows Home / Home N editions (Hyper-V not supported) */
static BOOL g_is_home_edition = FALSE;
static BOOL g_prereq_ok = FALSE;
static BOOL g_prereq_reboot_pending = FALSE;

static BOOL detect_home_edition(void)
{
    DWORD product_type = 0;
    if (GetProductInfo(10, 0, 0, 0, &product_type)) {
        switch (product_type) {
        case 0x00000065: /* PRODUCT_CORE */
        case 0x00000062: /* PRODUCT_CORE_N */
        case 0x00000064: /* PRODUCT_CORE_SINGLELANGUAGE */
        case 0x00000063: /* PRODUCT_CORE_COUNTRYSPECIFIC */
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- Forward declarations ---- */

static LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void send_vm_list(void);
static void send_full_state(void);
static void census_on_consult(void);
static void send_templates(void);

/* ---- Safe display teardown ---- */

static void safe_destroy_rdp(int idx)
{
    if (idx >= 0 && idx < ASB_MAX_VMS && g_displays[idx]) {
        VmDisplay *d = g_displays[idx];
        g_displays[idx] = NULL;
        vm_display_disconnect(d);
        vm_display_destroy(d);
    }
}

static void safe_destroy_idd(int idx)
{
    if (idx >= 0 && idx < ASB_MAX_VMS && g_idd_displays[idx]) {
        VmDisplayIdd *d = g_idd_displays[idx];
        g_idd_displays[idx] = NULL;
        vm_display_idd_destroy(d);
    }
}

/* ---- JSON state builders ---- */

static int query_disk_free_gb(const wchar_t *selected)
{
    wchar_t supplied[MAX_PATH], path[MAX_PATH + 1];
    ULARGE_INTEGER available;
    ULONGLONG gb;
    DWORD length, attrs;
    size_t i, len;

    if (!selected || !selected[0]) {
        asb_default_disk_directory(supplied, MAX_PATH);
    } else {
        if (wcslen(selected) >= MAX_PATH) return -1;
        wcscpy_s(supplied, MAX_PATH, selected);
    }
    len = wcslen(supplied);
    for (i = 0; i < len; i++) {
        if (supplied[i] == L'/') supplied[i] = L'\\';
        if (supplied[i] < 32 || wcschr(L"*?\"<>|", supplied[i]) ||
            (supplied[i] == L':' && i != 1))
            return -1;
    }
    if (!((len >= 3 && ((supplied[0] >= L'A' && supplied[0] <= L'Z') ||
                        (supplied[0] >= L'a' && supplied[0] <= L'z')) &&
                        supplied[1] == L':' && supplied[2] == L'\\') ||
          (len >= 5 && supplied[0] == L'\\' && supplied[1] == L'\\')))
        return -1;
    length = GetFullPathNameW(supplied, MAX_PATH, path, NULL);
    if (!length || length >= MAX_PATH) return -1;
    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    /* UNC volume queries require a trailing separator. */
    if (path[length - 1] != L'\\') {
        path[length++] = L'\\';
        path[length] = 0;
    }
    if (!GetDiskFreeSpaceExW(path, &available, NULL, NULL)) return -1;
    gb = available.QuadPart / (1024ULL * 1024 * 1024);
    return gb > INT_MAX ? INT_MAX : (int)gb;
}

static void send_disk_space(const wchar_t *json, BOOL query)
{
    size_t input_len = wcslen(json);
    wchar_t *path, *response;
    JsonBuilder jb;
    int request_id = 0, free_gb = -1;

    /* Preserve the requested path in the reply even when it exceeds MAX_PATH
       and is therefore rejected by the filesystem query. */
    if (input_len > (((size_t)-1) / sizeof(wchar_t) - 128) / 2) return;
    path = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                               (input_len + 1) * sizeof(wchar_t));
    response = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                   (input_len * 2 + 128) * sizeof(wchar_t));
    if (!path || !response) {
        if (path) HeapFree(GetProcessHeap(), 0, path);
        if (response) HeapFree(GetProcessHeap(), 0, response);
        return;
    }
    if (json_get_string(json, L"path", path, input_len + 1) && query)
        free_gb = query_disk_free_gb(path);
    json_get_int(json, L"requestId", &request_id);
    jb_init(&jb, response, input_len * 2 + 128);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"diskSpace");
    jb_string(&jb, L"path", path);
    jb_int(&jb, L"requestId", request_id);
    jb_int(&jb, L"freeGb", free_gb);
    jb_object_end(&jb);
    /* WebView2 is apartment-bound; deliver the response on the UI thread. */
    if (!PostMessageW(g_hwnd_main, WM_DISK_SPACE, 0, (LPARAM)response))
        HeapFree(GetProcessHeap(), 0, response);
    HeapFree(GetProcessHeap(), 0, path);
}

static DWORD WINAPI disk_space_thread(LPVOID param)
{
    wchar_t *json = (wchar_t *)param;
    send_disk_space(json, TRUE);
    free(json);
    return 0;
}

static void build_host_info_json(JsonBuilder *jb)
{
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    GpuList *gpu_list = asb_gpu_list();
    DWORD host_cores, host_ram_mb;
    DWORD vm_cores = 0, vm_ram_mb = 0, vm_hdd_gb = 0;
    wchar_t base_dir[MAX_PATH];
    int free_gb;
    int i, count = asb_vm_count();

    GetSystemInfo(&si);
    host_cores = si.dwNumberOfProcessors;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    host_ram_mb = (DWORD)(ms.ullTotalPhys / (1024 * 1024));

    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (v && v->running) { vm_cores += v->cpu_cores; vm_ram_mb += v->ram_mb; }
        if (v) vm_hdd_gb += v->hdd_gb;
    }

    asb_default_disk_directory(base_dir, MAX_PATH);
    free_gb = query_disk_free_gb(base_dir);

    jb_int(jb, L"hostCores", (int)host_cores);
    jb_int(jb, L"hostRamMb", (int)host_ram_mb);
    jb_int(jb, L"vmCores", (int)vm_cores);
    jb_int(jb, L"vmRamMb", (int)vm_ram_mb);
    jb_int(jb, L"freeGb", free_gb);
    jb_int(jb, L"vmHddGb", (int)vm_hdd_gb);
    jb_string(jb, L"defaultDiskDirectory", base_dir);
    jb_array_begin(jb, L"gpus");
    for (i = 0; gpu_list && i < gpu_list->count; i++) {
        if (i > 0) jb_append(jb, L",");
        jb_object_begin(jb);
        jb_string(jb, L"id", gpu_list->gpus[i].interface_path);
        jb_string(jb, L"name", gpu_list->gpus[i].name);
        jb_string(jb, L"location", gpu_list->gpus[i].location);
        jb_object_end(jb);
    }
    jb_array_end(jb);
}

static ULONGLONG get_file_size_bytes(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULARGE_INTEGER sz;
    if (!path || !path[0]) return 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return 0;
    sz.LowPart = fad.nFileSizeLow;
    sz.HighPart = fad.nFileSizeHigh;
    return sz.QuadPart;
}

static void jb_size_gb(JsonBuilder *jb, const wchar_t *key, ULONGLONG bytes)
{
    wchar_t buf[32];
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 10.0)
        swprintf_s(buf, 32, L"%.0f", gb);
    else
        swprintf_s(buf, 32, L"%.1f", gb);
    jb_string(jb, key, buf);
}

static void build_vm_json(JsonBuilder *jb, int i)
{
    wchar_t disk_directory[MAX_PATH];
    VmInstance *v = asb_vm_instance(asb_vm_get(i));
    SnapshotTree *st_ = asb_vm_snap_tree(asb_vm_get(i));
    if (!v || !st_) return;

    jb_object_begin(jb);
    jb_string(jb, L"name", v->name);
    jb_string(jb, L"osType", v->os_type);
    asb_vm_disk_directory(asb_vm_get(i), disk_directory, MAX_PATH);
    jb_string(jb, L"diskDirectory", disk_directory);
    jb_bool(jb, L"running", v->running);
    jb_bool(jb, L"shuttingDown", v->shutdown_requested);
    jb_bool(jb, L"agentOnline", v->agent_online);
    jb_int(jb, L"ramMb", (int)v->ram_mb);
    jb_int(jb, L"hddGb", (int)v->hdd_gb);
    jb_int(jb, L"cpuCores", (int)v->cpu_cores);
    jb_int(jb, L"gpuMode", v->gpu_mode);
    jb_string(jb, L"gpuId", v->gpu_id);
    jb_string(jb, L"gpuName", v->gpu_name);
    jb_int(jb, L"networkMode", v->network_mode);
    jb_string(jb, L"netAdapter", v->net_adapter);
    jb_string(jb, L"internalSwitch", v->internal_switch);
    jb_bool(jb, L"internalSwitchInvalid", v->internal_switch_invalid);
    jb_bool(jb, L"isTemplate", v->is_template);
    jb_bool(jb, L"hypervVideoOff", v->hyperv_video_off);
    jb_bool(jb, L"buildingVhdx", v->building_vhdx);
    jb_bool(jb, L"vhdxStaging", v->vhdx_staging);
    jb_int(jb, L"vhdxProgress", v->vhdx_progress);
    jb_bool(jb, L"installComplete", v->install_complete);
    jb_bool(jb, L"sshEnabled", v->ssh_enabled);
    jb_int(jb, L"sshPort", (int)v->ssh_port);
    jb_int(jb, L"sshState", (v->ssh_key_deployed && v->ssh_state == 2) ? 4 : v->ssh_state);
    jb_bool(jb, L"sshDeployKey", v->ssh_deploy_key);
    jb_bool(jb, L"sshKeyDeployed", v->ssh_key_deployed);

    /* Snapshot tree */
    {
        int s, b;
        int cur_snap, cur_branch;
        snapshot_find_current(st_, v->vhdx_path, &cur_snap, &cur_branch);

        jb_int(jb, L"snapCurrent", cur_snap);
        jb_int(jb, L"snapCurrentBranch", cur_branch);
        jb_bool(jb, L"hasSnapshots", st_->base_vhdx[0] != L'\0');

        {
            ULONGLONG base_size = get_file_size_bytes(st_->base_vhdx);

            /* Base branches */
            jb_array_begin(jb, L"baseBranches");
            for (b = 0; b < st_->base_branch_count; b++) {
                FILETIME bft;
                if (!st_->base_branches[b].valid) continue;
                if (b > 0) jb_append(jb, L",");
                jb_object_begin(jb);
                jb_string(jb, L"name", st_->base_branches[b].friendly_name);
                if (snapshot_get_branch_time(st_, -2, b, &bft)) {
                    FILETIME lft; SYSTEMTIME st;
                    wchar_t db[64];
                    FileTimeToLocalFileTime(&bft, &lft);
                    FileTimeToSystemTime(&lft, &st);
                    swprintf_s(db, 64, L"%04d-%02d-%02d %02d:%02d",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
                    jb_string(jb, L"date", db);
                }
                jb_size_gb(jb, L"sizeGb", base_size + get_file_size_bytes(st_->base_branches[b].vhdx_path));
                jb_object_end(jb);
            }
            jb_array_end(jb);

            /* Snapshots */
            jb_array_begin(jb, L"snapshots");
            for (s = 0; s < st_->count; s++) {
                FILETIME local_ft;
                SYSTEMTIME sys_t;
                wchar_t date_buf[64];
                if (!st_->nodes[s].valid) continue;
                FileTimeToLocalFileTime(&st_->nodes[s].created, &local_ft);
                FileTimeToSystemTime(&local_ft, &sys_t);
                swprintf_s(date_buf, 64, L"%04d-%02d-%02d %02d:%02d",
                    sys_t.wYear, sys_t.wMonth, sys_t.wDay, sys_t.wHour, sys_t.wMinute);
                if (s > 0) jb_append(jb, L",");
                jb_object_begin(jb);
                jb_string(jb, L"name", st_->nodes[s].name);
                jb_string(jb, L"date", date_buf);

                {
                    ULONGLONG snap_size = get_file_size_bytes(st_->nodes[s].snap_vhdx);
                    jb_array_begin(jb, L"branches");
                    for (b = 0; b < st_->nodes[s].branch_count; b++) {
                        FILETIME bft;
                        if (!st_->nodes[s].branches[b].valid) continue;
                        if (b > 0) jb_append(jb, L",");
                        jb_object_begin(jb);
                        jb_string(jb, L"name", st_->nodes[s].branches[b].friendly_name);
                        if (snapshot_get_branch_time(st_, s, b, &bft)) {
                            FILETIME lft; SYSTEMTIME bst;
                            wchar_t db[64];
                            FileTimeToLocalFileTime(&bft, &lft);
                            FileTimeToSystemTime(&lft, &bst);
                            swprintf_s(db, 64, L"%04d-%02d-%02d %02d:%02d",
                                bst.wYear, bst.wMonth, bst.wDay, bst.wHour, bst.wMinute);
                            jb_string(jb, L"date", db);
                        }
                        jb_size_gb(jb, L"sizeGb", base_size + snap_size + get_file_size_bytes(st_->nodes[s].branches[b].vhdx_path));
                        jb_object_end(jb);
                    }
                    jb_array_end(jb);
                }

                jb_object_end(jb);
            }
            jb_array_end(jb);
        }
    }

    jb_object_end(jb);
}

/* The shared capacity mechanism for BOTH build_vm_json consumers (the
   vmList and fullState envelopes differ; the capacity logic does not): a
   131072-wchar heap buffer, ONE retry at 2x on detected overflow, and the
   suppress case on overflow past the retry or an allocation failure at
   either size - a fixed-size stateUnavailable envelope (never a silent
   drop, never partial JSON; the allocation-failure case names itself).
   Anything that posted before still posts: the sizing is the baseline
   heap constant with headroom, never a delta-only calculation. */
static void send_state_unavailable(const wchar_t *source, const wchar_t *reason)
{
    wchar_t env[128];
    JsonBuilder jb;

    jb_init(&jb, env, ARRAYSIZE(env));
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"stateUnavailable");
    jb_string(&jb, L"source", source);
    jb_string(&jb, L"reason", reason);
    jb_object_end(&jb);
    if (jb.overflow)
        return;   /* the fixed envelope cannot overflow; defensive only */
    webview2_post(env);
}

/* Build the vmListChanged envelope's contents (the vms array + hostInfo)
   into a caller-provided buffer. */
static void build_vm_list_contents(JsonBuilder *jb)
{
    int i, count = asb_vm_count();

    jb_object_begin(jb);
    jb_string(jb, L"type", L"vmListChanged");

    jb_array_begin(jb, L"vms");
    for (i = 0; i < count; i++) {
        if (i > 0) jb_append(jb, L",");
        build_vm_json(jb, i);
    }
    jb_array_end(jb);

    {
        wchar_t hi_buf[32768];
        JsonBuilder hi;
        jb_init(&hi, hi_buf, ARRAYSIZE(hi_buf));
        jb_object_begin(&hi);
        build_host_info_json(&hi);
        jb_object_end(&hi);
        if (jb->count > 0) jb_append(jb, L",");
        jb_append(jb, L"\"hostInfo\":");
        jb_append(jb, hi_buf);
        jb->count++;
    }
    jb_object_end(jb);
}

static void send_vm_list(void)
{
    wchar_t *buf;
    JsonBuilder jb;
    size_t cap = 131072;
    int attempt;

    for (attempt = 0; attempt < 2; attempt++, cap *= 2) {
        buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   cap * sizeof(wchar_t));
        if (!buf) {
            send_state_unavailable(L"vmList", L"allocFailed");
            return;
        }
        jb_init(&jb, buf, (int)cap);
        build_vm_list_contents(&jb);
        if (!jb.overflow) {
            webview2_post(buf);
            HeapFree(GetProcessHeap(), 0, buf);
            return;
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    ui_log(L"UI: VM-list message exceeded the retry buffer; suppressed.");
    send_state_unavailable(L"vmList", L"tooLarge");
}

static void send_host_info(void)
{
    wchar_t buf[32768];
    JsonBuilder jb;
    jb_init(&jb, buf, ARRAYSIZE(buf));
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

/* Selectable adapter entries. The names array is heap-allocated: the
 * backend already omitted duplicate-name groups, so the capacity only
 * bounds distinct-named NICs and is logged if reached. */
#define UI_ADAPTER_CAP 64

typedef struct {
    wchar_t (*names)[256];
    int count;
    int capacity;
    BOOL overflowed;   /* at least one distinct name was actually dropped */
} AdapterList;

static void adapter_enum_cb(const wchar_t *name, int if_type, void *ctx)
{
    AdapterList *al = (AdapterList *)ctx;
    (void)if_type;   /* the default is (Auto) at index 0; type tracking is gone */
    if (al->count >= al->capacity) {
        al->overflowed = TRUE;   /* log only when an entry was really dropped */
        return;
    }
    wcscpy_s(al->names[al->count], 256, name);
    al->count++;
}

/* Collect the backend's safe adapter list (L0-based; duplicate-name
   groups already omitted there). The capacity is applied only after the
   backend's full duplicate-group detection, and only then logged.
   Returns FALSE on allocation failure (the caller emits "(Auto)" only). */
static BOOL collect_adapters(AdapterList *al)
{
    al->capacity = UI_ADAPTER_CAP;
    al->count = 0;
    al->overflowed = FALSE;
    al->names = (wchar_t (*)[256])HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            UI_ADAPTER_CAP * 256 * sizeof(wchar_t));
    if (!al->names)
        return FALSE;
    hcn_enum_adapters(adapter_enum_cb, al);
    if (al->overflowed)
        ui_log(L"External: adapter list capped at %d distinct names; the "
               L"rest are omitted from the dropdown (Explicit lookup still "
               L"resolves the full inventory).", al->capacity);
    return TRUE;
}

static void free_adapters(AdapterList *al)
{
    if (al->names) {
        HeapFree(GetProcessHeap(), 0, al->names);
        al->names = NULL;
    }
}

static void send_templates(void)
{
    wchar_t buf[8192];
    JsonBuilder jb;
    int i, count = asb_template_count();

    jb_init(&jb, buf, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"templates");
    jb_array_begin(&jb, L"templates");
    for (i = 0; i < count; i++) {
        if (i > 0) jb_append(&jb, L",");
        jb_object_begin(&jb);
        jb_string(&jb, L"name", asb_template_name(i));
        jb_string(&jb, L"osType", asb_template_os_type(i));
        jb_object_end(&jb);
    }
    jb_array_end(&jb);
    jb_object_end(&jb);
    webview2_post(buf);
}

static int CALLBACK disk_folder_browse_callback(HWND hwnd, UINT message, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (message == BFFM_INITIALIZED && data)
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void send_full_state(void)
{
    /* The fullState consumer of the shared capacity mechanism (with the
       vmList sender): a 131072-wchar heap, ONE retry at 2x on detected
       overflow, and the suppress envelope - never the whole-message drop
       the baseline guard performed. The startup state and getState both
       ride this. */
    wchar_t *buf;
    JsonBuilder jb;
    size_t cap = 131072;
    int attempt;

    for (attempt = 0; attempt < 2; attempt++, cap *= 2) {
        int i, count;

        buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   cap * sizeof(wchar_t));
        if (!buf) {
            send_state_unavailable(L"fullState", L"allocFailed");
            return;
        }
        jb_init(&jb, buf, (int)cap);
        jb_object_begin(&jb);
        jb_string(&jb, L"type", L"fullState");

        count = asb_vm_count();
        jb_array_begin(&jb, L"vms");
        for (i = 0; i < count; i++) {
            if (i > 0) jb_append(&jb, L",");
            build_vm_json(&jb, i);
        }
        jb_array_end(&jb);

        /* Host info */
        {
            wchar_t hi[32768];
            JsonBuilder hj;
            jb_init(&hj, hi, ARRAYSIZE(hi));
            jb_object_begin(&hj);
            build_host_info_json(&hj);
            jb_object_end(&hj);
            if (jb.count > 0) jb_append(&jb, L",");
            jb_append(&jb, L"\"hostInfo\":");
            jb_append(&jb, hi);
            jb.count++;
        }

        /* Adapters: the backend's safe list (L0, bound NICs visible,
           duplicate groups omitted). The default is index 0: (Auto) -
           selecting External does not pin an adapter. */
        {
            AdapterList al;

            if (collect_adapters(&al)) {
                jb_array_begin(&jb, L"adapters");
                for (i = 0; i < al.count; i++) {
                    if (i > 0) jb_append(&jb, L",");
                    jb_append(&jb, L"\"");
                    jb_append_escaped(&jb, al.names[i]);
                    jb_append(&jb, L"\"");
                }
                jb_array_end(&jb);
                jb_int(&jb, L"defaultAdapter", 0);
                free_adapters(&al);
            } else {
                ui_log(L"UI: adapter list allocation failed; the dropdown is "
                       L"(Auto) only.");
                jb_array_begin(&jb, L"adapters");
                jb_array_end(&jb);
                jb_int(&jb, L"defaultAdapter", 0);
            }
        }

        /* Templates */
        {
            int tc = asb_template_count();
            jb_array_begin(&jb, L"templates");
            for (i = 0; i < tc; i++) {
                if (i > 0) jb_append(&jb, L",");
                jb_object_begin(&jb);
                jb_string(&jb, L"name", asb_template_name(i));
                jb_string(&jb, L"osType", asb_template_os_type(i));
                jb_object_end(&jb);
            }
            jb_array_end(&jb);
        }

        jb_object_end(&jb);
        if (!jb.overflow) {
            webview2_post(buf);
            HeapFree(GetProcessHeap(), 0, buf);
            return;
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    ui_log(L"UI: full-state message exceeded the retry buffer; suppressed.");
    send_state_unavailable(L"fullState", L"tooLarge");
}

/* ---- UI logging ---- */

static void ui_log_post(const wchar_t *msg)
{
    wchar_t json[8192];
    JsonBuilder jb;
    jb_init(&jb, json, 8192);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"log");
    jb_string(&jb, L"message", msg);
    jb_object_end(&jb);
    if (jb.overflow)
        return;   /* partial JSON is never posted */
    webview2_post(json);
}

static void ui_show_alert(const wchar_t *message)
{
    if (GetCurrentThreadId() == g_ui_thread_id) {
        /* The message text is JSON-escaped through the builder: the
           reason strings contain quotes and raw %s formatting would
           emit invalid JSON. 4096-wide buffer for the
           one-line reason plus wrapping. */
        wchar_t json[4096];
        JsonBuilder jb;
        jb_init(&jb, json, 4096);
        jb_object_begin(&jb);
        jb_string(&jb, L"type", L"alert");
        jb_string(&jb, L"message", message ? message : L"");
        jb_object_end(&jb);
        if (jb.overflow)
            return;   /* partial JSON is never posted */
        webview2_post(json);
    } else if (g_hwnd_main) {
        size_t len = wcslen(message ? message : L"") + 1;
        wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
        if (copy) {
            wcscpy_s(copy, len, message ? message : L"");
            PostMessageW(g_hwnd_main, WM_SHOW_ALERT, 0, (LPARAM)copy);
        }
    }
}

/* ---- Display connections ---- */

/* Linux / legacy display path: connects to vmwp.exe's Basic Session named
   pipe (\\.\pipe\<vm-name>.BasicSession) and renders via MsRdpClient10
   ActiveX. Reads the synthetic Hyper-V Video adapter framebuffer — works
   for any guest from BIOS / GRUB onward, no guest cooperation required.
   Used as a side-debug window during install: while cloud-init is still
   bringing up the desktop and our IDD agent isn't up yet, this is the
   only way to see what the VM is actually doing. Coexists with IDD —
   they render from independent sources so both can stay open. */
static void do_connect_rdp(int idx)
{
    VmInstance *v;
    wchar_t pipe[300];
    int tries;
    if (idx < 0 || idx >= asb_vm_count()) return;
    v = asb_vm_instance(asb_vm_get(idx));
    if (!v || !v->running) { ui_log(L"VM \"%s\" is not running.", v ? v->name : L"?"); return; }
    if (g_displays[idx] && vm_display_is_open(g_displays[idx])) {
        ui_log(L"RDP display already open."); return;
    }
    safe_destroy_rdp(idx);

    /* vmwp.exe creates the pipe a couple of seconds after the VM starts;
       poll briefly so the first Connect click doesn't race the worker. */
    swprintf_s(pipe, 300, L"\\\\.\\pipe\\%s.BasicSession", v->name);
    for (tries = 0; tries < 50; tries++) {
        if (WaitNamedPipeW(pipe, 0)) break;
        {
            DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_SEM_TIMEOUT) break;
        }
        Sleep(100);
    }

    ui_log(L"Opening RDP basic-session display for \"%s\"...", v->name);
    g_displays[idx] = vm_display_create(v, g_hInstance, g_hwnd_main);
    if (!g_displays[idx]) ui_log(L"Error: Failed to create RDP display window.");
}

static void do_connect_idd(int idx)
{
    VmInstance *v;
    if (idx < 0 || idx >= asb_vm_count()) return;
    v = asb_vm_instance(asb_vm_get(idx));
    if (!v || !v->running) { ui_log(L"VM \"%s\" is not running.", v ? v->name : L"?"); return; }
    if (g_idd_displays[idx] && vm_display_idd_is_open(g_idd_displays[idx])) {
        vm_display_idd_focus(g_idd_displays[idx]);
        return;
    }
    safe_destroy_idd(idx);
    ui_log(L"Opening IDD display for \"%s\"...", v->name);
    g_idd_displays[idx] = vm_display_idd_create(v, g_hInstance, g_hwnd_main);
    if (!g_idd_displays[idx]) ui_log(L"Error: Failed to create IDD display window.");
}

/* User-initiated Connect: open BOTH the IDD display and the RDP
   basic-session display side-by-side.
     - IDD renders the in-VM agent's captured framebuffer (Windows: VDD,
       Linux: appsandbox-display via vsock :2). Empty until the agent
       comes online.
     - RDP renders the Hyper-V Video adapter framebuffer via vmwp.exe's
       BasicSession named pipe. Works from BIOS / GRUB onward, no guest
       cooperation needed. Crucial for debugging install / first boot —
       you can watch cloud-init, kernel messages, GDM coming up.
   Both windows are independent; either can be closed manually. The
   automatic RDP -> IDD switching handlers (WM_VM_AGENT_ONLINE,
   WM_VM_HYPERV_VIDEO_OFF) keep using the per-display functions so they
   can close one without re-opening the other. */
static void do_connect_vm(int idx)
{
    /* Direct ISO->VHDX flow boots straight into the configured rootfs with
       IDD live from the start, so the RDP fallback window is no longer
       needed. do_connect_rdp() + vm_display.c stay on disk as reference
       for the RDP-over-named-pipe pattern but are no longer invoked.
       The g_displays[]/safe_destroy_rdp cleanup paths remain inert
       because nothing populates the slots. */
    do_connect_idd(idx);
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

static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

/* ---- Themed tray menu ----
 *
 * HMENU is retained for layout + input handling, but every item is MF_OWNERDRAW
 * so we can paint it in the same palette as the WebView2 front end
 * (see web/style.css). A WH_CALLWNDPROC hook paints the outer frame dark
 * via DWMWA_BORDER_COLOR (no-op on Win10 / pre-22H2). */

#define TRAY_BG_COLOR       RGB(0x1e, 0x1e, 0x1e)  /* --bg */
#define TRAY_SEL_COLOR      RGB(0x2a, 0x5a, 0x8a)  /* primary btn bg */
#define TRAY_TEXT_COLOR     RGB(0xe6, 0xe6, 0xe6)  /* --text */
#define TRAY_TEXT_DIM_COLOR RGB(0x88, 0x88, 0x88)  /* --text-dim */
#define TRAY_SEP_COLOR      RGB(0x3e, 0x3e, 0x3e)  /* --ctrl-border */

typedef struct {
    wchar_t label[256];
    BOOL    is_separator;
    BOOL    is_header;    /* dim label used as a section heading */
    BOOL    is_indented;  /* indent under a header */
} TrayDrawItem;

static TrayDrawItem g_tray_items[64];
static int          g_tray_item_count;
static HBRUSH       g_tray_bg_brush;

static HFONT tray_get_font(void)
{
    static HFONT font = NULL;
    if (!font) {
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            font = CreateFontIndirectW(&ncm.lfMenuFont);
        if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return font;
}

static void tray_append(HMENU menu, UINT id, const wchar_t *label,
                        BOOL is_separator, BOOL is_header, BOOL is_indented)
{
    TrayDrawItem *it;
    UINT flags;
    if (g_tray_item_count >= 64) return;

    it = &g_tray_items[g_tray_item_count];
    ZeroMemory(it, sizeof(*it));
    if (label) wcscpy_s(it->label, 256, label);
    it->is_separator = is_separator;
    it->is_header    = is_header;
    it->is_indented  = is_indented;

    flags = MF_OWNERDRAW | (is_separator ? MF_SEPARATOR : 0);
    AppendMenuW(menu, flags, id, (LPCWSTR)(UINT_PTR)g_tray_item_count);
    g_tray_item_count++;
}

static void tray_handle_measure(MEASUREITEMSTRUCT *mis)
{
    TrayDrawItem *it;
    HDC hdc;
    HFONT old;
    SIZE sz;

    if (!mis || mis->CtlType != ODT_MENU) return;
    if ((int)mis->itemData >= g_tray_item_count) return;

    it = &g_tray_items[mis->itemData];
    if (it->is_separator) {
        mis->itemHeight = 7;
        mis->itemWidth  = 0;
        return;
    }

    hdc = GetDC(NULL);
    old = (HFONT)SelectObject(hdc, tray_get_font());
    GetTextExtentPoint32W(hdc, it->label, (int)wcslen(it->label), &sz);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);

    mis->itemHeight = sz.cy + (it->is_header ? 6 : 10);
    mis->itemWidth  = sz.cx + (it->is_indented ? 40 : 28);
}

static void tray_handle_draw(DRAWITEMSTRUCT *dis)
{
    TrayDrawItem *it;
    RECT rc;
    BOOL selected, grayed;
    COLORREF bg, fg;
    HBRUSH bg_brush;
    HFONT old_font;
    int indent;

    if (!dis || dis->CtlType != ODT_MENU) return;
    if ((int)dis->itemData >= g_tray_item_count) return;

    it = &g_tray_items[dis->itemData];
    rc = dis->rcItem;

    if (it->is_separator) {
        HPEN pen, old_pen;
        int y;
        bg_brush = CreateSolidBrush(TRAY_BG_COLOR);
        FillRect(dis->hDC, &rc, bg_brush);
        DeleteObject(bg_brush);
        pen = CreatePen(PS_SOLID, 1, TRAY_SEP_COLOR);
        old_pen = (HPEN)SelectObject(dis->hDC, pen);
        y = (rc.top + rc.bottom) / 2;
        MoveToEx(dis->hDC, rc.left + 14, y, NULL);
        LineTo(dis->hDC, rc.right - 14, y);
        SelectObject(dis->hDC, old_pen);
        DeleteObject(pen);
        return;
    }

    selected = (dis->itemState & ODS_SELECTED) && !(dis->itemState & ODS_GRAYED);
    grayed   = (dis->itemState & ODS_GRAYED) != 0;

    bg = selected ? TRAY_SEL_COLOR : TRAY_BG_COLOR;
    fg = (grayed || it->is_header) ? TRAY_TEXT_DIM_COLOR : TRAY_TEXT_COLOR;

    bg_brush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &rc, bg_brush);
    DeleteObject(bg_brush);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    old_font = (HFONT)SelectObject(dis->hDC, tray_get_font());

    indent = it->is_indented ? 28 : 14;
    rc.left += indent;
    rc.right -= 14;
    DrawTextW(dis->hDC, it->label, -1, &rc,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    SelectObject(dis->hDC, old_font);
}

/* WH_CALLWNDPROC hook: catches messages sent to the popup menu window
   (class "#32768") as TrackPopupMenu brings it up, and paints its border
   the same color as the menu background so it visually disappears. */
static LRESULT CALLBACK tray_menu_hook_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        CWPSTRUCT *cwp = (CWPSTRUCT *)lp;
        wchar_t cls[16];
        if (GetClassNameW(cwp->hwnd, cls, 16) > 0 && wcscmp(cls, L"#32768") == 0) {
            COLORREF border = TRAY_BG_COLOR;
            DwmSetWindowAttribute(cwp->hwnd, DWMWA_BORDER_COLOR,
                                  &border, sizeof(border));
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int i, cmd, count = asb_vm_count();
    HHOOK hook;
    MENUINFO mi;

    if (!g_tray_bg_brush) g_tray_bg_brush = CreateSolidBrush(TRAY_BG_COLOR);
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize  = sizeof(mi);
    mi.fMask   = MIM_BACKGROUND;
    mi.hbrBack = g_tray_bg_brush;
    SetMenuInfo(menu, &mi);

    g_tray_item_count = 0;

    tray_append(menu, TRAY_CMD_SHOW, L"Show App Sandbox", FALSE, FALSE, FALSE);
    tray_append(menu, 0,             NULL,                TRUE,  FALSE, FALSE);

    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!v || !v->running) continue;
        tray_append(menu, 0, v->name, FALSE, TRUE, FALSE);
        tray_append(menu, TRAY_CMD_CONNECT_BASE + i,  L"\U0001F4FA  Connect",    FALSE, FALSE, TRUE);
        tray_append(menu, TRAY_CMD_SHUTDOWN_BASE + i, L"\u23FB  Shutdown",       FALSE, FALSE, TRUE);
        tray_append(menu, TRAY_CMD_STOP_BASE + i,     L"\u2715  Force Stop",     FALSE, FALSE, TRUE);
        tray_append(menu, 0, NULL, TRUE, FALSE, FALSE);
    }

    tray_append(menu, TRAY_CMD_EXIT, L"Exit", FALSE, FALSE, FALSE);

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);

    hook = SetWindowsHookExW(WH_CALLWNDPROC, tray_menu_hook_proc,
                             NULL, GetCurrentThreadId());
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    if (hook) UnhookWindowsHookEx(hook);
    DestroyMenu(menu);

    if (cmd == TRAY_CMD_SHOW) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
    } else if (cmd == TRAY_CMD_EXIT) {
        DestroyWindow(hwnd);
    } else if (cmd >= TRAY_CMD_STOP_BASE) {
        int vi = cmd - TRAY_CMD_STOP_BASE;
        safe_destroy_rdp(vi);
        safe_destroy_idd(vi);
        asb_vm_stop(asb_vm_get(vi));
        send_vm_list();
    } else if (cmd >= TRAY_CMD_SHUTDOWN_BASE) {
        asb_vm_shutdown(asb_vm_get(cmd - TRAY_CMD_SHUTDOWN_BASE));
        send_vm_list();
    } else if (cmd >= TRAY_CMD_CONNECT_BASE) {
        do_connect_vm(cmd - TRAY_CMD_CONNECT_BASE);
    }
}

/* ---- Prerequisite feature enable (background thread) ---- */

#define WM_PREREQ_PROGRESS (WM_APP + 18)

static void prereq_progress_cb(float pct, void *user_data)
{
    (void)user_data;
    /* Send integer percentage via PostMessage (truncate to int) */
    PostMessageW(g_hwnd_main, WM_PREREQ_PROGRESS, (WPARAM)(int)(pct + 0.5f), 0);
}

static DWORD WINAPI enable_feature_thread(LPVOID param)
{
    BOOL reboot_required = FALSE;
    BOOL ok;
    (void)param;

    ok = prereq_enable_feature(L"VirtualMachinePlatform", &reboot_required,
                                prereq_progress_cb, NULL);

    /* Pack result: WPARAM = success, LPARAM = reboot_required */
    PostMessageW(g_hwnd_main, WM_PREREQ_DONE, (WPARAM)ok, (LPARAM)reboot_required);
    return 0;
}

/* ---- WebView2 message dispatch ---- */

static void on_webview2_message(const wchar_t *json)
{
    wchar_t action[64] = { 0 };

    if (!json_get_string(json, L"action", action, 64))
        return;

    if (wcscmp(action, L"uiReady") == 0) {
        /* JS has loaded and registered its message listener; deliver any
         * messages queued before WebView2 was ready (e.g. early ui_log lines). */
        webview2_flush_queue();
        if (!prereq_check_all()) {
            webview2_post(L"{\"type\":\"prereqRequired\"}");
            return;
        }
        g_prereq_ok = TRUE;
        asb_init();
        send_full_state();
    } else if (wcscmp(action, L"createVm") == 0) {
        if (!g_prereq_ok) {
            webview2_post(g_prereq_reboot_pending
                ? L"{\"type\":\"prereqReboot\"}"
                : L"{\"type\":\"prereqRequired\"}");
            return;
        }
        AsbVmConfig cfg;
        wchar_t name_buf[256] = {0}, os_buf[32] = {0}, img_buf[MAX_PATH] = {0};
        wchar_t tpl_buf[256] = {0}, user_buf[128] = {0}, pass_buf[256] = {0};
        wchar_t adapter_buf[256] = {0}, disk_buf[MAX_PATH + 1] = {0};
        wchar_t sw_buf[INTERNAL_SWITCH_CAP] = {0};
        wchar_t gpu_id[512] = {0};
        int val;
        BOOL is_tpl = FALSE;

        json_get_string(json, L"name", name_buf, 256);
        json_get_string(json, L"osType", os_buf, 32);
        json_get_string(json, L"imagePath", img_buf, MAX_PATH);
        json_get_string(json, L"templateName", tpl_buf, 256);
        if (!json_get_string(json, L"adminUser", user_buf, ARRAYSIZE(user_buf))) {
            const wchar_t *error = !json_has_key(json, L"adminUser")
                ? L"Username is required."
                : user_buf[0]
                    ? (_wcsicmp(os_buf, L"Linux") == 0 && !tpl_buf[0]
                        ? L"Username cannot exceed 32 characters (Linux limit)."
                        : L"Username cannot exceed 20 characters.")
                    : L"adminUser must be a valid JSON string without NUL characters.";
            ui_show_alert(error);
            return;
        }
        if (!json_get_string(json, L"adminPass", pass_buf, ARRAYSIZE(pass_buf))) {
            const wchar_t *error = !json_has_key(json, L"adminPass")
                ? L"Password is required."
                : pass_buf[0]
                    ? (_wcsicmp(os_buf, L"Linux") == 0 && !tpl_buf[0]
                        ? L"Password is too long (max 255 bytes)."
                        : L"Password is too long (max 127 characters for Windows).")
                    : L"adminPass must be a valid JSON string without NUL characters.";
            SecureZeroMemory(pass_buf, sizeof(pass_buf));
            ui_show_alert(error);
            return;
        }
        json_get_string(json, L"netAdapter", adapter_buf, 256);
        /* reject-on-false (the diskDirectory precedent, NOT netAdapter's
           ignore-false branch): a present key whose decode fails - embedded
           NUL, over-long, malformed escape - aborts the create. */
        if (!json_get_string(json, L"internalSwitch", sw_buf,
                             INTERNAL_SWITCH_CAP) &&
            json_has_key(json, L"internalSwitch")) {
            SecureZeroMemory(pass_buf, sizeof(pass_buf));
            ui_show_alert(L"The internal switch name is invalid or too long.");
            return;
        }
        if (!json_get_string(json, L"gpuId", gpu_id, ARRAYSIZE(gpu_id)) &&
            json_has_key(json, L"gpuId")) {
            SecureZeroMemory(pass_buf, sizeof(pass_buf));
            ui_show_alert(L"GPU selection is invalid or too long.");
            return;
        }
        if (!json_get_string(json, L"diskDirectory", disk_buf, MAX_PATH + 1) &&
            json_has_key(json, L"diskDirectory")) {
            ui_show_alert(L"Disk folder is invalid or too long.");
            return;
        }
        json_get_bool(json, L"isTemplate", &is_tpl);

        ZeroMemory(&cfg, sizeof(cfg));
        cfg.name = name_buf;
        cfg.os_type = os_buf;
        cfg.image_path = img_buf;
        cfg.template_name = tpl_buf;
        cfg.username = user_buf;
        cfg.password = pass_buf;
        cfg.net_adapter = adapter_buf;
        cfg.internal_switch = sw_buf;
        cfg.is_template = is_tpl;
        cfg.disk_directory = disk_buf;
        cfg.gpu_id = gpu_id;

        if (json_get_int(json, L"hddGb", &val)) cfg.hdd_gb = (DWORD)val;
        if (json_get_int(json, L"ramMb", &val)) cfg.ram_mb = (DWORD)val;
        if (json_get_int(json, L"cpuCores", &val)) cfg.cpu_cores = (DWORD)val;
        if (json_get_int(json, L"gpuMode", &val)) cfg.gpu_mode = val;
        if (json_get_int(json, L"networkMode", &val)) cfg.network_mode = val;
        json_get_bool(json, L"testMode", &cfg.test_mode);
        json_get_bool(json, L"sshEnabled", &cfg.ssh_enabled);
        json_get_bool(json, L"sshDeployKey", &cfg.ssh_deploy_key);

        {
            const wchar_t *error = asb_validate_gpu_selection(cfg.gpu_mode, cfg.gpu_id);
            if (error) {
                SecureZeroMemory(pass_buf, sizeof(pass_buf));
                ui_show_alert(error);
                return;
            }
        }
        {
            /* The create's own gates alert their own lines (the invalid
               selector, the acquire's fail-closed reasons); on FAILED the
               flow aborts here without re-alerting - the row was not
               added, so no list refresh is needed (harmless if it were). */
            HRESULT create_hr = asb_vm_create(&cfg);
            SecureZeroMemory(pass_buf, sizeof(pass_buf));
            if (FAILED(create_hr))
                return;
        }
        send_vm_list();
    } else if (wcscmp(action, L"startVm") == 0) {
        if (!g_prereq_ok) {
            webview2_post(g_prereq_reboot_pending
                ? L"{\"type\":\"prereqReboot\"}"
                : L"{\"type\":\"prereqRequired\"}");
            return;
        }
        int idx, si = -1, bi = -1;
        wchar_t bname[128] = {0};
        if (json_get_int(json, L"vmIndex", &idx)) {
            json_get_int(json, L"snapIndex", &si);
            json_get_int(json, L"branchIndex", &bi);
            json_get_string(json, L"branchName", bname, 128);
            asb_vm_start(asb_vm_get(idx), si, bi, bname);
            send_vm_list();
        }
    } else if (wcscmp(action, L"shutdownVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx)) {
            VmInstance *inst = asb_vm_instance(asb_vm_get(idx));
            if (inst) inst->shutdown_requested = TRUE;
            safe_destroy_rdp(idx);
            safe_destroy_idd(idx);
            asb_vm_shutdown(asb_vm_get(idx));
            send_vm_list();
        }
    } else if (wcscmp(action, L"stopVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx)) {
            VmInstance *inst = asb_vm_instance(asb_vm_get(idx));
            if (inst) inst->shutdown_requested = TRUE;
            safe_destroy_rdp(idx);
            safe_destroy_idd(idx);
            asb_vm_stop(asb_vm_get(idx));
            send_vm_list();
        }
    } else if (wcscmp(action, L"connectIddVm") == 0) {
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_connect_vm(idx);
    } else if (wcscmp(action, L"sshConnect") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx) && idx >= 0 && idx < asb_vm_count()) {
            VmInstance *inst = asb_vm_instance(asb_vm_get(idx));
            if (inst && inst->ssh_enabled && inst->ssh_port) {
                wchar_t cmd[1024], keyopt[700] = L"";
                STARTUPINFOW si_;
                PROCESS_INFORMATION pi_;
                ZeroMemory(&si_, sizeof(si_));
                si_.cb = sizeof(si_);
                ZeroMemory(&pi_, sizeof(pi_));
                /* If this VM had the AppSandbox key deployed, use it (-i) so the
                   terminal logs in with key auth instead of a password prompt.
                   IdentitiesOnly avoids offering the user's other keys, and -- since
                   these are ephemeral loopback VMs -- StrictHostKeyChecking=no +
                   a throwaway known_hosts skips the host-key fingerprint prompt and
                   keeps it out of the user's real known_hosts. Password logins (no
                   key) keep the normal fingerprint prompt. */
                if (inst->ssh_deploy_key) {
                    wchar_t base[MAX_PATH], tmp[MAX_PATH];
                    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
                        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
                    if (!GetTempPathW(MAX_PATH, tmp))
                        wcscpy_s(tmp, MAX_PATH, L"C:\\Windows\\Temp\\");
                    _snwprintf_s(keyopt, 700, _TRUNCATE,
                        L"-i \"%s\\AppSandbox\\ssh\\id_appsandbox\" -o IdentitiesOnly=yes "
                        L"-o StrictHostKeyChecking=no -o \"UserKnownHostsFile=%sappsandbox_known_hosts\" ",
                        base, tmp);
                }
                if (inst->admin_user[0])
                    _snwprintf_s(cmd, 1024, _TRUNCATE,
                        L"cmd.exe /v:off /k ssh %s-p %lu -l \"%s\" localhost",
                        keyopt, inst->ssh_port, inst->admin_user);
                else
                    _snwprintf_s(cmd, 1024, _TRUNCATE,
                        L"cmd.exe /k ssh %s-p %lu localhost",
                        keyopt, inst->ssh_port);
                ui_log(L"SSH: %s", cmd);
                if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                                    CREATE_NEW_CONSOLE, NULL, NULL, &si_, &pi_)) {
                    CloseHandle(pi_.hProcess);
                    CloseHandle(pi_.hThread);
                }
            }
        }
    } else if (wcscmp(action, L"deleteVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx) && idx >= 0 && idx < asb_vm_count()) {
            int j;
            HRESULT hr;
            ui_log(L"Deleting VM \"%s\"...", asb_vm_name(asb_vm_get(idx)));
            safe_destroy_rdp(idx);
            safe_destroy_idd(idx);
            hr = asb_vm_delete(asb_vm_get(idx));
            if (FAILED(hr)) {
                ui_log(L"VM deletion failed (0x%08X). Check that its disk storage is available and writable.", hr);
                ui_show_alert(L"VM deletion failed. Check that its disk storage is available and writable.");
                send_vm_list();
                return;
            }
            /* Compact display arrays */
            for (j = idx; j < asb_vm_count(); j++) {
                g_displays[j] = g_displays[j + 1];
                g_idd_displays[j] = g_idd_displays[j + 1];
            }
            g_displays[asb_vm_count()] = NULL;
            g_idd_displays[asb_vm_count()] = NULL;
            g_selected_vm = -1;
            send_vm_list();
            ui_log(L"VM deleted.");
        }
    } else if (wcscmp(action, L"deleteTemplate") == 0) {
        wchar_t tpl_name[256] = { 0 };
        json_get_string(json, L"name", tpl_name, 256);
        if (tpl_name[0] != L'\0') {
            HRESULT hr = asb_template_delete(tpl_name);
            if (FAILED(hr)) {
                ui_log(L"Template deletion failed (0x%08X). Check that its disk storage is available and writable.", hr);
                ui_show_alert(L"Template deletion failed. Check that its disk storage is available and writable.");
            }
            send_templates();
        }
    } else if (wcscmp(action, L"editVm") == 0) {
        int idx;
        wchar_t field[64], value[256];
        if (json_get_int(json, L"vmIndex", &idx) && idx >= 0 && idx < asb_vm_count() &&
            json_get_string(json, L"field", field, 64) &&
            json_get_string(json, L"value", value, 256)) {
            AsbVm vm = asb_vm_get(idx);
            if (asb_vm_is_running(vm) || asb_vm_is_building(vm)) {
                ui_show_alert(L"VM settings can only be changed when the VM is stopped and its disk build has finished.");
            } else {
                HRESULT hr = E_INVALIDARG;
                const wchar_t *error = NULL;
                if (wcscmp(field, L"name") == 0) hr = asb_vm_set_name(vm, value);
                else if (wcscmp(field, L"ramMb") == 0) hr = asb_vm_set_ram(vm, (DWORD)_wtoi(value));
                else if (wcscmp(field, L"cpuCores") == 0) hr = asb_vm_set_cpu(vm, (DWORD)_wtoi(value));
                else if (wcscmp(field, L"gpuMode") == 0) {
                    wchar_t gpu_id[512] = {0};
                    int mode = _wtoi(value);
                    if (!json_get_string(json, L"gpuId", gpu_id, ARRAYSIZE(gpu_id)) &&
                        json_has_key(json, L"gpuId"))
                        error = L"GPU selection is invalid or too long.";
                    else
                        error = asb_validate_gpu_selection(mode, gpu_id);
                    if (!error) hr = asb_vm_set_gpu_selection(vm, mode, gpu_id);
                }
                else if (wcscmp(field, L"networkMode") == 0) hr = asb_vm_set_network(vm, _wtoi(value));
                else if (wcscmp(field, L"netAdapter") == 0) hr = asb_vm_set_net_adapter(vm, value);
                else if (wcscmp(field, L"internalSwitch") == 0) hr = asb_vm_set_internal_switch(vm, value);
                if (FAILED(hr)) ui_show_alert(error ? error : L"VM configuration could not be updated.");
                else asb_save();
            }
        } else {
            ui_show_alert(L"VM configuration could not be updated.");
        }
        send_vm_list();
    } else if (wcscmp(action, L"selectVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx)) g_selected_vm = idx;
    } else if (wcscmp(action, L"openUrl") == 0) {
        /* A linkified URL in a modal (setModalMessage). A WebView2 window
           cannot open new windows itself, so the host opens the default
           browser. https only: never hand arbitrary schemes to the shell. */
        wchar_t url[512];
        if (json_get_string(json, L"url", url, 512) &&
            wcsncmp(url, L"https://", 8) == 0)
            ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    } else if (wcscmp(action, L"getDiskSpace") == 0) {
        wchar_t *copy = _wcsdup(json);
        HANDLE thread = copy ? CreateThread(NULL, 0, disk_space_thread, copy, 0, NULL) : NULL;
        if (thread) CloseHandle(thread);
        else {
            free(copy);
            send_disk_space(json, FALSE);
        }
    } else if (wcscmp(action, L"browseDiskDirectory") == 0) {
        BROWSEINFOW browse;
        PIDLIST_ABSOLUTE selection;
        wchar_t initial[MAX_PATH] = {0}, path[MAX_PATH] = {0};
        json_get_string(json, L"path", initial, MAX_PATH);
        if (!initial[0]) asb_default_disk_directory(initial, MAX_PATH);
        ZeroMemory(&browse, sizeof(browse));
        browse.hwndOwner = g_hwnd_main;
        browse.lpszTitle = L"Choose a folder for VM disks";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        browse.lpfn = disk_folder_browse_callback;
        browse.lParam = (LPARAM)initial;
        selection = SHBrowseForFolderW(&browse);
        if (selection) {
            if (SHGetPathFromIDListW(selection, path)) {
                wchar_t json_buf[2048];
                JsonBuilder jb;
                jb_init(&jb, json_buf, 2048);
                jb_object_begin(&jb);
                jb_string(&jb, L"type", L"diskDirectoryBrowseResult");
                jb_string(&jb, L"path", path);
                jb_object_end(&jb);
                webview2_post(json_buf);
            }
            CoTaskMemFree(selection);
        }
    } else if (wcscmp(action, L"browseImage") == 0) {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH] = { 0 };
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd_main;
        ofn.lpstrFilter = L"ISO Files (*.iso)\0*.iso\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            wchar_t json_buf[2048];
            JsonBuilder jb;
            asb_set_last_iso_path(file);
            jb_init(&jb, json_buf, 2048);
            jb_object_begin(&jb);
            jb_string(&jb, L"type", L"browseResult");
            jb_string(&jb, L"path", file);
            jb_object_end(&jb);
            webview2_post(json_buf);
        }
    } else if (wcscmp(action, L"snapTake") == 0) {
        int vi;
        wchar_t sname[128] = {0};
        if (json_get_int(json, L"vmIndex", &vi)) {
            json_get_string(json, L"name", sname, 128);
            asb_snap_take(asb_vm_get(vi), sname);
            send_vm_list();
        }
    } else if (wcscmp(action, L"snapDelete") == 0) {
        int vi, si;
        if (json_get_int(json, L"vmIndex", &vi) && json_get_int(json, L"snapIndex", &si)) {
            asb_snap_delete(asb_vm_get(vi), si);
            send_vm_list();
        }
    } else if (wcscmp(action, L"snapDeleteBranch") == 0) {
        int vi, si, bi;
        if (json_get_int(json, L"vmIndex", &vi) && json_get_int(json, L"snapIndex", &si) &&
            json_get_int(json, L"branchIndex", &bi)) {
            asb_snap_delete_branch(asb_vm_get(vi), si, bi);
            send_vm_list();
        }
    } else if (wcscmp(action, L"snapRename") == 0) {
        int vi, si, bi = -1;
        wchar_t new_name[128];
        if (json_get_int(json, L"vmIndex", &vi) && json_get_int(json, L"snapIndex", &si) &&
            json_get_string(json, L"name", new_name, 128)) {
            json_get_int(json, L"branchIndex", &bi);
            asb_snap_rename(asb_vm_get(vi), si, bi, new_name);
            send_vm_list();
        }
    } else if (wcscmp(action, L"enableFeature") == 0) {
        HANDLE h = CreateThread(NULL, 0, enable_feature_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    } else if (wcscmp(action, L"enableFeatureReboot") == 0) {
        prereq_reboot();
    } else if (wcscmp(action, L"getInternalSwitches") == 0) {
        /* The dropdown consult: cache-first (the web renders its own copy
           or its pending state); the host stages the priorities and kicks
           a rebuild only when the cache is expired or never built. */
        census_on_consult();
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

/* ---- Library callbacks (bridge library events → UI thread) ---- */

static void CALLBACK ui_log_callback(const wchar_t *message, void *user_data)
{
    (void)user_data;
    if (GetCurrentThreadId() == g_ui_thread_id) {
        ui_log_post(message);
    } else if (g_hwnd_main) {
        size_t len = wcslen(message) + 1;
        wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
        if (copy) {
            wcscpy_s(copy, len, message);
            PostMessageW(g_hwnd_main, WM_WEBVIEW2_LOG, 0, (LPARAM)copy);
        }
    }
}

static void CALLBACK ui_state_callback(AsbVm vm, BOOL running, void *user_data)
{
    (void)user_data;
    if (g_hwnd_main)
        PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, (WPARAM)running, (LPARAM)vm);
}

static void CALLBACK ui_progress_callback(AsbVm vm, int pct, BOOL staging, void *user_data)
{
    (void)pct; (void)staging; (void)user_data;
    /* Progress is already set in the VmInstance by the library - just refresh */
    if (g_hwnd_main)
        PostMessageW(g_hwnd_main, WM_VM_STATE_CHANGED, (WPARAM)0, (LPARAM)vm);
}

static void CALLBACK ui_alert_callback(const wchar_t *message, void *user_data)
{
    (void)user_data;
    ui_show_alert(message);
}

static void CALLBACK ui_vm_removed_callback(int index, void *user_data)
{
    (void)user_data;
    /* Post to UI thread to compact display arrays */
    if (g_hwnd_main)
        PostMessageW(g_hwnd_main, WM_VM_REMOVED, (WPARAM)index, 0);
}

/* ---- Internal vSwitch census manager ----
 *
 * One WMI/HCN census worker (display-only), a UI-thread watchdog, a TTL
 * cache, and the scheduling bits. The worker is a one-shot CreateThread
 * thread that releases its own session, then posts its completion
 * notification as its last act; the UI-side handler owns the
 * publish/discard/launch decision. The census CRITICAL_SECTION is never
 * deleted and the cancel event is never closed on ANY path: an abandoned
 * (wedged) worker's last act is to enter the lock and post, so nothing it
 * can still touch is ever destroyed - all of it is reclaimed by the OS at
 * process exit, exactly like the module itself.
 *
 * Scheduling bits and their windows (each row's setter/clearer/reader):
 *   worker_active     launch helper -> handler / expiry probe / request
 *                     probe / drain
 *   pending_request   ping/consult   launch helper             handler,
 *                     (genuine)                                expiry probe,
 *                     request probe
 *   io_complete       worker (inside its one lock take, ONLY on a
 *                     successful PostMessageW)  launch helper  expiry guard
 *                     / request probe gate
 *   priorities_dirty  a staging write that changes the list  launch helper
 *                     / kick arms
 *   published_once    commit / expiry finalize / request-entry terminal
 *                     publish - NEVER cleared   ping's gate
 *   invalidation_epoch  ping (only once published_once) / expiry
 *                     (monotonic)              handler / kick arms / launch
 *   build_epoch       launch helper             handler / kick arms
 *   build_thread      launch (same hold; NULL on CreateThread failure)
 *                     every slot release (same hold)  expiry / request
 *                     probes - a NON-OWNING alias into the ONE handle set,
 *                     never closed through
 *   deadline_fired    first finalize / request-entry terminal publish
 *                     launch helper (with the launch-cleared set)  expiry
 *                     handler (the supervision discriminator)
 *   armed_timer_id    launch (the never-reused ID it arms) / expiry (the
 *                     kill + the re-arm's fresh ID) / WM_DESTROY
 *                     (0 = none armed)   WM_TIMER's identity test
 *   closing           drain (first hold, one-way)  both handlers / guards
 *   generation        every payload producer (under the lock, before
 *                     serialization)  the web store's filter
 *
 * The worker's one lock take covers the post itself: PostMessageW ONLY
 * (the SendMessage family is FORBIDDEN under this lock - a cross-thread
 * synchronous send while the UI thread holds it is an instant deadlock).
 * The UI-side handler and the expiry commit under the lock and deliver
 * (the webview post) after releasing it. */

#define WM_CENSUS_DONE         (WM_APP + 20)   /* completion notification */
#define WM_CENSUS_STALE        (WM_APP + 21)   /* stale-ping (marshaled) */

/* Cache TTL and the per-build HCN probe budget (tuning values from the
   design's display tolerance; the deadline itself is the UI-side
   watchdog, never the worker). */
#define CENSUS_TTL_MS          30000
#define CENSUS_PROBE_BUDGET    8

/* One census build: the deep-copied priority snapshot, the request, and
   the payload (worker-owned until the post; the UI side owns it after). */
typedef struct {
    UINT64 token;
    UINT64 build_epoch;
    HcnInternalCensusRequest req;
    wchar_t (*priority_store)[INTERNAL_SWITCH_CAP];
    const wchar_t *priority_names[ASB_MAX_VMS];
    HcnInternalSwitchCensus census;
} CensusWork;

typedef struct {
    UINT64 generation;
    const wchar_t *reason;
} CensusNotice;

typedef struct {
    CensusNotice items[4];
    size_t count;
} CensusNoticeBatch;

typedef struct {
    CRITICAL_SECTION lock;            /* never deleted (the abandon branch) */
    BOOL closing;
    BOOL initialized;

    BOOL worker_active;
    BOOL pending_request;
    BOOL io_complete;
    BOOL priorities_dirty;
    BOOL published_once;
    BOOL deadline_fired;
    UINT64 invalidation_epoch;
    UINT64 build_epoch;
    UINT64 token_seq;
    UINT64 timer_seq;
    UINT_PTR armed_timer_id;
    UINT64 generation;

    HANDLE build_thread;              /* non-owning alias into the set */
    HANDLE handles[16];
    size_t handle_count;
    HANDLE cancel_event;              /* manual-reset; never closed */

    UINT64 active_token;              /* the completion identity the
                                         notification carries */

    /* The staged priority list: the ordered array of non-empty stored
       selectors of Internal-mode VMs (exactly what the launch helper
       deep-copies); a staging write is dirty iff the array is not
       identical under ordered, case-sensitive comparison. */
    wchar_t staged[ASB_MAX_VMS][INTERNAL_SWITCH_CAP];
    size_t staged_count;

    /* The webview owns the displayed rows; the UI needs only cache validity
       and age to decide whether a consult is fresh. */
    BOOL cached_valid;
    ULONGLONG cache_time;
} CensusManager;

static CensusManager g_census;

static DWORD WINAPI census_worker_thread(LPVOID param);
static void census_queue_unavailable_locked(const wchar_t *reason,
                                            CensusNoticeBatch *notices);
static void census_flush_notices(CensusNoticeBatch *notices);

/* Arm a fresh, never-reused watchdog ID. Store only the ID Windows
   actually returned; zero means the caller must publish a terminal state
   instead of leaving a build with no deadline. */
static BOOL census_arm_timer_locked(HWND hwnd)
{
    UINT_PTR timer_id = (UINT_PTR)++g_census.timer_seq;
    UINT_PTR armed;

    if (!timer_id)
        timer_id = (UINT_PTR)++g_census.timer_seq;
    armed = SetTimer(hwnd, timer_id, WMI_CENSUS_DEADLINE_MS, NULL);
    if (!armed) {
        g_census.armed_timer_id = 0;
        return FALSE;
    }
    g_census.armed_timer_id = armed;
    return TRUE;
}

/* The stale-ping callback: marshaled to the UI thread with PostMessageW
   ONLY (never webview2_post from the callback - it may fire on a worker). */
static void census_stale_cb(void *user_data)
{
    (void)user_data;
    if (g_hwnd_main)
        PostMessageW(g_hwnd_main, WM_CENSUS_STALE, 0, 0);
}

/* Free a work (payload included): safe on every shape the enum returns. */
static void census_work_free(CensusWork *work)
{
    if (!work)
        return;
    hcn_internal_switch_census_free(&work->census);
    if (work->priority_store)
        HeapFree(GetProcessHeap(), 0, work->priority_store);
    HeapFree(GetProcessHeap(), 0, work);
}

/* Prune the handle set: a zero-timeout probe plus CloseHandle on the
   exits. Called from the launch path and the drain (both UI-thread, under
   the lock). A still-live handle stays for the drain's wait. */
static void census_prune_handles_locked(void)
{
    size_t i = 0;
    while (i < g_census.handle_count) {
        HANDLE h = g_census.handles[i];
        if (!h || WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
            if (h) CloseHandle(h);
            g_census.handles[i] = g_census.handles[g_census.handle_count - 1];
            g_census.handle_count--;
        } else {
            i++;
        }
    }
}

/* Stage the latest priority list: the distinct non-empty stored selectors
   of Internal-mode VMs in VM-list order. Sets priorities_dirty iff the
   ordered, case-sensitive array actually changed (a miss is the unbounded
   harm; a false dirty is one bounded rebuild). */
static void census_stage_priorities_locked(void)
{
    wchar_t fresh[ASB_MAX_VMS][INTERNAL_SWITCH_CAP];
    size_t count = 0;
    int i, j, n = asb_vm_count();
    BOOL dirty;

    ZeroMemory(fresh, sizeof(fresh));
    for (i = 0; i < n && count < ASB_MAX_VMS; i++) {
        const wchar_t *sel = asb_vm_internal_switch(asb_vm_get(i));
        if (!sel || !sel[0])
            continue;
        for (j = 0; j < (int)count; j++)
            if (wcscmp(fresh[j], sel) == 0)
                break;
        if (j == (int)count) {
            wcsncpy_s(fresh[count], INTERNAL_SWITCH_CAP, sel, _TRUNCATE);
            count++;
        }
    }

    dirty = (count != g_census.staged_count);
    if (!dirty) {
        for (i = 0; i < (int)count; i++) {
            if (wcscmp(fresh[i], g_census.staged[i]) != 0) {
                dirty = TRUE;
                break;
            }
        }
    }
    if (dirty) {
        memcpy(g_census.staged, fresh, sizeof(fresh));
        g_census.staged_count = count;
        g_census.priorities_dirty = TRUE;
    }
}

/* The ONE launch helper (UI-thread-only; called with the lock held): it
   establishes the watchdog and active token before any fallible work
   allocation. Allocation/CreateThread/capacity failures therefore reach
   an UNAVAILABLE terminal through the timer. A timer-arm failure has no
   watchdog to do that work, so it publishes UNAVAILABLE immediately. */
static void census_launch_locked(CensusNoticeBatch *notices)
{
    CensusWork *work = NULL;
    HANDLE thread = NULL;
    size_t i;
    UINT64 token = ++g_census.token_seq;

    g_census.build_epoch = g_census.invalidation_epoch;
    g_census.pending_request = FALSE;
    g_census.io_complete = FALSE;
    g_census.priorities_dirty = FALSE;
    g_census.deadline_fired = FALSE;
    g_census.active_token = token;
    g_census.worker_active = TRUE;
    g_census.build_thread = NULL;

    if (g_census.armed_timer_id) {
        KillTimer(g_hwnd_main, (UINT_PTR)g_census.armed_timer_id);
        g_census.armed_timer_id = 0;
    }
    if (!census_arm_timer_locked(g_hwnd_main)) {
        g_census.worker_active = FALSE;
        g_census.pending_request = FALSE;
        g_census.deadline_fired = TRUE;
        ui_log(L"UI: census watchdog SetTimer failed (0x%08X).",
               (unsigned)GetLastError());
        census_queue_unavailable_locked(
            L"Internal: host inventory unavailable (the census watchdog could not be armed).",
            notices);
        return;
    }

    census_prune_handles_locked();
    if (g_census.handle_count >= ARRAYSIZE(g_census.handles)) {
        ui_log(L"UI: census handle set full; build deferred to watchdog.");
        return;  /* no worker starts without an owned handle slot */
    }

    work = (CensusWork *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    sizeof(CensusWork));
    if (!work) {
        ui_log(L"UI: census work allocation failed.");
        return;  /* active token + timer publishes UNAVAILABLE */
    }
    work->priority_store = (wchar_t (*)[INTERNAL_SWITCH_CAP])HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        g_census.staged_count * INTERNAL_SWITCH_CAP * sizeof(wchar_t));
    if (!work->priority_store && g_census.staged_count > 0) {
        ui_log(L"UI: census priority snapshot allocation failed.");
        census_work_free(work);
        return;  /* active token + timer publishes UNAVAILABLE */
    }
    if (g_census.staged_count > 0) {
        memcpy(work->priority_store, g_census.staged,
               g_census.staged_count * INTERNAL_SWITCH_CAP * sizeof(wchar_t));
        for (i = 0; i < g_census.staged_count; i++)
            work->priority_names[i] = work->priority_store[i];
    }

    work->token = token;
    work->build_epoch = g_census.invalidation_epoch;
    work->req.priority_names = work->priority_names;
    work->req.priority_count = g_census.staged_count;
    work->req.probe_budget = CENSUS_PROBE_BUDGET;
    work->req.project_for_ui = TRUE;
    work->req.cancel_event = g_census.cancel_event;

    ResetEvent(g_census.cancel_event);
    thread = CreateThread(NULL, 0, census_worker_thread, work, 0, NULL);
    if (thread) {
        g_census.handles[g_census.handle_count++] = thread;
        g_census.build_thread = thread;
    } else {
        census_work_free(work);
        ui_log(L"UI: census worker CreateThread failed (0x%08X).",
               (unsigned)GetLastError());
    }
}

static DWORD WINAPI census_worker_thread(LPVOID param)
{
    CensusWork *work = (CensusWork *)param;
    BOOL posted;

    /* The session's lifetime (open, topology, census, probes, close) is
       inside the enum call; this thread owns the COM apartment. */
    hcn_enum_internal_switches(&work->req, &work->census);

    /* The one lock take covers the post itself: PostMessageW is bounded
       and non-blocking. io_complete is stored ONLY on a successful post
       (SET means the message is queued and the handler will run); a FALSE
       post frees the payload, sets NOTHING, and exits with worker_active
       still TRUE and the handle signalled - exactly the state the expiry's
       handle probe arm exists for. */
    EnterCriticalSection(&g_census.lock);
    posted = PostMessageW(g_hwnd_main, WM_CENSUS_DONE, (WPARAM)work->token,
                          (LPARAM)work);
    if (posted)
        g_census.io_complete = TRUE;
    LeaveCriticalSection(&g_census.lock);
    if (!posted) {
        ui_log(L"UI: census completion post failed; build dropped.");
        census_work_free(work);
    }
    return 0;
}

/* The fixed-size UNAVAILABLE envelope for the census message (overflow,
   allocation failure, the expiry, and the request entry's terminal
   publish): a post is always emitted, never partial JSON, and the
   generation (already taken under the lock) guarantees the store consumes
   it. */
static void census_post_unavailable(UINT64 generation, const wchar_t *reason)
{
    wchar_t env[512];
    JsonBuilder jb;

    jb_init(&jb, env, ARRAYSIZE(env));
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"internalSwitchCensus");
    jb_int(&jb, L"generation", (int)(generation & 0x7FFFFFFF));
    jb_string(&jb, L"state", L"unavailable");
    jb_string(&jb, L"reason", reason);
    jb_array_begin(&jb, L"switches");
    jb_array_end(&jb);
    jb_object_end(&jb);
    if (jb.overflow)
        return;   /* the fixed envelope cannot overflow; defensive only */
    webview2_post(env);
}

/* Record a terminal result under the manager lock and defer its WebView
   delivery until the caller releases that lock. A handler can produce at
   most two notices (for example, an expired worker followed by a failed
   follow-up timer arm); the batch preserves their generation order. */
static void census_queue_unavailable_locked(const wchar_t *reason,
                                            CensusNoticeBatch *notices)
{
    UINT64 generation = ++g_census.generation;

    g_census.cached_valid = TRUE;
    g_census.cache_time = GetTickCount64() - CENSUS_TTL_MS - 1;
    g_census.published_once = TRUE;
    g_census.deadline_fired = TRUE;

    if (notices && notices->count < ARRAYSIZE(notices->items)) {
        notices->items[notices->count].generation = generation;
        notices->items[notices->count].reason = reason;
        notices->count++;
    } else {
        /* Capacity is sized for the maximum transitions in one UI event.
           Preserve fail-closed behavior if a future caller exceeds it. */
        ui_log(L"UI: census unavailable notice batch is full.");
    }
}

static void census_flush_notices(CensusNoticeBatch *notices)
{
    size_t i;
    if (!notices)
        return;
    for (i = 0; i < notices->count; i++)
        census_post_unavailable(notices->items[i].generation,
                                notices->items[i].reason);
    notices->count = 0;
}

/* Serialize the census message (schema: swClass 0=Internal 1=External
   2=Private 3=Unclassified; verdict 0=Owned 1=Borrowed 2=Rejected
   3=Unknown - the integers are an ABI, never reordered). Heap buffer with
   the overflow-suppress: on overflow or allocation failure the fixed
   unavailable envelope (same generation) is posted instead - never a
   silent drop, never partial JSON. */
static void census_post_census(const HcnInternalSwitchCensus *c,
                               UINT64 generation)
{
    wchar_t *buf;
    JsonBuilder jb;
    size_t i;

    buf = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                               131072 * sizeof(wchar_t));
    if (!buf) {
        census_post_unavailable(generation, L"census message allocation failed");
        return;
    }
    jb_init(&jb, buf, 131072);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"internalSwitchCensus");
    jb_int(&jb, L"generation", (int)(generation & 0x7FFFFFFF));
    switch (c->state) {
    case HCN_CENSUS_OK:      jb_string(&jb, L"state", L"ok"); break;
    case HCN_CENSUS_EMPTY:   jb_string(&jb, L"state", L"empty"); break;
    default:                 jb_string(&jb, L"state", L"unavailable"); break;
    }
    jb_string(&jb, L"reason", c->reason_text);
    jb_array_begin(&jb, L"switches");
    for (i = 0; i < c->count; i++) {
        const HcnInternalSwitchEntry *e = &c->entries[i];
        if (i > 0) jb_append(&jb, L",");
        jb_object_begin(&jb);
        jb_string(&jb, L"name", e->name);
        jb_int(&jb, L"swClass", (int)e->sw_class);
        jb_string(&jb, L"type", e->type);
        jb_int(&jb, L"verdict", (int)e->verdict);
        jb_bool(&jb, L"nameUnusable", e->name_unusable);
        jb_bool(&jb, L"inDuplicateGroup", e->in_duplicate_name_group);
        jb_bool(&jb, L"deferEligible", e->defer_eligible);
        jb_bool(&jb, L"selectable", e->selectable);
        jb_string(&jb, L"reason", e->reason_text);
        jb_object_end(&jb);
    }
    jb_array_end(&jb);
    jb_object_end(&jb);
    if (jb.overflow) {
        ui_log(L"UI: census message exceeded the buffer; suppressed.");
        census_post_unavailable(generation, L"census message too large");
    } else {
        webview2_post(buf);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

/* The completion handler's commit/discard/follow-up, all under one hold;
   the delivery (webview post) and the frees run after it. */
static void census_on_done(WPARAM wp, LPARAM lp)
{
    CensusWork *work = (CensusWork *)lp;
    const HcnInternalSwitchCensus *to_post = NULL;
    CensusWork *to_free = NULL;
    CensusNoticeBatch notices = {0};
    UINT64 gen = 0;

    EnterCriticalSection(&g_census.lock);
    if ((UINT64)wp != g_census.active_token) {
        /* A stale message: take its payload for freeing, do nothing else
           (an older build's completion must neither kill the new timer
           nor publish over it). */
        to_free = work;
    } else {
        /* A matching completion owns this token's timer. Kill it before
           committing or starting a follow-up so a queued WM_TIMER cannot
           act on the next build. Stale completions never touch this ID. */
        if (g_census.armed_timer_id) {
            KillTimer(g_hwnd_main, (UINT_PTR)g_census.armed_timer_id);
            g_census.armed_timer_id = 0;
        }
        if (!g_census.closing &&
            work->build_epoch == g_census.invalidation_epoch) {
            /* Commit: the generation is taken here (under the lock, at the
               moment the payload is produced - before serialization). The
               work payload stays alive through the post; the cache stores
               metadata only, so a follow-up cannot invalidate to_post. */
            gen = ++g_census.generation;
            g_census.cached_valid = TRUE;
            g_census.cache_time = GetTickCount64();
            to_post = &work->census;
            g_census.published_once = TRUE;
        } else {
            /* Discard (a cancelled build's abort, or closing): no cache
               write; the payload is freed below. The notification is still
               consumed - "no post" never means no control notification. */
            to_free = work;
            work = NULL;
        }
        /* The follow-up decision: a kick that landed while the build ran
           set pending_request; exactly one follow-up starts now (the
           launch keeps the slot taken), else the slot frees. */
        if (g_census.pending_request && !g_census.closing)
            census_launch_locked(&notices);
        else {
            g_census.worker_active = FALSE;
            g_census.build_thread = NULL;
        }
        if (work && work != to_free)
            to_free = work;   /* the shell (store) always frees */
    }
    LeaveCriticalSection(&g_census.lock);

    /* Deliver after the hold. */
    if (to_post)
        census_post_census(to_post, gen);
    if (to_free)
        census_work_free(to_free);
    census_flush_notices(&notices);
}

/* The request entry (the ping or the consult), shared shape: the ghost
   probe runs FIRST and only when the build has NOT delivered (io_complete
   SET means the completion message is queued and the handler will run - a
   normally-exited worker in the [post, handler] sliver is NOT a ghost; a
   SIGNALED/NULL build_thread with the flag clear is). A dead undelivered
   token whose deadline never fired gets its terminal published FIRST (one
   UNAVAILABLE, one generation, published_once, deadline_fired SET - an
   already-finalized token is not re-published), then the slot clears in
   the same hold and the request's own tail launches directly. The ping
   additionally invalidates (only once published_once) and unconditionally
   queues; the consult is a genuine request only when the cache is expired
  or never built (cache-first answers a fresh consult; the web already
  holds that payload). Notice batches carry fixed envelopes for posting
  after the hold. */
static void census_request_locked(BOOL is_ping, CensusNoticeBatch *notices)
{
    BOOL genuine;

    genuine = is_ping || !g_census.cached_valid ||
              GetTickCount64() - g_census.cache_time >= CENSUS_TTL_MS;
    if (!genuine)
        return;

    if (g_census.worker_active && !g_census.io_complete &&
        (!g_census.build_thread ||
         WaitForSingleObject(g_census.build_thread, 0) == WAIT_OBJECT_0)) {
        if (!g_census.deadline_fired)
            census_queue_unavailable_locked(
                L"Internal: host inventory unavailable (the census build "
                L"did not deliver).", notices);
        g_census.worker_active = FALSE;
        g_census.build_thread = NULL;
    }

    if (is_ping) {
        /* A PING: evidence that the DISPLAYED data is wrong. Invalidates
           (only once a terminal payload has been published - during the
           first wait there is no displayed list to be wrong, and the
           in-flight build's result is the first answer), and queues the
           correction unconditionally. */
        if (g_census.published_once)
            g_census.invalidation_epoch++;
        g_census.pending_request = TRUE;
    }

    /* The arms (the request IS the request: the tail launches directly,
     no follow-up rule): !worker_active -> launch (gated !closing; the
     fall-through of a cleared ghost and the ordinary free case share this
     ONE step); the in-flight build can no longer publish or the staged
     priorities changed -> set pending_request (the handler's follow-up
     runs it when the slot frees); otherwise -> merge, do nothing - the
     in-flight build answers this request. */
    if (!g_census.worker_active) {
        if (!g_census.closing)
            census_launch_locked(notices);
    } else if (g_census.build_epoch != g_census.invalidation_epoch ||
               g_census.priorities_dirty) {
        g_census.pending_request = TRUE;
    }
}

static void census_on_stale(void)
{
    CensusNoticeBatch notices = {0};

    EnterCriticalSection(&g_census.lock);
    if (g_census.closing) {
        LeaveCriticalSection(&g_census.lock);
        return;
    }
    census_stage_priorities_locked();
    census_request_locked(TRUE, &notices);
    LeaveCriticalSection(&g_census.lock);
    census_flush_notices(&notices);
}

/* The dropdown consult (the frontend's getInternalSwitches): cache-first
   - the web holds its copy and renders it (or its pending state); the
   host side stages the priorities and kicks a rebuild only when the cache
   is expired or never built. */
static void census_on_consult(void)
{
    CensusNoticeBatch notices = {0};

    if (!g_census.initialized)
        return;
    EnterCriticalSection(&g_census.lock);
    if (g_census.closing) {
        LeaveCriticalSection(&g_census.lock);
        return;
    }
    census_stage_priorities_locked();
    census_request_locked(FALSE, &notices);
    LeaveCriticalSection(&g_census.lock);
    census_flush_notices(&notices);
}

/* The WM_TIMER watchdog: the FIRST test is the message's own ID against
   armed_timer_id (read under the census lock; a mismatch is a stale tick
   from a killed predecessor: kill wParam's timer and return, nothing
   else - WM_TIMER carries only an ID, so a token test is meaningless for
   it). A matching tick with deadline_fired SET is a supervision tick (the
   probe alone); CLEAR is the first deadline (the full finalize). */
static void census_on_timer(HWND hwnd, WPARAM wp)
{
    CensusNoticeBatch notices = {0};

    EnterCriticalSection(&g_census.lock);

    if ((UINT_PTR)wp != g_census.armed_timer_id) {
        KillTimer(hwnd, (UINT_PTR)wp);
        LeaveCriticalSection(&g_census.lock);
        return;
    }

    if (g_census.deadline_fired) {
        /* A supervision tick: the guard's skip conditions, then the probe
           alone - NEVER the full finalize (no publish, no epoch bump, no
           cancel, no launch: those are the first finalize's one-time
           actions). LIVE handle => re-arm a fresh never-reused ID on the
           same token; GONE => clear the slot, run the follow-up rule, do
           not re-arm. */
        if (g_census.closing || !g_census.worker_active || g_census.io_complete) {
            /* Disarmed by the matching completion (the handler's commit
               killed the timer, but a queued tick still lands), by a new
               launch, or by closing: kill and disarm. */
            KillTimer(hwnd, (UINT_PTR)wp);
            g_census.armed_timer_id = 0;
        } else if (!g_census.build_thread ||
                   WaitForSingleObject(g_census.build_thread, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd, (UINT_PTR)wp);
            g_census.armed_timer_id = 0;
            g_census.worker_active = FALSE;
            g_census.build_thread = NULL;
            if (g_census.pending_request && !g_census.closing)
                census_launch_locked(&notices);
        } else {
            /* Not proven exited: re-arm a fresh probe-only watchdog ID. */
            KillTimer(hwnd, (UINT_PTR)wp);
            g_census.armed_timer_id = 0;
            if (!census_arm_timer_locked(hwnd))
                ui_log(L"UI: census supervision SetTimer failed (0x%08X); "
                       L"worker remains tracked.", (unsigned)GetLastError());
        }
        LeaveCriticalSection(&g_census.lock);
        census_flush_notices(&notices);
        return;
    }

    /* The first deadline's guard: io_complete SET (the message is queued,
       the handler will run - flushing now would be a FALSE timeout),
       worker_active clear, or closing - skip the finalize, kill only. */
    if (g_census.io_complete || !g_census.worker_active || g_census.closing) {
        KillTimer(hwnd, (UINT_PTR)wp);
        g_census.armed_timer_id = 0;
        LeaveCriticalSection(&g_census.lock);
        return;
    }

    /* The finalize (one hold): the generation is taken here; the cache
       entry is the fixed UNAVAILABLE payload with an EXPIRED TTL; the
       epoch increments (a late physical completion fails its build_epoch
       check and is discarded); the cancel event is signalled (a worker
       between queries exits and releases the slot); deadline_fired SET;
       wParam's timer killed (a SetTimer timer is periodic until killed);
       the live-handle branch re-arms a fresh never-reused ID as a
       probe-only supervision timer; a GONE handle clears the slot and
       runs the follow-up rule. The fixed envelope's webview post runs
       AFTER the hold. */
    census_queue_unavailable_locked(
        L"Internal: host inventory unavailable (the census build did not "
        L"deliver within its deadline).", &notices);
    g_census.deadline_fired = TRUE;
    g_census.invalidation_epoch++;
    if (g_census.cancel_event)
        SetEvent(g_census.cancel_event);
    KillTimer(hwnd, (UINT_PTR)wp);
    g_census.armed_timer_id = 0;

    if (g_census.build_thread &&
        WaitForSingleObject(g_census.build_thread, 0) != WAIT_OBJECT_0) {
        /* LIVE: re-arm the probe-only supervision timer (fresh ID, same
           token - the token cannot discriminate, deadline_fired does). */
        if (!census_arm_timer_locked(hwnd))
            ui_log(L"UI: census supervision SetTimer failed (0x%08X); "
                   L"worker remains tracked.", (unsigned)GetLastError());
    } else {
        /* GONE (a failed post, a crash, a CreateThread failure): the slot
           frees; the follow-up rule runs; a LONE timeout still starts
           nothing. */
        g_census.worker_active = FALSE;
        g_census.build_thread = NULL;
        if (g_census.pending_request && !g_census.closing)
            census_launch_locked(&notices);
    }
    LeaveCriticalSection(&g_census.lock);

    census_flush_notices(&notices);
}

/* The WM_DESTROY drain: two brief lock holds with the wait between them
   lock-free (the worker's one lock take - the post - sits inside the
   window being waited; a wait under the lock deadlocks every shutdown
   with an in-flight build). BOTH outcomes at the budget are passes:
   within it the worker exits and the unload proceeds; at expiry the drain
   ABANDONS the unload (asb_inhibit_hcn_unload + the module reclaimed at
   process exit) - and nothing the abandoned worker can still touch is
   destroyed: the lock is never deleted, the event never closed, the
   manager's fields are static storage, the remaining handles stay open. */
static void census_drain(HWND hwnd)
{
    HANDLE live[ARRAYSIZE(g_census.handles)];
    size_t count = 0;
    size_t i;
    ULONGLONG deadline;
    BOOL timed_out = FALSE;

    if (!g_census.initialized)
        return;

    /* Hold 1: closing, the armed timer killed, the cancel event and the
       live handle set taken; cache metadata cleared (UI-thread-only). */
    EnterCriticalSection(&g_census.lock);
    g_census.closing = TRUE;
    if (g_census.armed_timer_id) {
        KillTimer(hwnd, (UINT_PTR)g_census.armed_timer_id);
        g_census.armed_timer_id = 0;
    }
    census_prune_handles_locked();
    memcpy(live, g_census.handles, g_census.handle_count * sizeof(HANDLE));
    count = g_census.handle_count;
    g_census.cached_valid = FALSE;
    LeaveCriticalSection(&g_census.lock);

    /* Signal the cancel event - NO LOCK HELD. */
    if (g_census.cancel_event)
        SetEvent(g_census.cancel_event);

    /* The single-budget wait over every live handle (lock-free). */
    deadline = GetTickCount64() + CENSUS_DRAIN_WAIT_MS;
    if (count > 0) {
        for (;;) {
            ULONGLONG remaining = deadline - GetTickCount64();
            DWORD wait_rc;
            size_t n = count > 64 ? 64 : count;   /* the API's hard cap */
            if (remaining > CENSUS_DRAIN_WAIT_MS) {   /* underflow: expired */
                wait_rc = WAIT_TIMEOUT;
            } else if (remaining == 0) {
                wait_rc = WAIT_TIMEOUT;
            } else {
                wait_rc = WaitForMultipleObjects((DWORD)n, live, TRUE,
                                                  (DWORD)remaining);
            }
            if (wait_rc == WAIT_OBJECT_0)
                break;
            if (wait_rc == WAIT_TIMEOUT && GetTickCount64() >= deadline)
                break;
            if (wait_rc == WAIT_FAILED)
                break;
        }
        /* Elapsed time never proves thread exit, including after a failed
           wait. Probe each captured handle; any unknown/unsignaled result
           inhibits HCN unload. */
        for (i = 0; i < count; i++) {
            if (WaitForSingleObject(live[i], 0) != WAIT_OBJECT_0) {
                timed_out = TRUE;
                break;
            }
        }
    }

    /* Hold 2: prune the set (zero-timeout probes + CloseHandle on the
       exits; a wedged handle stays open for the OS). */
    EnterCriticalSection(&g_census.lock);
    census_prune_handles_locked();
    LeaveCriticalSection(&g_census.lock);

    /* PeekMessage-drain any already-queued census payloads on the UI
       thread and free them (the closing handler is not a no-op: it frees
       and starts nothing; a notification posted after the budget expired
       belongs to the abandoned branch - nothing will consume it, the same
       accepted trade-off as the abandoned thread itself). */
    {
        MSG msg;
        while (PeekMessageW(&msg, NULL, WM_CENSUS_DONE, WM_CENSUS_DONE,
                            PM_REMOVE)) {
            CensusWork *work = (CensusWork *)msg.lParam;
            census_work_free(work);
        }
        while (PeekMessageW(&msg, NULL, WM_CENSUS_STALE, WM_CENSUS_STALE,
                            PM_REMOVE)) {
            /* dropped: closing ignores pings */
        }
    }

    if (timed_out) {
        ui_log(L"UI: census worker did not exit within %lu ms; the HCN "
               L"module stays loaded (reclaimed at process exit).",
               (unsigned long)CENSUS_DRAIN_WAIT_MS);
        asb_inhibit_hcn_unload();
    }
}

/* Initialize the manager (UI init, before any worker thread exists - the
   thread-creation barrier is the publication fence for the stale-ping
   callback pair). */
static void census_init(void)
{
    InitializeCriticalSection(&g_census.lock);
    g_census.cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_census.initialized = TRUE;
    asb_set_census_stale_callback(census_stale_cb, NULL);
}

/* ---- Window creation ---- */

HWND ui_create_main_window(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wc;
    HWND hwnd;
    BOOL dark = TRUE;

    g_ui_thread_id = GetCurrentThreadId();
    g_hInstance = hInstance;
    g_is_home_edition = detect_home_edition();
    asb_set_hinstance(hInstance);

    /* Set library callbacks before init */
    asb_set_log_callback(ui_log_callback, NULL);
    asb_set_state_callback(ui_state_callback, NULL);
    asb_set_progress_callback(ui_progress_callback, NULL);
    asb_set_alert_callback(ui_alert_callback, NULL);
    asb_set_vm_removed_callback(ui_vm_removed_callback, NULL);

    /* The census manager's one-time init: the CRITICAL_SECTION, the
       manual-reset cancel event, and the stale-ping registration - before
       any worker thread exists (the thread-creation barrier is the
       publication fence for the callback pair). */
    census_init();

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
        0, L"AppSandbox_Main", L"App Sandbox",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1575, 900,
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
        hcs_set_monitor_hwnd(hwnd);
        vm_agent_set_hwnd(hwnd);
        asb_idd_probe_set_hwnd(hwnd);
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

    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            tray_handle_measure((MEASUREITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) {
            tray_handle_draw((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        if (g_min_width > 0)  mmi->ptMinTrackSize.x = g_min_width;
        if (g_min_height > 0) mmi->ptMinTrackSize.y = g_min_height;
        return 0;
    }

    case WM_VM_STATE_CHANGED:
    {
        /* Library already handled cleanup - just refresh UI and manage displays */
        AsbVm vm = (AsbVm)lp;
        BOOL running = (BOOL)wp;
        int idx = asb_vm_index(vm);

        if (idx >= 0 && !running) {
            safe_destroy_rdp(idx);
            safe_destroy_idd(idx);
        }

        send_vm_list();
        return 0;
    }

    case WM_VM_REMOVED:
    {
        /* Library removed a VM at this index - compact display arrays */
        int idx = (int)wp;
        int count = asb_vm_count();
        int j;
        safe_destroy_rdp(idx);
        safe_destroy_idd(idx);
        for (j = idx; j < count; j++) {
            g_displays[j] = g_displays[j + 1];
            g_idd_displays[j] = g_idd_displays[j + 1];
        }
        g_displays[count] = NULL;
        g_idd_displays[count] = NULL;
        send_vm_list();
        send_templates();
        return 0;
    }

    case WM_VM_AGENT_STATUS:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst && inst->agent_online) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (g_displays[i] && vm_display_is_open(g_displays[i])) {
                    ui_log(L"Agent online - switching \"%s\" from RDP to IDD.", inst->name);
                    safe_destroy_rdp(i);
                    do_connect_idd(i);
                } else if (!g_idd_displays[i] && !inst->shutdown_requested) {
                    do_connect_idd(i);
                }
                break;
            }
        }
        send_vm_list();
        return 0;
    }

    case WM_VM_IDD_READY:
    {
        /* lp is a stable per-VM id (UINT64), not a VmInstance*. Resolve
           fresh; returns NULL if the VM was deleted between PostMessage
           and our processing. */
        VmInstance *inst = asb_find_vm_by_id((UINT64)lp);
        if (inst && inst->running && !inst->install_complete && !inst->shutdown_requested) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (!g_idd_displays[i]) {
                    ui_log(L"VDD ready - opening IDD display for \"%s\".", inst->name);
                    do_connect_idd(i);
                }
                break;
            }
        }
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
        if (inst && inst->running) {
            int i, count = asb_vm_count();
            inst->hyperv_video_off = TRUE;
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (g_displays[i] && vm_display_is_open(g_displays[i])) {
                    ui_log(L"Hyper-V Video disabled - switching to IDD.");
                    safe_destroy_rdp(i);
                    do_connect_idd(i);
                }
                break;
            }
        }
        send_vm_list();
        return 0;
    }

    case WM_VM_DISPLAY_CLOSED:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (wp == 0) safe_destroy_rdp(i);
                if (wp == 1) safe_destroy_idd(i);
                break;
            }
        }
        return 0;
    }

    case WM_VM_MONITOR_DETECTED:
    {
        /* Safety net: monitor thread detected VM stopped.
           The library's HCS callback should handle this, but the monitor
           provides a fallback. Mark the VM as stopped and refresh. */
        VmInstance *inst = (VmInstance *)lp;
        if (inst && inst->running) {
            int i, count = asb_vm_count();
            ui_log(L"Monitor detected VM \"%s\" stopped.", inst->name);
            inst->running = FALSE;
            inst->shutdown_requested = FALSE;
            inst->hyperv_video_off = FALSE;
            hcs_stop_monitor(inst);
            vm_agent_stop(inst);
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                safe_destroy_rdp(i);
                safe_destroy_idd(i);
                break;
            }
            asb_vm_cleanup_network(inst);
            hcs_close_vm(inst);
        }
        send_vm_list();
        asb_save();
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

    case WM_VM_AGENT_GPUCOPY:
    {
        VmInstance *inst = (VmInstance *)lp;
        if (inst) {
            int i, count = asb_vm_count();
            for (i = 0; i < count; i++) {
                if (asb_vm_instance(asb_vm_get(i)) != inst) continue;
                if (g_displays[i]) {
                    safe_destroy_rdp(i);
                    ui_log(L"RDP display torn down for \"%s\" (GPU driver activation).", inst->name);
                }
                break;
            }
        }
        return 0;
    }

    case WM_WEBVIEW2_LOG:
    {
        wchar_t *log_text = (wchar_t *)lp;
        if (log_text) { ui_log_post(log_text); free(log_text); }
        return 0;
    }

    case WM_DISK_SPACE:
    {
        wchar_t *response = (wchar_t *)lp;
        if (response) {
            webview2_post(response);
            HeapFree(GetProcessHeap(), 0, response);
        }
        return 0;
    }

    case WM_PREREQ_PROGRESS:
    {
        wchar_t buf[128];
        _snwprintf_s(buf, 128, _TRUNCATE,
            L"{\"type\":\"prereqProgress\",\"pct\":%d}", (int)wp);
        webview2_post(buf);
        return 0;
    }

    case WM_PREREQ_DONE:
    {
        BOOL ok = (BOOL)wp;
        BOOL reboot_required = (BOOL)lp;
        if (ok && !reboot_required) {
            /* Feature enabled, no reboot needed - initialize now */
            g_prereq_ok = TRUE;
            webview2_post(L"{\"type\":\"prereqResult\",\"ok\":true,\"reboot\":false}");
            asb_init();
            send_full_state();
        } else if (ok && reboot_required) {
            g_prereq_reboot_pending = TRUE;
            webview2_post(L"{\"type\":\"prereqResult\",\"ok\":true,\"reboot\":true}");
        } else {
            webview2_post(L"{\"type\":\"prereqResult\",\"ok\":false,\"reboot\":false}");
        }
        return 0;
    }

    case WM_CENSUS_DONE:
        /* The census worker's completion notification (posted, never
           sent): the handler owns the publish/discard/launch decision. */
        census_on_done(wp, lp);
        return 0;

    case WM_CENSUS_STALE:
        /* The acquire's failure ping, marshaled from the worker thread:
         invalidates the TTL and queues a rebuild. */
        census_on_stale();
        return 0;

    case WM_TIMER:
        /* The census watchdog (every WM_TIMER in this window belongs to
           it; the ID test drops stale ticks from killed predecessors). */
        census_on_timer(hwnd, wp);
        return 0;

    case WM_SHOW_ALERT:
    {
        wchar_t *msg_text = (wchar_t *)lp;
        if (msg_text) { ui_show_alert(msg_text); free(msg_text); }
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
        int i, count = asb_vm_count();
        BOOL has_running = FALSE;
        for (i = 0; i < count; i++) {
            if (asb_vm_is_running(asb_vm_get(i))) { has_running = TRUE; break; }
        }
        if (has_running) {
            if (!asb_get_suppress_tray_warn()) {
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
                    asb_set_suppress_tray_warn(TRUE);
                    asb_save();
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
        /* The census drain runs FIRST: it signals the worker's cancel
           event, waits the single bounded budget (lock-free), prunes,
           drains queued payloads, and on a timeout abandons the HCN
           unload (asb_cleanup then skips hcn_cleanup/FreeLibrary - never
           unload code under a live thread). */
        census_drain(hwnd);
        tray_remove();
        webview2_cleanup();
        for (i = 0; i < ASB_MAX_VMS; i++) {
            safe_destroy_rdp(i);
            safe_destroy_idd(i);
        }
        asb_cleanup();
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
