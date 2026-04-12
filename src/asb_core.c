/*
 * asb_core.c -- App Sandbox Core Library implementation.
 *
 * All VM orchestration extracted from ui.c: persistence, lifecycle
 * (create/start/stop/delete), snapshots, templates, config editing.
 *
 * Uses callbacks instead of PostMessage for cross-thread notification.
 */

#include <winsock2.h>
#include "asb_core.h"
#include "hcs_vm.h"
#include "gpu_enum.h"
#include "hcn_network.h"
#include "disk_util.h"
#include "snapshot.h"
#include "vmms_cert.h"
#include "vm_agent.h"
#include "vm_ssh_proxy.h"
#include "prereq.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <shlobj.h>
#include <stdarg.h>

/* ---- DLL module handle (for locating iso-patch.exe, resources, etc.) ---- */

static HMODULE g_dll_module = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH)
        g_dll_module = hinstDLL;
    return TRUE;
}

/* ---- Handle conversion macros ---- */

#define vm_inst(h)    ((VmInstance *)(h))
#define vm_handle(p)  ((AsbVm)(p))

/* ---- Globals ---- */

static GpuList g_gpu_list;

static VmInstance g_vms[ASB_MAX_VMS];
static int g_vm_count = 0;

static SnapshotTree g_snap_trees[ASB_MAX_VMS];

typedef struct {
    wchar_t name[256];
    wchar_t os_type[32];
    wchar_t image_path[MAX_PATH];
    wchar_t vhdx_path[MAX_PATH];
} TemplateInfo;
static TemplateInfo g_templates[ASB_MAX_TEMPLATES];
static int g_template_count = 0;

static wchar_t g_last_iso_path[MAX_PATH] = { 0 };
static BOOL g_suppress_tray_warn = FALSE;

static CRITICAL_SECTION g_cs;
static BOOL g_initialized = FALSE;

/* ---- Callbacks ---- */

static AsbLogCallback       g_log_cb       = NULL;
static void                *g_log_ud       = NULL;
static AsbStateCallback     g_state_cb     = NULL;
static void                *g_state_ud     = NULL;
static AsbProgressCallback  g_progress_cb  = NULL;
static void                *g_progress_ud  = NULL;
static AsbAlertCallback     g_alert_cb     = NULL;
static void                *g_alert_ud     = NULL;
static AsbVmRemovedCallback g_removed_cb   = NULL;
static void                *g_removed_ud   = NULL;

/* ---- Internal logging (replaces ui_log for library code) ---- */

static void asb_log(const wchar_t *fmt, ...)
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

    if (g_log_cb) g_log_cb(buf, g_log_ud);
}

static void asb_alert(const wchar_t *message)
{
    if (g_alert_cb) g_alert_cb(message, g_alert_ud);
}

/* ---- Public: ui_log (called by shared modules like hcs_vm.c, etc.) ---- */

ASB_API void ui_log(const wchar_t *fmt, ...)
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

    if (g_log_cb) g_log_cb(buf, g_log_ud);
}

/* ---- IDD probe (detect VDD availability during install) ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV_PROBE {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV_PROBE;

/* Frame channel: {A5B0CAFE-0002-4000-8000-000000000001} */
static const GUID IDD_FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

#define WM_VM_IDD_READY (WM_APP + 7)

static HWND g_idd_probe_hwnd = NULL;

ASB_API void asb_idd_probe_set_hwnd(HWND hwnd)
{
    g_idd_probe_hwnd = hwnd;
}

static DWORD WINAPI idd_probe_thread_proc(LPVOID param)
{
    VmInstance *vm = (VmInstance *)param;
    SOCKET s;
    SOCKADDR_HV_PROBE addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;

    asb_log(L"IDD probe: starting for \"%s\"", vm->name);

    while (!vm->idd_probe_stop && vm->running && !vm->install_complete) {
        /* Try to connect to VDD frame service */
        static const GUID zero_guid = {0};
        GUID runtime_id = vm->runtime_id;

        if (memcmp(&runtime_id, &zero_guid, sizeof(GUID)) == 0) {
            if (!hcs_find_runtime_id(vm->name, &runtime_id))  {
                Sleep(3000);
                continue;
            }
            vm->runtime_id = runtime_id;
        }

        s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        if (s == INVALID_SOCKET) {
            Sleep(3000);
            continue;
        }

        nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);

        memset(&addr, 0, sizeof(addr));
        addr.Family = AF_HYPERV;
        addr.VmId = runtime_id;
        addr.ServiceId = IDD_FRAME_SERVICE_GUID;

        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                closesocket(s);
                Sleep(3000);
                continue;
            }

            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(s, &wfds);
            FD_SET(s, &efds);
            tv.tv_sec = 3;
            tv.tv_usec = 0;

            if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
                closesocket(s);
                continue;  /* timeout or error — VDD not ready yet */
            }
        }

        /* Connection succeeded — VDD is accepting frames */
        closesocket(s);

        if (!vm->idd_probe_stop && vm->running && !vm->install_complete) {
            asb_log(L"IDD probe: VDD ready for \"%s\", opening display", vm->name);
            if (g_idd_probe_hwnd)
                PostMessageW(g_idd_probe_hwnd, WM_VM_IDD_READY, 0, (LPARAM)vm);
        }
        break;
    }

    asb_log(L"IDD probe: exiting for \"%s\"", vm->name);
    return 0;
}

static void idd_probe_start(VmInstance *vm)
{
    if (vm->idd_probe_thread) return;
    if (vm->install_complete) return;
    if (vm->is_template) return;

    vm->idd_probe_stop = FALSE;
    vm->idd_probe_thread = CreateThread(NULL, 0, idd_probe_thread_proc, vm, 0, NULL);
}

static void idd_probe_stop(VmInstance *vm)
{
    if (!vm->idd_probe_thread) return;
    vm->idd_probe_stop = TRUE;
    WaitForSingleObject(vm->idd_probe_thread, 5000);
    CloseHandle(vm->idd_probe_thread);
    vm->idd_probe_thread = NULL;
}

/* ---- Handle helpers ---- */

static int vm_index_of(AsbVm vm)
{
    int idx;
    if (!vm) return -1;
    idx = (int)(vm_inst(vm) - g_vms);
    return (idx >= 0 && idx < g_vm_count) ? idx : -1;
}

/* ---- Per-VM state JSON (beside disk.vhdx) ---- */

static void get_state_json_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_len)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_len, L"%s\\vm_state.json", dir);
}

ASB_API void vm_save_state_json(const wchar_t *vhdx_path, BOOL install_complete)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    get_state_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"installComplete\":%d}\n", install_complete ? 1 : 0);
    fclose(f);
}

ASB_API BOOL vm_load_state_json(const wchar_t *vhdx_path)
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
        if (g_vms[i].nat_ip[0] != '\0')
            fwprintf(f, L"NatIp=%S\n", g_vms[i].nat_ip);
        if (g_vms[i].is_template)
            fwprintf(f, L"IsTemplate=1\n");
        if (g_vms[i].test_mode)
            fwprintf(f, L"TestMode=1\n");
        if (g_vms[i].admin_user[0])
            fwprintf(f, L"AdminUser=%s\n", g_vms[i].admin_user);
        if (g_vms[i].ssh_enabled)
            fwprintf(f, L"SshEnabled=1\n");
        if (g_vms[i].ssh_port)
            fwprintf(f, L"SshPort=%lu\n", g_vms[i].ssh_port);
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
            if (g_vm_count >= ASB_MAX_VMS) break;
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
        else if (wcsncmp(line, L"NatIp=", 6) == 0)
            WideCharToMultiByte(CP_UTF8, 0, line + 6, -1, vm->nat_ip, sizeof(vm->nat_ip), NULL, NULL);
        else if (wcsncmp(line, L"IsTemplate=", 11) == 0)
            vm->is_template = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"TestMode=", 9) == 0)
            vm->test_mode = (_wtoi(line + 9) != 0);
        else if (wcsncmp(line, L"AdminUser=", 10) == 0)
            wcscpy_s(vm->admin_user, 128, line + 10);
        else if (wcsncmp(line, L"SshEnabled=", 11) == 0)
            vm->ssh_enabled = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"SshPort=", 8) == 0)
            vm->ssh_port = (DWORD)_wtoi(line + 8);
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
            if (vm_load_state_json(g_vms[i].vhdx_path))
                g_vms[i].install_complete = TRUE;
            wcscpy_s(snap_dir, MAX_PATH, g_vms[i].vhdx_path);
            last_slash = wcsrchr(snap_dir, L'\\');
            if (last_slash) *last_slash = L'\0';
            {
                size_t dlen = wcslen(snap_dir);
                if (dlen >= 10 && _wcsicmp(snap_dir + dlen - 10, L"\\snapshots") == 0) {
                    /* Already points to snapshots dir */
                } else {
                    wcscat_s(snap_dir, MAX_PATH, L"\\snapshots");
                }
            }
            snapshot_init(&g_snap_trees[i], snap_dir);
        }
    }
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
        if (g_template_count >= ASB_MAX_TEMPLATES) break;

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

