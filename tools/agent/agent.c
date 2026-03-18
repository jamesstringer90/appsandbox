/*
 * appsandbox-agent.exe — Guest-side Windows service for AppSandbox.
 *
 * Listens on a Hyper-V socket (AF_HYPERV) for commands from the host.
 * Maintains a persistent connection: sends "hello" on connect, then
 * periodic "heartbeat" messages. Processes commands inline.
 *
 * Also handles GPU driver file copy via embedded 9P client (p9copy)
 * when the host sends gpu_query_response with share metadata.
 *
 * Supports: ping, shutdown, restart, gpu_copy, gpu_query_response,
 *           gpu_none, idd_connect.
 *
 * Usage:
 *   appsandbox-agent.exe --install   Install and start the service
 *   appsandbox-agent.exe --remove    Stop and remove the service
 *   (no args)                        Run as Windows service (SCM only)
 */

#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdarg.h>
#include "p9copy.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

/* GUID_DEVCLASS_DISPLAY = {4D36E968-E325-11CE-BFC1-08002BE10318} */
static const GUID GUID_DISPLAY_CLASS =
    { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

/* DEVPKEY_Device_DriverInfPath = {A8B865DD-2E3D-4094-AD97-E593A70C75D6}, 5 */
static const DEVPROPKEY DPKEY_DriverInfPath =
    { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 5 };

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* HV_GUID_WILDCARD = {00000000-0000-0000-0000-000000000000} */
static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Must match VM_AGENT_SERVICE_GUID_STR in vm_agent.h
   {A5B0CAFE-0001-4000-8000-000000000001} */
static const GUID AGENT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0001, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Service constants ---- */

#define SERVICE_NAME    "AppSandboxAgent"
#define DISPLAY_NAME    "AppSandbox Guest Agent"

static SERVICE_STATUS        g_status;
static SERVICE_STATUS_HANDLE g_status_handle;
static HANDLE                g_stop_event;
static SOCKET                g_listen_sock = INVALID_SOCKET;
static SOCKET                g_client_sock = INVALID_SOCKET; /* Active persistent connection */
static volatile BOOL         g_os_shutting_down = FALSE;
static CRITICAL_SECTION      g_send_cs;     /* Protects send_line from concurrent callers */

/* ---- Logging ---- */

static int send_line(SOCKET s, const char *msg);

static void agent_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\agent.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* Send a log message to the host via the command channel (non-recursive, won't call agent_log) */
static void agent_log_to_host(const char *fmt, ...)
{
    char msg[1024];
    char line[1040];
    va_list ap;

    if (g_client_sock == INVALID_SOCKET) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    snprintf(line, sizeof(line), "log:%s", msg);
    line[sizeof(line) - 1] = '\0';
    send_line(g_client_sock, line);
}

/* ---- p9copy log adapter ---- */

/* Wraps agent_log for p9copy's P9LogFn signature (identical, but explicit). */
static void p9copy_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\agent.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- GPU copy state ---- */

#define MAX_GPU_SHARES 64

typedef struct {
    char share_name[128];
    char dest_path[512];    /* wchar_t as UTF-8 — converted when used */
    char filter[4096];
} GpuShareInfo;

typedef struct {
    GpuShareInfo shares[MAX_GPU_SHARES];
    int          count;
    SOCKET       notify_sock;    /* Socket to send progress/done to host */
    volatile BOOL copying;
} GpuCopyState;

static GpuCopyState g_gpu_copy = {0};

/* Forward declarations — defined later but needed by GPU copy */
static int recv_line(SOCKET s, char *buf, int buf_size);
static BOOL enable_privilege(LPCWSTR priv_name);

/* ---- GPU copy background thread ---- */

/* Check if any Display class device using vrd.inf has error code 43.
   Returns TRUE if at least one vrd.inf device has problem 43. */
static BOOL check_gpu_error43(void)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    BOOL found_error43 = FALSE;

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        agent_log("GPU check: SetupDiGetClassDevs failed (%lu).", GetLastError());
        return FALSE;
    }

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        ULONG status = 0, problem = 0;
        CONFIGRET cr;
        wchar_t dev_id[512] = {0};

        CM_Get_Device_IDW(dev_info.DevInst, dev_id, 512, 0);

        /* Get driver inf path */
        if (!SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath,
                                        &prop_type, (PBYTE)inf_path,
                                        sizeof(inf_path), NULL, 0)) {
            continue;
        }

        /* Only care about vrd.inf (GPU-PV virtual render driver) */
        if (_wcsicmp(inf_path, L"vrd.inf") != 0)
            continue;

        agent_log("GPU check: found vrd.inf device: %ls", dev_id);

        cr = CM_Get_DevNode_Status(&status, &problem, dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU check: CM_Get_DevNode_Status failed (%lu) for %ls.", cr, dev_id);
            continue;
        }

        if (problem == 43) {
            agent_log("GPU check: device %ls has error 43.", dev_id);
            found_error43 = TRUE;
        } else {
            agent_log("GPU check: device %ls OK (status=0x%lx, problem=%lu).",
                      dev_id, status, problem);
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
    return found_error43;
}

static void disable_hyperv_video(SOCKET notify_sock);


/* Disable and re-enable vrd.inf display devices.
   If only_error43 is TRUE, only cycles devices with problem code 43.
   If FALSE, cycles all vrd.inf devices (used at preshutdown). */
static void cycle_gpu_devices(SOCKET notify_sock, BOOL only_error43)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    char msg[512];

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return;

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        DEVPROPTYPE prop_type;
        wchar_t inf_path[MAX_PATH] = {0};
        ULONG status = 0, problem = 0;
        CONFIGRET cr;
        wchar_t dev_id[512] = {0};

        CM_Get_Device_IDW(dev_info.DevInst, dev_id, 512, 0);

        if (!SetupDiGetDevicePropertyW(devs, &dev_info, &DPKEY_DriverInfPath,
                                        &prop_type, (PBYTE)inf_path,
                                        sizeof(inf_path), NULL, 0))
            continue;

        if (_wcsicmp(inf_path, L"vrd.inf") != 0)
            continue;

        if (only_error43) {
            cr = CM_Get_DevNode_Status(&status, &problem, dev_info.DevInst, 0);
            if (cr != CR_SUCCESS || problem != 43)
                continue;
        }

        agent_log("GPU cycle: disabling %ls...", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:disabling %ls", dev_id);
        if (notify_sock != INVALID_SOCKET) send_line(notify_sock, msg);

        cr = CM_Disable_DevNode(dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU cycle: CM_Disable_DevNode failed (%lu).", cr);
            sprintf_s(msg, sizeof(msg), "gpu_device_status:disable failed (%lu)", cr);
            if (notify_sock != INVALID_SOCKET) send_line(notify_sock, msg);
            continue;
        }

        Sleep(1000);

        agent_log("GPU cycle: re-enabling %ls...", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:re-enabling %ls", dev_id);
        if (notify_sock != INVALID_SOCKET) send_line(notify_sock, msg);

        cr = CM_Enable_DevNode(dev_info.DevInst, 0);
        if (cr != CR_SUCCESS) {
            agent_log("GPU cycle: CM_Enable_DevNode failed (%lu).", cr);
            sprintf_s(msg, sizeof(msg), "gpu_device_status:enable failed (%lu)", cr);
            if (notify_sock != INVALID_SOCKET) send_line(notify_sock, msg);
            continue;
        }

        agent_log("GPU cycle: device %ls re-enabled.", dev_id);
        sprintf_s(msg, sizeof(msg), "gpu_device_status:re-enabled %ls", dev_id);
        if (notify_sock != INVALID_SOCKET) send_line(notify_sock, msg);
    }

    SetupDiDestroyDeviceInfoList(devs);
}

