#include "hcs_vm.h"
#include "vm_agent.h"
#include "ui.h"
#include <stdio.h>
#include <sddl.h>
#include <aclapi.h>
#include <tlhelp32.h>

/* Get current user's SID as a string (e.g. "S-1-5-21-...").
   Caller must LocalFree the returned string. Returns NULL on failure. */
static wchar_t *get_current_user_sid(void)
{
    HANDLE token;
    TOKEN_USER *tu = NULL;
    DWORD len = 0;
    wchar_t *sid_str = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return NULL;

    GetTokenInformation(token, TokenUser, NULL, 0, &len);
    tu = (TOKEN_USER *)HeapAlloc(GetProcessHeap(), 0, len);
    if (tu && GetTokenInformation(token, TokenUser, tu, len, &len))
        ConvertSidToStringSidW(tu->User.Sid, &sid_str);

    if (tu) HeapFree(GetProcessHeap(), 0, tu);
    CloseHandle(token);
    return sid_str;
}

/* ---- HCS function pointer types ---- */

typedef void (CALLBACK *HCS_OPERATION_COMPLETION)(HCS_OPERATION op, void *ctx);

typedef HCS_OPERATION (WINAPI *PFN_HcsCreateOperation)(
    const void *ctx, HCS_OPERATION_COMPLETION callback);
typedef HRESULT (WINAPI *PFN_HcsWaitForOperationResult)(
    HCS_OPERATION op, DWORD timeoutMs, PWSTR *resultDoc);
typedef void (WINAPI *PFN_HcsCloseOperation)(HCS_OPERATION op);

typedef HRESULT (WINAPI *PFN_HcsCreateComputeSystem)(
    PCWSTR id, PCWSTR configuration, HCS_OPERATION op,
    const SECURITY_DESCRIPTOR *sd, HCS_SYSTEM *cs);
typedef HRESULT (WINAPI *PFN_HcsStartComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR options);
typedef HRESULT (WINAPI *PFN_HcsShutDownComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR options);
typedef HRESULT (WINAPI *PFN_HcsTerminateComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR options);
typedef void (WINAPI *PFN_HcsCloseComputeSystem)(HCS_SYSTEM cs);
typedef HRESULT (WINAPI *PFN_HcsPauseComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR options);
typedef HRESULT (WINAPI *PFN_HcsResumeComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR options);
typedef HRESULT (WINAPI *PFN_HcsSaveComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR options);
typedef HRESULT (WINAPI *PFN_HcsGetComputeSystemProperties)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR propertyQuery);
typedef HRESULT (WINAPI *PFN_HcsModifyComputeSystem)(
    HCS_SYSTEM cs, HCS_OPERATION op, PCWSTR configuration, HANDLE identity);
typedef HRESULT (WINAPI *PFN_HcsOpenComputeSystem)(
    PCWSTR id, DWORD requestedAccess, HCS_SYSTEM *cs);
typedef HRESULT (WINAPI *PFN_HcsGrantVmAccess)(
    PCWSTR vmId, PCWSTR filePath);
typedef HRESULT (WINAPI *PFN_HcsCreateEmptyGuestStateFile)(PCWSTR path);
typedef HRESULT (WINAPI *PFN_HcsCreateEmptyRuntimeStateFile)(PCWSTR path);
typedef HRESULT (WINAPI *PFN_HcsEnumerateComputeSystems)(
    PCWSTR query, HCS_OPERATION op);

/* HCS V1 callback API — matches hcsshim/moby (proven reliable).
   HcsRegisterComputeSystemCallback / HcsUnregisterComputeSystemCallback */

typedef void *HCS_CALLBACK;  /* Opaque callback handle */

/* V1 notification types (from hcsshim callback.go) */
#define HCS_NOTIFY_INVALID                        0x00000000
#define HCS_NOTIFY_SYSTEM_EXITED                  0x00000001
#define HCS_NOTIFY_SYSTEM_CREATE_COMPLETED        0x00000002
#define HCS_NOTIFY_SYSTEM_START_COMPLETED         0x00000003
#define HCS_NOTIFY_SYSTEM_PAUSE_COMPLETED         0x00000004
#define HCS_NOTIFY_SYSTEM_RESUME_COMPLETED        0x00000005
#define HCS_NOTIFY_SYSTEM_CRASH_REPORT            0x00000006
#define HCS_NOTIFY_SYSTEM_SILO_JOB_CREATED        0x00000007
#define HCS_NOTIFY_SYSTEM_SAVE_COMPLETED          0x00000008
#define HCS_NOTIFY_SYSTEM_RDP_ENHANCED_CHANGED    0x00000009
#define HCS_NOTIFY_SYSTEM_SHUTDOWN_FAILED         0x0000000A
#define HCS_NOTIFY_SYSTEM_GET_PROPS_COMPLETED     0x0000000B
#define HCS_NOTIFY_SYSTEM_MODIFY_COMPLETED        0x0000000C
#define HCS_NOTIFY_SYSTEM_CRASH_INITIATED         0x0000000D
#define HCS_NOTIFY_SYSTEM_GUEST_CONN_CLOSED       0x0000000E
#define HCS_NOTIFY_SERVICE_DISCONNECT             0x01000000

/* V1 callback signature: (notificationType, context, notificationStatus, notificationData) */
typedef void (CALLBACK *HCS_NOTIFICATION_CALLBACK)(
    DWORD notificationType, void *context, HRESULT notificationStatus, PCWSTR notificationData);

typedef HRESULT (WINAPI *PFN_HcsRegisterComputeSystemCallback)(
    HCS_SYSTEM cs, HCS_NOTIFICATION_CALLBACK callback, void *context, HCS_CALLBACK *callbackHandle);
typedef HRESULT (WINAPI *PFN_HcsUnregisterComputeSystemCallback)(HCS_CALLBACK callbackHandle);

/* V2 callback API (HcsSetComputeSystemCallback) — fallback if V1 not available */
typedef enum {
    HcsEventOptionNone           = 0x00000000,
    HcsEventOptionEnableOperationCallbacks = 0x00000001
} HCS_EVENT_OPTIONS;

typedef struct {
    DWORD Type;
    PCWSTR EventData;
    HCS_OPERATION Operation;
} HCS_EVENT;

typedef void (CALLBACK *HCS_EVENT_CALLBACK)(HCS_EVENT *event, void *context);

typedef HRESULT (WINAPI *PFN_HcsSetComputeSystemCallback)(
    HCS_SYSTEM cs, HCS_EVENT_OPTIONS options, void *context,
    HCS_EVENT_CALLBACK callback);

/* ---- Loaded function pointers ---- */

static HMODULE g_hcs_dll = NULL;

static PFN_HcsCreateOperation          pfnCreateOp;
static PFN_HcsWaitForOperationResult   pfnWaitOp;
static PFN_HcsCloseOperation           pfnCloseOp;
static PFN_HcsCreateComputeSystem      pfnCreate;
static PFN_HcsStartComputeSystem       pfnStart;
static PFN_HcsShutDownComputeSystem    pfnShutDown;
static PFN_HcsTerminateComputeSystem   pfnTerminate;
static PFN_HcsCloseComputeSystem       pfnClose;
static PFN_HcsPauseComputeSystem       pfnPause;
static PFN_HcsResumeComputeSystem      pfnResume;
static PFN_HcsSaveComputeSystem        pfnSave;
static PFN_HcsGetComputeSystemProperties pfnGetProps;
static PFN_HcsModifyComputeSystem      pfnModify;
static PFN_HcsOpenComputeSystem        pfnOpen;
static PFN_HcsGrantVmAccess           pfnGrantAccess;
static PFN_HcsCreateEmptyGuestStateFile   pfnCreateVmgs;
static PFN_HcsCreateEmptyRuntimeStateFile pfnCreateVmrs;
static PFN_HcsEnumerateComputeSystems     pfnEnumSystems;
static PFN_HcsRegisterComputeSystemCallback pfnRegisterCallback;
static PFN_HcsUnregisterComputeSystemCallback pfnUnregisterCallback;
static PFN_HcsSetComputeSystemCallback    pfnSetCallback;  /* V2 fallback */
static BOOL g_using_v1_callback = FALSE;

/* ---- Helpers ---- */