/* ---- Utility: recursive directory delete ---- */

static void remove_dir_recursive(const wchar_t *dir)
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
                remove_dir_recursive(full);
            else
                DeleteFileW(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* Delete all files in a directory (non-recursive, skips subdirectories) */
static void delete_files_in_dir(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH];
    HANDLE hFind;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    hFind = FindFirstFileW(pat, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t fp[MAX_PATH];
                swprintf_s(fp, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
                DeleteFileW(fp);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

/* ---- HCS state callback (called from HCS worker thread) ---- */

static void asb_hcs_state_changed(VmInstance *instance, DWORD event)
{
    int i;

    if (!instance) return;

    for (i = 0; i < g_vm_count; i++) {
        if (&g_vms[i] != instance) continue;

        if (event == 0x00000001) {
            /* VM exited */
            if (!instance->running) break;
            instance->running = FALSE;
            instance->shutdown_requested = FALSE;
            instance->hyperv_video_off = FALSE;
            hcs_stop_monitor(instance);
            vm_ssh_proxy_stop(instance);
            vm_agent_stop(instance);
            idd_probe_stop(instance);
            asb_log(L"VM \"%s\" exited (event=0x%08X).", instance->name, event);

            if (instance->network_mode != NET_NONE && !instance->network_cleaned) {
                hcn_delete_endpoint(&instance->endpoint_id);
                hcn_delete_network(&instance->network_id);
                instance->network_cleaned = TRUE;
            }
            instance->nat_ip[0] = '\0';
            hcs_close_vm(instance);

            /* Template finalization */
            if (instance->is_template) {
                wchar_t tpl_dir[MAX_PATH], tmp[MAX_PATH], jp[MAX_PATH];
                wchar_t *sl;
                int j;
                wcscpy_s(tpl_dir, MAX_PATH, instance->vhdx_path);
                sl = wcsrchr(tpl_dir, L'\\'); if (sl) *sl = L'\0';
                swprintf_s(tmp, MAX_PATH, L"%s\\vm.vmgs", tpl_dir); DeleteFileW(tmp);
                swprintf_s(tmp, MAX_PATH, L"%s\\vm.vmrs", tpl_dir); DeleteFileW(tmp);
                swprintf_s(tmp, MAX_PATH, L"%s\\resources.iso", tpl_dir); DeleteFileW(tmp);
                swprintf_s(jp, MAX_PATH, L"%s\\%s.json", tpl_dir, instance->name);
                { FILE *jf; if (_wfopen_s(&jf, jp, L"w,ccs=UTF-8") == 0 && jf) {
                    fwprintf(jf, L"{\n  \"os_type\": \"%s\",\n  \"image_path\": \"%s\"\n}\n",
                             instance->os_type, instance->image_path);
                    fclose(jf);
                }}
                asb_log(L"Template \"%s\" created successfully.", instance->name);

                if (g_removed_cb) g_removed_cb(i, g_removed_ud);

                EnterCriticalSection(&g_cs);
                for (j = i; j < g_vm_count - 1; j++) {
                    g_vms[j] = g_vms[j+1];
                    g_snap_trees[j] = g_snap_trees[j+1];
                }
                ZeroMemory(&g_vms[g_vm_count-1], sizeof(VmInstance));
                g_vm_count--;
                LeaveCriticalSection(&g_cs);

                scan_templates();
            }

            save_vm_list();
        } else if (event == 0x01000000 || event == 0x02000000) {
            instance->callbacks_dead = TRUE;
            asb_log(L"VM \"%s\": HCS ServiceDisconnect.", instance->name);
        }
        break;
    }

    /* Notify consumer */
    if (g_state_cb && instance)
        g_state_cb(vm_handle(instance), instance->running, g_state_ud);
}

/* ---- NAT IP allocation ---- */

/* Allocate the next free IP in the 172.20.0.0/16 range.
   Scans g_vms[] for IPs already in use.  Returns FALSE if pool exhausted. */
static BOOL allocate_nat_ip(VmInstance *vm)
{
    BOOL used[256] = { 0 };  /* Track .0.2 through .0.254 */
    int i, octet;

    for (i = 0; i < g_vm_count; i++) {
        if (&g_vms[i] == vm) continue;
        if (g_vms[i].nat_ip[0] == '\0') continue;
        /* Parse last octet from "172.20.0.X" */
        {
            int a, b, c, d;
            if (sscanf_s(g_vms[i].nat_ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4 &&
                a == 172 && b == 20 && c == 0 && d >= 2 && d <= 254)
                used[d] = TRUE;
        }
    }

    for (octet = 2; octet <= 254; octet++) {
        if (!used[octet]) {
            sprintf_s(vm->nat_ip, sizeof(vm->nat_ip), "172.20.0.%d", octet);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- Background VM start thread ---- */

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

    /* Allocate NAT IP before endpoint creation (only for NAT mode) */
    if (args->network_mode == NET_NAT) {
        if (allocate_nat_ip(vm)) {
            asb_log(L"Allocated NAT IP %S for \"%s\".", vm->nat_ip, vm->name);
            save_vm_list();
        } else {
            asb_log(L"Warning: NAT IP pool exhausted.");
        }
    } else {
        vm->nat_ip[0] = '\0';
    }

    if (args->network_mode != NET_NONE) {
        switch (args->network_mode) {
        case NET_NAT:      hr = hcn_create_nat_network(&args->network_id); break;
        case NET_INTERNAL: hr = hcn_create_internal_network(&args->network_id); break;
        case NET_EXTERNAL: hr = hcn_create_external_network(&args->network_id, args->vm->net_adapter); break;
        default:           hr = E_INVALIDARG; break;
        }
        if (SUCCEEDED(hr)) {
            hr = hcn_create_endpoint(&args->network_id, &args->endpoint_id, endpoint_guid_str, 64,
                                     vm->nat_ip[0] ? vm->nat_ip : NULL);
            if (FAILED(hr)) {
                asb_log(L"Error: Network endpoint failed (0x%08X).", hr);
                if (g_state_cb) g_state_cb(vm_handle(vm), FALSE, g_state_ud);
                free(args); return 1;
            }
            vm->network_id = args->network_id;
            vm->endpoint_id = args->endpoint_id;
        } else {
            asb_log(L"Error: Network unavailable (0x%08X).", hr);
            if (g_state_cb) g_state_cb(vm_handle(vm), FALSE, g_state_ud);
            free(args); return 1;
        }
    }

    if (args->config.gpu_mode == GPU_DEFAULT)
        gpu_get_driver_shares(&g_gpu_list, &args->config.gpu_shares);

    asb_log(L"Re-creating HCS compute system for \"%s\"...", vm->name);
    hr = (endpoint_guid_str[0] != L'\0')
        ? hcs_create_vm_with_endpoint(&args->config, endpoint_guid_str, vm)
        : hcs_create_vm(&args->config, vm);
    if (FAILED(hr)) {
        asb_log(L"Error: Failed to create compute system (0x%08X)", hr);
        asb_alert(L"Failed to start VM, check its configuration.");
        if (g_state_cb) g_state_cb(vm_handle(vm), FALSE, g_state_ud);
        free(args); return 1;
    }

    asb_log(L"Starting VM \"%s\"...", vm->name);
    hr = hcs_start_vm(vm);
    if (FAILED(hr)) {
        asb_log(L"Error: Failed to start VM (0x%08X)", hr);
        if (hr == (HRESULT)0x800705AF)
            asb_alert(L"The host doesn't have enough resources to start this VM.");
    } else {
        asb_log(L"VM \"%s\" started.", vm->name);
        vm_agent_start(vm);
        idd_probe_start(vm);
        hcs_start_monitor(vm);
    }

    if (g_state_cb) g_state_cb(vm_handle(vm), vm->running, g_state_ud);
    free(args);
    return 0;
}

/* ---- Background VHDX creation thread ---- */

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
    VmInstance *vm_inst;
    wchar_t  language[32];
    BOOL     vhdx_created;
} VhdxCreateArgs;

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
        if (!generate_unattend_vhdx_template(file_path, args->config.name, args->config.test_mode)) {
            args->result = E_FAIL;
            wcscpy_s(args->error_msg, 512, L"Failed to generate template unattend.xml");
            goto done;
        }
    } else {
        if (!generate_unattend_vhdx(file_path, args->config.name, args->config.admin_user,
                                     args->config.admin_pass, args->config.test_mode, L"en-US")) {
            args->result = E_FAIL;
            wcscpy_s(args->error_msg, 512, L"Failed to generate unattend.xml");
            goto done;
        }
    }

    /* Generate setup.cmd */
    swprintf_s(file_path, MAX_PATH, L"%s\\setup.cmd", staging);
    generate_vhdx_setup_cmd(file_path);

    /* Generate SetupComplete.cmd */
    swprintf_s(file_path, MAX_PATH, L"%s\\SetupComplete.cmd", staging);
    generate_vhdx_setupcomplete(file_path, args->config.ssh_enabled);

    /* Determine res_dir */
    GetModuleFileNameW(g_dll_module, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\'); if (slash) *slash = L'\0';
    swprintf_s(res_dir, MAX_PATH, L"%s\\resources", exe_dir);
    if (GetFileAttributesW(res_dir) == INVALID_FILE_ATTRIBUTES)
        wcscpy_s(res_dir, MAX_PATH, exe_dir);

    /* Generate manifest */
    swprintf_s(manifest, MAX_PATH, L"%s\\manifest.txt", staging);
    manifest_count = generate_vhdx_manifest(manifest, staging, res_dir, &args->config.gpu_shares, args->config.ssh_enabled);
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

    /* Create pipe */
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
    si.hStdError = hWritePipe;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        args->result = HRESULT_FROM_WIN32(GetLastError());
        swprintf_s(args->error_msg, 512, L"Failed to launch iso-patch.exe (error %lu)", GetLastError());
        goto done;
    }

    CloseHandle(hWritePipe);
    hWritePipe = INVALID_HANDLE_VALUE;

    /* Read pipe output line by line */
    args->result = E_FAIL;
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
                            /* Informational */
                        } else if (strncmp(line, "PROGRESS:", 9) == 0) {
                            int pct = atoi(line + 9);
                            BOOL is_staging = (strstr(line + 9, "Staging") != NULL);
                            if (args->vm_index >= 0 && args->vm_index < g_vm_count) {
                                g_vms[args->vm_index].vhdx_progress = pct;
                                g_vms[args->vm_index].vhdx_staging = is_staging;
                            }
                            if (g_progress_cb && args->vm_index >= 0 && args->vm_index < g_vm_count)
                                g_progress_cb(vm_handle(&g_vms[args->vm_index]), pct, is_staging, g_progress_ud);
                        } else if (strncmp(line, "ERROR:", 6) == 0) {
                            if (args->error_msg[0] == L'\0')
                                MultiByteToWideChar(CP_ACP, 0, line + 6, -1, args->error_msg, 512);
                            args->result = E_FAIL;
                        } else if (strncmp(line, "LANG:", 5) == 0) {
                            MultiByteToWideChar(CP_ACP, 0, line + 5, -1, args->language, 32);
                            asb_log(L"Detected ISO language: %s", args->language);
                            if (!args->config.is_template) {
                                wchar_t unattend_path[MAX_PATH];
                                wchar_t stg[MAX_PATH];
                                swprintf_s(stg, MAX_PATH, L"%s\\_vhdx_staging", args->vhdx_dir);
                                swprintf_s(unattend_path, MAX_PATH, L"%s\\unattend.xml", stg);
                                generate_unattend_vhdx(unattend_path, args->config.name,
                                                        args->config.admin_user, args->config.admin_pass,
                                                        args->config.test_mode, args->language);
                            }
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

    SecureZeroMemory(args->config.admin_pass, sizeof(args->config.admin_pass));

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

    args->vhdx_created = TRUE;

    /* Allocate NAT IP before endpoint creation.
       args->vm_inst isn't set yet (HCS VM not created), so allocate
       into the g_vms[] entry directly via vm_index. */
    if (args->config.network_mode == NET_NAT && args->vm_index >= 0 && args->vm_index < g_vm_count) {
        if (allocate_nat_ip(&g_vms[args->vm_index])) {
            asb_log(L"Allocated NAT IP %S for new VM.", g_vms[args->vm_index].nat_ip);
            save_vm_list();
        }
    }

    /* Network */
    if (args->config.network_mode != NET_NONE) {
        char *nat_ip = (args->vm_index >= 0 && args->vm_index < g_vm_count)
                        ? g_vms[args->vm_index].nat_ip : NULL;
        switch (args->config.network_mode) {
        case NET_NAT:      hr = hcn_create_nat_network(&args->network_id); break;
        case NET_INTERNAL: hr = hcn_create_internal_network(&args->network_id); break;
        case NET_EXTERNAL: hr = hcn_create_external_network(&args->network_id, args->net_adapter); break;
        default:           hr = E_INVALIDARG; break;
        }
        if (SUCCEEDED(hr)) {
            hr = hcn_create_endpoint(&args->network_id, &args->endpoint_id, args->endpoint_guid, 64,
                                     (nat_ip && nat_ip[0]) ? nat_ip : NULL);
            if (SUCCEEDED(hr))
                args->has_network = TRUE;
            else
                hcn_delete_network(&args->network_id);
        }
        if (FAILED(hr))
            args->config.network_mode = NET_NONE;
    }

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

        hr = hcs_start_vm(&temp_inst);
        if (FAILED(hr)) {
            args->result = hr;
            swprintf_s(args->error_msg, 512, L"Failed to start VM (0x%08X)", hr);
            hcs_close_vm(&temp_inst);
            goto done;
        }

        /* Allocate heap copy to pass to completion */
        {
            VmInstance *heap_inst = (VmInstance *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmInstance));
            if (heap_inst) memcpy(heap_inst, &temp_inst, sizeof(VmInstance));
            args->vm_inst = heap_inst;
        }

        args->result = S_OK;

        if (args->language[0] != L'\0')
            vm_save_language_json(args->config.vhdx_path, args->language);
    }

done:
    /* Clean up staging dir */
    {
        wchar_t staging_dir[MAX_PATH];
        swprintf_s(staging_dir, MAX_PATH, L"%s\\_vhdx_staging", args->vhdx_dir);
        remove_dir_recursive(staging_dir);
    }

    if (hReadPipe != INVALID_HANDLE_VALUE) CloseHandle(hReadPipe);
    if (hWritePipe != INVALID_HANDLE_VALUE) CloseHandle(hWritePipe);

    /* ---- Completion: update globals ---- */
    {
        int idx = args->vm_index;
        if (idx >= 0 && idx < g_vm_count) {
            VmInstance *inst = &g_vms[idx];

            EnterCriticalSection(&g_cs);
            inst->building_vhdx = FALSE;

            if (SUCCEEDED(args->result)) {
                VmInstance *heap_inst = args->vm_inst;
                if (heap_inst) {
                    hcs_unregister_vm_callback(heap_inst);
                    inst->handle = heap_inst->handle;
                    inst->runtime_id = heap_inst->runtime_id;
                    inst->running = TRUE;
                    inst->network_mode = heap_inst->network_mode;
                    inst->network_id = args->network_id;
                    inst->endpoint_id = args->endpoint_id;
                    HeapFree(GetProcessHeap(), 0, heap_inst);
                    hcs_register_vm_callback(inst);
                }
                LeaveCriticalSection(&g_cs);

                hcs_start_monitor(inst);
                if (inst->is_template) {
                    asb_log(L"Template \"%s\" building (sysprep will shut down when ready).", inst->name);
                } else {
                    vm_agent_start(inst);
                    idd_probe_start(inst);
                    asb_log(L"VM \"%s\" created and started.", inst->name);
                }
                save_vm_list();
            } else if (args->vhdx_created) {
                LeaveCriticalSection(&g_cs);
                asb_log(L"VM \"%s\" created but failed to start: %s", inst->name, args->error_msg);
                asb_log(L"You can adjust settings and start it manually.");
                if (args->result == (HRESULT)0x800705AF)
                    asb_alert(L"The host doesn't have enough resources to start this VM.");
                else
                    asb_alert(L"Failed to start VM, check its configuration.");
                save_vm_list();
            } else {
                /* VHDX never created — remove VM from list and clean up files */
                int j;
                asb_log(L"Error creating VM \"%s\": %s", inst->name, args->error_msg);
                if (g_removed_cb) g_removed_cb(idx, g_removed_ud);
                for (j = idx; j < g_vm_count - 1; j++) {
                    g_vms[j] = g_vms[j + 1];
                    g_snap_trees[j] = g_snap_trees[j + 1];
                }
                ZeroMemory(&g_vms[g_vm_count - 1], sizeof(VmInstance));
                g_vm_count--;
                LeaveCriticalSection(&g_cs);
                /* Delete leftover directory so a retry doesn't hit FILE_EXISTS */
                remove_dir_recursive(args->vhdx_dir);
                save_vm_list();
            }

            if (g_state_cb)
                g_state_cb(vm_handle(inst), inst->running, g_state_ud);
        }
    }

    HeapFree(GetProcessHeap(), 0, args);
    return 0;
}

/* ================================================================
 * Public API Implementation
 * ================================================================ */

/* ---- Callback setters ---- */

ASB_API void asb_set_log_callback(AsbLogCallback cb, void *user_data)
{
    g_log_cb = cb;
    g_log_ud = user_data;
}

ASB_API void asb_set_state_callback(AsbStateCallback cb, void *user_data)
{
    g_state_cb = cb;
    g_state_ud = user_data;
}

ASB_API void asb_set_progress_callback(AsbProgressCallback cb, void *user_data)
{
    g_progress_cb = cb;
    g_progress_ud = user_data;
}

ASB_API void asb_set_alert_callback(AsbAlertCallback cb, void *user_data)
{
    g_alert_cb = cb;
    g_alert_ud = user_data;
}

ASB_API void asb_set_vm_removed_callback(AsbVmRemovedCallback cb, void *user_data)
{
    g_removed_cb = cb;
    g_removed_ud = user_data;
}

/* ---- Init / Cleanup ---- */

ASB_API HRESULT asb_init(void)
{
    if (g_initialized) return S_OK;
    InitializeCriticalSection(&g_cs);

    if (!hcs_init())
        asb_log(L"HCS: NOT available (is Hyper-V enabled?)");
    else
        asb_log(L"HCS: Available");

    vmms_cert_ensure();

    if (!hcn_init())
        asb_log(L"HCN: NOT available");
    else
        asb_log(L"HCN: Available");

    gpu_enumerate(&g_gpu_list);
    {
        int gi;
        asb_log(L"GPUs: %d found, %d driver shares", g_gpu_list.count, g_gpu_list.shares.count);
        for (gi = 0; gi < g_gpu_list.count; gi++)
            asb_log(L"  [%d] %s (GPU-PV)", gi, g_gpu_list.gpus[gi].name);
    }

    hcs_set_state_callback(asb_hcs_state_changed);

    load_vm_list();
    scan_templates();

    if (g_vm_count > 0) asb_log(L"Loaded %d VM(s) from config.", g_vm_count);
    if (g_template_count > 0) asb_log(L"Found %d template(s).", g_template_count);

    g_initialized = TRUE;
    return S_OK;
}

ASB_API void asb_cleanup(void)
{
    int i;
    if (!g_initialized) return;

    for (i = 0; i < g_vm_count; i++) {
        hcs_stop_monitor(&g_vms[i]);
        vm_ssh_proxy_stop(&g_vms[i]);
        vm_agent_stop(&g_vms[i]);
        idd_probe_stop(&g_vms[i]);
        if (g_vms[i].running) hcs_terminate_vm(&g_vms[i]);
        hcs_close_vm(&g_vms[i]);
    }

    hcs_cleanup();
    hcn_cleanup();
    DeleteCriticalSection(&g_cs);
    g_initialized = FALSE;
}

ASB_API void asb_detach(void)
{
    int i;
    if (!g_initialized) return;

    /* Cleanly release HCS resources WITHOUT terminating running VMs.
       Used by short-lived consumers that start a VM
       and exit — the VM keeps running after the process is gone.

       Unlike asb_cleanup(), this does NOT call hcs_terminate_vm().
       We unregister callbacks and stop threads, but do NOT close the HCS
       handle — HcsCloseComputeSystem terminates the VM when it's the last
       handle.  Instead, let the OS close it during process teardown after
       the callback is already gone. */
    for (i = 0; i < g_vm_count; i++) {
        hcs_stop_monitor(&g_vms[i]);
        vm_ssh_proxy_stop(&g_vms[i]);
        vm_agent_stop(&g_vms[i]);
        idd_probe_stop(&g_vms[i]);
        hcs_unregister_vm_callback(&g_vms[i]);
        g_vms[i].handle = NULL;  /* abandon handle — OS will close it */
    }

    g_initialized = FALSE;
}

/* ---- VM Create ---- */

ASB_API HRESULT asb_vm_create(const AsbVmConfig *config)
{
    VmConfig cfg;
    VmInstance *inst;
    wchar_t vhdx_dir[MAX_PATH];
    wchar_t endpoint_guid_str[64] = { 0 };
    HRESULT hr;
    int existing_idx;
    BOOL is_template_create;
    int template_idx = -1;
    BOOL from_template = FALSE;

    if (!config || !config->name || config->name[0] == L'\0') {
        asb_log(L"Error: VM name is required.");
        return E_INVALIDARG;
    }

    ZeroMemory(&cfg, sizeof(cfg));

    /* Copy config strings into local VmConfig */
    wcscpy_s(cfg.name, 256, config->name);
    if (config->os_type) wcscpy_s(cfg.os_type, 32, config->os_type);
    if (config->image_path) wcscpy_s(cfg.image_path, MAX_PATH, config->image_path);
    if (config->username) wcscpy_s(cfg.admin_user, 128, config->username);
    if (config->password) wcscpy_s(cfg.admin_pass, 128, config->password);
    cfg.ram_mb = config->ram_mb;
    cfg.hdd_gb = config->hdd_gb;
    cfg.cpu_cores = config->cpu_cores;
    cfg.gpu_mode = config->gpu_mode;
    cfg.network_mode = config->network_mode;
    cfg.test_mode = config->test_mode;
    cfg.ssh_enabled = config->ssh_enabled;
    is_template_create = config->is_template;
    cfg.is_template = is_template_create;

    /* Defaults */
    if (cfg.hdd_gb == 0) cfg.hdd_gb = 64;
    if (cfg.ram_mb == 0) cfg.ram_mb = 4096;
    if (cfg.cpu_cores == 0) cfg.cpu_cores = 4;

    /* Resolve template */
    if (config->template_name && config->template_name[0] != L'\0') {
        int i;
        for (i = 0; i < g_template_count; i++) {
            if (_wcsicmp(g_templates[i].name, config->template_name) == 0) {
                template_idx = i;
                from_template = TRUE;
                wcscpy_s(cfg.os_type, 32, g_templates[i].os_type);
                break;
            }
        }
    }

    if (is_template_create && from_template) {
        asb_log(L"Error: Cannot create a template from another template.");
        return E_INVALIDARG;
    }

    if (cfg.image_path[0] != L'\0')
        wcscpy_s(g_last_iso_path, MAX_PATH, cfg.image_path);

    /* Check duplicate name */
    existing_idx = -1;
    {
        int i;
        for (i = 0; i < g_vm_count; i++) {
            if (_wcsicmp(g_vms[i].name, cfg.name) == 0) { existing_idx = i; break; }
        }
    }
    if (existing_idx >= 0) {
        asb_log(L"Error: A VM named \"%s\" already exists.", cfg.name);
        return E_INVALIDARG;
    }

    /* Check duplicate template name */
    {
        int i;
        for (i = 0; i < g_template_count; i++) {
            if (_wcsicmp(g_templates[i].name, cfg.name) == 0) {
                asb_log(L"Error: A template named \"%s\" already exists.", cfg.name);
                return E_INVALIDARG;
            }
        }
    }

    if (g_vm_count >= ASB_MAX_VMS) {
        asb_log(L"Maximum VM count reached (%d)", ASB_MAX_VMS);
        return E_OUTOFMEMORY;
    }

    inst = &g_vms[g_vm_count];
    ZeroMemory(inst, sizeof(VmInstance));

    /* Net adapter */
    if (config->net_adapter && config->net_adapter[0] != L'\0' &&
        wcscmp(config->net_adapter, L"(Auto)") != 0)
        wcscpy_s(inst->net_adapter, 256, config->net_adapter);

    /* Template flag */
    if (is_template_create) {
        cfg.gpu_mode = GPU_NONE;
        cfg.network_mode = NET_NONE;
    }

    if (cfg.admin_user[0] == L'\0')
        wcscpy_s(cfg.admin_user, 128, L"User");

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
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\templates\\%s", base_dir, cfg.name);
        } else {
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\%s", base_dir, cfg.name);
        }
    }
    CreateDirectoryW(vhdx_dir, NULL);
    swprintf_s(cfg.vhdx_path, MAX_PATH, L"%s\\disk.vhdx", vhdx_dir);

    /* GPU driver shares */
    if (cfg.gpu_mode == GPU_DEFAULT && !is_template_create)
        gpu_get_driver_shares(&g_gpu_list, &cfg.gpu_shares);

    /* ---- VHDX-first path (Windows, from ISO) ---- */
    {
        BOOL use_vhdx_first = (!from_template &&
                                _wcsicmp(cfg.os_type, L"Windows") == 0 &&
                                cfg.image_path[0] != L'\0' &&
                                (is_template_create || cfg.admin_pass[0] != L'\0'));

        if (use_vhdx_first) {
            VhdxCreateArgs *args;

            wcscpy_s(inst->name, 256, cfg.name);
            wcscpy_s(inst->os_type, 32, cfg.os_type);
            wcscpy_s(inst->vhdx_path, MAX_PATH, cfg.vhdx_path);
            wcscpy_s(inst->image_path, MAX_PATH, cfg.image_path);
            inst->ram_mb = cfg.ram_mb;
            inst->hdd_gb = cfg.hdd_gb;
            inst->cpu_cores = cfg.cpu_cores;
            inst->gpu_mode = cfg.gpu_mode;
            wcscpy_s(inst->gpu_name, 256, (cfg.gpu_mode == GPU_DEFAULT) ? L"Default GPU" : L"None");
            inst->network_mode = cfg.network_mode;
            inst->is_template = is_template_create;
            inst->test_mode = cfg.test_mode;
            wcscpy_s(inst->admin_user, 128, cfg.admin_user);
            inst->ssh_enabled = cfg.ssh_enabled;
            inst->building_vhdx = TRUE;
            inst->vhdx_progress = 0;
            memcpy(&inst->gpu_shares, &cfg.gpu_shares, sizeof(GpuDriverShareList));

            { wchar_t sd[MAX_PATH]; swprintf_s(sd, MAX_PATH, L"%s\\snapshots", vhdx_dir);
              snapshot_init(&g_snap_trees[g_vm_count], sd); }
            g_vm_count++;

            if (!is_template_create)
                vm_save_state_json(cfg.vhdx_path, FALSE);

            save_vm_list();

            args = (VhdxCreateArgs *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VhdxCreateArgs));
            if (!args) {
                asb_log(L"Error: Out of memory for VHDX create args.");
                g_vm_count--;
                return E_OUTOFMEMORY;
            }
            memcpy(&args->config, &cfg, sizeof(VmConfig));
            args->vm_index = g_vm_count - 1;
            wcscpy_s(args->vhdx_dir, MAX_PATH, vhdx_dir);
            wcscpy_s(args->net_adapter, 256, inst->net_adapter);

            asb_log(L"Building VHDX for \"%s\" (this may take several minutes)...", cfg.name);
            CloseHandle(CreateThread(NULL, 0, vhdx_create_thread, args, 0, NULL));

            if (g_state_cb) g_state_cb(vm_handle(inst), FALSE, g_state_ud);
            return S_OK;
        }
    }

    /* ---- Synchronous path (from-template, non-Windows, etc.) ---- */

    /* Create disk */
    if (from_template) {
        asb_log(L"Creating differencing VHDX from template \"%s\"...", g_templates[template_idx].name);
        DeleteFileW(cfg.vhdx_path);
        hr = vhdx_create_differencing(cfg.vhdx_path, g_templates[template_idx].vhdx_path);
        if (FAILED(hr)) { asb_log(L"Error: Failed to create differencing VHDX (0x%08X)", hr); return hr; }
        asb_log(L"Differencing VHDX created.");
    } else {
        asb_log(L"Creating VHDX: %s (%lu GB)...", cfg.vhdx_path, cfg.hdd_gb);
        hr = vhdx_create(cfg.vhdx_path, (ULONGLONG)cfg.hdd_gb);
        if (FAILED(hr)) { asb_log(L"Error: Failed to create VHDX (0x%08X)", hr); return hr; }
        asb_log(L"VHDX created successfully.");
    }

    /* Resources ISO */
    {
        wchar_t res_iso[MAX_PATH], exe_dir[MAX_PATH], res_dir_buf[MAX_PATH];
        wchar_t *sl;
        swprintf_s(res_iso, MAX_PATH, L"%s\\resources.iso", vhdx_dir);
        GetModuleFileNameW(g_dll_module, exe_dir, MAX_PATH);
        sl = wcsrchr(exe_dir, L'\\'); if (sl) *sl = L'\0';
        swprintf_s(res_dir_buf, MAX_PATH, L"%s\\resources", exe_dir);
        if (GetFileAttributesW(res_dir_buf) == INVALID_FILE_ATTRIBUTES)
            wcscpy_s(res_dir_buf, MAX_PATH, exe_dir);

        if (is_template_create) {
            if (_wcsicmp(cfg.os_type, L"Windows") == 0 && cfg.image_path[0] != L'\0') {
                hr = iso_create_resources(res_iso, cfg.name, cfg.admin_user, cfg.admin_pass,
                                           res_dir_buf, TRUE, cfg.test_mode, cfg.ssh_enabled, L"en-US");
                if (SUCCEEDED(hr)) wcscpy_s(cfg.resources_iso_path, MAX_PATH, res_iso);
                else asb_log(L"Warning: Failed to create template resources ISO (0x%08X).", hr);
            }
        } else if (from_template) {
            if (_wcsicmp(cfg.os_type, L"Windows") == 0 && cfg.admin_pass[0] != L'\0') {
                wchar_t template_lang[32] = L"en-US";
                vm_load_language_json(g_templates[template_idx].vhdx_path, template_lang, 32);
                hr = iso_create_instance_resources(res_iso, cfg.name, cfg.admin_user, cfg.admin_pass,
                                                    res_dir_buf, cfg.ssh_enabled, template_lang);
                if (SUCCEEDED(hr)) {
                    wcscpy_s(cfg.resources_iso_path, MAX_PATH, res_iso);
                    vm_save_language_json(cfg.vhdx_path, template_lang);
                }
                else asb_log(L"Warning: Failed to create instance resources ISO (0x%08X).", hr);
            }
        } else {
            if (_wcsicmp(cfg.os_type, L"Windows") == 0 && cfg.image_path[0] != L'\0' &&
                cfg.admin_pass[0] != L'\0') {
                hr = iso_create_resources(res_iso, cfg.name, cfg.admin_user, cfg.admin_pass,
                                           res_dir_buf, FALSE, cfg.test_mode, cfg.ssh_enabled, L"en-US");
                if (SUCCEEDED(hr)) wcscpy_s(cfg.resources_iso_path, MAX_PATH, res_iso);
                else asb_log(L"Warning: Failed to create resources ISO (0x%08X).", hr);
            }
        }
    }
    SecureZeroMemory(cfg.admin_pass, sizeof(cfg.admin_pass));

    inst->network_cleaned = FALSE;

    /* Allocate NAT IP before endpoint creation */
    if (cfg.network_mode == NET_NAT) {
        if (allocate_nat_ip(inst)) {
            asb_log(L"Allocated NAT IP %S for \"%s\".", inst->nat_ip, inst->name);
            save_vm_list();
        }
    } else {
        inst->nat_ip[0] = '\0';
    }

    /* Networking */
    if (cfg.network_mode != NET_NONE) {
        switch (cfg.network_mode) {
        case NET_NAT:      hr = hcn_create_nat_network(&inst->network_id); break;
        case NET_INTERNAL: hr = hcn_create_internal_network(&inst->network_id); break;
        case NET_EXTERNAL: hr = hcn_create_external_network(&inst->network_id, inst->net_adapter); break;
        default:           hr = E_INVALIDARG; break;
        }
        if (FAILED(hr)) {
            asb_log(L"Warning: Network failed (0x%08X). Continuing without.", hr);
            cfg.network_mode = NET_NONE;
        } else {
            hr = hcn_create_endpoint(&inst->network_id, &inst->endpoint_id, endpoint_guid_str, 64,
                                     inst->nat_ip[0] ? inst->nat_ip : NULL);
            if (FAILED(hr)) {
                asb_log(L"Warning: Endpoint failed (0x%08X).", hr);
                hcn_delete_network(&inst->network_id);
                cfg.network_mode = NET_NONE;
            }
        }
    }

    /* Create HCS VM */
    asb_log(L"Creating %sVM \"%s\"...", is_template_create ? L"template " : L"", cfg.name);
    hr = (endpoint_guid_str[0] != L'\0')
        ? hcs_create_vm_with_endpoint(&cfg, endpoint_guid_str, inst)
        : hcs_create_vm(&cfg, inst);
    if (FAILED(hr)) {
        asb_log(L"Error: Failed to create compute system (0x%08X)", hr);
        asb_alert(L"Failed to start VM, check its configuration.");
        return hr;
    }

    wcscpy_s(inst->gpu_name, 256, (cfg.gpu_mode == GPU_DEFAULT) ? L"Default GPU" : L"None");
    wcscpy_s(inst->resources_iso_path, MAX_PATH, cfg.resources_iso_path);

    { wchar_t sd[MAX_PATH]; swprintf_s(sd, MAX_PATH, L"%s\\snapshots", vhdx_dir);
      snapshot_init(&g_snap_trees[g_vm_count], sd); }
    g_vm_count++;

    if (!is_template_create)
        vm_save_state_json(cfg.vhdx_path, FALSE);

    /* Auto-start */
    asb_log(L"Starting VM \"%s\"...", cfg.name);
    hr = hcs_start_vm(inst);
    if (SUCCEEDED(hr)) {
        hcs_start_monitor(inst);
        vm_agent_start(inst);
        idd_probe_start(inst);
        asb_log(L"VM \"%s\" %s.", cfg.name, is_template_create ? L"started (template)" : L"created and started");
    } else {
        asb_log(L"VM \"%s\" created but failed to start (0x%08X).", cfg.name, hr);
        if (hr == (HRESULT)0x800705AF)
            asb_alert(L"The host doesn't have enough resources to start this VM.");
        else
            asb_alert(L"Failed to start VM, check its configuration.");
    }

    save_vm_list();

    if (g_state_cb) g_state_cb(vm_handle(inst), inst->running, g_state_ud);
    return S_OK;
}

