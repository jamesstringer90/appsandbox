/*
 * asb_core.c -- App Sandbox Core Library implementation.
 *
 * VM orchestration: persistence, lifecycle (create/start/stop/delete),
 * snapshots, templates, config editing. Notifies consumers via callbacks
 * registered through asb_set_*_callback().
 */

#include <winsock2.h>
#include "asb_core.h"
#include "hcs_vm.h"
#include "gpu_enum.h"
#include "hcn_network.h"
#include "disk_util.h"
#include "d3dlayers.h"
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
#include <virtdisk.h>
#pragma comment(lib, "virtdisk.lib")

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

/* Prepare the GL mapping-layer Plan9 share for a Windows GPU guest: ensure the
 * D3D mapping layers (OpenCL/Vulkan/dxil) are fetched/cached on the host, stage
 * Mesa's standalone OpenGL trio (opengl32/gallium_wgl/z-1) alongside them, then
 * append the "AppSandbox.GlLayers" share. The guest agent copies this share
 * AFTER the GPU driver shares and provisions it (deploys the Mesa trio + dxil.dll
 * to System32, sets the Khronos OpenCL/Vulkan ICD keys). Best-effort: if the
 * layers can't be fetched, no share is added and GL/CL/Vulkan acceleration is
 * simply unavailable.
 *
 * The Mesa trio is vendored prebuilt per-arch (tools/win-mesa-gl/prebuilt) and
 * staged into resources\win-mesa-gl by the build (only this build's arch, since
 * host arch == guest arch). We copy it flat into the share dir, matching the
 * flat layout the share already uses for the MS mapping layers. */
static void prepare_gl_layers_share(GpuDriverShareList *shares)
{
    static const wchar_t *const mesa_trio[] = {
        L"opengl32.dll", L"gallium_wgl.dll", L"z-1.dll"
    };
    wchar_t dir[MAX_PATH], exe[MAX_PATH], res[MAX_PATH];
    wchar_t src[MAX_PATH], dst[MAX_PATH], *slash;
    int i;

    if (!d3dlayers_ensure_cached(dir, MAX_PATH))
        return;

    /* Locate the staged Mesa trio (resources\win-mesa-gl beside the exe). */
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) *slash = L'\0';
    swprintf_s(res, MAX_PATH, L"%s\\resources\\win-mesa-gl", exe);
    if (GetFileAttributesW(res) == INVALID_FILE_ATTRIBUTES)
        swprintf_s(res, MAX_PATH, L"%s\\win-mesa-gl", exe);

    /* Copy the trio flat into the share dir so it rides to the guest with the
     * other mapping layers; the agent deploys it into System32. */
    for (i = 0; i < (int)(sizeof(mesa_trio) / sizeof(mesa_trio[0])); i++) {
        swprintf_s(src, MAX_PATH, L"%s\\%s", res, mesa_trio[i]);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", dir, mesa_trio[i]);
        CopyFileW(src, dst, FALSE);  /* best-effort; trio may not be staged yet */
    }

    gpu_append_gl_layers_share(shares, dir);
}

static VmInstance g_vms[ASB_MAX_VMS];
static int g_vm_count = 0;

/* Monotonically-increasing per-VM ID. Never reused; 0 is reserved as
   "unassigned." Used by long-lived threads (agent, IDD probe) so they
   keep working across compactions of g_vms[]. */
static UINT64 g_next_vm_id = 1;

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

/* ---- Internal logging (writes to appsandbox.log and the registered log callback) ---- */

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

/* Frame channel: {A5B0CAFE-0002-4000-8000-000000000001} for Windows guests.
   Linux guests resolve to the AF_VSOCK template GUID instead — see
   hcs_service_guid(os_type, 2, ...). */
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
    UINT64 vm_id = (UINT64)(ULONG_PTR)param;
    VmInstance *vm;
    SOCKET s;
    SOCKADDR_HV_PROBE addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;

    vm = asb_find_vm_by_id(vm_id);
    if (!vm) return 0;
    asb_log(L"IDD probe: starting for \"%s\"", vm->name);

    /* Resolve VM by stable id each iteration. Returns NULL if the VM
       has been deleted, so a single shift+compaction of g_vms[] can no
       longer leave us holding a stale pointer. */
    while ((vm = asb_find_vm_by_id(vm_id)) != NULL &&
           !vm->idd_probe_stop && vm->running && !vm->install_complete) {
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
        hcs_service_guid(vm->os_type, 2, &addr.ServiceId);

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
                continue;  /* timeout or error - VDD not ready yet */
            }
        }

        /* Connection succeeded - VDD is accepting frames.
           Re-resolve vm one last time inside this branch; vm could
           have been shifted/deleted between the loop test and now. */
        closesocket(s);

        vm = asb_find_vm_by_id(vm_id);
        if (vm && !vm->idd_probe_stop && vm->running && !vm->install_complete) {
            asb_log(L"IDD probe: VDD ready for \"%s\", opening display", vm->name);
            /* Pass the stable ID, not a VmInstance*: the UI thread looks
               it up freshly with asb_find_vm_by_id before acting. */
            if (g_idd_probe_hwnd)
                PostMessageW(g_idd_probe_hwnd, WM_VM_IDD_READY, 0, (LPARAM)vm_id);
        }
        break;
    }

    /* vm may have been deleted; re-resolve before logging the exit. */
    vm = asb_find_vm_by_id(vm_id);
    asb_log(L"IDD probe: exiting for \"%s\"", vm ? vm->name : L"(deleted)");
    return 0;
}

static void idd_probe_start(VmInstance *vm)
{
    /* The probe exists only to AUTO-OPEN the display in the GUI: it posts
       WM_VM_IDD_READY to g_idd_probe_hwnd when the VDD comes up. With no hwnd
       (the headless daemon) there is nothing to notify, and auto-opening a window
       the CLI caller didn't ask for would be wrong -- so don't run it. Headless
       opens the display only on an explicit request, gated by asb_vm_idd_ready. */
    if (!g_idd_probe_hwnd) return;
    if (vm->idd_probe_thread) return;
    if (vm->install_complete) return;
    if (vm->is_template) return;

    vm->idd_probe_stop = FALSE;
    /* Pass stable ID, not pointer -- the thread is long-lived and must
       survive g_vms[] compaction. */
    vm->idd_probe_thread = CreateThread(NULL, 0, idd_probe_thread_proc,
                                         (LPVOID)(ULONG_PTR)vm->unique_id, 0, NULL);
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

ASB_API VmInstance *asb_find_vm_by_id(UINT64 id)
{
    int i;
    if (id == 0) return NULL;
    for (i = 0; i < g_vm_count; i++) {
        if (g_vms[i].unique_id == id) return &g_vms[i];
    }
    return NULL;
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

/* Ensure the AppSandbox SSH keypair exists under %ProgramData%\AppSandbox\ssh\,
   creating an ed25519 pair (via the system ssh-keygen) if absent, and return its
   public-key line in pubkey_out (newline-trimmed). ed25519 keeps the public key
   short enough to fit the agent's command buffer. The private key stays host-side
   so clients can connect with `ssh -i id_appsandbox`. Returns TRUE on success. */
static BOOL ensure_appsandbox_ssh_key(wchar_t *pubkey_out, int cap)
{
    wchar_t base[MAX_PATH], ssh_dir[MAX_PATH], priv[MAX_PATH], pub[MAX_PATH];
    char line[1024] = {0};
    FILE *f = NULL;

    if (cap > 0) pubkey_out[0] = 0;
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(ssh_dir, MAX_PATH, L"%s\\AppSandbox\\ssh", base);
    swprintf_s(priv, MAX_PATH, L"%s\\id_appsandbox", ssh_dir);
    swprintf_s(pub,  MAX_PATH, L"%s\\id_appsandbox.pub", ssh_dir);

    if (GetFileAttributesW(pub) == INVALID_FILE_ATTRIBUTES) {
        wchar_t dir1[MAX_PATH], sysdir[MAX_PATH], keygen[MAX_PATH], cmdline[1280];
        STARTUPINFOW si; PROCESS_INFORMATION pi;
        swprintf_s(dir1, MAX_PATH, L"%s\\AppSandbox", base);
        CreateDirectoryW(dir1, NULL);
        CreateDirectoryW(ssh_dir, NULL);
        DeleteFileW(priv);   /* avoid ssh-keygen's interactive overwrite prompt on a partial key */
        GetSystemDirectoryW(sysdir, MAX_PATH);
        swprintf_s(keygen, MAX_PATH, L"%s\\OpenSSH\\ssh-keygen.exe", sysdir);
        swprintf_s(cmdline, 1280, L"\"%s\" -t ed25519 -f \"%s\" -N \"\" -C appsandbox -q", keygen, priv);
        ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));
        if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            asb_log(L"ssh key: ssh-keygen launch failed (%lu)", GetLastError());
            return FALSE;
        }
        /* Check the outcome: a timed-out or failed ssh-keygen must not leave
           partial key material behind to be trusted on the next run (the
           regen check above keys only on the .pub existing). */
        {
            DWORD wait_rc = WaitForSingleObject(pi.hProcess, 30000);
            DWORD exit_rc = 1;
            if (wait_rc == WAIT_OBJECT_0)
                GetExitCodeProcess(pi.hProcess, &exit_rc);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            if (wait_rc != WAIT_OBJECT_0 || exit_rc != 0) {
                asb_log(L"ssh key: ssh-keygen failed (wait=%lu exit=%lu)", wait_rc, exit_rc);
                DeleteFileW(priv);
                DeleteFileW(pub);
                return FALSE;
            }
        }
        asb_log(L"ssh key: generated AppSandbox keypair at %s", priv);
    }

    if (_wfopen_s(&f, pub, L"r") != 0 || !f) {
        asb_log(L"ssh key: cannot read public key %s", pub);
        return FALSE;
    }
    if (fgets(line, sizeof(line), f)) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
    }
    fclose(f);
    MultiByteToWideChar(CP_UTF8, 0, line, -1, pubkey_out, cap);
    return pubkey_out[0] != 0;
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
        if (g_vms[i].ssh_deploy_key)
            fwprintf(f, L"SshDeployKey=1\n");
        if (g_vms[i].ssh_pubkey[0])
            fwprintf(f, L"SshPubKey=%s\n", g_vms[i].ssh_pubkey);
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
            vm->unique_id = g_next_vm_id++;
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
            { /* ignored - backwards compat */ }
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
        else if (wcsncmp(line, L"SshDeployKey=", 13) == 0)
            vm->ssh_deploy_key = (_wtoi(line + 13) != 0);
        else if (wcsncmp(line, L"SshPubKey=", 10) == 0)
            /* Truncating copy: wcscpy_s ABORTS the process on overflow, and this
               line comes from an editable config file. Generated ed25519 lines
               are ~100 chars; anything longer is corrupt -- truncate, don't die. */
            wcsncpy_s(vm->ssh_pubkey, 512, line + 10, _TRUNCATE);
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
            /* Re-deploy on next boot: the guest disk may change while stopped
               (snapshot/branch revert), so "deployed" is only valid per boot.
               The guest-side write is idempotent. */
            instance->ssh_key_deployed = FALSE;
            hcs_stop_monitor(instance);
            vm_ssh_proxy_stop(instance);
            vm_agent_stop(instance);
            idd_probe_stop(instance);
            asb_log(L"VM \"%s\" exited (event=0x%08X).", instance->name, event);

            asb_vm_cleanup_network(instance);
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

                EnterCriticalSection(&g_cs);
                for (j = i; j < g_vm_count - 1; j++) {
                    g_vms[j] = g_vms[j+1];
                    g_snap_trees[j] = g_snap_trees[j+1];
                }
                ZeroMemory(&g_vms[g_vm_count-1], sizeof(VmInstance));
                g_vm_count--;
                LeaveCriticalSection(&g_cs);

                scan_templates();

                /* Fire the removed callback last - WM_VM_REMOVED is async, so
                   both g_vms[] and g_templates[] must be fully updated before
                   the UI thread can pull the message and re-query them. */
                if (g_removed_cb) g_removed_cb(i, g_removed_ud);
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