static void escape_json_path(const wchar_t *in, wchar_t *out, size_t out_size)
{
    size_t j = 0;
    size_t i;
    for (i = 0; in[i] && j < out_size - 2; i++) {
        if (in[i] == L'\\') {
            out[j++] = L'\\';
            out[j++] = L'\\';
        } else if (in[i] == L'"') {
            out[j++] = L'\\';
            out[j++] = L'"';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = L'\0';
}

/* HCS_E_OPERATION_TIMEOUT — returned by HcsWaitForOperationResult on timeout */
#ifndef HCS_E_OPERATION_TIMEOUT
#define HCS_E_OPERATION_TIMEOUT ((HRESULT)0x80370102L)
#endif

static DWORD g_ui_thread_id = 0;

/* Execute an HCS operation and wait for result.
   On the UI thread: polls with 200ms waits + message pump to keep UI responsive.
   On background threads: INFINITE wait (no pump needed). */
static HRESULT hcs_exec_and_wait(HRESULT call_result, HCS_OPERATION op)
{
    HRESULT hr = E_FAIL;
    PWSTR result_doc = NULL;

    if (FAILED(call_result)) {
        if (op) pfnCloseOp(op);
        return call_result;
    }

    if (g_ui_thread_id && GetCurrentThreadId() == g_ui_thread_id) {
        /* UI thread — pump messages while waiting */
        int elapsed = 0;
        while (elapsed < 30000) {
            MSG msg;
            hr = pfnWaitOp(op, 200, &result_doc);
            if (hr != HCS_E_OPERATION_TIMEOUT)
                break;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            elapsed += 200;
        }
    } else {
        /* Background thread — block until done */
        hr = pfnWaitOp(op, INFINITE, &result_doc);
    }

    if (result_doc) {
        if (FAILED(hr))
            ui_log(L"HCS: %s", result_doc);
        LocalFree(result_doc);
    }
    pfnCloseOp(op);

    return hr;
}


/* ---- VM state change callback ---- */

static HcsVmStateCallback g_state_callback = NULL;

void hcs_set_state_callback(HcsVmStateCallback cb)
{
    g_state_callback = cb;
}

/* V1 names (HcsRegisterComputeSystemCallback — hcsshim/moby numbering) */
static const wchar_t *hcs_notify_name_v1(DWORD type)
{
    switch (type) {
    case 0x00000000: return L"Invalid";
    case 0x00000001: return L"SystemExited";
    case 0x00000002: return L"SystemCreateCompleted";
    case 0x00000003: return L"SystemStartCompleted";
    case 0x00000004: return L"SystemPauseCompleted";
    case 0x00000005: return L"SystemResumeCompleted";
    case 0x00000006: return L"SystemCrashReport";
    case 0x00000007: return L"SystemSiloJobCreated";
    case 0x00000008: return L"SystemSaveCompleted";
    case 0x00000009: return L"SystemRdpEnhancedChanged";
    case 0x0000000A: return L"SystemShutdownFailed";
    case 0x0000000B: return L"SystemGetPropsCompleted";
    case 0x0000000C: return L"SystemModifyCompleted";
    case 0x0000000D: return L"SystemCrashInitiated";
    case 0x0000000E: return L"SystemGuestConnClosed";
    case 0x01000000: return L"ServiceDisconnect";
    default:         return L"Unknown";
    }
}

/* V2 names (HcsSetComputeSystemCallback — HCS_EVENT_TYPE numbering) */
static const wchar_t *hcs_notify_name_v2(DWORD type)
{
    switch (type) {
    case 0x00000000: return L"Invalid";
    case 0x00000001: return L"SystemExited";
    case 0x00000002: return L"SystemCrashReport";
    case 0x00000003: return L"SystemCrashInitiated";
    case 0x00000004: return L"SystemGuestConnClosed";
    case 0x00000005: return L"SystemStartCompleted";
    case 0x00000007: return L"SystemPauseCompleted";
    case 0x00000008: return L"SystemResumeCompleted";
    case 0x00000009: return L"SystemRdpEnhancedChanged";
    case 0x02000000: return L"ServiceDisconnect";
    default:         return L"Unknown";
    }
}

/* Log from HCS callback thread — write directly to file only.
   NEVER call ui_log() here: it uses SendMessageW which deadlocks
   if the UI thread is blocked on an HCS/COM operation. */
static void hcs_cb_log(const wchar_t *fmt, ...)
{
    FILE *lf = NULL;
    va_list ap;
    if (_wfopen_s(&lf, L"appsandbox.log", L"a") == 0 && lf) {
        va_start(ap, fmt);
        vfwprintf(lf, fmt, ap);
        va_end(ap);
        fwprintf(lf, L"\n");
        fclose(lf);
    }
}

/* V1 callback — matches hcsshim notificationWatcher signature */
static void CALLBACK hcs_notify_cb_v1(
    DWORD notificationType, void *context, HRESULT notificationStatus, PCWSTR notificationData)
{
    VmInstance *instance = (VmInstance *)context;

    hcs_cb_log(L"HCS notify [V1]: %s (0x%08X) status=0x%08X for \"%s\"",
           hcs_notify_name_v1(notificationType), notificationType,
           notificationStatus,
           instance ? instance->name : L"(null)");

    if (notificationData && notificationData[0])
        hcs_cb_log(L"  Data: %.512s", notificationData);

    if (g_state_callback)
        g_state_callback(instance, notificationType);
}

/* V2 callback — HcsSetComputeSystemCallback (HCS_EVENT based) */
static void CALLBACK hcs_notify_cb_v2(HCS_EVENT *event, void *context)
{
    VmInstance *instance = (VmInstance *)context;
    DWORD type = event ? event->Type : 0;

    hcs_cb_log(L"HCS notify [V2]: %s (0x%08X) for \"%s\"",
           hcs_notify_name_v2(type), type,
           instance ? instance->name : L"(null)");

    if (event && event->EventData && event->EventData[0]) {
        hcs_cb_log(L"  Data: %.512s", event->EventData);
        /* Also show exit data in UI for debugging */
        if (type == 0x00000001)
            ui_log(L"HCS exit data: %.256s", event->EventData);
    }

    if (g_state_callback)
        g_state_callback(instance, type);
}

void hcs_register_vm_callback(VmInstance *instance)
{
    HRESULT hr;

    if (!instance->handle) {
        ui_log(L"HCS callback: No handle for \"%s\".", instance->name);
        return;
    }

    /* Try V1 first (hcsshim/moby pattern) */
    if (pfnRegisterCallback) {
        HCS_CALLBACK cb_handle = NULL;
        hr = pfnRegisterCallback(instance->handle, hcs_notify_cb_v1, instance, &cb_handle);
        if (SUCCEEDED(hr)) {
            instance->hcs_callback = cb_handle;
            g_using_v1_callback = TRUE;
            ui_log(L"HCS callback registered [V1] for \"%s\".", instance->name);
            return;
        }
        ui_log(L"HCS V1 callback failed (0x%08X), trying V2...", hr);
    }

    /* Fall back to V2 */
    if (pfnSetCallback) {
        hr = pfnSetCallback(instance->handle, HcsEventOptionNone, instance, hcs_notify_cb_v2);
        if (SUCCEEDED(hr)) {
            g_using_v1_callback = FALSE;
            ui_log(L"HCS callback registered [V2] for \"%s\".", instance->name);
            return;
        }
        ui_log(L"HCS V2 callback failed (0x%08X).", hr);
    }

    ui_log(L"HCS callback: No callback API available.");
}

void hcs_unregister_vm_callback(VmInstance *instance)
{
    if (g_using_v1_callback && pfnUnregisterCallback && instance->hcs_callback) {
        pfnUnregisterCallback(instance->hcs_callback);
        instance->hcs_callback = NULL;
    } else if (!g_using_v1_callback && pfnSetCallback && instance->handle) {
        /* V2: clear callback by setting NULL */
        pfnSetCallback(instance->handle, HcsEventOptionNone, NULL, NULL);
    }
}

/* ---- Background VM monitor thread ---- */

/* WM_APP messages posted by the monitor thread to the UI */
#define WM_VM_MONITOR_DETECTED  (WM_APP + 6)
#define WM_VM_SHUTDOWN_TIMEOUT  (WM_APP + 9)

static HWND g_monitor_hwnd = NULL;

void hcs_set_monitor_hwnd(HWND hwnd)
{
    g_monitor_hwnd = hwnd;
}

static DWORD WINAPI vm_monitor_thread_proc(LPVOID param)
{
    VmInstance *inst = (VmInstance *)param;

    hcs_cb_log(L"Monitor: started for \"%s\"", inst->name);

    while (!inst->monitor_stop) {
        DWORD interval;

        /* Adjust polling speed based on state */
        if (inst->shutdown_requested)
            interval = 2000;
        else if (inst->callbacks_dead)
            interval = 3000;
        else
            interval = 5000;

        /* Wait on stop event — wakes instantly when signaled */
        if (WaitForSingleObject(inst->monitor_stop_event, interval) == WAIT_OBJECT_0)
            break;

        if (inst->monitor_stop) break;
        if (!inst->running) break;
        if (!inst->handle) break;

        /* Query HCS properties to check actual VM state */
        if (pfnCreateOp && pfnGetProps && pfnWaitOp && pfnCloseOp) {
            HCS_OPERATION op = pfnCreateOp(NULL, NULL);
            if (op) {
                PWSTR result_doc = NULL;
                HRESULT hr = pfnGetProps(inst->handle, op,
                    L"{\"PropertyTypes\":[\"Basic\"]}");
                if (SUCCEEDED(hr))
                    hr = pfnWaitOp(op, 5000, &result_doc);
                pfnCloseOp(op);

                if (FAILED(hr)) {
                    hcs_cb_log(L"Monitor: props query failed 0x%08X for \"%s\"",
                               hr, inst->name);
                    /* If callbacks are dead and query fails, VM is likely gone */
                    if (inst->callbacks_dead && inst->running && g_monitor_hwnd) {
                        hcs_cb_log(L"Monitor: forcing cleanup for \"%s\" "
                                   L"(callbacks dead + query failed)", inst->name);
                        PostMessageW(g_monitor_hwnd, WM_VM_MONITOR_DETECTED,
                                     0, (LPARAM)inst);
                        break;
                    }
                } else if (result_doc) {
                    BOOL is_running = (wcsstr(result_doc, L"\"Running\"") != NULL);

                    if (!is_running && inst->running) {
                        hcs_cb_log(L"Monitor: VM \"%s\" stopped (callback missed). "
                                   L"State: %.200s", inst->name, result_doc);
                        LocalFree(result_doc);
                        if (g_monitor_hwnd)
                            PostMessageW(g_monitor_hwnd, WM_VM_MONITOR_DETECTED,
                                         0, (LPARAM)inst);
                        break;
                    }
                    LocalFree(result_doc);
                }
            }
        }

        /* Agent heartbeat timeout check */
        if (inst->agent_online && inst->last_heartbeat > 0) {
            ULONGLONG now = GetTickCount64();
            if (now - inst->last_heartbeat > 30000)
                hcs_cb_log(L"Monitor: agent heartbeat timeout for \"%s\" "
                           L"(%llu seconds)", inst->name,
                           (now - inst->last_heartbeat) / 1000);
        }

        /* Shutdown watchdog */
        if (inst->shutdown_requested && inst->shutdown_time > 0) {
            ULONGLONG elapsed = (GetTickCount64() - inst->shutdown_time) / 1000;

            if (elapsed > 60 && g_monitor_hwnd) {
                hcs_cb_log(L"Monitor: shutdown watchdog for \"%s\" "
                           L"(%llu seconds)", inst->name, elapsed);
                PostMessageW(g_monitor_hwnd, WM_VM_SHUTDOWN_TIMEOUT,
                             (WPARAM)elapsed, (LPARAM)inst);
            } else if (elapsed > 30) {
                hcs_cb_log(L"Monitor: waiting for shutdown of \"%s\" "
                           L"(%llu seconds)...", inst->name, elapsed);
            }
        }
    }

    hcs_cb_log(L"Monitor: exiting for \"%s\"", inst->name);
    return 0;
}

void hcs_start_monitor(VmInstance *instance)
{
    if (instance->monitor_thread) return; /* Already running */

    instance->monitor_stop = FALSE;
    instance->callbacks_dead = FALSE;
    instance->shutdown_requested = FALSE;
    instance->shutdown_time = 0;

    instance->monitor_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    instance->monitor_thread = CreateThread(NULL, 0, vm_monitor_thread_proc,
                                            instance, 0, NULL);
    if (!instance->monitor_thread) {
        ui_log(L"Warning: Failed to start monitor thread for \"%s\".", instance->name);
        if (instance->monitor_stop_event) {
            CloseHandle(instance->monitor_stop_event);
            instance->monitor_stop_event = NULL;
        }
    }
}

void hcs_stop_monitor(VmInstance *instance)
{
    if (!instance->monitor_thread) return;

    instance->monitor_stop = TRUE;
    if (instance->monitor_stop_event)
        SetEvent(instance->monitor_stop_event);
    WaitForSingleObject(instance->monitor_thread, 5000);
    CloseHandle(instance->monitor_thread);
    instance->monitor_thread = NULL;
    if (instance->monitor_stop_event) {
        CloseHandle(instance->monitor_stop_event);
        instance->monitor_stop_event = NULL;
    }
}

/* Parse RuntimeId GUID from an HCS Basic properties JSON document.
   Returns TRUE if found and parsed. */
static BOOL parse_runtime_id(const wchar_t *doc, GUID *out)
{
    const wchar_t *p;
    wchar_t guid_str[64];
    int i = 0;

    p = wcsstr(doc, L"\"RuntimeId\":\"");
    if (!p) return FALSE;

    p += 13; /* skip "RuntimeId":" */
    while (*p && *p != L'"' && i < 63)
        guid_str[i++] = *p++;
    guid_str[i] = L'\0';

    if (i >= 36) {
        unsigned long d1;
        unsigned int d2, d3;
        unsigned int b[8];
        if (swscanf_s(guid_str,
                L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                &d1, &d2, &d3,
                &b[0], &b[1], &b[2], &b[3],
                &b[4], &b[5], &b[6], &b[7]) == 11) {
            out->Data1 = d1;
            out->Data2 = (unsigned short)d2;
            out->Data3 = (unsigned short)d3;
            out->Data4[0] = (unsigned char)b[0];
            out->Data4[1] = (unsigned char)b[1];
            out->Data4[2] = (unsigned char)b[2];
            out->Data4[3] = (unsigned char)b[3];
            out->Data4[4] = (unsigned char)b[4];
            out->Data4[5] = (unsigned char)b[5];
            out->Data4[6] = (unsigned char)b[6];
            out->Data4[7] = (unsigned char)b[7];
            return TRUE;
        }
    }
    return FALSE;
}

/* Query Basic properties and cache the RuntimeId in the VmInstance. */
static void cache_runtime_id(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;
    PWSTR result_doc = NULL;

    if (!pfnGetProps || !instance->handle) return;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return;

    hr = pfnGetProps(instance->handle, op, L"{\"PropertyTypes\":[\"Basic\"]}");
    if (SUCCEEDED(hr))
        hr = pfnWaitOp(op, 5000, &result_doc);
    pfnCloseOp(op);

    if (SUCCEEDED(hr) && result_doc) {
        if (parse_runtime_id(result_doc, &instance->runtime_id)) {
            ui_log(L"Cached VM RuntimeId: %08lx-%04x-%04x-...",
                   instance->runtime_id.Data1,
                   instance->runtime_id.Data2,
                   instance->runtime_id.Data3);
        }
        LocalFree(result_doc);
    }
}

/* ---- Public API ---- */

BOOL hcs_init(void)
{
    g_ui_thread_id = GetCurrentThreadId();

    g_hcs_dll = LoadLibraryW(L"computecore.dll");
    if (!g_hcs_dll)
        return FALSE;

    pfnCreateOp  = (PFN_HcsCreateOperation)GetProcAddress(g_hcs_dll, "HcsCreateOperation");
    pfnWaitOp    = (PFN_HcsWaitForOperationResult)GetProcAddress(g_hcs_dll, "HcsWaitForOperationResult");
    pfnCloseOp   = (PFN_HcsCloseOperation)GetProcAddress(g_hcs_dll, "HcsCloseOperation");
    pfnCreate    = (PFN_HcsCreateComputeSystem)GetProcAddress(g_hcs_dll, "HcsCreateComputeSystem");
    pfnStart     = (PFN_HcsStartComputeSystem)GetProcAddress(g_hcs_dll, "HcsStartComputeSystem");
    pfnShutDown  = (PFN_HcsShutDownComputeSystem)GetProcAddress(g_hcs_dll, "HcsShutDownComputeSystem");
    pfnTerminate = (PFN_HcsTerminateComputeSystem)GetProcAddress(g_hcs_dll, "HcsTerminateComputeSystem");
    pfnClose     = (PFN_HcsCloseComputeSystem)GetProcAddress(g_hcs_dll, "HcsCloseComputeSystem");
    pfnPause     = (PFN_HcsPauseComputeSystem)GetProcAddress(g_hcs_dll, "HcsPauseComputeSystem");
    pfnResume    = (PFN_HcsResumeComputeSystem)GetProcAddress(g_hcs_dll, "HcsResumeComputeSystem");
    pfnSave      = (PFN_HcsSaveComputeSystem)GetProcAddress(g_hcs_dll, "HcsSaveComputeSystem");
    pfnGetProps  = (PFN_HcsGetComputeSystemProperties)GetProcAddress(g_hcs_dll, "HcsGetComputeSystemProperties");
    pfnModify    = (PFN_HcsModifyComputeSystem)GetProcAddress(g_hcs_dll, "HcsModifyComputeSystem");
    pfnOpen      = (PFN_HcsOpenComputeSystem)GetProcAddress(g_hcs_dll, "HcsOpenComputeSystem");
    pfnGrantAccess = (PFN_HcsGrantVmAccess)GetProcAddress(g_hcs_dll, "HcsGrantVmAccess");
    pfnCreateVmgs  = (PFN_HcsCreateEmptyGuestStateFile)GetProcAddress(g_hcs_dll, "HcsCreateEmptyGuestStateFile");
    pfnCreateVmrs  = (PFN_HcsCreateEmptyRuntimeStateFile)GetProcAddress(g_hcs_dll, "HcsCreateEmptyRuntimeStateFile");
    pfnEnumSystems = (PFN_HcsEnumerateComputeSystems)GetProcAddress(g_hcs_dll, "HcsEnumerateComputeSystems");
    pfnRegisterCallback = (PFN_HcsRegisterComputeSystemCallback)GetProcAddress(g_hcs_dll, "HcsRegisterComputeSystemCallback");
    pfnUnregisterCallback = (PFN_HcsUnregisterComputeSystemCallback)GetProcAddress(g_hcs_dll, "HcsUnregisterComputeSystemCallback");
    pfnSetCallback = (PFN_HcsSetComputeSystemCallback)GetProcAddress(g_hcs_dll, "HcsSetComputeSystemCallback");

    /* Check that essential functions loaded */
    if (!pfnCreateOp || !pfnWaitOp || !pfnCloseOp ||
        !pfnCreate || !pfnStart || !pfnClose) {
        FreeLibrary(g_hcs_dll);
        g_hcs_dll = NULL;
        return FALSE;
    }

    return TRUE;
}

void hcs_cleanup(void)
{
    if (g_hcs_dll) {
        FreeLibrary(g_hcs_dll);
        g_hcs_dll = NULL;
    }
}

/* Pump messages and sleep for the given duration without blocking the UI */
static void pump_sleep(int ms)
{
    int elapsed = 0;
    while (elapsed < ms) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(100);
        elapsed += 100;
    }
}

static void kill_all_vmwp(void); /* forward declaration */

void hcs_destroy_stale(const wchar_t *name)
{
    HCS_SYSTEM stale = NULL;
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnClose || !pfnOpen) return;

    hr = pfnOpen(name, GENERIC_ALL, &stale);
    if (FAILED(hr) || !stale) {
        ui_log(L"hcs_destroy_stale: open \"%s\" failed (0x%08X), killing orphaned vmwp...", name, hr);
        kill_all_vmwp();
        return;
    }

    ui_log(L"hcs_destroy_stale: found stale system \"%s\", terminating...", name);

    if (pfnTerminate) {
        op = pfnCreateOp(NULL, NULL);
        if (op) {
            hr = pfnTerminate(stale, op, NULL);
            hr = hcs_exec_and_wait(hr, op);
            ui_log(L"hcs_destroy_stale: terminate result 0x%08X", hr);
        }
    }

    pfnClose(stale);

    /* Give vmwp.exe time to fully exit and release file handles */
    pump_sleep(2000);
}