/* ---- VM Start ---- */

ASB_API HRESULT asb_vm_start(AsbVm vm, int snap_idx, int branch_idx,
                              const wchar_t *branch_name)
{
    VmInstance *inst;
    int idx;

    idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    inst = &g_vms[idx];

    if (inst->running) { asb_log(L"VM \"%s\" is already running.", inst->name); return S_FALSE; }

    /* Switch to snapshot/base branch before booting */
    if (snap_idx >= 0 || snap_idx == -2) {
        HRESULT hr;
        if (branch_idx >= 0) {
            hr = snapshot_select_branch(&g_snap_trees[idx], inst, snap_idx, branch_idx);
        } else {
            hr = snapshot_new_branch(&g_snap_trees[idx], inst, snap_idx);
            if (SUCCEEDED(hr) && branch_name && branch_name[0] != L'\0') {
                int new_bi;
                if (snap_idx == -2)
                    new_bi = g_snap_trees[idx].base_branch_count - 1;
                else
                    new_bi = g_snap_trees[idx].nodes[snap_idx].branch_count - 1;
                if (new_bi >= 0)
                    snapshot_rename(&g_snap_trees[idx], snap_idx, new_bi, branch_name);
            }
        }
        if (FAILED(hr)) {
            asb_log(L"Error: Failed to select branch (0x%08X)", hr);
            if (g_state_cb) g_state_cb(vm, FALSE, g_state_ud);
            return hr;
        }
        if (snap_idx == -2) asb_log(L"Switching to base branch...");
        else asb_log(L"Switching to \"%s\" branch...", g_snap_trees[idx].nodes[snap_idx].name);
        save_vm_list();
    }

    if (!inst->handle) {
        /* Need to re-create HCS system — do it in a background thread */
        StartVmArgs *args = (StartVmArgs *)calloc(1, sizeof(StartVmArgs));
        if (!args) return E_OUTOFMEMORY;
        args->vm = inst;
        wcscpy_s(args->config.name, 256, inst->name);
        wcscpy_s(args->config.os_type, 32, inst->os_type);
        wcscpy_s(args->config.image_path, MAX_PATH, inst->image_path);
        wcscpy_s(args->config.vhdx_path, MAX_PATH, inst->vhdx_path);
        args->config.ram_mb = inst->ram_mb;
        args->config.hdd_gb = inst->hdd_gb;
        args->config.cpu_cores = inst->cpu_cores;
        args->config.gpu_mode = inst->gpu_mode;
        args->config.network_mode = inst->network_mode;
        args->config.test_mode = inst->test_mode;
        wcscpy_s(args->config.admin_user, 128, inst->admin_user);
        args->config.ssh_enabled = inst->ssh_enabled;
        wcscpy_s(args->config.resources_iso_path, MAX_PATH, inst->resources_iso_path);
        args->network_mode = inst->network_mode;
        inst->network_cleaned = FALSE;
        asb_log(L"Starting VM \"%s\" (background)...", inst->name);
        CloseHandle(CreateThread(NULL, 0, start_vm_thread, args, 0, NULL));
    } else {
        HRESULT hr = hcs_start_vm(inst);
        if (hr == (HRESULT)0x80370110L && inst->handle) {
            hcs_terminate_vm(inst);
            hcs_close_vm(inst);
            return asb_vm_start(vm, -1, -1, NULL);
        }
        if (FAILED(hr)) {
            asb_log(L"Error: Failed to start VM (0x%08X)", hr);
            if (hr == (HRESULT)0x800705AF)
                asb_alert(L"The host doesn't have enough resources to start this VM.");
            return hr;
        }
        asb_log(L"VM \"%s\" started.", inst->name);
        vm_agent_start(inst);
        idd_probe_start(inst);
        hcs_start_monitor(inst);
        if (g_state_cb) g_state_cb(vm, TRUE, g_state_ud);
    }

    return S_OK;
}