/* Returns TRUE if any other running VM (excluding `self`) is still attached
   to a network of the given mode. Used to decide whether it's safe to tear
   down the shared HCN network when `self` stops. */
static BOOL another_vm_uses_network_mode(const VmInstance *self, int mode)
{
    int i;
    if (mode == NET_NONE) return FALSE;
    for (i = 0; i < g_vm_count; i++) {
        const VmInstance *v = &g_vms[i];
        if (v == self) continue;
        if (!v->running) continue;
        if (v->network_cleaned) continue;
        if (v->network_mode != mode) continue;
        return TRUE;
    }
    return FALSE;
}

ASB_API void asb_vm_cleanup_network(VmInstance *vm)
{
    if (!vm) return;
    if (vm->network_mode == NET_NONE) return;
    if (vm->network_cleaned) return;
    hcn_delete_endpoint(&vm->endpoint_id);
    /* The HCN network is shared across all VMs of the same mode
       (fixed GUID). Only tear it down if no other running VM is
       still attached to it. */
    if (!another_vm_uses_network_mode(vm, vm->network_mode))
        hcn_delete_network(&vm->network_id);
    vm->network_cleaned = TRUE;
}

/* ---- NAT IP allocation ---- */

/* Allocate the next free IP in the chosen NAT /24 (see hcn_nat_subnet_base).
   Scans g_vms[] for IPs already in use. Returns FALSE if pool exhausted. */