/* Try to open an existing HCS compute system by name.
   If found and running, populates instance->handle and returns TRUE.
   If not found or not running, returns FALSE. */
BOOL hcs_try_open_vm(VmInstance *instance)
{
    HCS_SYSTEM sys = NULL;
    HCS_OPERATION op;
    HRESULT hr;
    PWSTR result_doc = NULL;
    BOOL is_running = FALSE;

    if (!g_hcs_dll || !pfnOpen || !pfnGetProps) {
        ui_log(L"hcs_try_open: HCS not loaded (dll=%p open=%p props=%p)",
               g_hcs_dll, pfnOpen, pfnGetProps);
        return FALSE;
    }

    hr = pfnOpen(instance->name, GENERIC_ALL, &sys);
    if (FAILED(hr) || !sys) {
        ui_log(L"hcs_try_open: \"%s\" open failed (0x%08X)", instance->name, hr);
        return FALSE;
    }

    /* Query properties to check state */
    op = pfnCreateOp(NULL, NULL);
    if (op) {
        hr = pfnGetProps(sys, op, L"{\"PropertyTypes\":[\"Basic\"]}");
        if (SUCCEEDED(hr))
            hr = pfnWaitOp(op, 5000, &result_doc);
        if (SUCCEEDED(hr) && result_doc) {
            ui_log(L"hcs_try_open: \"%s\" props: %s", instance->name, result_doc);
            if (wcsstr(result_doc, L"\"Running\""))
                is_running = TRUE;
            /* Cache RuntimeId while we have valid properties */
            parse_runtime_id(result_doc, &instance->runtime_id);
        } else {
            ui_log(L"hcs_try_open: \"%s\" query failed (0x%08X)", instance->name, hr);
        }
        if (result_doc) LocalFree(result_doc);
        pfnCloseOp(op);
    }

    if (is_running) {
        instance->handle = sys;
        instance->running = TRUE;
        ui_log(L"hcs_try_open: \"%s\" is running, reconnected.", instance->name);
        if (instance->runtime_id.Data1 != 0)
            ui_log(L"hcs_try_open: RuntimeId: %08lx-%04x-%04x-...",
                   instance->runtime_id.Data1,
                   instance->runtime_id.Data2,
                   instance->runtime_id.Data3);
        return TRUE;
    }

    ui_log(L"hcs_try_open: \"%s\" exists but not running.", instance->name);
    pfnClose(sys);
    return FALSE;
}