/* ---- VM Shutdown (graceful) ---- */

ASB_API HRESULT asb_vm_shutdown(AsbVm vm)
{
    VmInstance *inst;
    HRESULT hr;
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    inst = &g_vms[idx];

    if (!inst->running) { asb_log(L"VM \"%s\" is not running.", inst->name); return S_FALSE; }

    asb_log(L"Sending shutdown signal to \"%s\"...", inst->name);
    hr = hcs_stop_vm(inst);
    if (FAILED(hr)) {
        asb_log(L"Shutdown failed (0x%08X). Use Force Stop.", hr);
        return hr;
    }

    inst->shutdown_requested = TRUE;
    inst->shutdown_time = GetTickCount64();
    asb_log(L"Shutdown signal sent to \"%s\".", inst->name);

    if (g_state_cb) g_state_cb(vm, TRUE, g_state_ud);
    return S_OK;
}

/* ---- VM Stop (force terminate) ---- */

ASB_API HRESULT asb_vm_stop(AsbVm vm)
{
    VmInstance *inst;
    HRESULT hr;
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    inst = &g_vms[idx];

    if (!inst->running) { asb_log(L"VM \"%s\" is not running.", inst->name); return S_FALSE; }

    inst->running = FALSE;
    inst->shutdown_requested = FALSE;
    inst->hyperv_video_off = FALSE;
    hcs_stop_monitor(inst);
    vm_ssh_proxy_stop(inst);
    vm_agent_stop(inst);
    idd_probe_stop(inst);

    asb_log(L"Force stopping VM \"%s\"...", inst->name);
    hr = hcs_terminate_vm(inst);
    if (FAILED(hr))
        asb_log(L"Error: Terminate failed (0x%08X)", hr);

    hcs_close_vm(inst);

    if (inst->network_mode != NET_NONE && !inst->network_cleaned) {
        hcn_delete_endpoint(&inst->endpoint_id);
        hcn_delete_network(&inst->network_id);
        inst->network_cleaned = TRUE;
    }
    inst->nat_ip[0] = '\0';

    asb_log(L"VM \"%s\" terminated.", inst->name);
    save_vm_list();

    if (g_state_cb) g_state_cb(vm, FALSE, g_state_ud);
    return S_OK;
}