static BOOL allocate_nat_ip(VmInstance *vm)
{
    BOOL used[256] = { 0 };
    const char *base = hcn_nat_subnet_base();   /* e.g. "192.168.42" */
    int ba, bb, bc;
    int i, octet;

    if (sscanf_s(base, "%d.%d.%d", &ba, &bb, &bc) != 3) return FALSE;

    for (i = 0; i < g_vm_count; i++) {
        int a, b, c, d;
        if (&g_vms[i] == vm) continue;
        if (g_vms[i].nat_ip[0] == '\0') continue;
        /* Only consider entries that share our current /24. Stale entries
           from a prior run with a different subnet are ignored - their
           endpoints died with the previous NAT network. */
        if (sscanf_s(g_vms[i].nat_ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4 &&
            a == ba && b == bb && c == bc && d >= 2 && d <= 254)
            used[d] = TRUE;
    }

    for (octet = 2; octet <= 254; octet++) {
        if (!used[octet]) {
            sprintf_s(vm->nat_ip, sizeof(vm->nat_ip), "%s.%d", base, octet);
            return TRUE;
        }
    }
    return FALSE;
}

/* Try hcn_create_endpoint, retrying on NAT failure by bumping the last
   octet of nat_ip up to 10 times. The buffer pointed to by nat_ip is
   mutated in place on a successful retry; the caller is responsible for
   save_vm_list() if it cares about persistence.

   Defends against HCN_E_ADDR_INVALID_OR_RESERVED (0x803B002F), which
   HNS returns when an endpoint at the chosen IP was orphaned by a
   previous run -- the IP is in our allocator's free list but HNS still
   has a phantom reservation. Bumping past it usually wins. */
static HRESULT try_endpoint_with_retry(const GUID *net_id, GUID *ep_id,
                                       wchar_t *ep_guid_str, size_t str_len,
                                       char *nat_ip, size_t nat_ip_size,
                                       BOOL is_nat)
{
    HRESULT hr;
    int retry;

    hr = hcn_create_endpoint(net_id, ep_id, ep_guid_str, str_len,
                              (nat_ip && nat_ip[0]) ? nat_ip : NULL);
    if (SUCCEEDED(hr) || !is_nat || !nat_ip || !nat_ip[0]) return hr;

    asb_log(L"Endpoint failed for %S, trying next IP...", nat_ip);
    for (retry = 0; retry < 10; retry++) {
        int a, b, c, d;
        if (sscanf_s(nat_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4 || d >= 254) break;
        sprintf_s(nat_ip, nat_ip_size, "%d.%d.%d.%d", a, b, c, d + 1);
        asb_log(L"Retrying with %S...", nat_ip);
        hr = hcn_create_endpoint(net_id, ep_id, ep_guid_str, str_len, nat_ip);
        if (SUCCEEDED(hr)) return hr;
    }
    return hr;
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
            hr = try_endpoint_with_retry(&args->network_id, &args->endpoint_id,
                                          endpoint_guid_str, 64,
                                          vm->nat_ip, sizeof(vm->nat_ip),
                                          args->network_mode == NET_NAT);
            if (SUCCEEDED(hr) && args->network_mode == NET_NAT) save_vm_list();
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

    if (args->config.gpu_mode == GPU_DEFAULT || args->config.gpu_mode == GPU_MIRROR) {
        gpu_get_driver_shares(&g_gpu_list, &args->config.gpu_shares);
        /* For Linux guests, also expose the host's lxss\lib so the
         * Linux agent mounts it at /usr/lib/wsl/lib alongside the
         * per-GPU driver shares. Windows guests have no use for it. */
        if (_wcsicmp(args->config.os_type, L"Linux") == 0)
            gpu_append_lxsslib_share(&args->config.gpu_shares);
        else if (_wcsicmp(args->config.os_type, L"Windows") == 0)
            prepare_gl_layers_share(&args->config.gpu_shares);
    }

    asb_log(L"Re-creating HCS compute system for \"%s\"...", vm->name);
    hr = (endpoint_guid_str[0] != L'\0')
        ? hcs_create_vm_with_endpoint(&args->config, endpoint_guid_str, vm)
        : hcs_create_vm(&args->config, vm);
    if (FAILED(hr)) {
        asb_log(L"Error: Failed to create compute system (0x%08X)", hr);
        asb_alert(L"Failed to start VM, check its configuration.");
        /* HCS rejected the VM after we already created the HCN endpoint.
           Free the endpoint so its IP reservation doesn't leak into the
           next attempt as a phantom HCN_E_ADDR_INVALID_OR_RESERVED. */
        if (endpoint_guid_str[0] != L'\0') {
            hcn_delete_endpoint(&args->endpoint_id);
            endpoint_guid_str[0] = L'\0';
        }
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
        /* Agent + IDD probe run for any OS: hcs_service_guid() resolves
           per-OS to the correct HV-socket service GUID, and the in-VM
           agent listens on AF_HYPERV (Windows) or AF_VSOCK (Linux). */
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
    UINT64   vm_unique_id;   /* stable id; survives g_vms[] compaction (H13) */
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
                            /* Resolve by stable id under g_cs: g_vms[] may
                               have been compacted while we were building (H13). */
                            VmInstance *pvm;
                            EnterCriticalSection(&g_cs);
                            pvm = asb_find_vm_by_id(args->vm_unique_id);
                            if (pvm) {
                                pvm->vhdx_progress = pct;
                                pvm->vhdx_staging = is_staging;
                            }
                            LeaveCriticalSection(&g_cs);
                            if (g_progress_cb && pvm)
                                g_progress_cb(vm_handle(pvm), pct, is_staging, g_progress_ud);
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
            BOOL is_nat = (args->config.network_mode == NET_NAT);
            size_t ip_size = (args->vm_index >= 0 && args->vm_index < g_vm_count)
                              ? sizeof(g_vms[args->vm_index].nat_ip) : 0;
            hr = try_endpoint_with_retry(&args->network_id, &args->endpoint_id,
                                          args->endpoint_guid, 64,
                                          nat_ip, ip_size, is_nat);
            if (SUCCEEDED(hr)) {
                args->has_network = TRUE;
                if (is_nat) save_vm_list();
            }
            /* If endpoint create fails, leave the network alone - it may be
               shared with other VMs. Orphan networks are cleaned up at next
               launch by hcn_cleanup_stale_networks(). */
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
            /* HCS rejected the VM after we already created the HCN endpoint.
               Free the endpoint so its IP reservation doesn't leak into the
               next attempt as a phantom HCN_E_ADDR_INVALID_OR_RESERVED. */
            if (args->has_network) {
                hcn_delete_endpoint(&args->endpoint_id);
                args->has_network = FALSE;
                args->endpoint_guid[0] = L'\0';
            }
            goto done;
        }

        hr = hcs_start_vm(&temp_inst);
        if (FAILED(hr)) {
            args->result = hr;
            swprintf_s(args->error_msg, 512, L"Failed to start VM (0x%08X)", hr);
            hcs_close_vm(&temp_inst);
            /* VM created but failed to start. Free the endpoint so its
               IP reservation doesn't leak into the next attempt. */
            if (args->has_network) {
                hcn_delete_endpoint(&args->endpoint_id);
                args->has_network = FALSE;
                args->endpoint_guid[0] = L'\0';
            }
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
        VmInstance *inst;
        int idx;

        /* Resolve by stable id under g_cs: g_vms[] may have been compacted
           (entries shifted down) while we were building, so the cached
           vm_index can now point at a different VM or be out of range (H13). */
        EnterCriticalSection(&g_cs);
        inst = asb_find_vm_by_id(args->vm_unique_id);
        idx = inst ? (int)(inst - g_vms) : -1;
        if (inst) {
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
                    args->vm_inst = NULL;
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
                /* VHDX never created - remove VM from list and clean up files */
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
        } else {
            /* VM was deleted while building: don't leak the started HCS
               handle copy, and don't write into a stale slot. */
            LeaveCriticalSection(&g_cs);
            if (args->vm_inst) {
                hcs_unregister_vm_callback(args->vm_inst);
                hcs_close_vm(args->vm_inst);
                HeapFree(GetProcessHeap(), 0, args->vm_inst);
                args->vm_inst = NULL;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, args);
    return 0;
}

/* ---- Generate staging manifest for the Ubuntu direct-ISO->VHDX flow.
 *
 * 1. Calls stage_linux_agent_and_extras to populate <staging>/extras/
 *    with the full set of agent ELFs, kernel modules, DKMS source trees,
 *    systemd units, modprobe.d/modules-load.d, wsl-mesa, wsl-deps.
 *
 * 2. Walks <staging>/extras/ recursively and writes one tab-separated
 *    line per file to the manifest:
 *
 *        <host absolute path>\t/opt/appsandbox/<relative path>
 *
 *    iso-patch's stage_manifest_into_rootfs reads this and writes each
 *    file into the ext4 rootfs via ext4_writer_add_file.
 *
 *    Forward slashes used in the rootfs destination (Linux convention).
 *    Parent directories handled by iso-patch's ext4_mkdir_p.
 *
 * Returns the count of files staged, or -1 on error. */
static int write_manifest_walk_dir(FILE *f,
                                   const wchar_t *abs_dir,
                                   const char *rootfs_rel_prefix)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    int count = 0;
    swprintf_s(pattern, MAX_PATH, L"%s\\*", abs_dir);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        wchar_t abs_path[MAX_PATH];
        swprintf_s(abs_path, MAX_PATH, L"%s\\%s", abs_dir, fd.cFileName);
        /* Convert wide filename -> utf8 for the rootfs-side path. */
        char fn_utf8[260];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                            fn_utf8, sizeof(fn_utf8), NULL, NULL);
        char rootfs_rel[1024];
        snprintf(rootfs_rel, sizeof(rootfs_rel), "%s/%s",
                 rootfs_rel_prefix, fn_utf8);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += write_manifest_walk_dir(f, abs_path, rootfs_rel);
        } else {
            /* manifest line: <host path>\t<rootfs path>\n */
            fwprintf(f, L"%s\t", abs_path);
            /* Write rootfs path as wide for utf-8-bom-safe round-trip. */
            wchar_t rootfs_wide[1024];
            MultiByteToWideChar(CP_UTF8, 0, rootfs_rel, -1,
                                rootfs_wide, 1024);
            fwprintf(f, L"%s\n", rootfs_wide);
            count++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

/* Write a small utf-8 file at <staging>\<basename>, and append a manifest
   line mapping it into the rootfs at <rootfs_path>. Returns 1 on success,
   0 on failure (and logs). Content is written WITHOUT a trailing newline
   so the firstboot script can read it back unchanged. */
static int stage_marker_file(FILE *manifest_f, const wchar_t *staging,
                             const wchar_t *basename,
                             const char *content, size_t content_len,
                             const wchar_t *rootfs_path)
{
    wchar_t host[MAX_PATH];
    swprintf_s(host, MAX_PATH, L"%s\\%s", staging, basename);
    HANDLE h = CreateFileW(host, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        asb_log(L"WARN: could not create marker on host: %s", basename);
        return 0;
    }
    if (content_len > 0) {
        DWORD wr = 0;
        WriteFile(h, content, (DWORD)content_len, &wr, NULL);
    }
    CloseHandle(h);
    fwprintf(manifest_f, L"%s\t%s\n", host, rootfs_path);
    return 1;
}

/* ====================================================================
 * Host locale / keyboard / timezone detection + Windows->Linux mapping
 *
 * At VM-create time we read the *current user's* Windows regional
 * settings and translate them to the Ubuntu guest's equivalents, which
 * firstboot applies (replacing the old hardcoded en_US / us / New_York).
 *
 * Language is GATED: Ubuntu's ISO only ships pre-installed UI
 * translations for 8 languages (de, en, es, fr, it, pt, ru, zh). If the
 * host language isn't one of those, the locale falls back to en_US.UTF-8
 * so the guest never lands on a half-translated desktop. Keyboard and
 * timezone are NOT gated — every XKB layout and IANA zone is present in
 * the squashfs offline, so they always match the host.
 * ==================================================================== */

/* Map a BCP-47 tag (e.g. "en-GB", "zh-Hans-CN", "ja-JP") to a Linux
 * locale string ("en_GB.UTF-8"). Returns the en_US.UTF-8 fallback when
 * the primary language subtag is not one of the 8 preinstalled langs. */
static void win_locale_to_linux(const wchar_t *bcp47, char *out, size_t out_sz)
{
    static const wchar_t *supported[] = {
        L"de", L"en", L"es", L"fr", L"it", L"pt", L"ru", L"zh"
    };
    /* Default region per language when the host tag carries no region. */
    static const struct { const wchar_t *lang; const char *loc; } defloc[] = {
        { L"de", "de_DE" }, { L"en", "en_US" }, { L"es", "es_ES" },
        { L"fr", "fr_FR" }, { L"it", "it_IT" }, { L"pt", "pt_PT" },
        { L"ru", "ru_RU" }, { L"zh", "zh_CN" },
    };

    strcpy_s(out, out_sz, "en_US.UTF-8");
    if (!bcp47 || !bcp47[0]) return;

    /* Split into up to 4 subtags. */
    wchar_t buf[64];
    wcsncpy_s(buf, 64, bcp47, _TRUNCATE);
    wchar_t lang[16] = L"", script[16] = L"", region[16] = L"";
    {
        wchar_t *ctx = NULL;
        wchar_t *tok = wcstok_s(buf, L"-", &ctx);
        if (tok) { wcsncpy_s(lang, 16, tok, _TRUNCATE); _wcslwr_s(lang, 16); }
        while ((tok = wcstok_s(NULL, L"-", &ctx)) != NULL) {
            size_t l = wcslen(tok);
            if (l == 4) { wcsncpy_s(script, 16, tok, _TRUNCATE); }       /* script */
            else if (l == 2 || l == 3) { wcsncpy_s(region, 16, tok, _TRUNCATE);
                                         _wcsupr_s(region, 16); }        /* region */
        }
    }
    if (!lang[0]) return;

    /* Gate on the 8 supported languages. */
    int ok = 0;
    for (int i = 0; i < (int)(sizeof(supported)/sizeof(supported[0])); i++)
        if (_wcsicmp(lang, supported[i]) == 0) { ok = 1; break; }
    if (!ok) return;  /* keep en_US.UTF-8 fallback */

    /* Chinese needs script->region resolution (zh_CN vs zh_TW etc.). */
    if (_wcsicmp(lang, L"zh") == 0) {
        const char *loc = "zh_CN";
        if (region[0]) {
            if      (!_wcsicmp(region, L"TW")) loc = "zh_TW";
            else if (!_wcsicmp(region, L"HK")) loc = "zh_HK";
            else if (!_wcsicmp(region, L"SG")) loc = "zh_SG";
            else                               loc = "zh_CN";
        } else if (script[0]) {
            loc = (!_wcsicmp(script, L"Hant")) ? "zh_TW" : "zh_CN";
        }
        strcpy_s(out, out_sz, loc);
        strcat_s(out, out_sz, ".UTF-8");
        return;
    }

    if (region[0]) {
        char lang_a[16], region_a[16];
        WideCharToMultiByte(CP_UTF8, 0, lang, -1, lang_a, sizeof(lang_a), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, region, -1, region_a, sizeof(region_a), NULL, NULL);
        snprintf(out, out_sz, "%s_%s.UTF-8", lang_a, region_a);
    } else {
        for (int i = 0; i < (int)(sizeof(defloc)/sizeof(defloc[0])); i++)
            if (_wcsicmp(lang, defloc[i].lang) == 0) {
                snprintf(out, out_sz, "%s.UTF-8", defloc[i].loc);
                break;
            }
    }
}

/* Map a Windows keyboard input-language id (LOWORD of an HKL) to an XKB
 * layout. Never gated — all layouts ship in xkb-data. Fallback "us". */
static void langid_to_xkb(WORD langid, char *out, size_t out_sz)
{
    static const struct { WORD id; const char *xkb; } map[] = {
        { 0x0409, "us" }, { 0x0809, "gb" }, { 0x0c09, "us" }, /* en-AU->us */
        { 0x0407, "de" }, { 0x0c07, "at" }, { 0x0807, "ch" },
        { 0x040c, "fr" }, { 0x0c0c, "ca" }, { 0x080c, "be" }, { 0x100c, "ch" },
        { 0x0410, "it" }, { 0x0810, "ch" },
        { 0x040a, "es" }, { 0x080a, "latam" }, { 0x0c0a, "es" },
        { 0x0416, "br" }, { 0x0816, "pt" },
        { 0x0411, "jp" }, { 0x0412, "kr" },
        { 0x0804, "cn" }, { 0x0404, "tw" }, { 0x0c04, "cn" }, { 0x1404, "cn" },
        { 0x0419, "ru" }, { 0x0415, "pl" },
        { 0x0413, "nl" }, { 0x0813, "be" },
        { 0x041d, "se" }, { 0x0414, "no" }, { 0x0406, "dk" }, { 0x040b, "fi" },
        { 0x0405, "cz" }, { 0x040e, "hu" }, { 0x041f, "tr" },
        { 0x0401, "ara" }, { 0x040d, "il" }, { 0x041e, "th" },
        { 0x0422, "ua" }, { 0x0418, "ro" }, { 0x0408, "gr" },
        { 0x0402, "bg" }, { 0x041a, "hr" }, { 0x041b, "sk" }, { 0x0424, "si" },
        { 0x0425, "ee" }, { 0x0426, "lv" }, { 0x0427, "lt" },
    };
    strcpy_s(out, out_sz, "us");
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if (map[i].id == langid) { strcpy_s(out, out_sz, map[i].xkb); return; }
    /* Fall back on the primary language id (low 10 bits) for unlisted
     * sublang variants. */
    WORD prim = (WORD)(langid & 0x3ff);
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if ((WORD)(map[i].id & 0x3ff) == prim) { strcpy_s(out, out_sz, map[i].xkb); return; }
}

/* Map a Windows time-zone key name to an IANA zone. Never gated — all
 * zones ship in tzdata. Fallback "Etc/UTC". Subset of CLDR windowsZones
 * covering the common zones; unknown keys fall back to UTC. */
static void win_tz_to_iana(const wchar_t *keyname, char *out, size_t out_sz)
{
    static const struct { const wchar_t *win; const char *iana; } map[] = {
        { L"Dateline Standard Time",        "Etc/GMT+12" },
        { L"Hawaiian Standard Time",        "Pacific/Honolulu" },
        { L"Alaskan Standard Time",         "America/Anchorage" },
        { L"Pacific Standard Time",         "America/Los_Angeles" },
        { L"US Mountain Standard Time",     "America/Phoenix" },
        { L"Mountain Standard Time",        "America/Denver" },
        { L"Central Standard Time",         "America/Chicago" },
        { L"Canada Central Standard Time",  "America/Regina" },
        { L"Central America Standard Time", "America/Guatemala" },
        { L"Eastern Standard Time",         "America/New_York" },
        { L"US Eastern Standard Time",      "America/Indiana/Indianapolis" },
        { L"Atlantic Standard Time",        "America/Halifax" },
        { L"SA Pacific Standard Time",      "America/Bogota" },
        { L"SA Eastern Standard Time",      "America/Cayenne" },
        { L"E. South America Standard Time","America/Sao_Paulo" },
        { L"Argentina Standard Time",       "America/Buenos_Aires" },
        { L"Greenwich Standard Time",       "Atlantic/Reykjavik" },
        { L"GMT Standard Time",             "Europe/London" },
        { L"W. Europe Standard Time",       "Europe/Berlin" },
        { L"Central Europe Standard Time",  "Europe/Budapest" },
        { L"Romance Standard Time",         "Europe/Paris" },
        { L"Central European Standard Time","Europe/Warsaw" },
        { L"W. Central Africa Standard Time","Africa/Lagos" },
        { L"FLE Standard Time",             "Europe/Kiev" },
        { L"GTB Standard Time",             "Europe/Bucharest" },
        { L"E. Europe Standard Time",       "Europe/Chisinau" },
        { L"Russian Standard Time",         "Europe/Moscow" },
        { L"Turkey Standard Time",          "Europe/Istanbul" },
        { L"Israel Standard Time",          "Asia/Jerusalem" },
        { L"Egypt Standard Time",           "Africa/Cairo" },
        { L"South Africa Standard Time",    "Africa/Johannesburg" },
        { L"Arabian Standard Time",         "Asia/Dubai" },
        { L"Arab Standard Time",            "Asia/Riyadh" },
        { L"Iran Standard Time",            "Asia/Tehran" },
        { L"Pakistan Standard Time",        "Asia/Karachi" },
        { L"India Standard Time",           "Asia/Kolkata" },
        { L"Bangladesh Standard Time",      "Asia/Dhaka" },
        { L"SE Asia Standard Time",         "Asia/Bangkok" },
        { L"China Standard Time",           "Asia/Shanghai" },
        { L"Singapore Standard Time",       "Asia/Singapore" },
        { L"Taipei Standard Time",          "Asia/Taipei" },
        { L"Tokyo Standard Time",           "Asia/Tokyo" },
        { L"Korea Standard Time",           "Asia/Seoul" },
        { L"W. Australia Standard Time",    "Australia/Perth" },
        { L"AUS Central Standard Time",     "Australia/Darwin" },
        { L"AUS Eastern Standard Time",     "Australia/Sydney" },
        { L"E. Australia Standard Time",    "Australia/Brisbane" },
        { L"New Zealand Standard Time",     "Pacific/Auckland" },
        { L"UTC",                           "Etc/UTC" },
    };
    strcpy_s(out, out_sz, "Etc/UTC");
    if (!keyname || !keyname[0]) return;
    for (int i = 0; i < (int)(sizeof(map)/sizeof(map[0])); i++)
        if (_wcsicmp(keyname, map[i].win) == 0) {
            strcpy_s(out, out_sz, map[i].iana); return;
        }
}

/* Read the current user's locale, keyboard layout, and timezone from the
 * Windows host and translate to Linux equivalents. Best-effort: any
 * failure leaves the corresponding output at its safe default. */
static void detect_host_locale_settings(char *locale, size_t locale_sz,
                                        char *xkb, size_t xkb_sz,
                                        char *tz, size_t tz_sz)
{
    /* Locale (gated). */
    {
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"";
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0)
            win_locale_to_linux(name, locale, locale_sz);
        else
            strcpy_s(locale, locale_sz, "en_US.UTF-8");
        asb_log(L"Host locale: %s -> %hs", name[0] ? name : L"(none)", locale);
    }
    /* Keyboard (not gated). */
    {
        HKL list[16];
        int n = GetKeyboardLayoutList(16, list);
        WORD langid = 0x0409;
        if (n > 0) langid = (WORD)((UINT_PTR)list[0] & 0xFFFF);
        langid_to_xkb(langid, xkb, xkb_sz);
        asb_log(L"Host keyboard: langid=0x%04x -> %hs", langid, xkb);
    }
    /* Timezone (not gated). */
    {
        DYNAMIC_TIME_ZONE_INFORMATION dtzi;
        DWORD r = GetDynamicTimeZoneInformation(&dtzi);
        if (r != TIME_ZONE_ID_INVALID && dtzi.TimeZoneKeyName[0])
            win_tz_to_iana(dtzi.TimeZoneKeyName, tz, tz_sz);
        else
            strcpy_s(tz, tz_sz, "Etc/UTC");
        asb_log(L"Host timezone: %s -> %hs",
                (r != TIME_ZONE_ID_INVALID) ? dtzi.TimeZoneKeyName : L"(invalid)", tz);
    }
}

static int generate_vhdx_manifest_ubuntu(const wchar_t *manifest_path,
                                         const wchar_t *staging,
                                         const wchar_t *res_dir,
                                         BOOL ssh_enabled,
                                         const wchar_t *admin_user,
                                         const char *admin_pw_hash,
                                         const char *host_locale,
                                         const char *host_xkb,
                                         const char *host_tz,
                                         const wchar_t *vm_name)
{
    wchar_t extras[MAX_PATH];
    CreateDirectoryW(staging, NULL);
    /* Populate staging\extras\ with the full agent + module + units tree. */
    stage_linux_agent_and_extras(staging, res_dir, ssh_enabled);

    swprintf_s(extras, MAX_PATH, L"%s\\extras", staging);
    if (GetFileAttributesW(extras) == INVALID_FILE_ATTRIBUTES) {
        asb_log(L"Linux extras dir was not created: %s", extras);
        return -1;
    }

    FILE *f = NULL;
    if (_wfopen_s(&f, manifest_path, L"w, ccs=UTF-8") != 0 || !f) {
        asb_log(L"Failed to open Linux manifest for write: %s", manifest_path);
        return -1;
    }
    /* Walk extras\ -> /opt/appsandbox/ */
    int n = write_manifest_walk_dir(f, extras, "/opt/appsandbox");

    /* Admin user + password hash. firstboot STEP 5 reads these to create
     * the login account. Without them, the firstboot falls back to a
     * static test/test123 (which is a security smell anyway). */
    if (admin_user && admin_user[0] && admin_pw_hash && admin_pw_hash[0]) {
        char user_utf8[128];
        WideCharToMultiByte(CP_UTF8, 0, admin_user, -1,
                            user_utf8, sizeof(user_utf8), NULL, NULL);
        n += stage_marker_file(f, staging, L"admin-user.marker",
                               user_utf8, strlen(user_utf8),
                               L"/etc/appsandbox-admin-user");
        n += stage_marker_file(f, staging, L"admin-hash.marker",
                               admin_pw_hash, strlen(admin_pw_hash),
                               L"/etc/appsandbox-admin-hash");
        asb_log(L"Linux admin: staged user=%s + $6$ hash marker", admin_user);
    } else {
        asb_log(L"Linux admin: no admin_user/admin_pass — firstboot will use test/test123");
    }

    if (ssh_enabled) {
        /* Zero-byte marker the firstboot keys on to install + enable
         * openssh-server. */
        n += stage_marker_file(f, staging, L"ssh-enabled.marker",
                               NULL, 0, L"/etc/appsandbox-ssh-enabled");
        asb_log(L"ssh_enabled: staged /etc/appsandbox-ssh-enabled marker");
    }

    /* Host-derived locale / keyboard / timezone. firstboot STEP 3 + 4
     * read these; absent markers fall back to en_US.UTF-8 / us / Etc/UTC. */
    if (host_locale && host_locale[0])
        n += stage_marker_file(f, staging, L"locale.marker",
                               host_locale, strlen(host_locale),
                               L"/etc/appsandbox-locale");
    if (host_xkb && host_xkb[0])
        n += stage_marker_file(f, staging, L"keyboard.marker",
                               host_xkb, strlen(host_xkb),
                               L"/etc/appsandbox-keyboard");
    if (host_tz && host_tz[0])
        n += stage_marker_file(f, staging, L"timezone.marker",
                               host_tz, strlen(host_tz),
                               L"/etc/appsandbox-timezone");
    asb_log(L"Host regional markers staged: locale=%hs keyboard=%hs tz=%hs",
            host_locale ? host_locale : "(none)",
            host_xkb ? host_xkb : "(none)",
            host_tz ? host_tz : "(none)");

    /* Hostname from the AppSandbox VM name (mirrors the Windows
     * ComputerName path). The web UI already validates the name to a
     * legal lowercase RFC-1123 hostname, so we write it verbatim;
     * firstboot STEP 2 reads it and falls back to "ubuntu" if absent. */
    if (vm_name && vm_name[0]) {
        char host_utf8[256];
        WideCharToMultiByte(CP_UTF8, 0, vm_name, -1,
                            host_utf8, sizeof(host_utf8), NULL, NULL);
        n += stage_marker_file(f, staging, L"hostname.marker",
                               host_utf8, strlen(host_utf8),
                               L"/etc/appsandbox-hostname");
        asb_log(L"Linux hostname: staged /etc/appsandbox-hostname = %s", vm_name);
    }

    fclose(f);
    asb_log(L"Linux staging manifest written: %s (%d file(s))", manifest_path, n);
    return n;
}

/* ---- Detect the Ubuntu release codename + kernel version from an ISO.
 *
 * Mounts the ISO read-only via VirtDisk, reads:
 *   - the only subdir under <iso>:\dists\        -> codename (e.g. "resolute")
 *   - <iso>:\pool\main\l\linux\linux-image-*-generic_*.deb
 *     -> kernel version (e.g. "7.0.0-14-generic") parsed from filename
 *
 * Returns 0 on success and fills the two output buffers; non-zero on
 * any error (caller logs + skips the build-deps prefetch).
 *
 * Output buffers should be at least 64 wchars each. */
static int detect_iso_kernel(const wchar_t *iso_path,
                             wchar_t *codename_out, size_t codename_cap,
                             wchar_t *kver_out, size_t kver_cap)
{
    HANDLE iso_handle = INVALID_HANDLE_VALUE;
    DWORD cdrom_before, cdrom_after, newly;
    wchar_t iso_drive = 0;
    int rc = -1;

    codename_out[0] = 0;
    kver_out[0]     = 0;

    /* Snapshot current CD-ROM drive letters before mount. */
    cdrom_before = 0;
    for (int i = 0; i < 26; i++) {
        wchar_t root[4]; swprintf_s(root, 4, L"%c:\\", (wchar_t)(L'A' + i));
        if (GetDriveTypeW(root) == DRIVE_CDROM) cdrom_before |= (1u << i);
    }

    /* Mount the ISO via VirtDisk. */
    {
        VIRTUAL_STORAGE_TYPE st;
        OPEN_VIRTUAL_DISK_PARAMETERS op;
        ZeroMemory(&st, sizeof(st));
        st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
        ZeroMemory(&op, sizeof(op));
        op.Version = OPEN_VIRTUAL_DISK_VERSION_1;

        if (OpenVirtualDisk(&st, iso_path,
                            VIRTUAL_DISK_ACCESS_READ,
                            OPEN_VIRTUAL_DISK_FLAG_NONE,
                            &op, &iso_handle) != ERROR_SUCCESS) {
            asb_log(L"detect_iso_kernel: OpenVirtualDisk failed (%lu)",
                    GetLastError());
            return -1;
        }
        if (AttachVirtualDisk(iso_handle, NULL,
                              ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY,
                              0, NULL, NULL) != ERROR_SUCCESS) {
            asb_log(L"detect_iso_kernel: AttachVirtualDisk failed (%lu)",
                    GetLastError());
            CloseHandle(iso_handle);
            return -1;
        }
    }

    /* Wait for the drive letter (≤10 s). */
    for (int i = 0; i < 20; i++) {
        cdrom_after = 0;
        for (int b = 0; b < 26; b++) {
            wchar_t root[4]; swprintf_s(root, 4, L"%c:\\", (wchar_t)(L'A' + b));
            if (GetDriveTypeW(root) == DRIVE_CDROM) cdrom_after |= (1u << b);
        }
        newly = cdrom_after & ~cdrom_before;
        if (newly) {
            for (int b = 0; b < 26; b++) {
                if (newly & (1u << b)) { iso_drive = (wchar_t)(L'A' + b); break; }
            }
            break;
        }
        Sleep(500);
    }
    if (!iso_drive) {
        asb_log(L"detect_iso_kernel: ISO mounted but no drive letter");
        goto cleanup;
    }

    /* Wait for the filesystem to become accessible (the drive letter
     * appears before the volume's FS is ready to serve directory
     * enumeration — without this wait, FindFirstFileW on <drive>:\dists\
     * returns ERROR_PATH_NOT_FOUND even though the path exists). Same
     * pattern as ubuntu_vhdx.c's iso mount: poll GetVolumeInformationW
     * until it succeeds. Up to ~15 s. */
    {
        wchar_t root[4]; swprintf_s(root, 4, L"%c:\\", iso_drive);
        wchar_t vol_label[64];
        int ready = 0;
        for (int i = 0; i < 30; i++) {
            if (GetVolumeInformationW(root, vol_label, 64, NULL, NULL, NULL, NULL, 0)) {
                ready = 1; break;
            }
            Sleep(500);
        }
        if (!ready) {
            asb_log(L"detect_iso_kernel: %c:\\ never became accessible", iso_drive);
            goto cleanup;
        }
    }

    /* 1. Codename: first non-dotfile subdir under <iso>:\dists\ */
    {
        wchar_t spec[MAX_PATH];
        swprintf_s(spec, MAX_PATH, L"%c:\\dists\\*", iso_drive);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW(spec, &fd);
        if (fh == INVALID_HANDLE_VALUE) {
            asb_log(L"detect_iso_kernel: %c:\\dists\\ not found", iso_drive);
            goto cleanup;
        }
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            wcsncpy_s(codename_out, codename_cap, fd.cFileName, _TRUNCATE);
            break;
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
        if (!codename_out[0]) {
            asb_log(L"detect_iso_kernel: no release subdir under dists/");
            goto cleanup;
        }
    }

    /* 2. Kernel version from linux-headers-<X.Y.Z-generic>_*_amd64.deb
     *    in pool/main/l/linux/. We use linux-headers- not linux-image-
     *    because the image .deb lives under pool/main/l/linux-signed/
     *    on signed-kernel Ubuntu releases, but the headers (which we
     *    need anyway for DKMS builds) are in pool/main/l/linux/ on all
     *    release flavors. The kver substring is identical.
     *
     *    Filename pattern (literal example):
     *      linux-headers-7.0.0-14-generic_7.0.0-14.14_amd64.deb
     *
     *    FindFirstFileW doesn't reliably handle multi-`*` patterns on
     *    ISO 9660; glob with single wildcard and filter for "-generic_". */
    {
        wchar_t spec[MAX_PATH];
        swprintf_s(spec, MAX_PATH,
            L"%c:\\pool\\main\\l\\linux\\linux-headers-*.deb", iso_drive);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW(spec, &fd);
        if (fh == INVALID_HANDLE_VALUE) {
            asb_log(L"detect_iso_kernel: no linux-headers-*.deb in pool/main/l/linux");
            goto cleanup;
        }
        int found = 0;
        do {
            /* Skip the meta-package "linux-headers-X.Y.Z_<ver>_all.deb"
             * (no "-generic_"). We want the per-flavor headers. */
            if (!wcsstr(fd.cFileName, L"-generic_")) continue;
            const wchar_t *p = wcsstr(fd.cFileName, L"linux-headers-");
            if (!p) continue;
            p += wcslen(L"linux-headers-");
            const wchar_t *u = wcschr(p, L'_');
            if (!u) continue;
            size_t n = (size_t)(u - p);
            if (n == 0 || n >= kver_cap) continue;
            wcsncpy_s(kver_out, kver_cap, p, n);
            kver_out[n] = 0;
            found = 1;
            break;
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
        if (!found) {
            asb_log(L"detect_iso_kernel: no linux-headers-*-generic_*.deb found");
            goto cleanup;
        }
    }

    asb_log(L"detect_iso_kernel: codename=%s kver=%s", codename_out, kver_out);
    rc = 0;

cleanup:
    if (iso_handle != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(iso_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(iso_handle);
    }
    return rc;
}

/* ---- Spawn iso-patch.exe with one of the --prefetch-* modes and
 * block. No cache layer: each call writes directly into <out_dir>
 * which is a subdir of the per-VM staging extras dir.
 *
 * args: extra argv tail (no quotes — caller is responsible for safe paths).
 * Returns 0 on success; -1 on any failure. Logs progress to asb_log. */
static int spawn_iso_patch_prefetch(const wchar_t *args)
{
    wchar_t exe_dir[MAX_PATH];
    GetModuleFileNameW(g_dll_module, exe_dir, MAX_PATH);
    { wchar_t *s = wcsrchr(exe_dir, L'\\'); if (s) *s = L'\0'; }

    wchar_t cmdline[2048];
    swprintf_s(cmdline, 2048,
        L"\"%s\\iso-patch.exe\" %s", exe_dir, args);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        asb_log(L"prefetch: CreateProcess failed (%lu) for: %s",
                GetLastError(), cmdline);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ec == 0 ? 0 : -1;
}

/* ---- Run iso-patch.exe --ubuntu-to-vhdx and stream its progress. ----
 *
 * Parallel helper for the Linux flow. Spawns iso-patch.exe, parses
 * STATUS:/PROGRESS:/ERROR:/DONE: stdout lines, updates the per-VM
 * progress callback. Same shape as the iso-patch invocation block
 * inside vhdx_create_thread, but for the Ubuntu mode (no LANG: handling).
 */
static HRESULT run_iso_patch_ubuntu(const wchar_t *iso_path,
                                     const wchar_t *vhdx_path,
                                     ULONG hdd_gb,
                                     const wchar_t *manifest_path,
                                     UINT64 vm_unique_id,
                                     wchar_t *error_msg, size_t error_msg_cap)
{
    wchar_t exe_dir[MAX_PATH], cmdline[2048];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hReadPipe = INVALID_HANDLE_VALUE, hWritePipe = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES sa;
    BYTE line_buf[4096];
    int pos = 0;
    DWORD bytes_read = 0, exit_code = 0;
    HRESULT result = E_FAIL;

    GetModuleFileNameW(g_dll_module, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\'); if (slash) *slash = L'\0';

    if (manifest_path && manifest_path[0]) {
        swprintf_s(cmdline, 2048,
            L"\"%s\\iso-patch.exe\" --ubuntu-to-vhdx \"%s\" --output \"%s\" --size-gb %lu --stage \"%s\"",
            exe_dir, iso_path, vhdx_path, hdd_gb, manifest_path);
    } else {
        swprintf_s(cmdline, 2048,
            L"\"%s\\iso-patch.exe\" --ubuntu-to-vhdx \"%s\" --output \"%s\" --size-gb %lu",
            exe_dir, iso_path, vhdx_path, hdd_gb);
    }

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        if (error_msg) swprintf_s(error_msg, error_msg_cap, L"CreatePipe failed (%lu)", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        if (error_msg) swprintf_s(error_msg, error_msg_cap, L"Failed to launch iso-patch.exe (%lu)", err);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return HRESULT_FROM_WIN32(err);
    }
    CloseHandle(hWritePipe);
    hWritePipe = INVALID_HANDLE_VALUE;

    {
        char *cbuf = (char *)line_buf;
        int cbuf_size = (int)sizeof(line_buf);
        while (ReadFile(hReadPipe, cbuf + pos, (DWORD)(cbuf_size - pos - 1), &bytes_read, NULL) && bytes_read > 0) {
            int end = pos + (int)bytes_read;
            int start = 0;
            cbuf[end] = '\0';
            for (int ci = start; ci < end; ci++) {
                if (cbuf[ci] == '\n' || cbuf[ci] == '\r') {
                    cbuf[ci] = '\0';
                    if (ci > start) {
                        char *line = cbuf + start;
                        if (strncmp(line, "PROGRESS:", 9) == 0) {
                            int pct = atoi(line + 9);
                            /* Match the Windows path exactly: staging flag
                               flips only on the literal "Staging" substring,
                               so genuine staging phases set it (e.g.
                               "Staging grub modules") and ext4/squashfs
                               progress lines do not. */
                            BOOL is_staging = (strstr(line + 9, "Staging") != NULL);
                            /* Resolve by stable id under g_cs: g_vms[] may
                               have been compacted while we were building (H13). */
                            VmInstance *pvm;
                            EnterCriticalSection(&g_cs);
                            pvm = asb_find_vm_by_id(vm_unique_id);
                            if (pvm) {
                                pvm->vhdx_progress = pct;
                                pvm->vhdx_staging = is_staging;
                            }
                            LeaveCriticalSection(&g_cs);
                            if (g_progress_cb && pvm)
                                g_progress_cb(vm_handle(pvm), pct, is_staging, g_progress_ud);
                        } else if (strncmp(line, "ERROR:", 6) == 0) {
                            if (error_msg && error_msg[0] == L'\0')
                                MultiByteToWideChar(CP_ACP, 0, line + 6, -1, error_msg, (int)error_msg_cap);
                            result = E_FAIL;
                        } else if (strncmp(line, "DONE:", 5) == 0) {
                            result = S_OK;
                        }
                    }
                    start = ci + 1;
                    if (start < end && (cbuf[start] == '\n' || cbuf[start] == '\r')) start++;
                }
            }
            if (start < end) { memmove(cbuf, cbuf + start, end - start); pos = end - start; }
            else pos = 0;
        }
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0 && result != S_OK) {
        if (error_msg && error_msg[0] == L'\0')
            swprintf_s(error_msg, error_msg_cap, L"iso-patch.exe exited with code %lu", exit_code);
        result = E_FAIL;
    }
    return result;
}

/* ---- Background Linux create thread (direct ISO -> VHDX) ----
 *
 * Spawns iso-patch.exe --ubuntu-to-vhdx, which builds a fully-installed
 * ext4 rootfs + signed shim/grub ESP directly from the user-picked
 * Ubuntu Desktop ISO. Host-side install (squashfs -> ext4), no in-guest
 * installer and no user interaction.
 *
 * First-boot configuration (hostname, user, network, OOBE skip,
 * grub-install/update-grub) runs from a one-shot systemd unit planted
 * into the rootfs at build time.
 */
typedef struct {
    VmConfig    config;
    int         vm_index;
    UINT64      vm_unique_id;  /* stable id; survives g_vms[] compaction (H13) */
    wchar_t     vhdx_dir[MAX_PATH];
    wchar_t     endpoint_guid[64];
    GUID        network_id;
    GUID        endpoint_id;
    BOOL        has_network;
    wchar_t     net_adapter[256];
    HRESULT     result;
    wchar_t     error_msg[512];
    VmInstance *vm_inst;
    BOOL        vhdx_created;
} LinuxCreateArgs;

static DWORD WINAPI linux_create_thread(LPVOID param)
{
    LinuxCreateArgs *args = (LinuxCreateArgs *)param;
    HRESULT hr;
    wchar_t exe_dir[MAX_PATH], res_dir[MAX_PATH];
    wchar_t staging[MAX_PATH], manifest[MAX_PATH];
    wchar_t *slash;

    /* ---- 1. Build the VHDX directly from the Ubuntu ISO via iso-patch.
       Host-side install (squashfs -> ext4), no in-guest installer. The
       first-boot service planted inside the rootfs handles per-VM
       configuration. ---- */
    asb_log(L"Building Linux VHDX from %s (size=%lu GB)...",
            args->config.image_path, args->config.hdd_gb);
    DeleteFileW(args->config.vhdx_path);
    /* resources_iso_path is unused for the direct-ISO flow but the field
       still exists in the config; clear it so hcs_vm doesn't try to attach. */
    args->config.resources_iso_path[0] = L'\0';

    /* (Detection + prefetch happen below, after we've decided on the
       staging dir, since the prefetches write directly into it.) */

    /* ---- 1a. Set up staging dir + run 3 prefetches that populate it
       directly (no host cache). Order doesn't matter functionally;
       sequential for simpler logging. Failures are non-fatal — VM
       still builds; firstboot STEP 17 verification will flag any
       missing artifacts in the rootfs. ---- */
    swprintf_s(staging, MAX_PATH, L"%s\\_vhdx_staging", args->vhdx_dir);
    CreateDirectoryW(staging, NULL);
    swprintf_s(manifest, MAX_PATH, L"%s\\manifest.txt", staging);

    {
        wchar_t extras[MAX_PATH], args_buf[2048];
        swprintf_s(extras, MAX_PATH, L"%s\\extras", staging);
        CreateDirectoryW(extras, NULL);

        /* Prefetch 1: repo source from GitHub. Writes agent-src/,
           asb_drm-src/, dxgkrnl-src/, systemd/, modprobe.d-asb_drm.conf,
           50-appsandbox-gpu, org.gnome.Shell-no-gpu.conf, appsandbox-gpu,
           wsl-mesa.tar.zst directly into <staging>/extras/. */
        asb_log(L"Prefetch 1/3: cloning repo source from GitHub...");
        /* Branch must match the branch this binary was built from, so the Linux
           guest builds its agent/driver source from the SAME revision. */
        swprintf_s(args_buf, 2048,
            L"--prefetch-repo --branch \"main\" --out-dir \"%s\"",
            extras);
        if (spawn_iso_patch_prefetch(args_buf) != 0)
            asb_log(L"WARN: prefetch-repo failed (agent + DKMS build will fail)");

        /* Prefetch 2: apt build-deps closure from archive.ubuntu.com.
           Needs (codename, kernel) detected from the ISO. */
        asb_log(L"Prefetch 2/3: apt build-deps closure...");
        wchar_t codename[64] = L"", kver[64] = L"";
        if (detect_iso_kernel(args->config.image_path,
                              codename, ARRAYSIZE(codename),
                              kver,     ARRAYSIZE(kver)) == 0) {
            wchar_t apt_out[MAX_PATH];
            swprintf_s(apt_out, MAX_PATH, L"%s\\local-apt-extras", extras);
            swprintf_s(args_buf, 2048,
                L"--prefetch-build-deps --codename \"%s\" --kernel \"%s\" "
                L"--out-dir \"%s\"",
                codename, kver, apt_out);
            if (spawn_iso_patch_prefetch(args_buf) != 0)
                asb_log(L"WARN: prefetch-build-deps failed");
        } else {
            asb_log(L"WARN: could not detect ISO kernel — skipping build-deps");
        }

        /* Prefetch 3: wsl-deps proprietary .so libs from Microsoft NuGet. */
        asb_log(L"Prefetch 3/3: wsl-deps .so libs...");
        wchar_t wsl_out[MAX_PATH];
        swprintf_s(wsl_out, MAX_PATH, L"%s\\wsl-deps", extras);
        swprintf_s(args_buf, 2048,
            L"--prefetch-wsl-deps --out-dir \"%s\"", wsl_out);
        if (spawn_iso_patch_prefetch(args_buf) != 0)
            asb_log(L"WARN: prefetch-wsl-deps failed");
    }

    GetModuleFileNameW(g_dll_module, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\'); if (slash) *slash = L'\0';
    swprintf_s(res_dir, MAX_PATH, L"%s\\resources", exe_dir);
    if (GetFileAttributesW(res_dir) == INVALID_FILE_ATTRIBUTES)
        wcscpy_s(res_dir, MAX_PATH, exe_dir);

    /* Hash the modal-supplied admin password into glibc $6$ format so the
       firstboot can drop it straight into /etc/shadow via `usermod -p`.
       Plaintext is wiped immediately after — same pattern as the Windows
       autounattend path. If the user left password blank, fall through
       with empty strings; generate_vhdx_manifest_ubuntu logs a warning
       and the guest falls back to the test/test123 placeholder. */
    char admin_pw_hash[256] = {0};
    if (args->config.admin_pass[0] != L'\0') {
        if (!unix_password_hash(args->config.admin_pass,
                                admin_pw_hash, sizeof(admin_pw_hash))) {
            asb_log(L"WARN: failed to hash admin_pass — falling back to test/test123");
            admin_pw_hash[0] = '\0';
        }
    }
    SecureZeroMemory(args->config.admin_pass, sizeof(args->config.admin_pass));

    /* Read the current user's Windows regional settings and translate to
     * Linux. Language is gated to the 8 ISO-preinstalled languages
     * (else en_US.UTF-8); keyboard + timezone always match the host. */
    char host_locale[64], host_xkb[32], host_tz[64];
    detect_host_locale_settings(host_locale, sizeof(host_locale),
                                host_xkb, sizeof(host_xkb),
                                host_tz, sizeof(host_tz));

    int n_staged = generate_vhdx_manifest_ubuntu(manifest, staging, res_dir,
                                                  args->config.ssh_enabled,
                                                  args->config.admin_user,
                                                  admin_pw_hash,
                                                  host_locale, host_xkb, host_tz,
                                                  args->config.name);
    SecureZeroMemory(admin_pw_hash, sizeof(admin_pw_hash));
    if (n_staged < 0) {
        args->result = E_FAIL;
        swprintf_s(args->error_msg, 512, L"Failed to generate Linux staging manifest");
        goto done;
    }

    hr = run_iso_patch_ubuntu(args->config.image_path, args->config.vhdx_path,
                              args->config.hdd_gb,
                              n_staged > 0 ? manifest : NULL,
                              args->vm_unique_id, args->error_msg, 512);
    if (FAILED(hr)) {
        args->result = hr;
        if (args->error_msg[0] == L'\0')
            swprintf_s(args->error_msg, 512,
                       L"Failed to build Linux VHDX from ISO (0x%08X)", hr);
        goto done;
    }
    args->vhdx_created = TRUE;

    /* The Ubuntu ISO has its own bootable EFI files; if we leave image_path
       set, hcs_vm.c attaches it at SCSI slot 1 alongside the installed
       VHDX, and Hyper-V's UEFI firmware sometimes picks the ISO's
       /EFI/BOOT/BOOTX64.EFI instead of ours. Detach the ISO from the
       runtime VM — the installed VHDX is fully self-bootable. */
    args->config.image_path[0] = L'\0';

    /* ---- 2. NAT IP allocation. The chosen static /24 address is baked
       into the HCN endpoint policy below and pushed to the guest at
       runtime by the agent (set_ip -> static netplan); the guest never
       DHCPs in NAT mode. ---- */
    if (args->config.network_mode == NET_NAT && args->vm_index >= 0 && args->vm_index < g_vm_count) {
        if (allocate_nat_ip(&g_vms[args->vm_index])) {
            asb_log(L"Allocated NAT IP %S for new VM.", g_vms[args->vm_index].nat_ip);
            save_vm_list();
        }
    }

    /* ---- 5. Network + endpoint (same as vhdx_create_thread:1114). ---- */
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
            BOOL is_nat = (args->config.network_mode == NET_NAT);
            size_t ip_size = (args->vm_index >= 0 && args->vm_index < g_vm_count)
                              ? sizeof(g_vms[args->vm_index].nat_ip) : 0;
            hr = try_endpoint_with_retry(&args->network_id, &args->endpoint_id,
                                          args->endpoint_guid, 64,
                                          nat_ip, ip_size, is_nat);
            if (SUCCEEDED(hr)) {
                args->has_network = TRUE;
                if (is_nat) save_vm_list();
            }
        }
        if (FAILED(hr))
            args->config.network_mode = NET_NONE;
    }

    /* ---- 6. HCS create + start (same as vhdx_create_thread:1145). ---- */
    {
        VmInstance temp_inst;
        ZeroMemory(&temp_inst, sizeof(temp_inst));

        hr = (args->endpoint_guid[0] != L'\0')
            ? hcs_create_vm_with_endpoint(&args->config, args->endpoint_guid, &temp_inst)
            : hcs_create_vm(&args->config, &temp_inst);
        if (FAILED(hr)) {
            args->result = hr;
            swprintf_s(args->error_msg, 512, L"Failed to create HCS VM (0x%08X)", hr);
            if (args->has_network) {
                hcn_delete_endpoint(&args->endpoint_id);
                args->has_network = FALSE;
                args->endpoint_guid[0] = L'\0';
            }
            goto done;
        }

        hr = hcs_start_vm(&temp_inst);
        if (FAILED(hr)) {
            args->result = hr;
            swprintf_s(args->error_msg, 512, L"Failed to start VM (0x%08X)", hr);
            hcs_close_vm(&temp_inst);
            if (args->has_network) {
                hcn_delete_endpoint(&args->endpoint_id);
                args->has_network = FALSE;
                args->endpoint_guid[0] = L'\0';
            }
            goto done;
        }

        {
            VmInstance *heap_inst = (VmInstance *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmInstance));
            if (heap_inst) memcpy(heap_inst, &temp_inst, sizeof(VmInstance));
            args->vm_inst = heap_inst;
        }

        args->result = S_OK;
    }

done:
    /* ---- Completion (same as vhdx_create_thread:1206). ---- */
    {
        VmInstance *inst;
        int idx;

        /* Resolve by stable id under g_cs: g_vms[] may have been compacted
           (entries shifted down) while we were building, so the cached
           vm_index can now point at a different VM or be out of range (H13). */
        EnterCriticalSection(&g_cs);
        inst = asb_find_vm_by_id(args->vm_unique_id);
        idx = inst ? (int)(inst - g_vms) : -1;
        if (inst) {
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
                    args->vm_inst = NULL;
                    hcs_register_vm_callback(inst);
                }
                LeaveCriticalSection(&g_cs);

                hcs_start_monitor(inst);
                vm_agent_start(inst);
                idd_probe_start(inst);
                asb_log(L"VM \"%s\" created and started.", inst->name);
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
                remove_dir_recursive(args->vhdx_dir);
                save_vm_list();
            }

            if (g_state_cb)
                g_state_cb(vm_handle(inst), inst->running, g_state_ud);
        } else {
            /* VM was deleted while building: don't leak the started HCS
               handle copy, and don't write into a stale slot. */
            LeaveCriticalSection(&g_cs);
            if (args->vm_inst) {
                hcs_unregister_vm_callback(args->vm_inst);
                hcs_close_vm(args->vm_inst);
                HeapFree(GetProcessHeap(), 0, args->vm_inst);
                args->vm_inst = NULL;
            }
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

    if (!hcn_init()) {
        asb_log(L"HCN: NOT available");
    } else {
        asb_log(L"HCN: Available");
        /* Tear down any stale networks from a previous run. Must happen
           before any VM is started so we don't delete a live network. */
        hcn_cleanup_stale_networks();
    }

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
       and exit - the VM keeps running after the process is gone.

       Unlike asb_cleanup(), this does NOT call hcs_terminate_vm().
       We unregister callbacks and stop threads, but do NOT close the HCS
       handle - HcsCloseComputeSystem terminates the VM when it's the last
       handle.  Instead, let the OS close it during process teardown after
       the callback is already gone. */
    for (i = 0; i < g_vm_count; i++) {
        hcs_stop_monitor(&g_vms[i]);
        vm_ssh_proxy_stop(&g_vms[i]);
        vm_agent_stop(&g_vms[i]);
        idd_probe_stop(&g_vms[i]);
        hcs_unregister_vm_callback(&g_vms[i]);
        g_vms[i].handle = NULL;  /* abandon handle - OS will close it */
    }

    g_initialized = FALSE;
}

/* Validate a user-supplied storage folder, and (as a side effect) create
   the target sandbox folder if it doesn't exist. Returns S_OK if OK or
   empty; E_INVALIDARG with an asb_log explanation on failure. Empty folder
   is OK (caller falls through to the default ProgramData path). Creates
   parent directories recursively via SHCreateDirectoryExW so multi-level
   paths like R:\new\nested\sandboxes work in one call. The later
   single-level CreateDirectoryW in asb_vm_create still runs but is then
   a harmless redundant call (returns ERROR_ALREADY_EXISTS, ignored). */
static HRESULT validate_storage_folder(const wchar_t *folder, const wchar_t *vm_name)
{
    wchar_t root[4];
    wchar_t target_disk[MAX_PATH];
    UINT drive_type;
    DWORD attrs;
    int sh_rc;

    if (!folder || !folder[0]) return S_OK;  /* empty = use default; nothing to validate */

    /* 1. Absolute Windows path: <letter>:\... */
    if (wcslen(folder) < 3 ||
        !((folder[0] >= L'A' && folder[0] <= L'Z') || (folder[0] >= L'a' && folder[0] <= L'z')) ||
        folder[1] != L':' || folder[2] != L'\\') {
        asb_log(L"Error: Storage folder must be an absolute path like R:\\sandboxes (got: %s)", folder);
        return E_INVALIDARG;
    }

    /* 2. Drive is a local fixed volume. */
    root[0] = folder[0]; root[1] = L':'; root[2] = L'\\'; root[3] = L'\0';
    drive_type = GetDriveTypeW(root);
    if (drive_type != DRIVE_FIXED) {
        asb_log(L"Error: Storage folder must be on a local fixed drive, not network/removable (%s, type=%u)",
                root, drive_type);
        return E_INVALIDARG;
    }

    /* 3. Combined path fits in MAX_PATH. The downstream construction is
       <folder>\<name>\disk.vhdx — 11 fixed chars (separator + "\\disk.vhdx").
       Reject early so swprintf_s never silently empties the buffer. */
    if (wcslen(folder) + wcslen(vm_name) + 11 >= MAX_PATH) {
        asb_log(L"Error: Storage folder path is too long; folder + sandbox name + \\disk.vhdx must fit in %d characters.",
                MAX_PATH);
        return E_INVALIDARG;
    }

    /* 4. The target sandbox folder is creatable (recursively). Use
       SHCreateDirectoryExW so that something like R:\new\nested\path\<name>
       gets all intermediate levels created in one call. Treat
       ERROR_FILE_EXISTS / ERROR_ALREADY_EXISTS as success. */
    {
        wchar_t target_dir[MAX_PATH];
        swprintf_s(target_dir, MAX_PATH, L"%s\\%s", folder, vm_name);
        sh_rc = SHCreateDirectoryExW(NULL, target_dir, NULL);
        if (sh_rc != ERROR_SUCCESS &&
            sh_rc != ERROR_FILE_EXISTS &&
            sh_rc != ERROR_ALREADY_EXISTS) {
            asb_log(L"Error: Can't create storage folder %s (Win32 error %d)",
                    target_dir, sh_rc);
            return E_INVALIDARG;
        }
    }

    /* 5. No existing disk.vhdx at the target sandbox folder. */
    swprintf_s(target_disk, MAX_PATH, L"%s\\%s\\disk.vhdx", folder, vm_name);
    attrs = GetFileAttributesW(target_disk);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        asb_log(L"Error: A sandbox already exists at %s. Pick a different folder or sandbox name.",
                target_disk);
        return E_INVALIDARG;
    }

    return S_OK;
}

/* ---- VM Create ---- */

ASB_API HRESULT asb_vm_create(const AsbVmConfig *config)
{
    VmConfig cfg;
    VmInstance *inst;
    wchar_t vhdx_dir[MAX_PATH];
    wchar_t endpoint_guid_str[64] = { 0 };
    wchar_t ssh_pubkey[512] = { 0 };
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
    if (config->storage_folder) wcscpy_s(cfg.storage_folder, MAX_PATH, config->storage_folder);
    cfg.ram_mb = config->ram_mb;
    cfg.hdd_gb = config->hdd_gb;
    cfg.cpu_cores = config->cpu_cores;
    cfg.gpu_mode = config->gpu_mode;
    cfg.network_mode = config->network_mode;
    cfg.test_mode = config->test_mode;
    cfg.ssh_enabled = config->ssh_enabled;
    /* Key deploy needs SSH; prepare the AppSandbox keypair now so the build path
       stores the public key on the instance (the agent deploys it at runtime). */
    cfg.ssh_deploy_key = config->ssh_deploy_key && config->ssh_enabled;
    if (cfg.ssh_deploy_key && !ensure_appsandbox_ssh_key(ssh_pubkey, 512)) {
        asb_log(L"ssh key: could not prepare AppSandbox key; creating without key deploy.");
        cfg.ssh_deploy_key = FALSE;
    }
    is_template_create = config->is_template;
    cfg.is_template = is_template_create;

    /* Validate user-supplied storage folder (if any). Empty folder is OK. */
    if (!is_template_create) {
        HRESULT vhr = validate_storage_folder(cfg.storage_folder, cfg.name);
        if (FAILED(vhr)) return vhr;
    }

    /* Defaults */
    if (cfg.hdd_gb == 0) cfg.hdd_gb = 64;
    if (cfg.ram_mb == 0) cfg.ram_mb = 4096;
    if (cfg.cpu_cores == 0) cfg.cpu_cores = 4;

    /* Linux defaults: respect the UI's gpu_mode (GPU-PV works via
       DKMS-built dxgkrnl + asb_drm). Force test_mode=TRUE so the
       unsigned out-of-tree .ko's load — Secure Boot would otherwise
       reject them, and we don't ship a MOK enrollment flow. */
    if (_wcsicmp(cfg.os_type, L"Linux") == 0) {
        cfg.test_mode = TRUE;
    }

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

    /* Linux v1: require an ISO. Templates aren't supported for Linux yet. */
    if (_wcsicmp(cfg.os_type, L"Linux") == 0) {
        if (is_template_create) {
            asb_log(L"Error: Linux templates are not supported.");
            return E_INVALIDARG;
        }
        if (from_template) {
            asb_log(L"Error: Linux cannot be created from a template.");
            return E_INVALIDARG;
        }
        if (cfg.image_path[0] == L'\0') {
            asb_log(L"Error: Linux requires an ISO path.");
            return E_INVALIDARG;
        }
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
    inst->unique_id = g_next_vm_id++;

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
            /* Templates always live host-wide in ProgramData. */
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\templates", base_dir);
            CreateDirectoryW(vhdx_dir, NULL);
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\templates\\%s", base_dir, cfg.name);
        } else if (cfg.storage_folder[0]) {
            /* User-chosen folder: <storage_folder>\<name>. */
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\%s", cfg.storage_folder, cfg.name);
        } else {
            /* Default: %ProgramData%\AppSandbox\<name>. */
            swprintf_s(vhdx_dir, MAX_PATH, L"%s\\AppSandbox\\%s", base_dir, cfg.name);
        }
    }
    CreateDirectoryW(vhdx_dir, NULL);
    swprintf_s(cfg.vhdx_path, MAX_PATH, L"%s\\disk.vhdx", vhdx_dir);

    /* GPU driver shares */
    if ((cfg.gpu_mode == GPU_DEFAULT || cfg.gpu_mode == GPU_MIRROR) && !is_template_create) {
        gpu_get_driver_shares(&g_gpu_list, &cfg.gpu_shares);
        /* Linux guests additionally consume %SystemRoot%\System32\lxss\lib
         * (NVIDIA's WSL-staged Linux userspace .so files), mounted at
         * /usr/lib/wsl/lib by the agent on first connect. */
        if (_wcsicmp(cfg.os_type, L"Linux") == 0)
            gpu_append_lxsslib_share(&cfg.gpu_shares);
        else if (_wcsicmp(cfg.os_type, L"Windows") == 0)
            prepare_gl_layers_share(&cfg.gpu_shares);
    }

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
            wcscpy_s(inst->gpu_name, 256, cfg.gpu_mode == GPU_MIRROR ? L"Try all" :
                                          cfg.gpu_mode == GPU_DEFAULT ? L"Default GPU" : L"None");
            inst->network_mode = cfg.network_mode;
            inst->is_template = is_template_create;
            inst->test_mode = cfg.test_mode;
            wcscpy_s(inst->admin_user, 128, cfg.admin_user);
            inst->ssh_enabled = cfg.ssh_enabled;
            inst->ssh_deploy_key = cfg.ssh_deploy_key;
            if (cfg.ssh_deploy_key) wcscpy_s(inst->ssh_pubkey, 512, ssh_pubkey);
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
            args->vm_unique_id = inst->unique_id;
            wcscpy_s(args->vhdx_dir, MAX_PATH, vhdx_dir);
            wcscpy_s(args->net_adapter, 256, inst->net_adapter);

            asb_log(L"Building VHDX for \"%s\" (this may take several minutes)...", cfg.name);
            CloseHandle(CreateThread(NULL, 0, vhdx_create_thread, args, 0, NULL));

            if (g_state_cb) g_state_cb(vm_handle(inst), FALSE, g_state_ud);
            return S_OK;
        }
    }

    /* ---- Linux creation path (parallel to use_vhdx_first) ----
     *
     * Linux VMs build a bootable VHDX directly from the user-picked Ubuntu
     * Desktop ISO. The build (squashfs -> ext4 + host-side prefetches) is
     * slow, so the whole flow runs on a worker thread (linux_create_thread);
     * the UI thread returns immediately. */
    {
        /* image_path is the Ubuntu Desktop installer ISO the user picked;
           the VHDX is built from it on the host. admin_pass flows through
           to the guest's firstboot as a $6$ hash (see linux_create_thread). */
        BOOL use_linux_cloud = (!from_template && !is_template_create &&
                                _wcsicmp(cfg.os_type, L"Linux") == 0 &&
                                cfg.image_path[0] != L'\0');

        if (use_linux_cloud) {
            LinuxCreateArgs *args;

            wcscpy_s(inst->name, 256, cfg.name);
            wcscpy_s(inst->os_type, 32, cfg.os_type);
            wcscpy_s(inst->vhdx_path, MAX_PATH, cfg.vhdx_path);
            /* image_path isn't used by Linux but copy it through anyway so
               vms.cfg round-trips cleanly (UI sends it as the version tag). */
            wcscpy_s(inst->image_path, MAX_PATH, cfg.image_path);
            inst->ram_mb = cfg.ram_mb;
            inst->hdd_gb = cfg.hdd_gb;
            inst->cpu_cores = cfg.cpu_cores;
            inst->gpu_mode = cfg.gpu_mode;
            wcscpy_s(inst->gpu_name, 256, cfg.gpu_mode == GPU_MIRROR ? L"Try all" :
                                          cfg.gpu_mode == GPU_DEFAULT ? L"Default GPU" : L"None");
            inst->network_mode = cfg.network_mode;
            inst->is_template = FALSE;
            inst->test_mode = cfg.test_mode;
            wcscpy_s(inst->admin_user, 128, cfg.admin_user);
            inst->ssh_enabled = cfg.ssh_enabled;
            inst->ssh_deploy_key = cfg.ssh_deploy_key;
            if (cfg.ssh_deploy_key) wcscpy_s(inst->ssh_pubkey, 512, ssh_pubkey);
            inst->building_vhdx = TRUE;
            inst->vhdx_progress = 0;
            memcpy(&inst->gpu_shares, &cfg.gpu_shares, sizeof(GpuDriverShareList));

            { wchar_t sd[MAX_PATH]; swprintf_s(sd, MAX_PATH, L"%s\\snapshots", vhdx_dir);
              snapshot_init(&g_snap_trees[g_vm_count], sd); }
            g_vm_count++;

            vm_save_state_json(cfg.vhdx_path, FALSE);
            save_vm_list();

            args = (LinuxCreateArgs *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LinuxCreateArgs));
            if (!args) {
                asb_log(L"Error: Out of memory for Linux create args.");
                g_vm_count--;
                return E_OUTOFMEMORY;
            }
            memcpy(&args->config, &cfg, sizeof(VmConfig));
            args->vm_index = g_vm_count - 1;
            args->vm_unique_id = inst->unique_id;
            wcscpy_s(args->vhdx_dir, MAX_PATH, vhdx_dir);
            wcscpy_s(args->net_adapter, 256, inst->net_adapter);

            asb_log(L"Building Linux VM \"%s\" (direct ISO->VHDX, ~3 minutes)...", cfg.name);
            CloseHandle(CreateThread(NULL, 0, linux_create_thread, args, 0, NULL));

            /* Wipe the plaintext password from the local cfg; the worker
               thread has its own heap copy in args->config and wipes that
               after use (run_iso_patch_ubuntu doesn't consume it today —
               firstboot.sh uses a static test/test123 — but the field is
               retained on the config for future password injection via
               --stage manifest). */
            SecureZeroMemory(cfg.admin_pass, sizeof(cfg.admin_pass));

            if (g_state_cb) g_state_cb(vm_handle(inst), FALSE, g_state_ud);
            return S_OK;
        }
    }

    /* ---- Synchronous path (from-template, non-Windows, etc.) ---- */

    /* Populate name early so log lines (e.g. NAT IP allocation) identify the VM. */
    wcscpy_s(inst->name, 256, cfg.name);

    /* Create disk (synchronous path: from-template only — Linux ISO
       creates go through the use_linux_cloud branch above; Windows ISO
       installs go through use_vhdx_first above). */
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
            /* Linux: handled entirely in linux_create_thread via the
               use_linux_cloud branch above. No synchronous Linux path
               here. */
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
            hr = try_endpoint_with_retry(&inst->network_id, &inst->endpoint_id,
                                          endpoint_guid_str, 64,
                                          inst->nat_ip, sizeof(inst->nat_ip),
                                          cfg.network_mode == NET_NAT);
            if (SUCCEEDED(hr) && cfg.network_mode == NET_NAT) save_vm_list();
            if (FAILED(hr)) {
                asb_log(L"Warning: Endpoint failed (0x%08X).", hr);
                /* Leave the shared network alone - other VMs may be using it. */
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
        /* HCS rejected the VM after we already created the HCN endpoint.
           Free the endpoint so its IP reservation doesn't leak. */
        if (endpoint_guid_str[0] != L'\0') {
            hcn_delete_endpoint(&inst->endpoint_id);
            endpoint_guid_str[0] = L'\0';
        }
        return hr;
    }

    wcscpy_s(inst->gpu_name, 256, cfg.gpu_mode == GPU_MIRROR ? L"Try all" :
                                  cfg.gpu_mode == GPU_DEFAULT ? L"Default GPU" : L"None");
    wcscpy_s(inst->resources_iso_path, MAX_PATH, cfg.resources_iso_path);
    /* hcs_create_vm copies ssh_enabled onto the instance but not the deploy
       fields, so the from-template path sets them here (the ISO and Linux
       paths set them inline on their own instance copies). */
    inst->ssh_deploy_key = cfg.ssh_deploy_key;
    if (cfg.ssh_deploy_key) wcscpy_s(inst->ssh_pubkey, 512, ssh_pubkey);

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
        /* Need to re-create HCS system - do it in a background thread */
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
    /* hcs_stop_vm fire-and-forgets the agent "shutdown" command (it does NOT wait
       for the reply -- see vm_agent_send), so this returns promptly and never
       blocks the single-threaded HTTP request loop / the GUI thread. The guest
       acks then powers off; the HCS SystemExited monitor flips it to stopped. */
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
    inst->ssh_key_deployed = FALSE;   /* disk may change while stopped (revert) -- re-deploy next boot */
    hcs_stop_monitor(inst);
    vm_ssh_proxy_stop(inst);
    vm_agent_stop(inst);
    idd_probe_stop(inst);

    asb_log(L"Force stopping VM \"%s\"...", inst->name);
    hr = hcs_terminate_vm(inst);
    if (FAILED(hr))
        asb_log(L"Error: Terminate failed (0x%08X)", hr);

    hcs_close_vm(inst);

    asb_vm_cleanup_network(inst);
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

    asb_vm_cleanup_network(inst);

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

    /* Recursively remove the whole VM folder, including subdirectories
       (snapshots\ and the build's _vhdx_staging\); a file-only delete would
       leave those and orphan the dir. */
    remove_dir_recursive(dir);

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