/* Enable a privilege in the current process token. */
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

/* Kill all vmwp.exe processes (orphaned VM worker processes).
   Requires SeDebugPrivilege since vmwp.exe runs as SYSTEM. */
static void kill_all_vmwp(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int killed = 0;

    enable_privilege(SE_DEBUG_NAME);

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"vmwp.exe") == 0) {
                HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (proc) {
                    ui_log(L"Killing orphaned vmwp.exe (PID %lu)...", pe.th32ProcessID);
                    TerminateProcess(proc, 1);
                    CloseHandle(proc);
                    killed++;
                } else {
                    ui_log(L"Cannot open vmwp.exe PID %lu (error %lu)",
                           pe.th32ProcessID, GetLastError());
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (killed > 0) Sleep(2000);
}

/* Wait for a file to become unlocked (vmwp.exe releasing handles after terminate).
   If still locked after initial wait, kills the locking processes.
   Returns TRUE if file is available, FALSE if still locked. */
static BOOL wait_for_file_available(const wchar_t *path, int timeout_ms)
{
    int elapsed = 0;
    HANDLE h;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return TRUE; /* File doesn't exist — that's fine */

    /* First try: wait for natural release, pumping messages to avoid UI lockup */
    while (elapsed < timeout_ms) {
        MSG msg;
        h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return TRUE;
        }
        if (GetLastError() != ERROR_SHARING_VIOLATION)
            return TRUE; /* Not a sharing issue */
        /* Pump messages so UI stays responsive during wait */
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(500);
        elapsed += 500;
    }

    /* Still locked — kill orphaned vmwp.exe processes */
    ui_log(L"VHDX still locked after %ds, killing orphaned vmwp.exe...", timeout_ms / 1000);
    kill_all_vmwp();

    /* Final check */
    h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return TRUE;
    }
    return FALSE;
}