/* ---- VM Delete ---- */

ASB_API HRESULT asb_vm_delete(AsbVm vm)
{
    int idx, i;
    wchar_t dir[MAX_PATH];
    wchar_t *last_slash;
    VmInstance *inst;

    idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    inst = &g_vms[idx];

    hcs_stop_monitor(inst);
    vm_ssh_proxy_stop(inst);
    vm_agent_stop(inst);
    idd_probe_stop(inst);

    if (inst->running)
        hcs_terminate_vm(inst);

    if (inst->network_mode != NET_NONE && !inst->network_cleaned) {
        hcn_delete_endpoint(&inst->endpoint_id);
        hcn_delete_network(&inst->network_id);
        inst->network_cleaned = TRUE;
    }

    hcs_close_vm(inst);
    hcs_destroy_stale(inst->name);

    /* Determine VM root directory */
    wcscpy_s(dir, MAX_PATH, inst->vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) *last_slash = L'\0';
    {
        size_t dlen = wcslen(dir);
        if (dlen >= 10 && _wcsicmp(dir + dlen - 10, L"\\snapshots") == 0)
            dir[dlen - 10] = L'\0';
    }

    /* Delete all files in snapshots/ */
    {
        wchar_t snap_dir[MAX_PATH];
        swprintf_s(snap_dir, MAX_PATH, L"%s\\snapshots", dir);
        delete_files_in_dir(snap_dir);
        RemoveDirectoryW(snap_dir);
    }

    /* Delete all files in VM root */
    delete_files_in_dir(dir);
    RemoveDirectoryW(dir);

    /* Compact arrays */
    EnterCriticalSection(&g_cs);
    for (i = idx; i < g_vm_count - 1; i++) {
        g_vms[i] = g_vms[i + 1];
        g_snap_trees[i] = g_snap_trees[i + 1];
    }
    ZeroMemory(&g_vms[g_vm_count - 1], sizeof(VmInstance));
    g_vm_count--;
    LeaveCriticalSection(&g_cs);

    save_vm_list();
    return S_OK;
}