/* WTS constants — defined manually to avoid wtsapi32.h dependency (dynamic load) */
#define MY_WTS_USERNAME 5  /* WTS_INFO_CLASS value for WTSUserName */

/* Wait for a real user to log in (not defaultuser0 or SYSTEM).
   Polls WTS sessions every 2 seconds up to timeout_ms.
   Returns the session ID, or 0xFFFFFFFF on timeout. */
static DWORD wait_for_user_login(SOCKET notify_sock, int timeout_ms,
                                  const char *notify_msg)
{
    typedef BOOL (WINAPI *PFN_WTSQuerySessionInformationW)(
        HANDLE, DWORD, DWORD, LPWSTR *, DWORD *);
    typedef void (WINAPI *PFN_WTSFreeMemory)(PVOID);

    HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
    PFN_WTSQuerySessionInformationW pfnQuery;
    PFN_WTSFreeMemory pfnFree;
    DWORD start, now;

    if (!wts) {
        agent_log("wait_for_user_login: cannot load wtsapi32.dll.");
        return 0xFFFFFFFF;
    }

    pfnQuery = (PFN_WTSQuerySessionInformationW)
        GetProcAddress(wts, "WTSQuerySessionInformationW");
    pfnFree = (PFN_WTSFreeMemory)GetProcAddress(wts, "WTSFreeMemory");

    if (!pfnQuery || !pfnFree) {
        agent_log("wait_for_user_login: WTS functions not found.");
        FreeLibrary(wts);
        return 0xFFFFFFFF;
    }

    agent_log("Waiting for user login...");
    if (notify_sock != INVALID_SOCKET && notify_msg)
        send_line(notify_sock, notify_msg);

    start = GetTickCount();

    for (;;) {
        DWORD session_id = WTSGetActiveConsoleSessionId();
        if (session_id != 0xFFFFFFFF) {
            LPWSTR username = NULL;
            DWORD size = 0;

            if (pfnQuery(NULL, session_id, MY_WTS_USERNAME, &username, &size)) {
                if (username && wcslen(username) > 0 &&
                    _wcsicmp(username, L"defaultuser0") != 0 &&
                    _wcsicmp(username, L"SYSTEM") != 0) {
                    agent_log("User '%ls' logged in (session %lu).", username, session_id);
                    pfnFree(username);
                    FreeLibrary(wts);
                    return session_id;
                }
                if (username) pfnFree(username);
            }
        }

        now = GetTickCount();
        if ((now - start) >= (DWORD)timeout_ms) {
            agent_log("wait_for_user_login: timed out after %d ms.", timeout_ms);
            FreeLibrary(wts);
            return 0xFFFFFFFF;
        }

        Sleep(2000);
    }
}

/* Log off a specific user session via WTSLogoffSession. */
static BOOL logoff_session(DWORD session_id)
{
    typedef BOOL (WINAPI *PFN_WTSLogoffSession)(HANDLE, DWORD, BOOL);

    HMODULE wts = LoadLibraryW(L"wtsapi32.dll");
    PFN_WTSLogoffSession pfnLogoff;
    BOOL result;

    if (!wts) return FALSE;

    pfnLogoff = (PFN_WTSLogoffSession)GetProcAddress(wts, "WTSLogoffSession");
    if (!pfnLogoff) {
        FreeLibrary(wts);
        return FALSE;
    }

    agent_log("Logging off session %lu...", session_id);
    result = pfnLogoff(NULL, session_id, TRUE /* bWait */);
    if (!result)
        agent_log("WTSLogoffSession failed: %lu", GetLastError());
    else
        agent_log("Session %lu logged off.", session_id);

    FreeLibrary(wts);
    return result;
}

static DWORD WINAPI gpu_copy_thread(LPVOID param)
{
    GpuCopyState *state = (GpuCopyState *)param;
    int i;
    int total_files = 0;
    int failed_shares = 0;
    char msg[256];

    agent_log("GPU copy starting (%d shares)...", state->count);

    for (i = 0; i < state->count; i++) {
        GpuShareInfo *si = &state->shares[i];
        wchar_t dest_wide[MAX_PATH];
        int files = 0;
        int rc;

        MultiByteToWideChar(CP_UTF8, 0, si->dest_path, -1, dest_wide, MAX_PATH);

        agent_log("GPU copy share %d/%d: %s -> %s%s%s",
                  i + 1, state->count, si->share_name, si->dest_path,
                  si->filter[0] ? " [filter: " : "",
                  si->filter[0] ? si->filter : "");

        rc = p9_copy_share(50001, si->share_name, dest_wide,
                           si->filter[0] ? si->filter : NULL, &files);

        if (rc != P9_OK) {
            agent_log("GPU copy share '%s' failed (rc=%d).", si->share_name, rc);
            failed_shares++;
        } else {
            agent_log("GPU copy share '%s' done (%d files).", si->share_name, files);
        }
        total_files += files;

        /* Send progress to host */
        sprintf_s(msg, sizeof(msg), "gpu_copy_progress:%d/%d", i + 1, state->count);
        if (state->notify_sock != INVALID_SOCKET)
            send_line(state->notify_sock, msg);
    }

    /* If files were copied and vrd.inf has error 43, restart GPU + IDD devices
       and re-disable Hyper-V Video adapter. */
    if (total_files > 0 && check_gpu_error43()) {
        agent_log("GPU drivers copied and error 43 detected - attempting device restart.");
        cycle_gpu_devices(state->notify_sock, TRUE);

        /* TODO: re-enable once IDD display is stable
        Sleep(2000);
        disable_hyperv_video(state->notify_sock);
        */
    } else if (total_files > 0) {
        agent_log("GPU drivers copied, no error 43 - no device restart needed.");
    } else {
        agent_log("All GPU driver files already present (pre-staged) - no copy or restart needed.");
    }

    /* Send final result to host */
    if (failed_shares == 0) {
        sprintf_s(msg, sizeof(msg), "gpu_copy_done:%d", total_files);
        agent_log("GPU copy complete: %d files, %d shares.", total_files, state->count);
    } else {
        sprintf_s(msg, sizeof(msg), "gpu_copy_error:%d/%d shares failed",
                  failed_shares, state->count);
        agent_log("GPU copy finished with errors: %d/%d shares failed.",
                  failed_shares, state->count);
    }

    if (state->notify_sock != INVALID_SOCKET)
        send_line(state->notify_sock, msg);

    state->copying = FALSE;
    return 0;
}