BOOL hcs_build_vm_json(const VmConfig *config, const wchar_t *endpoint_guid,
                       wchar_t *json_out, size_t json_out_chars)
{
    wchar_t vhdx_esc[MAX_PATH * 2];
    wchar_t iso_esc[MAX_PATH * 2];
    wchar_t res_esc[MAX_PATH * 2];
    wchar_t vmgs_esc[MAX_PATH * 2];
    wchar_t vmrs_esc[MAX_PATH * 2];
    wchar_t iso_section[512];
    wchar_t res_section[512];
    wchar_t net_section[512];
    wchar_t secureboot_section[512];
    wchar_t security_section[512];
    wchar_t guest_state_section[1024];
    wchar_t video_section[1024];
    wchar_t plan9_section[5120];
    wchar_t vmgs_path[MAX_PATH];
    wchar_t vmrs_path[MAX_PATH];
    wchar_t dir[MAX_PATH];
    wchar_t *user_sid;
    BOOL is_windows;
    HRESULT vmgs_hr, vmrs_hr;
    int written;

    if (!config || !json_out || json_out_chars < 8192)
        return FALSE;

    is_windows = (_wcsicmp(config->os_type, L"Windows") == 0);

    escape_json_path(config->vhdx_path, vhdx_esc, MAX_PATH * 2);

    /* Optional ISO attachment at SCSI slot 1 */
    iso_section[0] = L'\0';
    if (config->image_path[0] != L'\0') {
        escape_json_path(config->image_path, iso_esc, MAX_PATH * 2);
        swprintf_s(iso_section, 512,
            L",\"1\":{\"Type\":\"Iso\",\"Path\":\"%s\"}", iso_esc);
    }

    /* Optional resources ISO at SCSI slot 2 (autounattend + p9client) */
    res_section[0] = L'\0';
    if (config->resources_iso_path[0] != L'\0') {
        escape_json_path(config->resources_iso_path, res_esc, MAX_PATH * 2);
        swprintf_s(res_section, 512,
            L",\"2\":{\"Type\":\"Iso\",\"Path\":\"%s\"}", res_esc);
    }

    /* Network adapter — skip for template VMs (no network during template creation) */
    net_section[0] = L'\0';
    if (endpoint_guid && endpoint_guid[0] != L'\0' && !config->is_template) {
        swprintf_s(net_section, 512,
            L",\"NetworkAdapters\":{\"Default\":{\"EndpointId\":\"%s\"}}",
            endpoint_guid);
    }

    /* Secure Boot — uses ApplySecureBootTemplate + SecureBootTemplateId
       inside the Uefi section. No BootThis — UEFI auto-discovers boot devices.
       Skip when test_mode is enabled (required for test-signed drivers like VDD). */
    secureboot_section[0] = L'\0';
    if (config->test_mode) {
        /* No Secure Boot — allows test-signed drivers to load */
    } else if (is_windows) {
        wcscpy_s(secureboot_section, 512,
            L",\"ApplySecureBootTemplate\":\"Apply\""
            L",\"SecureBootTemplateId\":\"1734c6e8-3154-4dda-ba5f-a874cc483422\"");
    } else {
        wcscpy_s(secureboot_section, 512,
            L",\"ApplySecureBootTemplate\":\"Apply\""
            L",\"SecureBootTemplateId\":\"272e7447-90a4-4563-a4b9-8e4ab00526ce\"");
    }

    /* VMGS + VMRS files — always created regardless of TPM/SecureBoot.
       GuestState section is always included in the JSON. */
    wcscpy_s(dir, MAX_PATH, config->vhdx_path);
    {
        wchar_t *last_slash = wcsrchr(dir, L'\\');
        if (last_slash) *last_slash = L'\0';
    }
    swprintf_s(vmgs_path, MAX_PATH, L"%s\\vm.vmgs", dir);
    swprintf_s(vmrs_path, MAX_PATH, L"%s\\vm.vmrs", dir);

    /* Delete stale VMGS/VMRS and recreate — stale files from a previous VM
       instance with different secure boot state cause 0x80370118 on create.
       After reboot, HcsGrantVmAccess ACLs persist on old files and may prevent
       deletion, so reset the DACL before deleting. */
    vmgs_hr = S_OK;
    vmrs_hr = S_OK;
    if (pfnCreateVmgs) {
        if (!DeleteFileW(vmgs_path) &&
            GetLastError() == ERROR_ACCESS_DENIED) {
            /* Reset DACL to allow delete — stale HcsGrantVmAccess ACLs */
            SetNamedSecurityInfoW(vmgs_path, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
            DeleteFileW(vmgs_path);
        }
        vmgs_hr = pfnCreateVmgs(vmgs_path);
        if (FAILED(vmgs_hr))
            ui_log(L"Warning: HcsCreateEmptyGuestStateFile failed (0x%08X)", vmgs_hr);
    }
    if (pfnCreateVmrs) {
        if (!DeleteFileW(vmrs_path) &&
            GetLastError() == ERROR_ACCESS_DENIED) {
            SetNamedSecurityInfoW(vmrs_path, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
            DeleteFileW(vmrs_path);
        }
        vmrs_hr = pfnCreateVmrs(vmrs_path);
        if (FAILED(vmrs_hr))
            ui_log(L"Warning: HcsCreateEmptyRuntimeStateFile failed (0x%08X)", vmrs_hr);
    }

    /* Grant VM access to VMGS/VMRS files */
    if (pfnGrantAccess) {
        if (GetFileAttributesW(vmgs_path) != INVALID_FILE_ATTRIBUTES)
            pfnGrantAccess(config->name, vmgs_path);
        if (GetFileAttributesW(vmrs_path) != INVALID_FILE_ATTRIBUTES)
            pfnGrantAccess(config->name, vmrs_path);
    }

    escape_json_path(vmgs_path, vmgs_esc, MAX_PATH * 2);
    escape_json_path(vmrs_path, vmrs_esc, MAX_PATH * 2);

    /* GuestState — always included */
    swprintf_s(guest_state_section, 1024,
        L",\"GuestState\":{"
        L"\"GuestStateFilePath\":\"%s\","
        L"\"RuntimeStateFilePath\":\"%s\""
        L"}", vmgs_esc, vmrs_esc);

    /* VideoMonitor + EnhancedModeVideo with named pipe ConnectionOptions.
       vmwp.exe creates an RDP stream on these pipes for the VM display.
       NOTE: VideoMonitor is REQUIRED — vmwp.exe crashes (0xC0000005) without it. */
    user_sid = get_current_user_sid();
    if (user_sid) {
        ui_log(L"VideoMonitor: pipe=%s.BasicSession, SID=%s", config->name, user_sid);
        swprintf_s(video_section, 1024,
            L"\"VideoMonitor\":{"
                L"\"HorizontalResolution\":1024,"
                L"\"VerticalResolution\":768,"
                L"\"ConnectionOptions\":{"
                    L"\"NamedPipe\":\"\\\\\\\\.\\\\pipe\\\\%s.BasicSession\","
                    L"\"AccessSids\":[\"%s\"]"
                L"}"
            L"},",
            config->name, user_sid);
        LocalFree(user_sid);
    } else {
        ui_log(L"WARNING: Could not get user SID — VideoMonitor pipes will have no AccessSids");
        wcscpy_s(video_section, 1024,
            L"\"VideoMonitor\":{"
                L"\"HorizontalResolution\":1024,"
                L"\"VerticalResolution\":768"
            L"},");
    }

    /* SecuritySettings with TPM — enabled for all Windows VMs.
       TPM does not block test-signed drivers; only Secure Boot does. */
    security_section[0] = L'\0';
    if (is_windows) {
        wcscpy_s(security_section, 512,
            L",\"SecuritySettings\":{"
            L"\"EnableTpm\":true,"
            L"\"Isolation\":{\"IsolationType\":\"GuestStateOnly\"}"
            L"}");
    }

    /* Plan9 shares — GPU driver directories + manifest for p9client copy.
       Skip for template VMs (no GPU assigned during template creation). */
    plan9_section[0] = L'\0';
    if (config->gpu_mode == GPU_DEFAULT && !config->is_template) {
        wchar_t esc_buf[MAX_PATH * 2];
        wchar_t shares[8192];
        int share_count = 0;
        int i;

        shares[0] = L'\0';

        /* Add a Plan9 share for each GPU driver directory */
        for (i = 0; i < config->gpu_shares.count; i++) {
            const GpuDriverShare *ds = &config->gpu_shares.shares[i];
            char share_name_a[128];
            wchar_t name_esc[256];

            if (GetFileAttributesW(ds->host_path) == INVALID_FILE_ATTRIBUTES)
                continue;

            if (pfnGrantAccess) pfnGrantAccess(config->name, ds->host_path);
            escape_json_path(ds->host_path, esc_buf, MAX_PATH * 2);

            /* Share name must be narrow for JSON */
            WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                share_name_a, 128, NULL, NULL);
            wcscpy_s(name_esc, 256, ds->share_name);

            swprintf_s(shares + wcslen(shares), 8192 - wcslen(shares),
                L"%s{\"Name\":\"%s\","
                L"\"AccessName\":\"%s\","
                L"\"Path\":\"%s\","
                L"\"Port\":50001,"
                L"\"Flags\":1}",
                share_count > 0 ? L"," : L"",
                name_esc, name_esc, esc_buf);
            share_count++;
            ui_log(L"Plan9 share: %s -> %s", ds->share_name, ds->host_path);
        }

        /* DISABLED: Manifest share — share metadata is now sent directly to the
           agent service over the HvSocket connection. To revert, uncomment this
           block and restore p9client.exe + gpudrv-copy.cmd in disk_util.c. */
#if 0 /* >>> REVERT: uncomment to restore AppSandbox.Manifest Plan9 share <<< */
        /* Manifest share: write shares.txt listing all share→dest mappings */
        if (share_count > 0) {
            wchar_t manifest_dir[MAX_PATH];
            wchar_t manifest_file[MAX_PATH];
            wchar_t base_dir[MAX_PATH];
            FILE *mf;

            if (!GetEnvironmentVariableW(L"ProgramData", base_dir, MAX_PATH))
                wcscpy_s(base_dir, MAX_PATH, L"C:\\ProgramData");
            swprintf_s(manifest_dir, MAX_PATH, L"%s\\AppSandbox\\%s\\manifest",
                       base_dir, config->name);
            /* Create nested dirs */
            {
                wchar_t tmp[MAX_PATH];
                wchar_t *p;
                wcscpy_s(tmp, MAX_PATH, manifest_dir);
                for (p = tmp + 3; *p; p++) {
                    if (*p == L'\\') {
                        *p = L'\0';
                        CreateDirectoryW(tmp, NULL);
                        *p = L'\\';
                    }
                }
                CreateDirectoryW(tmp, NULL);
            }

            swprintf_s(manifest_file, MAX_PATH, L"%s\\shares.txt", manifest_dir);
            if (_wfopen_s(&mf, manifest_file, L"w") == 0 && mf) {
                for (i = 0; i < config->gpu_shares.count; i++) {
                    const GpuDriverShare *ds = &config->gpu_shares.shares[i];
                    if (GetFileAttributesW(ds->host_path) != INVALID_FILE_ATTRIBUTES) {
                        if (ds->file_filter[0])
                            fprintf(mf, "%ls %ls %ls\n",
                                    ds->share_name, ds->guest_path, ds->file_filter);
                        else
                            fprintf(mf, "%ls %ls\n",
                                    ds->share_name, ds->guest_path);
                    }
                }
                fclose(mf);
            }

            /* Add manifest as a Plan9 share */
            if (pfnGrantAccess) pfnGrantAccess(config->name, manifest_dir);
            escape_json_path(manifest_dir, esc_buf, MAX_PATH * 2);
            swprintf_s(shares + wcslen(shares), 8192 - wcslen(shares),
                L",{\"Name\":\"AppSandbox.Manifest\","
                L"\"AccessName\":\"AppSandbox.Manifest\","
                L"\"Path\":\"%s\","
                L"\"Port\":50001,"
                L"\"Flags\":1}",
                esc_buf);
            share_count++;
        }
#endif /* >>> END REVERT AppSandbox.Manifest share <<< */

        /* lxss\lib: GPU libs for Linux guests */
        {
            wchar_t sys_dir[MAX_PATH];
            wchar_t path[MAX_PATH];
            GetSystemDirectoryW(sys_dir, MAX_PATH);
            swprintf_s(path, MAX_PATH, L"%s\\lxss\\lib", sys_dir);
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                if (pfnGrantAccess) pfnGrantAccess(config->name, path);
                escape_json_path(path, esc_buf, MAX_PATH * 2);
                swprintf_s(shares + wcslen(shares), 8192 - wcslen(shares),
                    L"%s{\"Name\":\"AppSandbox.HostLxssLib\","
                    L"\"AccessName\":\"AppSandbox.HostLxssLib\","
                    L"\"Path\":\"%s\","
                    L"\"Port\":50001,"
                    L"\"Flags\":1}",
                    share_count > 0 ? L"," : L"", esc_buf);
                share_count++;
            }
        }

        if (share_count > 0) {
            swprintf_s(plan9_section, 5120,
                L",\"Plan9\":{\"Shares\":[%s]}", shares);
        }
    }

    written = swprintf_s(json_out, json_out_chars,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":1},"
        L"\"Owner\":\"%s\","
        L"\"ShouldTerminateOnLastHandleClosed\":true,"
        L"\"VirtualMachine\":{"
            L"\"Chipset\":{"
                L"\"Uefi\":{"
                    L"\"Console\":\"Default\""
                    L"%s"
                L"}"
            L"},"
            L"\"ComputeTopology\":{"
                L"\"Memory\":{"
                    L"\"SizeInMB\":%lu,"
                    L"\"AllowOvercommit\":true"
                L"},"
                L"\"Processor\":{"
                    L"\"Count\":%lu"
                L"}"
            L"},"
            L"\"Devices\":{"
                L"\"Scsi\":{"
                    L"\"Primary\":{"
                        L"\"Attachments\":{"
                            L"\"0\":{\"Type\":\"VirtualDisk\",\"Path\":\"%s\"}"
                            L"%s"
                            L"%s"
                        L"}"
                    L"}"
                L"},"
                L"%s"
                L"\"HvSocket\":{\"HvSocketConfig\":{"
                    L"\"ServiceTable\":{"
                        L"\"a5b0cafe-0001-4000-8000-000000000001\":{"
                            L"\"BindSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"AllowWildcardBinds\":true"
                        L"},"
                        L"\"a5b0cafe-0002-4000-8000-000000000001\":{"
                            L"\"BindSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"AllowWildcardBinds\":true"
                        L"},"
                        L"\"a5b0cafe-0003-4000-8000-000000000001\":{"
                            L"\"BindSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"AllowWildcardBinds\":true"
                        L"},"
                        L"\"a5b0cafe-0004-4000-8000-000000000001\":{"
                            L"\"BindSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"AllowWildcardBinds\":true"
                        L"},"
                        L"\"a5b0cafe-0005-4000-8000-000000000001\":{"
                            L"\"BindSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"ConnectSecurityDescriptor\":\"D:P(A;;FA;;;WD)\","
                            L"\"AllowWildcardBinds\":true"
                        L"}"
                    L"}"
                L"}},"
                L"\"Keyboard\":{},"
                L"\"Mouse\":{}"
                L"%s"
                L"%s"
            L"}"
            L"%s"
            L"%s"
        L"}"
        L"}",
        config->name,
        secureboot_section,
        config->ram_mb,
        config->cpu_cores,
        vhdx_esc,
        iso_section,
        res_section,
        video_section,
        net_section,
        plan9_section,
        security_section,
        guest_state_section);

    return written > 0;
}