/* ---- VM Queries ---- */

ASB_API int asb_vm_count(void) { return g_vm_count; }

ASB_API AsbVm asb_vm_get(int index)
{
    if (index < 0 || index >= g_vm_count) return NULL;
    return vm_handle(&g_vms[index]);
}

ASB_API AsbVm asb_vm_find(const wchar_t *name)
{
    int i;
    if (!name) return NULL;
    for (i = 0; i < g_vm_count; i++) {
        if (_wcsicmp(g_vms[i].name, name) == 0)
            return vm_handle(&g_vms[i]);
    }
    return NULL;
}

ASB_API const wchar_t *asb_vm_name(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->name : L"";
}

ASB_API const wchar_t *asb_vm_os_type(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->os_type : L"";
}

ASB_API BOOL asb_vm_is_running(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->running : FALSE;
}

ASB_API BOOL asb_vm_agent_online(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->agent_online : FALSE;
}

ASB_API BOOL asb_vm_is_building(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->building_vhdx : FALSE;
}

ASB_API DWORD asb_vm_ram_mb(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->ram_mb : 0;
}

ASB_API DWORD asb_vm_hdd_gb(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->hdd_gb : 0;
}

ASB_API DWORD asb_vm_cpu_cores(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->cpu_cores : 0;
}