/* Parse gpu_query_response and start background copy.
   Format: "gpu_query_response:N" followed by N lines of "share|dest|filter"
   (filter may be empty). */
static void handle_gpu_query_response(SOCKET client, int share_count)
{
    int i;
    HANDLE thread;

    if (share_count <= 0 || share_count > MAX_GPU_SHARES) {
        agent_log("GPU query response: invalid share count %d.", share_count);
        return;
    }

    if (g_gpu_copy.copying) {
        agent_log("GPU copy already in progress, ignoring.");
        return;
    }

    g_gpu_copy.count = 0;

    for (i = 0; i < share_count; i++) {
        char line[8192];
        int n = recv_line(client, line, sizeof(line));
        if (n <= 0) {
            agent_log("GPU query: failed to read share line %d.", i);
            return;
        }

        /* Parse "share_name|dest_path|filter" */
        {
            char *p1, *p2;
            GpuShareInfo *si = &g_gpu_copy.shares[g_gpu_copy.count];

            p1 = strchr(line, '|');
            if (!p1) {
                agent_log("GPU query: malformed share line: %s", line);
                continue;
            }
            *p1 = '\0';
            p1++;

            p2 = strchr(p1, '|');
            if (p2) {
                *p2 = '\0';
                p2++;
                strncpy_s(si->filter, sizeof(si->filter), p2, _TRUNCATE);
            } else {
                si->filter[0] = '\0';
            }

            strncpy_s(si->share_name, sizeof(si->share_name), line, _TRUNCATE);
            strncpy_s(si->dest_path, sizeof(si->dest_path), p1, _TRUNCATE);
            g_gpu_copy.count++;

            agent_log("GPU share [%d]: %s -> %s%s%s",
                      g_gpu_copy.count, si->share_name, si->dest_path,
                      si->filter[0] ? " filter=" : "",
                      si->filter[0] ? si->filter : "");
        }
    }

    if (g_gpu_copy.count == 0) {
        agent_log("GPU query: no valid shares to copy.");
        send_line(client, "gpu_copy_done:0");
        return;
    }

    /* Start background copy */
    g_gpu_copy.notify_sock = client;
    g_gpu_copy.copying = TRUE;
    thread = CreateThread(NULL, 0, gpu_copy_thread, &g_gpu_copy, 0, NULL);
    if (thread)
        CloseHandle(thread);
    else {
        agent_log("Failed to create GPU copy thread (error %lu).", GetLastError());
        g_gpu_copy.copying = FALSE;
        send_line(client, "gpu_copy_error:thread creation failed");
    }
}

/* ---- Line I/O ---- */

static int recv_line(SOCKET s, char *buf, int buf_size)
{
    int pos = 0;
    while (pos < buf_size - 1) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int send_line(SOCKET s, const char *msg)
{
    int len = (int)strlen(msg);
    int n;
    EnterCriticalSection(&g_send_cs);
    n = send(s, msg, len, 0);
    if (n > 0) n = send(s, "\n", 1, 0);
    LeaveCriticalSection(&g_send_cs);
    return n;
}

/* ---- Privilege helper ---- */

static BOOL enable_privilege(LPCWSTR priv_name)
{
    HANDLE token;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(NULL, priv_name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }
    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(token);
    return GetLastError() == ERROR_SUCCESS;
}

/* ---- Input helper process (spawned into console session as SYSTEM) ---- */

static HANDLE g_input_process = NULL;
static DWORD  g_input_session = 0xFFFFFFFF;  /* session the helper was spawned into */
static volatile BOOL g_input_monitor_running = FALSE;
static HANDLE g_input_monitor_thread = NULL;

static void kill_input_helper(void)
{
    if (g_input_process) {
        TerminateProcess(g_input_process, 0);
        WaitForSingleObject(g_input_process, 3000);
        CloseHandle(g_input_process);
        g_input_process = NULL;
        g_input_session = 0xFFFFFFFF;
    }
}

/* Spawn appsandbox-input.exe as SYSTEM in the given session.
   Duplicates our own token, sets the session ID, then CreateProcessAsUser. */
static BOOL spawn_input_in_session(DWORD session_id)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;

    /* Build path to appsandbox-input.exe (same directory as agent) */
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-input.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Input helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("Input helper: OpenProcessToken failed (%lu).", GetLastError());
        return FALSE;
    }

    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("Input helper: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token);
        return FALSE;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("Input helper: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("Input helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    agent_log("Input helper: spawned PID %lu in session %lu.", pi.dwProcessId, session_id);
    g_input_process = pi.hProcess;
    g_input_session = session_id;
    CloseHandle(pi.hThread);
    CloseHandle(dup_token);
    return TRUE;
}

/* Monitor thread: every 3 seconds, check if the console session changed
   or if the helper process died.  Respawn as needed. */
static DWORD WINAPI input_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Input monitor: started.");

    while (g_input_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            BOOL helper_alive = g_input_process &&
                WaitForSingleObject(g_input_process, 0) != WAIT_OBJECT_0;

            if (cur_session != g_input_session) {
                /* Console session changed — kill old helper and respawn */
                if (helper_alive) {
                    agent_log("Input monitor: console session changed %lu -> %lu, respawning.",
                               g_input_session, cur_session);
                    kill_input_helper();
                }
                spawn_input_in_session(cur_session);
            } else if (!helper_alive) {
                /* Same session but helper died — respawn */
                if (g_input_process) {
                    CloseHandle(g_input_process);
                    g_input_process = NULL;
                }
                agent_log("Input monitor: helper died, respawning in session %lu.", cur_session);
                spawn_input_in_session(cur_session);
            }
        }

        /* Sleep 3 seconds, but check stop flag every 500ms */
        {
            int i;
            for (i = 0; i < 6 && g_input_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_input_helper();
    agent_log("Input monitor: stopped.");
    return 0;
}

static void start_input_monitor(void)
{
    g_input_monitor_running = TRUE;
    g_input_monitor_thread = CreateThread(NULL, 0, input_monitor_thread, NULL, 0, NULL);
}

static void stop_input_monitor(void)
{
    g_input_monitor_running = FALSE;
    if (g_input_monitor_thread) {
        WaitForSingleObject(g_input_monitor_thread, 5000);
        CloseHandle(g_input_monitor_thread);
        g_input_monitor_thread = NULL;
    }
}

/* ---- Clipboard helper process (same pattern as input helper) ---- */

static HANDLE g_clipboard_process = NULL;
static DWORD  g_clipboard_session = 0xFFFFFFFF;
static volatile BOOL g_clipboard_monitor_running = FALSE;
static HANDLE g_clipboard_monitor_thread = NULL;

static void kill_clipboard_helper(void)
{
    if (g_clipboard_process) {
        TerminateProcess(g_clipboard_process, 0);
        WaitForSingleObject(g_clipboard_process, 3000);
        CloseHandle(g_clipboard_process);
        g_clipboard_process = NULL;
        g_clipboard_session = 0xFFFFFFFF;
    }
}

static BOOL spawn_clipboard_in_session(DWORD session_id)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-clipboard.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Clipboard helper: %ls not found.", exe_path);
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("Clipboard helper: OpenProcessToken failed (%lu).", GetLastError());
        return FALSE;
    }

    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("Clipboard helper: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token);
        return FALSE;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("Clipboard helper: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("Clipboard helper: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token);
        return FALSE;
    }

    agent_log("Clipboard helper: spawned PID %lu in session %lu.", pi.dwProcessId, session_id);
    g_clipboard_process = pi.hProcess;
    g_clipboard_session = session_id;
    CloseHandle(pi.hThread);
    CloseHandle(dup_token);
    return TRUE;
}