HRESULT hcs_create_vm(const VmConfig *config, VmInstance *instance)
{
    wchar_t json[16384];
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnCreate)
        return E_NOT_VALID_STATE;

    /* Wait for VHDX to be released by old vmwp.exe (up to 3s) */
    if (!wait_for_file_available(config->vhdx_path, 3000)) {
        ui_log(L"Error: VHDX file is still locked. Reboot or kill vmwp.exe in Task Manager.");
        return HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
    }

    /* Grant VM access to VHDX and ISO files (required by vmwp.exe).
       After reboot, stale HcsGrantVmAccess ACLs may persist on reused files —
       reset DACL if grant fails with ACCESS_DENIED. */
    if (pfnGrantAccess) {
        HRESULT grant_hr;
        grant_hr = pfnGrantAccess(config->name, config->vhdx_path);
        if (FAILED(grant_hr)) {
            ui_log(L"GrantVmAccess failed on VHDX (0x%08X), resetting DACL...", grant_hr);
            { wchar_t tmp[MAX_PATH]; wcscpy_s(tmp, MAX_PATH, config->vhdx_path);
            SetNamedSecurityInfoW(tmp, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL); }
            grant_hr = pfnGrantAccess(config->name, config->vhdx_path);
            if (FAILED(grant_hr))
                ui_log(L"Warning: GrantVmAccess retry on VHDX failed (0x%08X)", grant_hr);
        }
        if (config->image_path[0] != L'\0')
            pfnGrantAccess(config->name, config->image_path);
        if (config->resources_iso_path[0] != L'\0')
            pfnGrantAccess(config->name, config->resources_iso_path);
    }

    /* Build JSON — endpoint_guid is set later by caller if networking is used */
    if (!hcs_build_vm_json(config, NULL, json, 16384))
        return E_FAIL;

    /* Dump JSON to file for debugging */
    {
        FILE *dbg = NULL;
        _wfopen_s(&dbg, L"C:\\ProgramData\\AppSandbox\\last_vm.json", L"w,ccs=UTF-8");
        if (dbg) { fwprintf(dbg, L"%s", json); fclose(dbg); }
    }

    /* Destroy any stale system with the same name before creating */
    hcs_destroy_stale(config->name);

    op = pfnCreateOp(NULL, NULL);
    if (!op)
        return E_OUTOFMEMORY;

    hr = pfnCreate(config->name, json, op, NULL, &instance->handle);
    hr = hcs_exec_and_wait(hr, op);

    /* Retry once — hcs_destroy_stale may have killed vmwp.exe to clear a stuck system */
    if (hr == (HRESULT)0x8037010DL || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        ui_log(L"Retrying VM create after stale cleanup...");
        pump_sleep(2000);
        op = pfnCreateOp(NULL, NULL);
        if (op) {
            hr = pfnCreate(config->name, json, op, NULL, &instance->handle);
            hr = hcs_exec_and_wait(hr, op);
        }
    }

    if (SUCCEEDED(hr)) {
        wcscpy_s(instance->name, 256, config->name);
        wcscpy_s(instance->os_type, 32, config->os_type);
        wcscpy_s(instance->vhdx_path, MAX_PATH, config->vhdx_path);
        wcscpy_s(instance->image_path, MAX_PATH, config->image_path);
        instance->ram_mb = config->ram_mb;
        instance->hdd_gb = config->hdd_gb;
        instance->cpu_cores = config->cpu_cores;
        instance->gpu_mode = config->gpu_mode;
        instance->network_mode = config->network_mode;
        instance->is_template = config->is_template;
        instance->test_mode = config->test_mode;
        memcpy(&instance->gpu_shares, &config->gpu_shares, sizeof(GpuDriverShareList));
        instance->running = FALSE;

        /* Register callback after create, before start */
        hcs_register_vm_callback(instance);
    }

    return hr;
}