ASB_API int asb_vm_gpu_mode(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->gpu_mode : 0;
}

ASB_API int asb_vm_network_mode(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->network_mode : 0;
}

ASB_API BOOL asb_vm_ssh_enabled(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->ssh_enabled : FALSE;
}

ASB_API DWORD asb_vm_ssh_port(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    return inst ? inst->ssh_port : 0;
}

/* ---- VM Config Editing ---- */

ASB_API HRESULT asb_vm_set_name(AsbVm vm, const wchar_t *name)
{
    VmInstance *inst;
    int idx = vm_index_of(vm);
    int i;
    if (idx < 0) return E_INVALIDARG;
    inst = &g_vms[idx];
    if (inst->running) return E_ACCESSDENIED;
    if (!name || name[0] == L'\0') return E_INVALIDARG;

    for (i = 0; i < g_vm_count; i++) {
        if (i != idx && _wcsicmp(g_vms[i].name, name) == 0) {
            asb_log(L"Name \"%s\" is already in use.", name);
            return E_INVALIDARG;
        }
    }
    wcscpy_s(inst->name, 256, name);
    save_vm_list();
    if (g_state_cb) g_state_cb(vm, inst->running, g_state_ud);
    return S_OK;
}

ASB_API HRESULT asb_vm_set_ram(AsbVm vm, DWORD ram_mb)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    if (g_vms[idx].running) return E_ACCESSDENIED;
    if (ram_mb < 4000) ram_mb = 4000;
    g_vms[idx].ram_mb = ram_mb;
    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return S_OK;
}

ASB_API HRESULT asb_vm_set_cpu(AsbVm vm, DWORD cores)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    if (g_vms[idx].running) return E_ACCESSDENIED;
    if (cores < 1) cores = 1;
    g_vms[idx].cpu_cores = cores;
    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return S_OK;
}

ASB_API HRESULT asb_vm_set_gpu(AsbVm vm, int gpu_mode)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    if (g_vms[idx].running) return E_ACCESSDENIED;
    g_vms[idx].gpu_mode = gpu_mode;
    wcscpy_s(g_vms[idx].gpu_name, 256, (gpu_mode == GPU_DEFAULT) ? L"Default GPU" : L"None");
    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return S_OK;
}

ASB_API HRESULT asb_vm_set_network(AsbVm vm, int mode)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    if (g_vms[idx].running) return E_ACCESSDENIED;
    if (mode < 0 || mode > 3) return E_INVALIDARG;
    g_vms[idx].network_mode = mode;
    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return S_OK;
}

/* ---- Snapshots ---- */

ASB_API HRESULT asb_snap_take(AsbVm vm, const wchar_t *name)
{
    HRESULT hr;
    int idx = vm_index_of(vm);
    wchar_t snap_name[128];
    if (idx < 0) return E_INVALIDARG;
    if (g_vms[idx].running) { asb_log(L"VM must be stopped to take a snapshot."); return E_ACCESSDENIED; }

    if (name && name[0] != L'\0')
        wcscpy_s(snap_name, 128, name);
    else
        swprintf_s(snap_name, 128, L"Snapshot %d", g_snap_trees[idx].count + 1);

    asb_log(L"Taking snapshot \"%s\" of VM \"%s\"...", snap_name, g_vms[idx].name);
    hr = snapshot_take(&g_snap_trees[idx], &g_vms[idx], snap_name);
    if (FAILED(hr))
        asb_log(L"Error: Snapshot failed (0x%08X)", hr);
    else
        asb_log(L"Snapshot \"%s\" created.", snap_name);

    save_vm_list();
    if (g_state_cb) g_state_cb(vm, FALSE, g_state_ud);
    return hr;
}

ASB_API HRESULT asb_snap_delete(AsbVm vm, int snap_idx)
{
    HRESULT hr;
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;
    if (snap_idx < 0) { asb_log(L"Select a snapshot to delete."); return E_INVALIDARG; }

    asb_log(L"Deleting snapshot %d...", snap_idx);
    hr = snapshot_delete(&g_snap_trees[idx], &g_vms[idx], snap_idx);
    if (FAILED(hr)) asb_log(L"Error: Failed to delete snapshot (0x%08X)", hr);
    else asb_log(L"Snapshot deleted.");

    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return hr;
}

ASB_API HRESULT asb_snap_new_branch(AsbVm vm, int snap_idx)
{
    HRESULT hr;
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;

    hr = snapshot_new_branch(&g_snap_trees[idx], &g_vms[idx], snap_idx);
    if (FAILED(hr)) asb_log(L"Error: Failed to create branch (0x%08X)", hr);

    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return hr;
}

ASB_API HRESULT asb_snap_delete_branch(AsbVm vm, int snap_idx, int branch_idx)
{
    HRESULT hr;
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;

    asb_log(L"Deleting branch %d of %s...", branch_idx,
            snap_idx == -2 ? L"base" : g_snap_trees[idx].nodes[snap_idx].name);
    hr = snapshot_delete_branch(&g_snap_trees[idx], &g_vms[idx], snap_idx, branch_idx);
    if (FAILED(hr)) asb_log(L"Error: Failed to delete branch (0x%08X)", hr);
    else asb_log(L"Branch deleted.");

    save_vm_list();
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return hr;
}

ASB_API HRESULT asb_snap_rename(AsbVm vm, int snap_idx, int branch_idx,
                                 const wchar_t *new_name)
{
    HRESULT hr;
    int idx = vm_index_of(vm);
    if (idx < 0) return E_INVALIDARG;

    hr = snapshot_rename(&g_snap_trees[idx], snap_idx, branch_idx, new_name);
    if (g_state_cb) g_state_cb(vm, g_vms[idx].running, g_state_ud);
    return hr;
}

ASB_API int asb_snap_count(AsbVm vm)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return 0;
    return g_snap_trees[idx].count;
}