ASB_API BOOL asb_vm_idd_ready(AsbVm vm)
{
    VmInstance *inst = vm_inst(vm);
    if (!inst || !inst->running) return FALSE;
    /* Both inputs come from the guest agent, never from a frame-channel probe:
       agent_online means the guest is up, and idd_ready is latched from the
       agent's idd_status report (the display driver is "running"). Connecting to
       the frame service to "check" would itself become the single consumer and
       blank the display, so readiness is a passive read of these latched flags. */
    return inst->agent_online && inst->idd_ready;
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
    wcscpy_s(g_vms[idx].gpu_name, 256, gpu_mode == GPU_MIRROR ? L"Try all" :
                                        gpu_mode == GPU_DEFAULT ? L"Default GPU" : L"None");
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
            snap_idx == -2 ? L"base"
              : (snap_idx >= 0 && snap_idx < g_snap_trees[idx].count
                   ? g_snap_trees[idx].nodes[snap_idx].name : L"?"));
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

ASB_API int asb_snap_base_branch_count(AsbVm vm)
{
    int idx = vm_index_of(vm);
    if (idx < 0) return 0;
    return g_snap_trees[idx].base_branch_count;
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
    while (!inst->running && !inst->building_vhdx) {
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
               or monitor - the start path will destroy the stale system and
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