static DWORD WINAPI clipboard_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Clipboard monitor: started.");

    while (g_clipboard_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            BOOL helper_alive = g_clipboard_process &&
                WaitForSingleObject(g_clipboard_process, 0) != WAIT_OBJECT_0;

            if (cur_session != g_clipboard_session) {
                if (helper_alive) {
                    agent_log("Clipboard monitor: console session changed %lu -> %lu, respawning.",
                               g_clipboard_session, cur_session);
                    kill_clipboard_helper();
                }
                spawn_clipboard_in_session(cur_session);
            } else if (!helper_alive) {
                if (g_clipboard_process) {
                    CloseHandle(g_clipboard_process);
                    g_clipboard_process = NULL;
                }
                agent_log("Clipboard monitor: helper died, respawning in session %lu.", cur_session);
                spawn_clipboard_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_clipboard_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_clipboard_helper();
    agent_log("Clipboard monitor: stopped.");
    return 0;
}

static void start_clipboard_monitor(void)
{
    g_clipboard_monitor_running = TRUE;
    g_clipboard_monitor_thread = CreateThread(NULL, 0, clipboard_monitor_thread, NULL, 0, NULL);
}

static void stop_clipboard_monitor(void)
{
    g_clipboard_monitor_running = FALSE;
    if (g_clipboard_monitor_thread) {
        WaitForSingleObject(g_clipboard_monitor_thread, 5000);
        CloseHandle(g_clipboard_monitor_thread);
        g_clipboard_monitor_thread = NULL;
    }
}

/* ---- Clipboard reader process (runs as USER, :0005) ---- */

static HANDLE g_clipboard_reader_process = NULL;
static DWORD  g_clipboard_reader_session = 0xFFFFFFFF;
static volatile BOOL g_clipboard_reader_monitor_running = FALSE;
static HANDLE g_clipboard_reader_monitor_thread = NULL;

static void kill_clipboard_reader(void)
{
    if (g_clipboard_reader_process) {
        TerminateProcess(g_clipboard_reader_process, 0);
        WaitForSingleObject(g_clipboard_reader_process, 3000);
        CloseHandle(g_clipboard_reader_process);
        g_clipboard_reader_process = NULL;
        g_clipboard_reader_session = 0xFFFFFFFF;
    }
}

static BOOL spawn_clipboard_reader_in_session(DWORD session_id)
{
    HANDLE user_token = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    LPVOID env = NULL;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-clipboard-reader.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("Clipboard reader: %ls not found.", exe_path);
        return FALSE;
    }

    /* Get the logged-in user's token — this runs the process as the user,
       which allows it to read the user's clipboard. */
    if (!WTSQueryUserToken(session_id, &user_token)) {
        agent_log("Clipboard reader: WTSQueryUserToken(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        return FALSE;
    }

    /* Create environment block for the user */
    if (!CreateEnvironmentBlock(&env, user_token, FALSE)) {
        agent_log("Clipboard reader: CreateEnvironmentBlock failed (%lu).", GetLastError());
        env = NULL;  /* proceed without it */
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(user_token, exe_path, NULL, NULL, NULL,
                               FALSE,
                               CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0),
                               env, NULL, &si, &pi)) {
        agent_log("Clipboard reader: CreateProcessAsUserW failed (%lu).", GetLastError());
        if (env) DestroyEnvironmentBlock(env);
        CloseHandle(user_token);
        return FALSE;
    }

    agent_log("Clipboard reader: spawned PID %lu in session %lu (as user).",
               pi.dwProcessId, session_id);
    g_clipboard_reader_process = pi.hProcess;
    g_clipboard_reader_session = session_id;
    CloseHandle(pi.hThread);
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(user_token);
    return TRUE;
}

static DWORD WINAPI clipboard_reader_monitor_thread(LPVOID param)
{
    (void)param;
    agent_log("Clipboard reader monitor: started.");

    while (g_clipboard_reader_monitor_running) {
        DWORD cur_session = WTSGetActiveConsoleSessionId();

        if (cur_session != 0xFFFFFFFF) {
            BOOL helper_alive = g_clipboard_reader_process &&
                WaitForSingleObject(g_clipboard_reader_process, 0) != WAIT_OBJECT_0;

            if (cur_session != g_clipboard_reader_session) {
                if (helper_alive) {
                    agent_log("Clipboard reader monitor: console session changed %lu -> %lu, respawning.",
                               g_clipboard_reader_session, cur_session);
                    kill_clipboard_reader();
                }
                spawn_clipboard_reader_in_session(cur_session);
            } else if (!helper_alive) {
                if (g_clipboard_reader_process) {
                    CloseHandle(g_clipboard_reader_process);
                    g_clipboard_reader_process = NULL;
                }
                agent_log("Clipboard reader monitor: helper died, respawning in session %lu.", cur_session);
                spawn_clipboard_reader_in_session(cur_session);
            }
        }

        {
            int i;
            for (i = 0; i < 6 && g_clipboard_reader_monitor_running; i++)
                Sleep(500);
        }
    }

    kill_clipboard_reader();
    agent_log("Clipboard reader monitor: stopped.");
    return 0;
}