ASB_API BOOL asb_snap_get_info(AsbVm vm, int snap_idx, AsbSnapshotInfo *out)
{
    int idx = vm_index_of(vm);
    if (idx < 0 || !out) return FALSE;
    if (snap_idx < 0 || snap_idx >= g_snap_trees[idx].count) return FALSE;
    if (!g_snap_trees[idx].nodes[snap_idx].valid) return FALSE;

    out->index = snap_idx;
    wcscpy_s(out->name, 128, g_snap_trees[idx].nodes[snap_idx].name);
    wcscpy_s(out->guid, 64, g_snap_trees[idx].nodes[snap_idx].guid);
    out->branch_count = g_snap_trees[idx].nodes[snap_idx].branch_count;
    return TRUE;
}

ASB_API BOOL asb_snap_get_branch_info(AsbVm vm, int snap_idx, int branch_idx,
                                       AsbBranchInfo *out)
{
    int idx = vm_index_of(vm);
    BranchEntry *br;
    if (idx < 0 || !out) return FALSE;

    if (snap_idx == -2) {
        if (branch_idx < 0 || branch_idx >= g_snap_trees[idx].base_branch_count) return FALSE;
        br = &g_snap_trees[idx].base_branches[branch_idx];
    } else {
        if (snap_idx < 0 || snap_idx >= g_snap_trees[idx].count) return FALSE;
        if (branch_idx < 0 || branch_idx >= g_snap_trees[idx].nodes[snap_idx].branch_count) return FALSE;
        br = &g_snap_trees[idx].nodes[snap_idx].branches[branch_idx];
    }

    if (!br->valid) return FALSE;
    out->index = branch_idx;
    wcscpy_s(out->name, 128, br->friendly_name);
    wcscpy_s(out->guid, 64, br->guid);
    return TRUE;
}

ASB_API void asb_snap_get_current(AsbVm vm, int *snap_idx, int *branch_idx)
{
    int idx = vm_index_of(vm);
    if (idx < 0 || !snap_idx || !branch_idx) {
        if (snap_idx) *snap_idx = -1;
        if (branch_idx) *branch_idx = -1;
        return;
    }
    snapshot_find_current(&g_snap_trees[idx], g_vms[idx].vhdx_path, snap_idx, branch_idx);
}

/* ---- Templates ---- */

ASB_API int asb_template_count(void) { return g_template_count; }

ASB_API const wchar_t *asb_template_name(int index)
{
    if (index < 0 || index >= g_template_count) return L"";
    return g_templates[index].name;
}

ASB_API const wchar_t *asb_template_os_type(int index)
{
    if (index < 0 || index >= g_template_count) return L"";
    return g_templates[index].os_type;
}

ASB_API HRESULT asb_template_delete(const wchar_t *name)
{
    wchar_t base_dir[MAX_PATH], tpl_dir[MAX_PATH];
    wchar_t json_path[MAX_PATH], vhdx_path[MAX_PATH];
    wchar_t vmgs[MAX_PATH], vmrs[MAX_PATH], res[MAX_PATH], snap_dir[MAX_PATH];

    if (!name || name[0] == L'\0') return E_INVALIDARG;

    if (!GetEnvironmentVariableW(L"ProgramData", base_dir, MAX_PATH))
        wcscpy_s(base_dir, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(tpl_dir, MAX_PATH, L"%s\\AppSandbox\\templates\\%s", base_dir, name);

    swprintf_s(json_path, MAX_PATH, L"%s\\%s.json", tpl_dir, name);
    swprintf_s(vhdx_path, MAX_PATH, L"%s\\disk.vhdx", tpl_dir);
    swprintf_s(vmgs, MAX_PATH, L"%s\\vm.vmgs", tpl_dir);
    swprintf_s(vmrs, MAX_PATH, L"%s\\vm.vmrs", tpl_dir);
    swprintf_s(res, MAX_PATH, L"%s\\resources.iso", tpl_dir);
    DeleteFileW(json_path);
    DeleteFileW(vhdx_path);
    DeleteFileW(vmgs); DeleteFileW(vmrs); DeleteFileW(res);

    swprintf_s(snap_dir, MAX_PATH, L"%s\\snapshots", tpl_dir);
    RemoveDirectoryW(snap_dir);
    RemoveDirectoryW(tpl_dir);

    asb_log(L"Template \"%s\" deleted.", name);
    scan_templates();
    return S_OK;
}

ASB_API void asb_template_rescan(void)
{
    scan_templates();
}

/* ---- Wait helpers ---- */

ASB_API HRESULT asb_vm_wait_running(AsbVm vm, DWORD timeout_ms)
{
    VmInstance *inst = vm_inst(vm);
    ULONGLONG start;
    if (!inst) return E_INVALIDARG;
    start = GetTickCount64();
    while (!inst->running && !inst->building_vhdx == FALSE) {
        if (GetTickCount64() - start > timeout_ms) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(200);
        /* Check if VM was removed from list */
        if (vm_index_of(vm) < 0) return E_HANDLE;
    }
    /* If still building, wait until building finishes */
    while (inst->building_vhdx) {
        if (GetTickCount64() - start > timeout_ms) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(500);
        if (vm_index_of(vm) < 0) return E_HANDLE;
    }
    return inst->running ? S_OK : E_FAIL;
}

ASB_API HRESULT asb_vm_wait_agent(AsbVm vm, DWORD timeout_ms)
{
    VmInstance *inst = vm_inst(vm);
    ULONGLONG start;
    if (!inst) return E_INVALIDARG;
    start = GetTickCount64();
    while (!inst->agent_online) {
        if (!inst->running) return E_FAIL;
        if (GetTickCount64() - start > timeout_ms) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(500);
        if (vm_index_of(vm) < 0) return E_HANDLE;
    }
    return S_OK;
}

ASB_API HRESULT asb_vm_wait_stopped(AsbVm vm, DWORD timeout_ms)
{
    VmInstance *inst = vm_inst(vm);
    ULONGLONG start;
    if (!inst) return E_INVALIDARG;
    start = GetTickCount64();
    while (inst->running) {
        if (GetTickCount64() - start > timeout_ms) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(200);
        if (vm_index_of(vm) < 0) return S_OK; /* VM deleted = stopped */
    }
    return S_OK;
}

/* ---- Reconnect to running VMs ---- */

ASB_API void asb_reconnect_running(void)
{
    int i;
    for (i = 0; i < g_vm_count; i++) {
        if (g_vms[i].running) continue;  /* already tracked */
        if (hcs_try_open_vm(&g_vms[i])) {
            /* Full reconnect: we have a handle, so register callback + monitor */
            hcs_register_vm_callback(&g_vms[i]);
            hcs_start_monitor(&g_vms[i]);
            vm_agent_start(&g_vms[i]);
            idd_probe_start(&g_vms[i]);
        } else if (hcs_is_running_by_enum(g_vms[i].name)) {
            /* HCS open failed (stale handle from dead process) but enumeration
               confirms the VM is running.  Mark it so queries return the right
               state.  We don't have a handle so we can't register callbacks
               or monitor — the start path will destroy the stale system and
               recreate it when needed. */
            g_vms[i].running = TRUE;
            asb_log(L"Reconnect: \"%s\" is running (via enumeration, no handle).",
                    g_vms[i].name);
        }
    }
}

/* ---- Persistence ---- */

ASB_API void asb_save(void) { save_vm_list(); }

/* ---- Settings accessors ---- */

ASB_API void asb_set_last_iso_path(const wchar_t *path)
{
    if (path) wcscpy_s(g_last_iso_path, MAX_PATH, path);
}

ASB_API const wchar_t *asb_get_last_iso_path(void)
{
    return g_last_iso_path;
}

ASB_API void asb_set_suppress_tray_warn(BOOL suppress)
{
    g_suppress_tray_warn = suppress;
}

ASB_API BOOL asb_get_suppress_tray_warn(void)
{
    return g_suppress_tray_warn;
}

/* ---- Internal access (for UI layer) ---- */

ASB_API VmInstance *asb_vm_instance(AsbVm vm) { return vm_inst(vm); }

ASB_API SnapshotTree *asb_vm_snap_tree(AsbVm vm)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return NULL;
    return &g_snap_trees[idx];
}

ASB_API GpuList *asb_gpu_list(void) { return &g_gpu_list; }

ASB_API int asb_vm_index(AsbVm vm) { return vm_index_of(vm); }

/* ---- HINSTANCE accessor (UI compatibility) ---- */

static HINSTANCE g_hInstance_core = NULL;

ASB_API HINSTANCE ui_get_instance(void) { return g_hInstance_core; }

ASB_API void asb_set_hinstance(HINSTANCE hInst) { g_hInstance_core = hInst; }
