/*
 * ui.c — Main window + WebView2 UI bridge.
 *
 * Thin UI shell: WebView2 hosting, JSON message dispatch, display windows,
 * tray icon. All VM orchestration is in asb_core.c (the core library).
 */

#include "ui.h"
#include "asb_core.h"
#include "resource.h"
#include "hcs_vm.h"
#include "hcn_network.h"
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
#include <shlobj.h>

#pragma comment(lib, "dwmapi.lib")
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

/* Tray */
#define TRAY_CMD_SHOW          1
#define TRAY_CMD_EXIT          2
#define TRAY_CMD_CONNECT_BASE  100
#define TRAY_CMD_SHUTDOWN_BASE 200
#define TRAY_CMD_STOP_BASE     300

static NOTIFYICONDATAW g_nid;

/* UI thread ID for thread-safe log dispatch */
static DWORD g_ui_thread_id;

/* TRUE on Windows Home / Home N (no Hyper-V video, so no RDP display) */
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
static void send_adapters(void);
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

static void build_host_info_json(JsonBuilder *jb)
{
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    DWORD host_cores, host_ram_mb;
    DWORD vm_cores = 0, vm_ram_mb = 0, vm_hdd_gb = 0;
    wchar_t base_dir[MAX_PATH];
    ULARGE_INTEGER free_bytes;
    DWORD free_gb = 0;
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
    VmInstance *v = asb_vm_instance(asb_vm_get(i));
    SnapshotTree *st_ = asb_vm_snap_tree(asb_vm_get(i));
    if (!v || !st_) return;

    jb_object_begin(jb);
    jb_string(jb, L"name", v->name);
    jb_string(jb, L"osType", v->os_type);
    jb_bool(jb, L"running", v->running);
    jb_bool(jb, L"shuttingDown", v->shutdown_requested);
    jb_bool(jb, L"agentOnline", v->agent_online);
    jb_int(jb, L"ramMb", (int)v->ram_mb);
    jb_int(jb, L"hddGb", (int)v->hdd_gb);
    jb_int(jb, L"cpuCores", (int)v->cpu_cores);
    jb_int(jb, L"gpuMode", v->gpu_mode);
    jb_string(jb, L"gpuName", v->gpu_name);
    jb_int(jb, L"networkMode", v->network_mode);
    jb_string(jb, L"netAdapter", v->net_adapter);
    jb_bool(jb, L"isTemplate", v->is_template);
    jb_bool(jb, L"hypervVideoOff", v->hyperv_video_off);
    jb_bool(jb, L"buildingVhdx", v->building_vhdx);
    jb_bool(jb, L"vhdxStaging", v->vhdx_staging);
    jb_int(jb, L"vhdxProgress", v->vhdx_progress);
    jb_bool(jb, L"installComplete", v->install_complete);
    jb_bool(jb, L"sshEnabled", v->ssh_enabled);
    jb_int(jb, L"sshPort", (int)v->ssh_port);
    jb_int(jb, L"sshState", v->ssh_state);

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

static void send_vm_list(void)
{
    wchar_t buf[16384];
    JsonBuilder jb;
    int i, count = asb_vm_count();

    jb_init(&jb, buf, 16384);
    jb_object_begin(&jb);
    jb_string(&jb, L"type", L"vmListChanged");

    jb_array_begin(&jb, L"vms");
    for (i = 0; i < count; i++) {
        if (i > 0) jb_append(&jb, L",");
        build_vm_json(&jb, i);
    }
    jb_array_end(&jb);

    {
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
        al->first_eth = al->count + 1;
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

    al.count = 0; al.first_eth = -1; al.first_wifi = -1;
    hcn_enum_adapters(adapter_enum_cb, &al);
    def_idx = (al.first_eth >= 0) ? al.first_eth : (al.first_wifi >= 0) ? al.first_wifi : 0;

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

static void send_full_state(void)
{
    wchar_t buf[32768];
    JsonBuilder jb;
    int i, count;

    jb_init(&jb, buf, 32768);
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
        def_idx = (al.first_eth >= 0) ? al.first_eth : (al.first_wifi >= 0) ? al.first_wifi : 0;

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
    webview2_post(buf);
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
    webview2_post(json);
}

static void ui_show_alert(const wchar_t *message)
{
    if (GetCurrentThreadId() == g_ui_thread_id) {
        wchar_t buf[1024];
        swprintf_s(buf, 1024, L"{\"type\":\"alert\",\"message\":\"%s\"}", message);
        webview2_post(buf);
    } else if (g_hwnd_main) {
        size_t len = wcslen(message) + 1;
        wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
        if (copy) {
            wcscpy_s(copy, len, message);
            PostMessageW(g_hwnd_main, WM_SHOW_ALERT, 0, (LPARAM)copy);
        }
    }
}

/* ---- Display connections ---- */

static void do_connect_idd(int idx)
{
    VmInstance *v;
    if (idx < 0 || idx >= asb_vm_count()) return;
    v = asb_vm_instance(asb_vm_get(idx));
    if (!v || !v->running) { ui_log(L"VM \"%s\" is not running.", v ? v->name : L"?"); return; }
    if (g_idd_displays[idx] && vm_display_idd_is_open(g_idd_displays[idx])) {
        ui_log(L"IDD display already open."); return;
    }
    safe_destroy_idd(idx);
    ui_log(L"Opening IDD display for \"%s\"...", v->name);
    g_idd_displays[idx] = vm_display_idd_create(v, g_hInstance, g_hwnd_main);
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

static void tray_remove(void) { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void tray_show_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int i, cmd, count = asb_vm_count();

    AppendMenuW(menu, MF_STRING, TRAY_CMD_SHOW, L"Show App Sandbox");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        wchar_t label[300];
        if (!v || !v->running) continue;
        swprintf_s(label, 300, L"%s  ", v->name);
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
        do_connect_idd(cmd - TRAY_CMD_CONNECT_BASE);
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
        wchar_t tpl_buf[256] = {0}, user_buf[128] = {0}, pass_buf[128] = {0};
        wchar_t adapter_buf[256] = {0};
        int val;
        BOOL is_tpl = FALSE;

        json_get_string(json, L"name", name_buf, 256);
        json_get_string(json, L"osType", os_buf, 32);
        json_get_string(json, L"imagePath", img_buf, MAX_PATH);
        json_get_string(json, L"templateName", tpl_buf, 256);
        json_get_string(json, L"adminUser", user_buf, 128);
        json_get_string(json, L"adminPass", pass_buf, 128);
        json_get_string(json, L"netAdapter", adapter_buf, 256);
        json_get_bool(json, L"isTemplate", &is_tpl);

        ZeroMemory(&cfg, sizeof(cfg));
        cfg.name = name_buf;
        cfg.os_type = os_buf;
        cfg.image_path = img_buf;
        cfg.template_name = tpl_buf;
        cfg.username = user_buf;
        cfg.password = pass_buf;
        cfg.net_adapter = adapter_buf;
        cfg.is_template = is_tpl;

        if (json_get_int(json, L"hddGb", &val)) cfg.hdd_gb = (DWORD)val;
        if (json_get_int(json, L"ramMb", &val)) cfg.ram_mb = (DWORD)val;
        if (json_get_int(json, L"cpuCores", &val)) cfg.cpu_cores = (DWORD)val;
        if (json_get_int(json, L"gpuMode", &val)) cfg.gpu_mode = val;
        if (json_get_int(json, L"networkMode", &val)) cfg.network_mode = val;
        json_get_bool(json, L"testMode", &cfg.test_mode);
        json_get_bool(json, L"sshEnabled", &cfg.ssh_enabled);

        asb_vm_create(&cfg);
        SecureZeroMemory(pass_buf, sizeof(pass_buf));
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
        int idx; if (json_get_int(json, L"vmIndex", &idx)) do_connect_idd(idx);
    } else if (wcscmp(action, L"sshConnect") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx) && idx >= 0 && idx < asb_vm_count()) {
            VmInstance *inst = asb_vm_instance(asb_vm_get(idx));
            if (inst && inst->ssh_enabled && inst->ssh_port) {
                wchar_t cmd[512];
                STARTUPINFOW si_;
                PROCESS_INFORMATION pi_;
                ZeroMemory(&si_, sizeof(si_));
                si_.cb = sizeof(si_);
                ZeroMemory(&pi_, sizeof(pi_));
                if (inst->admin_user[0])
                    _snwprintf_s(cmd, 512, _TRUNCATE,
                        L"cmd.exe /k ssh -p %lu %s@localhost",
                        inst->ssh_port, inst->admin_user);
                else
                    _snwprintf_s(cmd, 512, _TRUNCATE,
                        L"cmd.exe /k ssh -p %lu localhost",
                        inst->ssh_port);
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
            ui_log(L"Deleting VM \"%s\"...", asb_vm_name(asb_vm_get(idx)));
            safe_destroy_rdp(idx);
            safe_destroy_idd(idx);
            asb_vm_delete(asb_vm_get(idx));
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
            asb_template_delete(tpl_name);
            send_templates();
        }
    } else if (wcscmp(action, L"editVm") == 0) {
        int idx;
        wchar_t field[64], value[256];
        if (json_get_int(json, L"vmIndex", &idx) && idx >= 0 && idx < asb_vm_count() &&
            json_get_string(json, L"field", field, 64) &&
            json_get_string(json, L"value", value, 256)) {
            AsbVm vm = asb_vm_get(idx);
            if (wcscmp(field, L"name") == 0) asb_vm_set_name(vm, value);
            else if (wcscmp(field, L"ramMb") == 0) asb_vm_set_ram(vm, (DWORD)_wtoi(value));
            else if (wcscmp(field, L"cpuCores") == 0) asb_vm_set_cpu(vm, (DWORD)_wtoi(value));
            else if (wcscmp(field, L"gpuMode") == 0) asb_vm_set_gpu(vm, _wtoi(value));
            else if (wcscmp(field, L"networkMode") == 0) asb_vm_set_network(vm, _wtoi(value));
            asb_save();
            send_vm_list();
        }
    } else if (wcscmp(action, L"selectVm") == 0) {
        int idx;
        if (json_get_int(json, L"vmIndex", &idx)) g_selected_vm = idx;
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
    /* Progress is already set in the VmInstance by the library — just refresh */
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

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        if (g_min_width > 0)  mmi->ptMinTrackSize.x = g_min_width;
        if (g_min_height > 0) mmi->ptMinTrackSize.y = g_min_height;
        return 0;
    }

    case WM_VM_STATE_CHANGED:
    {
        /* Library already handled cleanup — just refresh UI and manage displays */
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
        /* Library removed a VM at this index — compact display arrays */
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
        VmInstance *inst = (VmInstance *)lp;
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
            if (inst->network_mode != NET_NONE && !inst->network_cleaned) {
                hcn_delete_endpoint(&inst->endpoint_id);
                hcn_delete_network(&inst->network_id);
                inst->network_cleaned = TRUE;
            }
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
            /* Feature enabled, no reboot needed — initialize now */
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