HRESULT hcs_create_vm_with_endpoint(const VmConfig *config, const wchar_t *endpoint_guid,
                                     VmInstance *instance)
{
    wchar_t json[16384];
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnCreate)
        return E_NOT_VALID_STATE;

    /* Wait for VHDX to be released by old vmwp.exe (up to 3s) */
    if (!wait_for_file_available(config->vhdx_path, 3000)) {
        ui_log(L"Error: VHDX file is still locked. Reboot or kill vmwp.exe in Task Manager.");
        return HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
    }

    /* Grant VM access to VHDX and ISO files (required by vmwp.exe).
       After reboot, stale HcsGrantVmAccess ACLs may persist on reused files —
       reset DACL if grant fails. */
    if (pfnGrantAccess) {
        HRESULT grant_hr;
        grant_hr = pfnGrantAccess(config->name, config->vhdx_path);
        if (FAILED(grant_hr)) {
            ui_log(L"GrantVmAccess failed on VHDX (0x%08X), resetting DACL...", grant_hr);
            { wchar_t tmp[MAX_PATH]; wcscpy_s(tmp, MAX_PATH, config->vhdx_path);
            SetNamedSecurityInfoW(tmp, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL); }
            grant_hr = pfnGrantAccess(config->name, config->vhdx_path);
            if (FAILED(grant_hr))
                ui_log(L"Warning: GrantVmAccess retry on VHDX failed (0x%08X)", grant_hr);
        }
        if (config->image_path[0] != L'\0')
            pfnGrantAccess(config->name, config->image_path);
        if (config->resources_iso_path[0] != L'\0')
            pfnGrantAccess(config->name, config->resources_iso_path);
    }

    if (!hcs_build_vm_json(config, endpoint_guid, json, 16384))
        return E_FAIL;

    /* Dump JSON to file for debugging */
    {
        FILE *dbg = NULL;
        _wfopen_s(&dbg, L"C:\\ProgramData\\AppSandbox\\last_vm.json", L"w,ccs=UTF-8");
        if (dbg) { fwprintf(dbg, L"%s", json); fclose(dbg); }
    }

    /* Destroy any stale system with the same name before creating */
    hcs_destroy_stale(config->name);

    op = pfnCreateOp(NULL, NULL);
    if (!op)
        return E_OUTOFMEMORY;

    hr = pfnCreate(config->name, json, op, NULL, &instance->handle);
    hr = hcs_exec_and_wait(hr, op);

    /* Retry once — hcs_destroy_stale may have killed vmwp.exe to clear a stuck system */
    if (hr == (HRESULT)0x8037010DL || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        ui_log(L"Retrying VM create after stale cleanup...");
        pump_sleep(2000);
        op = pfnCreateOp(NULL, NULL);
        if (op) {
            hr = pfnCreate(config->name, json, op, NULL, &instance->handle);
            hr = hcs_exec_and_wait(hr, op);
        }
    }

    if (SUCCEEDED(hr)) {
        wcscpy_s(instance->name, 256, config->name);
        wcscpy_s(instance->os_type, 32, config->os_type);
        wcscpy_s(instance->vhdx_path, MAX_PATH, config->vhdx_path);
        wcscpy_s(instance->image_path, MAX_PATH, config->image_path);
        instance->ram_mb = config->ram_mb;
        instance->hdd_gb = config->hdd_gb;
        instance->cpu_cores = config->cpu_cores;
        instance->gpu_mode = config->gpu_mode;
        instance->network_mode = config->network_mode;
        instance->is_template = config->is_template;
        instance->test_mode = config->test_mode;
        memcpy(&instance->gpu_shares, &config->gpu_shares, sizeof(GpuDriverShareList));
        instance->running = FALSE;

        /* Register callback after create, before start */
        hcs_register_vm_callback(instance);
    }

    return hr;
}

/* Apply GPU-PV via HcsModifyComputeSystem AFTER VM start.
   Sequence: Create → Start → ModifyGpu. */
static HRESULT hcs_apply_gpu(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;
    wchar_t modify_json[2048];

    if (!pfnModify || !instance->handle)
        return E_NOT_VALID_STATE;

    if (instance->gpu_mode == GPU_DEFAULT) {
        wcscpy_s(modify_json, 2048,
            L"{"
            L"\"ResourcePath\":\"VirtualMachine/ComputeTopology/Gpu\","
            L"\"RequestType\":\"Update\","
            L"\"Settings\":{"
                L"\"AssignmentMode\":\"Default\","
                L"\"AllowVendorExtension\":true"
            L"}"
            L"}");
    } else {
        return S_OK; /* No GPU to apply */
    }

    ui_log(L"Applying GPU-PV...");

    op = pfnCreateOp(NULL, NULL);
    if (!op) return E_OUTOFMEMORY;

    hr = pfnModify(instance->handle, op, modify_json, NULL);
    hr = hcs_exec_and_wait(hr, op);

    if (FAILED(hr))
        ui_log(L"Warning: GPU-PV apply failed (0x%08X). VM will run without GPU.", hr);
    else
        ui_log(L"GPU-PV applied successfully.");

    return hr;
}

HRESULT hcs_start_vm(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnStart || !instance->handle)
        return E_NOT_VALID_STATE;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return E_OUTOFMEMORY;

    hr = pfnStart(instance->handle, op, NULL);
    hr = hcs_exec_and_wait(hr, op);

    if (SUCCEEDED(hr)) {
        instance->running = TRUE;

        /* Cache RuntimeId for AF_HYPERV agent connections */
        cache_runtime_id(instance);

        /* Apply GPU-PV after start (Create → Start → ModifyGpu).
           Skip for template VMs (no GPU during template creation). */
        if (instance->gpu_mode != GPU_NONE && !instance->is_template)
            hcs_apply_gpu(instance);
    }

    return hr;
}