static void start_clipboard_reader_monitor(void)
{
    g_clipboard_reader_monitor_running = TRUE;
    g_clipboard_reader_monitor_thread = CreateThread(NULL, 0, clipboard_reader_monitor_thread, NULL, 0, NULL);
}

static void stop_clipboard_reader_monitor(void)
{
    g_clipboard_reader_monitor_running = FALSE;
    if (g_clipboard_reader_monitor_thread) {
        WaitForSingleObject(g_clipboard_reader_monitor_thread, 5000);
        CloseHandle(g_clipboard_reader_monitor_thread);
        g_clipboard_reader_monitor_thread = NULL;
    }
}

/* ---- IDD driver status check ---- */

/* Check if AppSandboxVDD driver is installed and report status.
   Sends "idd_status:<status>" to host where status is:
     ok          — device present and working
     error:<N>   — device present but has problem code N
     not_found   — no device with Root\AppSandboxVDD hardware ID found
*/
static void report_idd_status(SOCKET client)
{
    char output[4096];
    wchar_t cmd[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    DWORD bytes_read;
    int pos = 0;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = L'\0';
    swprintf_s(cmd, MAX_PATH, L"\"%sdrivers\\devcon.exe\" status Root\\AppSandboxVDD", exe_dir);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        send_line(client, "idd_status:not_found");
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("IDD status: devcon failed to launch (%lu).", GetLastError());
        CloseHandle(hRead);
        CloseHandle(hWrite);
        send_line(client, "idd_status:not_found");
        return;
    }
    CloseHandle(hWrite);

    while (ReadFile(hRead, output + pos, (DWORD)(sizeof(output) - pos - 1), &bytes_read, NULL) && bytes_read > 0)
        pos += (int)bytes_read;
    output[pos] = '\0';
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (strstr(output, "running")) {
        agent_log("IDD status: AppSandboxVDD running.");
        send_line(client, "idd_status:ok");
    } else if (strstr(output, "problem")) {
        agent_log("IDD status: AppSandboxVDD has a problem. Output: %s", output);
        send_line(client, "idd_status:error");
    } else if (strstr(output, "disabled")) {
        agent_log("IDD status: AppSandboxVDD disabled.");
        send_line(client, "idd_status:disabled");
    } else if (strstr(output, "No matching")) {
        agent_log("IDD status: AppSandboxVDD not found.");
        send_line(client, "idd_status:not_found");
    } else {
        agent_log("IDD status: unknown. Output: %s", output);
        send_line(client, "idd_status:unknown");
    }
}

/* ---- Helper: run devcon command and capture stdout ---- */