/* Hot-detach the network adapter from a running VM so that HCS can
   deliver SystemExited when the guest shuts down. */
void hcs_detach_network(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;
    static const wchar_t detach_json[] =
        L"{\"ResourcePath\":\"VirtualMachine/Devices/NetworkAdapters/Default\","
        L"\"RequestType\":\"Remove\"}";

    if (!pfnModify || !instance->handle)
        return;
    if (instance->network_mode == NET_NONE)
        return;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return;

    hr = pfnModify(instance->handle, op, detach_json, NULL);
    hr = hcs_exec_and_wait(hr, op);

    if (SUCCEEDED(hr))
        ui_log(L"Network adapter detached from VM.");
    else
        ui_log(L"Network adapter detach failed (0x%08X).", hr);
}

HRESULT hcs_stop_vm(VmInstance *instance)
{
    if (!instance->handle)
        return E_NOT_VALID_STATE;

    /* Try our guest agent first — sends shutdown via Hyper-V socket.
       Don't detach network or modify the VM — let it shut down undisturbed.
       Cleanup happens when SystemExited arrives via HCS callback. */
    ui_log(L"Requesting graceful shutdown via guest agent...");
    if (vm_agent_shutdown(instance)) {
        ui_log(L"Guest agent accepted shutdown. Waiting for VM to exit...");
        return S_OK;
    }

    ui_log(L"Guest agent unavailable. Use Force Stop to terminate the VM.");
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

HRESULT hcs_terminate_vm(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnTerminate || !instance->handle)
        return E_NOT_VALID_STATE;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return E_OUTOFMEMORY;

    hr = pfnTerminate(instance->handle, op, NULL);
    hr = hcs_exec_and_wait(hr, op);

    instance->running = FALSE;
    return hr;
}

HRESULT hcs_pause_vm(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnPause || !instance->handle)
        return E_NOT_VALID_STATE;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return E_OUTOFMEMORY;

    hr = pfnPause(instance->handle, op, NULL);
    hr = hcs_exec_and_wait(hr, op);
    return hr;
}

HRESULT hcs_resume_vm(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnResume || !instance->handle)
        return E_NOT_VALID_STATE;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return E_OUTOFMEMORY;

    hr = pfnResume(instance->handle, op, NULL);
    hr = hcs_exec_and_wait(hr, op);
    return hr;
}

HRESULT hcs_save_vm(VmInstance *instance, const wchar_t *state_path)
{
    wchar_t save_json[1024];
    wchar_t path_esc[MAX_PATH * 2];
    HCS_OPERATION op;
    HRESULT hr;

    if (!g_hcs_dll || !pfnSave || !instance->handle)
        return E_NOT_VALID_STATE;

    escape_json_path(state_path, path_esc, MAX_PATH * 2);
    swprintf_s(save_json, 1024,
        L"{\"SaveType\":\"ToFile\",\"SaveStateFilePath\":\"%s\"}", path_esc);

    op = pfnCreateOp(NULL, NULL);
    if (!op) return E_OUTOFMEMORY;

    hr = pfnSave(instance->handle, op, save_json);
    hr = hcs_exec_and_wait(hr, op);
    return hr;
}

static DWORD WINAPI close_vm_thread(LPVOID param)
{
    HCS_SYSTEM h = (HCS_SYSTEM)param;
    /* Callback already unregistered by hcs_close_vm before this thread starts */
    pfnClose(h);
    return 0;
}

void hcs_close_vm(VmInstance *instance)
{
    /* Unregister callback before closing handle — matches hcsshim pattern.
       Must NOT hold any locks during unregister (it waits for pending callbacks). */
    hcs_unregister_vm_callback(instance);

    if (g_hcs_dll && pfnClose && instance->handle) {
        HCS_SYSTEM h = instance->handle;
        instance->handle = NULL;
        /* HcsCloseComputeSystem can block — run it off the UI thread */
        CloseHandle(CreateThread(NULL, 0, close_vm_thread, h, 0, NULL));
    }
}

BOOL hcs_query_guest_status(VmInstance *instance)
{
    HCS_OPERATION op;
    HRESULT hr;
    PWSTR result_doc = NULL;
    BOOL ic_ok = FALSE;

    if (!g_hcs_dll || !pfnGetProps || !instance->handle)
        return FALSE;

    /* Query GuestConnection — returns protocol version, schema versions,
       guest-defined capabilities. Available once guest IC services connect. */
    op = pfnCreateOp(NULL, NULL);
    if (op) {
        hr = pfnGetProps(instance->handle, op,
            L"{\"PropertyTypes\":[\"GuestConnection\"]}");
        if (SUCCEEDED(hr))
            hr = pfnWaitOp(op, 5000, &result_doc);
        if (SUCCEEDED(hr) && result_doc) {
            ui_log(L"GuestConnection: %s", result_doc);
            ic_ok = TRUE;
        } else {
            ui_log(L"GuestConnection query failed (0x%08X)", hr);
        }
        if (result_doc) { LocalFree(result_doc); result_doc = NULL; }
        pfnCloseOp(op);
    }

    /* Query ICHeartbeatStatus — shows if integration components are alive */
    op = pfnCreateOp(NULL, NULL);
    if (op) {
        hr = pfnGetProps(instance->handle, op,
            L"{\"PropertyTypes\":[\"ICHeartbeatStatus\"]}");
        if (SUCCEEDED(hr))
            hr = pfnWaitOp(op, 5000, &result_doc);
        if (SUCCEEDED(hr) && result_doc) {
            ui_log(L"ICHeartbeat: %s", result_doc);
        } else {
            ui_log(L"ICHeartbeat query failed (0x%08X)", hr);
        }
        if (result_doc) { LocalFree(result_doc); result_doc = NULL; }
        pfnCloseOp(op);
    }

    return ic_ok;
}

wchar_t *hcs_query_properties(VmInstance *instance, const wchar_t *query)
{
    HCS_OPERATION op;
    HRESULT hr;
    wchar_t *doc = NULL;

    if (!g_hcs_dll || !pfnGetProps || !instance->handle) {
        ui_log(L"hcs_query_properties: not ready (dll=%p props=%p handle=%p)",
               g_hcs_dll, pfnGetProps, instance->handle);
        return NULL;
    }

    op = pfnCreateOp(NULL, NULL);
    if (!op) return NULL;

    hr = pfnGetProps(instance->handle, op, query);
    if (SUCCEEDED(hr))
        hr = pfnWaitOp(op, 5000, &doc);
    pfnCloseOp(op);

    if (FAILED(hr)) {
        ui_log(L"hcs_query_properties: failed (0x%08X)", hr);
        if (doc) LocalFree(doc);
        return NULL;
    }
    return doc; /* Caller must LocalFree */
}

BOOL hcs_find_runtime_id(const wchar_t *vm_name, GUID *out)
{
    HCS_OPERATION op;
    HRESULT hr;
    PWSTR result_doc = NULL;
    wchar_t needle[300];
    const wchar_t *p;

    if (!g_hcs_dll || !pfnEnumSystems || !pfnCreateOp || !pfnWaitOp)
        return FALSE;

    op = pfnCreateOp(NULL, NULL);
    if (!op) return FALSE;

    hr = pfnEnumSystems(NULL, op);
    if (SUCCEEDED(hr))
        hr = pfnWaitOp(op, 5000, &result_doc);
    pfnCloseOp(op);

    if (FAILED(hr) || !result_doc) {
        ui_log(L"hcs_find_runtime_id: enumerate failed (0x%08X)", hr);
        if (result_doc) LocalFree(result_doc);
        return FALSE;
    }

    ui_log(L"HCS enumerate: %s", result_doc);

    /* Find our VM by name in the JSON array, then find its RuntimeId */
    swprintf_s(needle, 300, L"\"%s\"", vm_name);
    p = wcsstr(result_doc, needle);
    if (p) {
        /* Look for RuntimeId after the name match */
        const wchar_t *rid = wcsstr(p, L"\"RuntimeId\":\"");
        if (rid && parse_runtime_id(rid - 1, out)) {
            ui_log(L"hcs_find_runtime_id: found %08lx-%04x-%04x-...",
                   out->Data1, out->Data2, out->Data3);
            LocalFree(result_doc);
            return TRUE;
        }
    }

    ui_log(L"hcs_find_runtime_id: VM \"%s\" not found in enumeration", vm_name);
    LocalFree(result_doc);
    return FALSE;
}