static BOOL run_devcon(const wchar_t *args, char *output, int output_size, DWORD *out_exit_code)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    wchar_t cmd[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    DWORD bytes_read;
    int pos = 0;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *(slash + 1) = L'\0';

    swprintf_s(cmd, MAX_PATH, L"\"%sdrivers\\devcon.exe\" %s", exe_dir, args);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return FALSE;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return FALSE;
    }
    CloseHandle(hWrite);

    /* Wait for devcon to finish (10s max). If it hangs, kill it. */
    if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) {
        agent_log("run_devcon: process timed out, killing.");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
    }

    /* Now read whatever output is available (process is dead, pipe will EOF) */
    while (ReadFile(hRead, output + pos, (DWORD)(output_size - pos - 1), &bytes_read, NULL) && bytes_read > 0)
        pos += (int)bytes_read;
    output[pos] = '\0';
    CloseHandle(hRead);

    if (out_exit_code) GetExitCodeProcess(pi.hProcess, out_exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

/* ---- VDD device check: restart if not running ---- */

static void ensure_vdd_running(void)
{
    char output[4096];
    DWORD exit_code = 0;

    if (!run_devcon(L"status Root\\AppSandboxVDD", output, sizeof(output), &exit_code)) {
        agent_log("ensure_vdd_running: devcon failed to launch (%lu).", GetLastError());
        return;
    }

    if (strstr(output, "running"))
        return;

    if (strstr(output, "No matching"))
        return; /* not installed yet */

    if (strstr(output, "disabled"))
        agent_log("VDD: disabled — restarting.");
    else if (strstr(output, "problem"))
        agent_log("VDD: problem — restarting.");
    else
        agent_log("VDD: unknown state — restarting.");

    if (!run_devcon(L"restart Root\\AppSandboxVDD", output, sizeof(output), &exit_code)) {
        agent_log("VDD restart: devcon failed to launch (%lu).", GetLastError());
        return;
    }
    agent_log("VDD restart (exit=%lu): %.400s", exit_code, output);
}

/* ---- Hyper-V Video device check: disable if running ---- */

#define HYPERV_VIDEO_HWID L"*DA0A7802*"

static void ensure_hyperv_video_disabled(SOCKET notify)
{
    char output[4096];
    DWORD exit_code = 0;

    if (!run_devcon(L"status " HYPERV_VIDEO_HWID, output, sizeof(output), &exit_code)) {
        agent_log("ensure_hyperv_video_disabled: devcon failed to launch (%lu).", GetLastError());
        return;
    }

    if (strstr(output, "disabled") || strstr(output, "No matching"))
        return;

    if (!strstr(output, "running"))
        return;

    agent_log("Hyper-V Video: running — disabling.");

    if (!run_devcon(L"disable " HYPERV_VIDEO_HWID, output, sizeof(output), &exit_code)) {
        agent_log("Hyper-V Video disable: devcon failed to launch (%lu).", GetLastError());
        return;
    }

    if (notify != INVALID_SOCKET)
        send_line(notify, "hyperv_video:disabled");
    agent_log("Hyper-V Video disable (exit=%lu): %.400s", exit_code, output);
}

/* ---- Report display monitor info to host ---- */

/* Spawn appsandbox-displays.exe in the interactive session and capture its
   stdout.  The helper prints "N,WxH,WxH,..." and exits.  We use the same
   token-dup + SetTokenInformation pattern as spawn_input_in_session(). */
static void report_displays(SOCKET notify)
{
    HANDLE cur_token = NULL, dup_token = NULL;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t exe_path[MAX_PATH];
    wchar_t *slash;
    DWORD session_id;
    char output[512];
    DWORD bytes_read;
    char msg[512];

    output[0] = '\0';

    session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xFFFFFFFF || session_id == 0) {
        /* No interactive user session — skip display enumeration.
           Session 0 is the services session (no desktop).
           0xFFFFFFFF means no console session at all. */
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }

    /* Build path to appsandbox-displays.exe (same directory as agent) */
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    slash = wcsrchr(exe_path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exe_path, MAX_PATH, L"appsandbox-displays.exe");

    if (GetFileAttributesW(exe_path) == INVALID_FILE_ATTRIBUTES) {
        agent_log("report_displays: %ls not found.", exe_path);
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }

    /* Create pipe for stdout capture */
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        agent_log("report_displays: CreatePipe failed (%lu).", GetLastError());
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Duplicate our token and set the interactive session */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &cur_token)) {
        agent_log("report_displays: OpenProcessToken failed (%lu).", GetLastError());
        CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }
    if (!DuplicateTokenEx(cur_token, TOKEN_ALL_ACCESS, NULL,
                           SecurityImpersonation, TokenPrimary, &dup_token)) {
        agent_log("report_displays: DuplicateTokenEx failed (%lu).", GetLastError());
        CloseHandle(cur_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }
    CloseHandle(cur_token);

    if (!SetTokenInformation(dup_token, TokenSessionId,
                              &session_id, sizeof(session_id))) {
        agent_log("report_displays: SetTokenInformation(session=%lu) failed (%lu).",
                   session_id, GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = L"WinSta0\\Default";
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessAsUserW(dup_token, exe_path, NULL, NULL, NULL,
                               TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        agent_log("report_displays: CreateProcessAsUserW failed (%lu).", GetLastError());
        CloseHandle(dup_token); CloseHandle(hReadPipe); CloseHandle(hWritePipe);
        if (notify != INVALID_SOCKET) send_line(notify, "displays:0,");
        return;
    }
    CloseHandle(dup_token);
    CloseHandle(hWritePipe);  /* Close write end so ReadFile sees EOF */
    hWritePipe = NULL;

    /* Wait for process (max 2s) then read output.
       Timeout is normal at early boot before the desktop is ready. */
    if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) {
        agent_log("report_displays: helper timed out (desktop may not be ready).");
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }

    {
        int total = 0;
        while (ReadFile(hReadPipe, output + total,
                        (DWORD)(sizeof(output) - 1 - total), &bytes_read, NULL) &&
               bytes_read > 0) {
            total += (int)bytes_read;
            if (total >= (int)sizeof(output) - 1) break;
        }
        output[total] = '\0';
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    /* Strip trailing whitespace */
    {
        int len = (int)strlen(output);
        while (len > 0 && (output[len-1] == '\r' || output[len-1] == '\n' || output[len-1] == ' '))
            output[--len] = '\0';
    }

    snprintf(msg, sizeof(msg), "displays:%s", output);
    agent_log("Displays: %s", output);
    if (notify != INVALID_SOCKET) send_line(notify, msg);
}

/* ---- IDD connect: respawn helpers in console session ---- */

static void handle_idd_connect(SOCKET client)
{
    DWORD new_console = WTSGetActiveConsoleSessionId();

    /* Kill and respawn input helper in the console session */
    kill_input_helper();
    agent_log("idd_connect: respawning input helper in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_input_in_session(new_console);

    /* Kill and respawn clipboard helper (SYSTEM, :0004) in the console session */
    kill_clipboard_helper();
    agent_log("idd_connect: respawning clipboard helper in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_clipboard_in_session(new_console);

    /* Kill and respawn clipboard reader (user, :0005) in the console session */
    kill_clipboard_reader();
    agent_log("idd_connect: respawning clipboard reader in session %lu.", new_console);
    if (new_console != 0xFFFFFFFF)
        spawn_clipboard_reader_in_session(new_console);

    send_line(client, "ok");
}

/* ---- Persistent client handler ---- */

/* Disable the Hyper-V synthetic video adapter (if present).
   Hardware ID: VMBUS\{da0a7802-e377-4aac-8e77-0558eb1073f8}
   Silently does nothing if the device doesn't exist. */
static void disable_hyperv_video(SOCKET notify_sock)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    DWORD idx;
    wchar_t hw_id[512];
    CONFIGRET cr;
    static const wchar_t *TARGET_HWID = L"VMBUS\\{DA0A7802-E377-4AAC-8E77-0558EB1073F8}";

    devs = SetupDiGetClassDevsW(&GUID_DISPLAY_CLASS, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        agent_log("disable_hyperv_video: SetupDiGetClassDevs(PRESENT) failed, trying ALLCLASSES.");
        devs = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
        if (devs == INVALID_HANDLE_VALUE) return;
    }

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        ULONG dev_status = 0, dev_problem = 0;
        hw_id[0] = L'\0';
        CM_Get_Device_IDW(dev_info.DevInst, hw_id, 512, 0);

        if (_wcsnicmp(hw_id, TARGET_HWID, wcslen(TARGET_HWID)) != 0)
            continue;

        /* Check if already disabled */
        cr = CM_Get_DevNode_Status(&dev_status, &dev_problem, dev_info.DevInst, 0);
        if (cr == CR_SUCCESS && !(dev_status & DN_STARTED) && (dev_problem == CM_PROB_DISABLED)) {
            agent_log("Hyper-V Video adapter already disabled: %ls", hw_id);
            if (notify_sock != INVALID_SOCKET)
                send_line(notify_sock, "hyperv_video:already_disabled");
            SetupDiDestroyDeviceInfoList(devs);
            return;
        }

        agent_log("Disabling Hyper-V Video adapter: %ls (status=0x%lX problem=%lu)", hw_id, dev_status, dev_problem);
        if (notify_sock != INVALID_SOCKET)
            send_line(notify_sock, "hyperv_video:disabling");

        cr = CM_Disable_DevNode(dev_info.DevInst, 0);
        if (cr == CR_SUCCESS) {
            agent_log("Hyper-V Video adapter disabled.");
            if (notify_sock != INVALID_SOCKET)
                send_line(notify_sock, "hyperv_video:disabled");
        } else {
            agent_log("Hyper-V Video disable failed (%lu).", cr);
        }
        SetupDiDestroyDeviceInfoList(devs);
        return;
    }

    agent_log("Hyper-V Video adapter not found (enumerated %lu devices).", idx);
    if (notify_sock != INVALID_SOCKET)
        send_line(notify_sock, "hyperv_video:not_found");
    SetupDiDestroyDeviceInfoList(devs);
}

static void handle_client(SOCKET client)
{
    char buf[256];
    int n;
    DWORD heartbeat_interval = 5000;     /* ms between heartbeats */
    DWORD device_check_interval = 20000; /* ms between VDD/Hyper-V device checks */
    DWORD last_heartbeat;
    DWORD last_device_check;

    agent_log("Client connected.");
    g_client_sock = client;

    /* Set socket timeouts so recv/send don't block forever if host disconnects */
    {
        DWORD timeout = 10000; /* 10 seconds */
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout));
    }

    /* Send hello */
    if (send_line(client, "hello") <= 0) {
        agent_log("Failed to send hello.");
        closesocket(client);
        return;
    }

    /* Report IDD driver status to host */
    report_idd_status(client);

    /* Initial device checks */
    ensure_vdd_running();
    ensure_hyperv_video_disabled(client);

    last_heartbeat = GetTickCount();
    last_device_check = last_heartbeat;

    /* Persistent connection loop */
    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        fd_set rfds;
        struct timeval tv;
        int ret;
        DWORD now;

        FD_ZERO(&rfds);
        FD_SET(client, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ret = select(0, &rfds, NULL, NULL, &tv);
        if (ret < 0) break;

        /* Send heartbeat if interval elapsed */
        now = GetTickCount();
        if (now - last_heartbeat >= heartbeat_interval) {
            if (send_line(client, "heartbeat") <= 0) {
                agent_log("Heartbeat send failed, client disconnected.");
                break;
            }
            last_heartbeat = now;
        }

        /* Device checks on a slower cadence (VDD recovery is expensive) */
        if (now - last_device_check >= device_check_interval) {
            last_device_check = now;
            ensure_vdd_running();
            ensure_hyperv_video_disabled(client);
        }


        if (ret == 0) continue; /* select timeout, no data */

        /* Read command */
        n = recv_line(client, buf, sizeof(buf));
        if (n <= 0) {
            agent_log("Client disconnected.");
            break;
        }

        agent_log("Command: %s", buf);

        if (strcmp(buf, "ping") == 0) {
            send_line(client, "ok");
        }
        else if (strcmp(buf, "shutdown") == 0) {
            send_line(client, "ok");
            agent_log("Initiating shutdown... killing input helper first.");
            stop_input_monitor();
            agent_log("Input monitor stopped, calling InitiateSystemShutdownExW...");
            if (!enable_privilege(SE_SHUTDOWN_NAME))
                agent_log("Warning: could not enable SeShutdownPrivilege (%lu)", GetLastError());
            if (!InitiateSystemShutdownExW(NULL, L"AppSandbox shutdown", 0, TRUE, FALSE,
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                agent_log("InitiateSystemShutdownExW failed: %lu", GetLastError());
            /* Keep connection open — host will see it drop when VM powers off */
        }
        else if (strcmp(buf, "restart") == 0) {
            send_line(client, "ok");
            agent_log("Initiating restart...");
            stop_input_monitor();
            if (!enable_privilege(SE_SHUTDOWN_NAME))
                agent_log("Warning: could not enable SeShutdownPrivilege (%lu)", GetLastError());
            if (!InitiateSystemShutdownExW(NULL, L"AppSandbox restart", 0, TRUE, TRUE,
                    SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
                agent_log("InitiateSystemShutdownExW failed: %lu", GetLastError());
        }
        else if (strncmp(buf, "gpu_query_response:", 19) == 0) {
            int share_count = atoi(buf + 19);
            agent_log("Received GPU share list (%d shares).", share_count);
            handle_gpu_query_response(client, share_count);
        }
        else if (strcmp(buf, "gpu_none") == 0) {
            agent_log("Host reports no GPU-PV assigned.");
        }
        else if (strcmp(buf, "gpu_copy") == 0) {
            /* Host re-triggered GPU copy — ask for share list */
            send_line(client, "gpu_query");
        }
        else if (strcmp(buf, "idd_connect") == 0) {
            handle_idd_connect(client);
        }
        else {
            send_line(client, "error:unknown");
        }
    }

    /* If GPU copy is still running, wait for it to finish */
    if (g_gpu_copy.copying) {
        agent_log("Waiting for GPU copy thread to finish...");
        /* Invalidate notify socket so it doesn't try to send on closed socket */
        g_gpu_copy.notify_sock = INVALID_SOCKET;
        {
            int wait;
            for (wait = 0; wait < 10000 && g_gpu_copy.copying; wait += 500)
                Sleep(500);
        }
    }

    g_client_sock = INVALID_SOCKET;
    closesocket(client);
    agent_log("Client handler exiting.");
}

/* ---- Socket listener ---- */

static DWORD WINAPI listener_thread(LPVOID param)
{
    WSADATA wsa;
    SOCKADDR_HV addr;
    SOCKET client;
    fd_set fds;
    struct timeval tv;

    (void)param;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        agent_log("WSAStartup failed: %d", WSAGetLastError());
        return 1;
    }

    while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        g_listen_sock = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
        if (g_listen_sock == INVALID_SOCKET) {
            agent_log("socket(AF_HYPERV) failed: %d, retrying in 3s", WSAGetLastError());
            if (WaitForSingleObject(g_stop_event, 3000) == WAIT_OBJECT_0) break;
            continue;
        }

        memset(&addr, 0, sizeof(addr));
        addr.Family = AF_HYPERV;
        addr.VmId = HV_GUID_WILDCARD;
        addr.ServiceId = AGENT_SERVICE_GUID;

        if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            agent_log("bind failed: %d, retrying in 3s", WSAGetLastError());
            closesocket(g_listen_sock);
            g_listen_sock = INVALID_SOCKET;
            if (WaitForSingleObject(g_stop_event, 3000) == WAIT_OBJECT_0) break;
            continue;
        }

        if (listen(g_listen_sock, 4) != 0) {
            agent_log("listen failed: %d, retrying in 3s", WSAGetLastError());
            closesocket(g_listen_sock);
            g_listen_sock = INVALID_SOCKET;
            if (WaitForSingleObject(g_stop_event, 3000) == WAIT_OBJECT_0) break;
            continue;
        }

        agent_log("Listening on AF_HYPERV (service GUID a5b0cafe-0001-4000-8000-000000000001)");

        /* Accept loop — breaks out on error to retry socket creation */
        while (WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
            FD_ZERO(&fds);
            FD_SET(g_listen_sock, &fds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            if (select(0, &fds, NULL, NULL, &tv) > 0) {
                client = accept(g_listen_sock, NULL, NULL);
                if (client != INVALID_SOCKET)
                    handle_client(client);
            }
        }

        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }

    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    WSACleanup();
    agent_log("Listener stopped.");
    return 0;
}

/* ---- Windows service plumbing ---- */

static void set_service_status(DWORD state, DWORD exit_code)
{
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    SetServiceStatus(g_status_handle, &g_status);
}

static DWORD WINAPI service_ctrl_ex(DWORD ctrl, DWORD event_type, LPVOID event_data, LPVOID context)
{
    (void)context;
    agent_log("service_ctrl received: %lu (event_type=%lu)", ctrl, event_type);

    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        if (ctrl == SERVICE_CONTROL_SHUTDOWN)
            g_os_shutting_down = TRUE;

        /* Notify host */
        if (g_client_sock != INVALID_SOCKET) {
            if (ctrl == SERVICE_CONTROL_SHUTDOWN)
                send_line(g_client_sock, "os_shutdown");
            else
                send_line(g_client_sock, "service_stopping");
            agent_log("Sent notification to host (ctrl=%lu).", ctrl);
        }

        set_service_status(SERVICE_STOP_PENDING, 0);
        agent_log("Setting stop event...");
        SetEvent(g_stop_event);
        return NO_ERROR;
    }

    if (ctrl == SERVICE_CONTROL_SESSIONCHANGE) {
        WTSSESSION_NOTIFICATION *sn = (WTSSESSION_NOTIFICATION *)event_data;
        if (event_type == WTS_SESSION_LOGOFF) {
            agent_log("Session logoff detected (session %lu).",
                       sn ? sn->dwSessionId : 0);
            /* Do NOT restart the VDD device here.  IddCx handles the DWM
               transition naturally: UnassignSwapChain (user DWM dies) then
               AssignSwapChain (login-screen DWM starts compositing).
               A devcon restart mid-transition destroys the adapter/monitor
               and prevents IddCx from completing the handoff. */

            /* Disable Hyper-V Video after logoff (may have been re-enabled) */
            ensure_hyperv_video_disabled(INVALID_SOCKET);
        }
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, LPSTR *argv)
{
    HANDLE thread;

    (void)argc; (void)argv;

    g_status_handle = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_ctrl_ex, NULL);
    if (!g_status_handle) return;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    set_service_status(SERVICE_START_PENDING, 0);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&g_send_cs);

    /* Wire p9copy logging into agent_log */
    p9_set_log(p9copy_log);

    /* Start listener on a worker thread */
    thread = CreateThread(NULL, 0, listener_thread, NULL, 0, NULL);

    /* Start input helper monitor (spawns/respawns in console session as SYSTEM) */
    start_input_monitor();

    /* Start clipboard helper monitor (SYSTEM, :0004) */
    start_clipboard_monitor();

    /* Start clipboard reader monitor (user, :0005) */
    start_clipboard_reader_monitor();

    set_service_status(SERVICE_RUNNING, 0);
    agent_log("Service started.");

    /* Ensure VDD device is running (may need restart after logout teardown) */
    ensure_vdd_running();

    /* Ensure Hyper-V Video adapter is disabled (VDD replaces it) */
    ensure_hyperv_video_disabled(INVALID_SOCKET);

    /* Wait until stop is signaled */
    WaitForSingleObject(g_stop_event, INFINITE);

    /* Clean up */
    agent_log("Stop event signaled, cleaning up...");
    agent_log("Stopping clipboard reader monitor...");
    stop_clipboard_reader_monitor();
    agent_log("Clipboard reader monitor stopped.");
    agent_log("Stopping clipboard monitor...");
    stop_clipboard_monitor();
    agent_log("Clipboard monitor stopped.");
    agent_log("Stopping input monitor...");
    stop_input_monitor();
    agent_log("Input monitor stopped.");
    if (g_listen_sock != INVALID_SOCKET)
        closesocket(g_listen_sock);
    if (thread) {
        agent_log("Waiting for listener thread...");
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
        agent_log("Listener thread exited.");
    }
    CloseHandle(g_stop_event);
    DeleteCriticalSection(&g_send_cs);

    agent_log("Service stopped.");
    set_service_status(SERVICE_STOPPED, 0);
}

/* ---- Install / Remove ---- */

static int install_service(void)
{
    SC_HANDLE scm, svc;
    char path[MAX_PATH];

    GetModuleFileNameA(NULL, path, MAX_PATH);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    svc = CreateServiceA(scm, SERVICE_NAME, DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);

    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_EXISTS) {
            printf("Service already exists.\n");
            svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_START);
        } else {
            printf("CreateService failed: %lu\n", GetLastError());
            CloseServiceHandle(scm);
            return 1;
        }
    } else {
        printf("Service installed.\n");

        /* Set description */
        SERVICE_DESCRIPTIONA desc;
        desc.lpDescription = "AppSandbox guest agent for host-guest communication via Hyper-V sockets.";
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        /* Set recovery: restart on failure */
        SC_ACTION actions[3] = {
            { SC_ACTION_RESTART, 5000 },
            { SC_ACTION_RESTART, 10000 },
            { SC_ACTION_RESTART, 30000 }
        };
        SERVICE_FAILURE_ACTIONSA sfa = { 0 };
        sfa.dwResetPeriod = 86400;
        sfa.cActions = 3;
        sfa.lpsaActions = actions;
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);
    }

    /* Start the service */
    if (svc) {
        if (StartServiceA(svc, 0, NULL))
            printf("Service started.\n");
        else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
            printf("Service already running.\n");
        else
            printf("StartService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
    }

    CloseServiceHandle(scm);
    return 0;
}

static int remove_service(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS ss;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!svc) {
        printf("Service not found.\n");
        CloseServiceHandle(scm);
        return 1;
    }

    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    Sleep(1000);

    if (DeleteService(svc))
        printf("Service removed.\n");
    else
        printf("DeleteService failed: %lu\n", GetLastError());

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

/* ---- Entry point ---- */

int main(int argc, char *argv[])
{
    if (argc > 1) {
        if (strcmp(argv[1], "--install") == 0)
            return install_service();
        if (strcmp(argv[1], "--remove") == 0)
            return remove_service();
        printf("Usage: appsandbox-agent.exe [--install | --remove]\n");
        return 1;
    }

    /* Launched by SCM — run as service */
    {
        SERVICE_TABLE_ENTRYA table[] = {
            { SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONA)service_main },
            { NULL, NULL }
        };
        if (!StartServiceCtrlDispatcherA(table)) {
            /* Not running as service — run listener directly for debugging */
            printf("Running in console mode (Ctrl+C to stop)...\n");
            p9_set_log(p9copy_log);
            InitializeCriticalSection(&g_send_cs);
            g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            listener_thread(NULL);
            CloseHandle(g_stop_event);
            DeleteCriticalSection(&g_send_cs);
        }
    }
    return 0;
}
