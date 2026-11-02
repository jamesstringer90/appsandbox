
#define _CRT_RAND_S
#include <winsock2.h>   /* before windows.h: guards out the winsock.h that http.h clashes with */
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma warning(push)
#pragma warning(disable: 4201)   /* <http.h> uses nameless structs/unions (C4201) */
#include <http.h>
#pragma warning(pop)
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "headless.h"
#include "ui.h"
#include "asb_events.h"
#include "asb_core.h"          /* full API incl. internal VmInstance */
#include "webview2_bridge.h"   /* json_get_string/int/bool for request bodies */
#include "prereq.h"            /* prereq_check_all -> VirtualMachinePlatform check */
#include "vm_display_idd.h"    /* IDD display window, opened on demand via the API */

#pragma comment(lib, "httpapi.lib")
#pragma comment(lib, "advapi32.lib")

#define ASB_API_VERSION  "v1"
/* Product version derives from Directory.Build.props (the single source of truth):
   Directory.Build.targets passes AsbVersionShort in as the unquoted ASB_VERSION_SHORT
   define, stringized here exactly as build\version.rc does for the binary VERSIONINFO.
   The fallback keeps a standalone compile (without the MSBuild define) valid. */
#define ASB_VER_STR2(x) #x
#define ASB_VER_STR(x)  ASB_VER_STR2(x)
#ifndef ASB_VERSION_SHORT
#define ASB_VERSION_SHORT 0.0.0
#endif
#define ASB_PRODUCT_VER  ASB_VER_STR(ASB_VERSION_SHORT)
#define DEFAULT_PORT     8787

/* forward decls (used by the event callbacks defined below) */
static void broadcast_event(const char *json);
static int append_json_str(char *out, int cap, int pos, const char *s);
static int  append_wstr(char *out, int cap, int pos, const wchar_t *w);

/* ---- SSE event broadcast (GET /v1/events) ---- */
#define EV_CAP 256
static CRITICAL_SECTION   g_ev_cs;
static CONDITION_VARIABLE g_ev_cv;
static char              *g_ev_ring[EV_CAP];   /* recent event JSON (ring) */
static LONG               g_ev_seq = 0;        /* total events ever produced */
static volatile LONG      g_stop_all = 0;      /* signals SSE threads to exit */

/* ---- IDD display windows (POST/DELETE /v1/vms/{name}/display) ---- */
static HWND g_gui_hwnd;
static DWORD g_gui_thread;
static HANDLE g_server_thread;
static unsigned g_api_subscription;
static BOOL g_api_initialized;
static LONG g_sse_count;
static HINSTANCE g_hinst;   /* captured in run_headless; passed to vm_display_idd */

/* Per-VM display handles, keyed by VM unique_id (survives g_vms[] compaction). The
   window runs on its own thread inside vm_display_idd, so it never blocks the
   single-threaded request loop. Sized to ASB_MAX_VMS; concurrent VMs are capped at
   16 by the agent table, so this never fills. */
typedef struct { UINT64 vm_id; VmDisplayIdd *disp; } DisplayEntry;
static DisplayEntry g_displays[ASB_MAX_VMS];

static DisplayEntry *display_find(UINT64 vm_id)
{
    int i;
    if (!vm_id) return NULL;
    for (i = 0; i < ASB_MAX_VMS; i++)
        if (g_displays[i].vm_id == vm_id) return &g_displays[i];
    return NULL;
}
static void display_drop(DisplayEntry *e)   /* destroy the window + free the slot */
{
    if (!e) return;
    if (e->disp) vm_display_idd_destroy(e->disp);
    e->disp = NULL; e->vm_id = 0;
}
/* TRUE if this VM has a live (not user-closed) display window. */
static BOOL display_is_open(UINT64 vm_id)
{
    if (g_gui_hwnd) return ui_display_is_open(vm_id);
    DisplayEntry *e = display_find(vm_id);
    return e && e->disp && vm_display_idd_is_open(e->disp);
}
/* If the user closed the window (X), vm_display_idd has already stopped its threads
   and freed the vsock frame connection (WM_CLOSE handler); drop our now-dead handle
   so the slot frees. Called before acting on any display request. */
static void display_reap_stale(UINT64 vm_id)
{
    DisplayEntry *e = display_find(vm_id);
    if (e && e->disp && !vm_display_idd_is_open(e->disp))
        display_drop(e);
}
/* A-gate: can this process put a window on a visible desktop? FALSE in a service /
   non-interactive session, so we refuse instead of spawning an invisible window. */
static BOOL host_can_show_window(void)
{
    HWINSTA ws = GetProcessWindowStation();
    USEROBJECTFLAGS uof; DWORD need = 0;
    if (ws && GetUserObjectInformationW(ws, UOI_FLAGS, &uof, sizeof(uof), &need))
        return (uof.dwFlags & WSF_VISIBLE) != 0;
    return FALSE;
}

/* ---- Logging ---- */

static FILE            *g_log;
static CRITICAL_SECTION g_log_cs;

static void hlog(const wchar_t *fmt, ...)
{
    va_list ap; SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_log_cs);
    if (g_log) {
        fwprintf(g_log, L"%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_start(ap, fmt); vfwprintf(g_log, fmt, ap); va_end(ap);
        fwprintf(g_log, L"\n"); fflush(g_log);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void api_event(const CoreVmEvent *event, void *context)
{
    char ev[4096], idText[ASB_VM_ID_TEXT_SIZE]; int pos;
    asb_vm_id_format(event->vm_id, idText);
    (void)context;
    if (g_stop_all) return;
    switch (event->type) {
    case CORE_VM_EVENT_LOG:
        hlog(L"%S", event->str_value ? event->str_value : "");
        break;
    case CORE_VM_EVENT_STATE_CHANGED:
    case CORE_VM_EVENT_PROGRESS:
        pos = sprintf_s(ev, sizeof(ev), "{\"event\":\"%s\",\"id\":\"%s\",\"name\":",
            event->type == CORE_VM_EVENT_PROGRESS ? "vmProgress" : "vmStateChanged", idText);
        pos = append_json_str(ev, sizeof(ev), pos, event->vm_name);
        if (event->type == CORE_VM_EVENT_PROGRESS)
            sprintf_s(ev + pos, sizeof(ev) - pos, ",\"progress\":%d,\"staging\":%s}",
                      event->int_value, event->staging ? "true" : "false");
        else sprintf_s(ev + pos, sizeof(ev) - pos, ",\"running\":%s}",
                       event->int_value ? "true" : "false");
        broadcast_event(ev);
        break;
    case CORE_VM_EVENT_LIST_CHANGED:
    case CORE_VM_EVENT_AGENT_STATUS:
        broadcast_event("{\"event\":\"vmListChanged\"}");
        break;
    case CORE_VM_EVENT_ALERT:
        pos = sprintf_s(ev, sizeof(ev), "{\"event\":\"alert\",\"message\":");
        pos = append_json_str(ev, sizeof(ev), pos, event->str_value ? event->str_value : "");
        sprintf_s(ev + pos, sizeof(ev) - pos, "}");
        broadcast_event(ev);
        break;
    default: break;
    }
}

/* ---- ProgramData paths ---- */

static void programdata_path(wchar_t *out, size_t cap, const wchar_t *leaf)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, cap, L"%s\\AppSandbox\\%s", base, leaf);
}

static void open_log(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH], base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(dir, MAX_PATH, L"%s\\AppSandbox", base);
    CreateDirectoryW(dir, NULL);
    programdata_path(path, MAX_PATH, L"headless.log");
    InitializeCriticalSection(&g_log_cs);
    _wfopen_s(&g_log, path, L"w, ccs=UTF-8");
}

/* ---- Bootstrap thread (init + park) ---- */

static volatile LONG g_init_done = 0;

static DWORD WINAPI bootstrap_thread(LPVOID p)
{
    (void)p;
    asb_init();
    asb_reconnect_running();
    InterlockedExchange(&g_init_done, 1);
    for (;;) Sleep(60000);   /* park; never exit */
}

/* ---- JSON helpers ---- */

/* Append a JSON string value with the minimal escaping JSON requires. */
static int append_json_str(char *out, int cap, int pos, const char *s)
{
    if (pos >= cap - 1) return pos;          /* no room even for an empty "" */
    out[pos++] = '"';
    /* -8 headroom so the worst-case body write (\uXXXX = 6) plus the closing
       quote + NUL can never run past cap. */
    for (; *s && pos < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[pos++] = '\\'; out[pos++] = c; }
        else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (c == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (c == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else if (c < 0x20)  { pos += sprintf_s(out + pos, cap - pos, "\\u%04x", c); }
        else out[pos++] = c;
    }
    if (pos < cap - 1) out[pos++] = '"';
    out[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

static int append_wstr(char *out, int cap, int pos, const wchar_t *w)
{
    char u[1536] = {0};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    return append_json_str(out, cap, pos, u);
}

static const char *derive_state(VmInstance *v)
{
    if (v->building_vhdx)      return "building";
    if (v->starting)           return "booting";
    if (!v->running)           return "stopped";
    if (v->shutdown_requested) return "stopping";
    if (!v->install_complete)  return "installing";
    if (!v->agent_online)      return "booting";
    return "online";
}

/* Cheap per-VM status object (no disk I/O -- snapshot tree is a separate route). */
static int append_vm_json(char *out, int cap, int pos, VmInstance *v)
{
    wchar_t disk_directory[MAX_PATH];
    char id[ASB_VM_ID_TEXT_SIZE];
    asb_vm_id_format(v->unique_id, id);
    pos += sprintf_s(out + pos, cap - pos, "{\"id\":\"%s\",\"name\":", id);
    pos  = append_wstr(out, cap, pos, v->name);
    pos += sprintf_s(out + pos, cap - pos, ",\"osType\":");
    pos  = append_wstr(out, cap, pos, v->os_type);
    asb_vm_disk_directory((AsbVm)v, disk_directory, MAX_PATH);
    pos += sprintf_s(out + pos, cap - pos, ",\"diskDirectory\":");
    pos = append_wstr(out, cap, pos, disk_directory);
    pos += sprintf_s(out + pos, cap - pos,
        ",\"state\":\"%s\",\"running\":%s,\"agentOnline\":%s,\"installComplete\":%s,"
        "\"building\":%s,\"progress\":%d,\"sshState\":%d,\"sshPort\":%lu,"
        "\"ramMb\":%lu,\"hddGb\":%lu,\"cpuCores\":%lu,\"gpuMode\":%d,\"networkMode\":%d,"
        "\"displayOpen\":%s,\"gpuId\":",
        derive_state(v),
        v->running ? "true" : "false", v->agent_online ? "true" : "false",
        v->install_complete ? "true" : "false", v->building_vhdx ? "true" : "false",
        v->vhdx_progress,
        (v->ssh_key_deployed && v->ssh_state == 2) ? 4 : v->ssh_state,   /* 4 = ready + key deployed */
        (unsigned long)v->ssh_port,
        (unsigned long)v->ram_mb, (unsigned long)v->hdd_gb, (unsigned long)v->cpu_cores,
        v->gpu_mode, v->network_mode,
        display_is_open(v->unique_id) ? "true" : "false");
    pos = append_wstr(out, cap, pos, v->gpu_id);
    pos += sprintf_s(out + pos, cap - pos, ",\"gpuName\":");
    pos = append_wstr(out, cap, pos, v->gpu_name);
    pos += sprintf_s(out + pos, cap - pos, "}");
    return pos;
}

static int build_host_info(char *buf, int cap)
{
    SYSTEM_INFO si; MEMORYSTATUSEX ms; ULARGE_INTEGER freeB;
    const GpuList *gpus = asb_gpu_list();
    wchar_t pd[MAX_PATH];
    int i, count, pos, vmCores = 0, vmRamMb = 0, vmHddGb = 0;
    GetSystemInfo(&si);
    ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    if (!GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) wcscpy_s(pd, MAX_PATH, L"C:\\");
    freeB.QuadPart = 0; GetDiskFreeSpaceExW(pd, &freeB, NULL, NULL);
    count = asb_vm_count();
    for (i = 0; i < count; i++) {
        VmInstance *v = asb_vm_instance(asb_vm_get(i));
        if (!v) continue;
        if (v->running) { vmCores += (int)v->cpu_cores; vmRamMb += (int)v->ram_mb; }
        vmHddGb += (int)v->hdd_gb;
    }
    pos = sprintf_s(buf, cap,
        "{\"hostCores\":%lu,\"hostRamMb\":%llu,\"freeGb\":%llu,"
        "\"vmCores\":%d,\"vmRamMb\":%d,\"vmHddGb\":%d,\"defaultDiskDirectory\":",
        (unsigned long)si.dwNumberOfProcessors,
        (unsigned long long)(ms.ullTotalPhys / (1024ULL * 1024)),
        (unsigned long long)(freeB.QuadPart / (1024ULL * 1024 * 1024)),
        vmCores, vmRamMb, vmHddGb);
    asb_default_disk_directory(pd, MAX_PATH);
    pos = append_wstr(buf, cap, pos, pd);
    pos += sprintf_s(buf + pos, cap - pos, ",\"gpus\":[");
    for (i = 0; gpus && i < gpus->count; i++) {
        if (i) pos += sprintf_s(buf + pos, cap - pos, ",");
        pos += sprintf_s(buf + pos, cap - pos, "{\"id\":");
        pos = append_wstr(buf, cap, pos, gpus->gpus[i].interface_path);
        pos += sprintf_s(buf + pos, cap - pos, ",\"name\":");
        pos = append_wstr(buf, cap, pos, gpus->gpus[i].name);
        pos += sprintf_s(buf + pos, cap - pos, ",\"location\":");
        pos = append_wstr(buf, cap, pos, gpus->gpus[i].location);
        pos += sprintf_s(buf + pos, cap - pos, "}");
    }
    pos += sprintf_s(buf + pos, cap - pos, "]}");
    return pos;
}

/* ---- HTTP helpers ---- */

static HANDLE                 g_req_queue = NULL;
static HTTP_SERVER_SESSION_ID g_session;
static HTTP_URL_GROUP_ID      g_url_group;
static char                   g_token[48];

typedef struct {
    PHTTP_REQUEST request;
    wchar_t body[8192];
    BOOL valid_body;
    USHORT status;
    char reason[64];
    char *response;
    int stop;
    HANDLE done;
} ApiCall;
static ApiCall *g_gui_call;

BOOL asb_api_busy(void) { return GetCurrentThreadId() == g_gui_thread && g_gui_call != NULL; }

static void send_response(HTTP_REQUEST_ID reqId, USHORT status, const char *reason,
                          const char *ctype, const char *body)
{
    HTTP_RESPONSE resp; HTTP_DATA_CHUNK chunk; ULONG sent = 0;
    if (asb_api_busy()) {
        g_gui_call->status = status;
        strcpy_s(g_gui_call->reason, sizeof(g_gui_call->reason), reason);
        free(g_gui_call->response);
        g_gui_call->response = _strdup(body ? body : "{}");
        return;
    }
    RtlZeroMemory(&resp, sizeof(resp));
    resp.StatusCode = status;
    resp.pReason = reason; resp.ReasonLength = (USHORT)strlen(reason);
    resp.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = ctype;
    resp.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)strlen(ctype);
    if (body) {
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = (PVOID)body;
        chunk.FromMemory.BufferLength = (ULONG)strlen(body);
        resp.EntityChunkCount = 1; resp.pEntityChunks = &chunk;
    }
    HttpSendHttpResponse(g_req_queue, reqId, 0, &resp, NULL, &sent, NULL, 0, NULL, NULL);
}

static void send_json(HTTP_REQUEST_ID id, USHORT status, const char *reason, const char *body)
{ send_response(id, status, reason, "application/json", body); }

static void send_err(HTTP_REQUEST_ID id, USHORT status, const char *reason, const char *code, const char *msg)
{
    char b[512];
    sprintf_s(b, sizeof(b), "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}", code, msg);
    send_json(id, status, reason, b);
}

static void send_hr(HTTP_REQUEST_ID id, const char *action, const char *nu, AsbVmId vm_id, HRESULT hr)
{
    char b[512], idText[ASB_VM_ID_TEXT_SIZE];
    asb_vm_id_format(vm_id, idText);
    if (SUCCEEDED(hr)) {
        sprintf_s(b, sizeof(b),
            "{\"ok\":true,\"accepted\":true,\"action\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"hr\":\"0x%08lX\"}",
            action, nu, idText, (unsigned long)hr);
        send_json(id, 202, "Accepted", b);
    } else {
        sprintf_s(b, sizeof(b),
            "{\"ok\":false,\"action\":\"%s\",\"name\":\"%s\",\"id\":\"%s\",\"error\":{\"hr\":\"0x%08lX\"}}",
            action, nu, idText, (unsigned long)hr);
        send_json(id, 500, "Internal Server Error", b);
    }
}

/* Read the request entity body explicitly (reliable) -> wide string for json_get_*. */
static BOOL body_to_wide(PHTTP_REQUEST req, wchar_t *wout, int wcap)
{
    char body[8192]; int pos = 0;
    wout[0] = 0;
    if (asb_api_busy()) {
        if (!g_gui_call->valid_body || wcslen(g_gui_call->body) >= (size_t)wcap) return FALSE;
        wcscpy_s(wout, wcap, g_gui_call->body);
        return TRUE;
    }
    for (;;) {
        ULONG bytes = 0;
        ULONG r = HttpReceiveRequestEntityBody(g_req_queue, req->RequestId, 0,
                    body + pos, (ULONG)(sizeof(body) - pos - 1), &bytes, NULL);
        pos += (int)bytes;
        if (r == ERROR_HANDLE_EOF || r != NO_ERROR) break;
        if (pos >= (int)sizeof(body) - 1) break;
    }
    body[pos] = 0;
    if (memchr(body, 0, pos)) return FALSE;
    /* On failure (e.g. the converted body would exceed wcap) MultiByteToWideChar
       leaves the buffer partially written WITHOUT a NUL -- treat that as an
       empty body rather than parse garbage. */
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body, -1, wout, wcap)) {
        wout[0] = 0;
        return FALSE;
    }
    return TRUE;
}

static int auth_ok(PHTTP_REQUEST req)
{
    const HTTP_KNOWN_HEADER *h = &req->Headers.KnownHeaders[HttpHeaderAuthorization];
    char expect[64];
    sprintf_s(expect, sizeof(expect), "Bearer %s", g_token);
    if (!h->pRawValue || h->RawValueLength == 0) return 0;
    if (h->RawValueLength != (USHORT)strlen(expect)) return 0;
    /* Case-sensitive: the token is lowercase hex; a case-insensitive compare
       needlessly multiplies the accepted token space. */
    return strncmp(h->pRawValue, expect, h->RawValueLength) == 0;
}

/* ---- SSE ---- */

static void broadcast_event(const char *json)
{
    EnterCriticalSection(&g_ev_cs);
    if (g_ev_ring[g_ev_seq % EV_CAP]) free(g_ev_ring[g_ev_seq % EV_CAP]);
    g_ev_ring[g_ev_seq % EV_CAP] = _strdup(json);
    g_ev_seq++;
    WakeAllConditionVariable(&g_ev_cv);
    LeaveCriticalSection(&g_ev_cs);
}

static int sse_write(HTTP_REQUEST_ID id, const char *data, int len)
{
    HTTP_DATA_CHUNK chunk; ULONG sent = 0;
    chunk.DataChunkType = HttpDataChunkFromMemory;
    chunk.FromMemory.pBuffer = (PVOID)data;
    chunk.FromMemory.BufferLength = (ULONG)len;
    return HttpSendResponseEntityBody(g_req_queue, id, HTTP_SEND_RESPONSE_FLAG_MORE_DATA,
                                      1, &chunk, &sent, NULL, 0, NULL, NULL) == NO_ERROR;
}

/* One thread per SSE connection. Streams broadcast events until the client
   disconnects (write fails) or the daemon stops. Does NOT touch the core. */
static DWORD WINAPI sse_thread(LPVOID param)
{
    HTTP_REQUEST_ID id = *(HTTP_REQUEST_ID *)param;
    HTTP_RESPONSE resp; ULONG sent = 0;
    LONG my_seq;
    free(param);

    RtlZeroMemory(&resp, sizeof(resp));
    resp.StatusCode = 200; resp.pReason = "OK"; resp.ReasonLength = 2;
    resp.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = "text/event-stream";
    resp.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = (USHORT)strlen("text/event-stream");
    resp.Headers.KnownHeaders[HttpHeaderCacheControl].pRawValue = "no-cache";
    resp.Headers.KnownHeaders[HttpHeaderCacheControl].RawValueLength = (USHORT)strlen("no-cache");
    if (HttpSendHttpResponse(g_req_queue, id, HTTP_SEND_RESPONSE_FLAG_MORE_DATA,
                             &resp, NULL, &sent, NULL, 0, NULL, NULL) != NO_ERROR)
        goto finished;
    if (!sse_write(id, "retry: 3000\n\n", 13)) goto finished;

    EnterCriticalSection(&g_ev_cs);
    my_seq = g_ev_seq;   /* only stream events from connect onward */
    LeaveCriticalSection(&g_ev_cs);

    while (!g_stop_all) {
        char *frames[64]; int nframes = 0, i, alive = 1;
        EnterCriticalSection(&g_ev_cs);
        while (my_seq == g_ev_seq && !g_stop_all)
            if (!SleepConditionVariableCS(&g_ev_cv, &g_ev_cs, 15000)) break; /* timeout -> heartbeat */
        if (g_ev_seq - my_seq > EV_CAP) my_seq = g_ev_seq - EV_CAP;          /* skip dropped */
        while (my_seq < g_ev_seq && nframes < 64) {
            char *s = g_ev_ring[my_seq % EV_CAP];
            frames[nframes++] = s ? _strdup(s) : NULL;
            my_seq++;
        }
        LeaveCriticalSection(&g_ev_cs);

        if (nframes == 0) {
            alive = sse_write(id, ": ping\n\n", 8);   /* heartbeat */
        } else {
            for (i = 0; i < nframes && alive; i++) {
                if (frames[i]) {
                    char frame[2048];
                    int n = sprintf_s(frame, sizeof(frame), "data: %s\n\n", frames[i]);
                    alive = sse_write(id, frame, n);
                }
            }
        }
        for (i = 0; i < nframes; i++) free(frames[i]);
        if (!alive) break;   /* client disconnected */
    }
finished:
    EnterCriticalSection(&g_ev_cs);
    --g_sse_count;
    WakeAllConditionVariable(&g_ev_cv);
    LeaveCriticalSection(&g_ev_cs);
    return 0;
}

/* Trim leading/trailing ASCII whitespace in place -- mirrors the GUI sending
   name/adminUser as `.value.trim()` (web/app.js), so the API normalizes the same
   inputs the UI does before validating. */
static void trim_ws(wchar_t *s)
{
    int n, i = 0;
    while (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\r' || s[i] == L'\n') i++;
    if (i) { int j = 0; while (s[i]) s[j++] = s[i++]; s[j] = 0; }
    n = (int)wcslen(s);
    while (n > 0 && (s[n-1]==L' '||s[n-1]==L'\t'||s[n-1]==L'\r'||s[n-1]==L'\n')) s[--n] = 0;
}

static const wchar_t *read_gpu_selection(const wchar_t *body, int *mode,
                                         wchar_t *id, size_t id_cap)
{
    if (json_has_key(body, L"gpuId") &&
        !json_get_string(body, L"gpuId", id, id_cap))
        return id[0] ? L"gpuId is too long (maximum 511 characters)."
                     : L"gpuId must be a valid JSON string without NUL characters.";
    if (!json_get_int(body, L"gpuMode", mode) && id[0]) *mode = GPU_DEFAULT;
    return asb_validate_gpu_selection(*mode, id);
}

static const char *validate_create(const wchar_t *name, const wchar_t *os,
                                   const wchar_t *tpl, const wchar_t *img,
                                   BOOL is_template, int ram_mb, int hdd_gb,
                                   int cpu_cores, int gpu_mode, int net_mode)
{
    int i, len; BOOL is_linux, is_win, all_digits, from_template;

    is_linux = (_wcsicmp(os, L"Linux") == 0);
    is_win   = (_wcsicmp(os, L"Windows") == 0);
    from_template = (tpl && tpl[0] != L'\0');

    /* name / hostname */
    if (!name || !name[0]) return "VM name is required.";
    len = (int)wcslen(name);
    if (is_win) { if (len > 15) return "VM name cannot exceed 15 characters (NetBIOS limit)."; }
    else if (len > 63) return "VM name cannot exceed 63 characters (hostname limit).";
    all_digits = TRUE;
    for (i = 0; i < len; i++) {
        wchar_t ch = name[i];
        if (!((ch>=L'a'&&ch<=L'z')||(ch>=L'A'&&ch<=L'Z')||(ch>=L'0'&&ch<=L'9')||ch==L'-'))
            return "VM name can only contain letters, digits, and hyphens.";
        if (!(ch>=L'0'&&ch<=L'9')) all_digits = FALSE;
        if (is_linux && ch>=L'A' && ch<=L'Z') return "Linux hostname must be lowercase.";
    }
    if (all_digits) return "VM name cannot be only digits.";
    if (name[0]==L'-' || name[len-1]==L'-') return "VM name cannot start or end with a hyphen.";
    for (i = 0; i < asb_vm_count(); i++) {
        const wchar_t *vn = asb_vm_name(asb_vm_get(i));
        if (vn && _wcsicmp(vn, name) == 0) return "A VM with this name already exists.";
    }
    for (i = 0; i < asb_template_count(); i++) {
        const wchar_t *tn = asb_template_name(i);
        if (tn && _wcsicmp(tn, name) == 0) return "A template with this name already exists.";
    }

    /* image-or-template required; template constraints */
    if (!from_template && (!img || !img[0])) return "An image (ISO) or template is required.";
    if (is_template && from_template)        return "Cannot create a template from another template.";
    if (is_template && !is_win)              return "Templates are only supported for Windows.";

    /* numeric ranges (HTML: ram>=512 step2, hdd>=1, cpu>=1; enums). 0 = unset
       -> the core fills a default, so only check explicitly-provided values. */
    if (ram_mb != 0) {
        if (ram_mb < 512)    return "RAM must be at least 512 MB.";
        if (ram_mb % 2 != 0) return "RAM must be 2 MB-aligned (an even number of MB).";
    }
    if (hdd_gb != 0 && hdd_gb < 1)       return "Disk size must be at least 1 GB.";
    if (cpu_cores != 0 && cpu_cores < 1) return "CPU cores must be at least 1.";
    if (gpu_mode < 0 || gpu_mode > 1)    return "gpuMode must be 0 (None) or 1 (Default GPU).";
    if (net_mode < 0 || net_mode > 3)    return "networkMode must be 0 (None), 1 (NAT), 2 (External), or 3 (Internal).";

    return NULL;
}

/* ---- Request dispatch. Returns 1 if the daemon should stop. ---- */

static int handle_request(PHTTP_REQUEST req)
{
    const wchar_t *path = req->CookedUrl.pAbsPath ? req->CookedUrl.pAbsPath : L"/";
    HTTP_VERB verb = req->Verb;
    static char buf[ASB_MAX_VMS * 16384];
    int pos, i;

    /* /v1/version is open; everything else requires the token. */
    if (verb == HttpVerbGET && wcscmp(path, L"/v1/version") == 0) {
        sprintf_s(buf, sizeof(buf),
            "{\"product\":\"AppSandbox\",\"version\":\"%s\",\"apiVersion\":\"%s\",\"hostOs\":\"Windows\","
            "\"capabilities\":{\"snapshots\":true,\"templates\":true}}",
            ASB_PRODUCT_VER, ASB_API_VERSION);
        send_json(req->RequestId, 200, "OK", buf);
        return 0;
    }
    if (!auth_ok(req)) {
        send_err(req->RequestId, 401, "Unauthorized", "unauthorized", "missing or bad bearer token");
        return 0;
    }

    /* SSE event stream -- handed to a dedicated thread so it doesn't block the loop. */
    if (verb == HttpVerbGET && wcscmp(path, L"/v1/events") == 0) {
        HTTP_REQUEST_ID *idp = (HTTP_REQUEST_ID *)malloc(sizeof(HTTP_REQUEST_ID));
        if (idp) {
            HANDLE h;
            *idp = req->RequestId;
            EnterCriticalSection(&g_ev_cs);
            ++g_sse_count;
            LeaveCriticalSection(&g_ev_cs);
            h = CreateThread(NULL, 0, sse_thread, idp, 0, NULL);
            if (h) CloseHandle(h);
            else {
                EnterCriticalSection(&g_ev_cs);
                --g_sse_count;
                LeaveCriticalSection(&g_ev_cs);
                free(idp);
                send_err(req->RequestId, 500, "Internal Server Error", "thread", "spawn failed");
            }
        }
        return 0;   /* response owned by the SSE thread */
    }

    if (verb == HttpVerbGET && wcscmp(path, L"/v1/host") == 0) {
        build_host_info(buf, sizeof(buf));
        send_json(req->RequestId, 200, "OK", buf);
        return 0;
    }

    /* ---- Templates ---- */
    if (wcscmp(path, L"/v1/templates") == 0 && verb == HttpVerbGET) {
        int count = asb_template_count();
        pos = sprintf_s(buf, sizeof(buf), "{\"templates\":[");
        for (i = 0; i < count; i++) {
            if (i) pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",");
            pos += sprintf_s(buf + pos, sizeof(buf) - pos, "{\"name\":");
            pos  = append_wstr(buf, sizeof(buf), pos, asb_template_name(i));
            pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",\"osType\":");
            pos  = append_wstr(buf, sizeof(buf), pos, asb_template_os_type(i));
            pos += sprintf_s(buf + pos, sizeof(buf) - pos, "}");
        }
        sprintf_s(buf + pos, sizeof(buf) - pos, "]}");
        send_json(req->RequestId, 200, "OK", buf);
        return 0;
    }
    if (verb == HttpVerbDELETE && wcsncmp(path, L"/v1/templates/", 14) == 0) {
        const wchar_t *name = path + 14;
        HRESULT hr = asb_template_delete(name);
        if (hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
            send_err(req->RequestId, 409, "Conflict", "template_in_use",
                     "This template is still required by an existing VM or an active disk copy.");
            return 0;
        }
        char nu[256] = {0}; WideCharToMultiByte(CP_UTF8,0,name,-1,nu,sizeof(nu),NULL,NULL);
        send_hr(req->RequestId, "deleteTemplate", nu, 0, hr);
        return 0;
    }

    /* ---- /v1/vms (collection) ---- */
    if (wcscmp(path, L"/v1/vms") == 0) {
        if (verb == HttpVerbGET) {
            int count = asb_vm_count(), emitted = 0;
            pos = sprintf_s(buf, sizeof(buf), "{\"vms\":[");
            for (i = 0; i < count; i++) {
                VmInstance *v = asb_vm_instance(asb_vm_get(i));
                if (!v) continue;
                if (emitted++) pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",");
                pos = append_vm_json(buf, sizeof(buf), pos, v);
            }
            sprintf_s(buf + pos, sizeof(buf) - pos, "]}");
            send_json(req->RequestId, 200, "OK", buf);
            return 0;
        }
        if (verb == HttpVerbPOST) {
            /* create */
            wchar_t body[8192];
            AsbVmConfig cfg; int iv; BOOL bv;
            wchar_t name[256]={0}, os[32]={0}, img[MAX_PATH]={0}, tpl[256]={0};
            wchar_t user[128]={0}, pass[256]={0}, adapter[256]={0};
            wchar_t disk_directory[MAX_PATH + 1]={0};
            wchar_t gpu_id[512]={0};
            char nu[256]={0};
            if (!body_to_wide(req, body, 8192)) {
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                         "Request body must contain valid UTF-8 JSON without NUL bytes.");
                return 0;
            }
            ZeroMemory(&cfg, sizeof(cfg));
            json_get_string(body, L"name", name, 256);
            json_get_string(body, L"osType", os, 32);
            json_get_string(body, L"imagePath", img, MAX_PATH);
            json_get_string(body, L"templateName", tpl, 256);
            if (!json_get_string(body, L"adminUser", user, ARRAYSIZE(user))) {
                const char *error = !json_has_key(body, L"adminUser")
                    ? "Username is required."
                    : user[0]
                        ? (_wcsicmp(os, L"Linux") == 0 && !tpl[0]
                            ? "Username cannot exceed 32 characters (Linux limit)."
                            : "Username cannot exceed 20 characters.")
                        : "adminUser must be a valid JSON string without NUL characters.";
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg", error);
                return 0;
            }
            if (!json_get_string(body, L"adminPass", pass, ARRAYSIZE(pass))) {
                const char *error = !json_has_key(body, L"adminPass")
                    ? "Password is required."
                    : pass[0]
                        ? (_wcsicmp(os, L"Linux") == 0 && !tpl[0]
                            ? "Password is too long (max 255 bytes)."
                            : "Password is too long (max 127 characters for Windows).")
                        : "adminPass must be a valid JSON string without NUL characters.";
                SecureZeroMemory(pass, sizeof(pass));
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg", error);
                return 0;
            }
            json_get_string(body, L"netAdapter", adapter, 256);
            if (!json_get_string(body, L"diskDirectory", disk_directory, MAX_PATH + 1) &&
                json_has_key(body, L"diskDirectory")) {
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                         disk_directory[0] ? "Disk storage path is too long." : "diskDirectory must be a string without NUL characters.");
                return 0;
            }
            trim_ws(disk_directory);
            trim_ws(name); trim_ws(user);   /* match the GUI's .value.trim() */
            cfg.name = name; cfg.os_type = os; cfg.image_path = img;
            cfg.template_name = tpl; cfg.username = user; cfg.password = pass;
            cfg.net_adapter = adapter;
            cfg.disk_directory = disk_directory;
            cfg.gpu_id = gpu_id;
            if (json_get_int(body, L"ramMb", &iv)) cfg.ram_mb = (DWORD)iv;
            if (json_get_int(body, L"hddGb", &iv)) cfg.hdd_gb = (DWORD)iv;
            if (json_get_int(body, L"cpuCores", &iv)) cfg.cpu_cores = (DWORD)iv;
            {
                const wchar_t *verr = read_gpu_selection(body, &cfg.gpu_mode,
                                                        gpu_id, ARRAYSIZE(gpu_id));
                if (verr) {
                    char message[512] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, verr, -1, message, sizeof(message), NULL, NULL);
                    SecureZeroMemory(pass, sizeof(pass));
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg", message);
                    return 0;
                }
            }
            if (json_get_int(body, L"networkMode", &iv)) cfg.network_mode = iv;
            if (json_get_bool(body, L"testMode", &bv)) cfg.test_mode = bv;
            if (json_get_bool(body, L"sshEnabled", &bv)) cfg.ssh_enabled = bv;
            if (json_get_bool(body, L"sshDeployKey", &bv)) cfg.ssh_deploy_key = bv;
            if (json_get_bool(body, L"isTemplate", &bv)) cfg.is_template = bv;
            if (cfg.ssh_deploy_key && !cfg.ssh_enabled) {
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                         "sshDeployKey requires sshEnabled");
                return 0;
            }
            {
                const wchar_t *password_os = os;
                const wchar_t *verr;
                for (i = 0; tpl[0] && i < asb_template_count(); i++) {
                    if (_wcsicmp(tpl, asb_template_name(i)) == 0) {
                        password_os = asb_template_os_type(i);
                        break;
                    }
                }
                verr = asb_validate_username(password_os, user, name, cfg.is_template);
                if (!verr) verr = asb_validate_password(password_os, pass);
                if (verr) {
                    char message[512];
                    WideCharToMultiByte(CP_UTF8, 0, verr, -1, message, sizeof(message), NULL, NULL);
                    SecureZeroMemory(pass, sizeof(pass));
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg", message);
                    return 0;
                }
            }
            {
                const char *verr = validate_create(name, os, tpl, img,
                    cfg.is_template, (int)cfg.ram_mb, (int)cfg.hdd_gb, (int)cfg.cpu_cores,
                    cfg.gpu_mode, cfg.network_mode);
                if (verr) {
                    SecureZeroMemory(pass, sizeof(pass));
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg", verr);
                    return 0;
                }
            }
            {
                const wchar_t *verr = asb_validate_disk_directory(name, disk_directory, cfg.is_template);
                if (verr) {
                    char message[512];
                    WideCharToMultiByte(CP_UTF8, 0, verr, -1, message, sizeof(message), NULL, NULL);
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg", message);
                    return 0;
                }
            }
            {
                const wchar_t *verr = asb_validate_template_disk_size(tpl, cfg.hdd_gb);
                if (verr) {
                    char message[512];
                    WideCharToMultiByte(CP_UTF8, 0, verr, -1, message, sizeof(message), NULL, NULL);
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg", message);
                    return 0;
                }
            }
            WideCharToMultiByte(CP_UTF8,0,name,-1,nu,sizeof(nu),NULL,NULL);
            {
                AsbVmId created_id = 0;
                HRESULT hr = asb_vm_create_with_id(&cfg, &created_id);
                SecureZeroMemory(pass, sizeof(pass));
                /* create is async + auto-starts; success/failure also via events/alerts */
                send_hr(req->RequestId, "createVm", nu, created_id, hr);
            }
            return 0;
        }
    }

    if (wcsncmp(path, L"/v1/vms/", 8) == 0 || wcsncmp(path, L"/v1/vm-ids/", 11) == 0) {
        BOOL by_id = wcsncmp(path, L"/v1/vm-ids/", 11) == 0;
        const wchar_t *rest = path + (by_id ? 11 : 8);
        const wchar_t *slash = wcschr(rest, L'/');
        wchar_t name[256];
        const wchar_t *sub = NULL;
        AsbVm vm;
        AsbVmId vm_id;
        char nu[256] = {0};

        if (slash && (size_t)(slash - rest) < 256) {
            wcsncpy_s(name, 256, rest, (size_t)(slash - rest));
            sub = slash + 1;
        } else if (wcslen(rest) < 256) {
            wcscpy_s(name, 256, rest);
        } else {
            send_err(req->RequestId, 400, "Bad Request", "invalid_arg", "name too long");
            return 0;
        }
        vm = by_id ? (AsbVm)asb_find_vm_by_id(asb_vm_id_parse_w(name)) : asb_vm_find(name);
        if (!vm) { send_err(req->RequestId, 404, "Not Found", "not_found", "no such VM"); return 0; }
        vm_id = asb_vm_instance(vm)->unique_id;
        if (verb != HttpVerbGET && asb_vm_instance(vm)->starting) {
            send_err(req->RequestId, 409, "Conflict", "vm_starting", "VM start is in progress; retry once it completes");
            return 0;
        }
        WideCharToMultiByte(CP_UTF8, 0, asb_vm_name(vm), -1, nu, sizeof(nu), NULL, NULL);

        if (sub == NULL) {
            VmInstance *inst = asb_vm_instance(vm);
            if (!inst) { send_err(req->RequestId, 404, "Not Found", "not_found", "no such VM"); return 0; }
            if (verb == HttpVerbGET) {
                append_vm_json(buf, sizeof(buf), 0, inst);
                send_json(req->RequestId, 200, "OK", buf);
                return 0;
            }
            if (verb == HttpVerbPUT) {   /* edit (PUT instead of PATCH: PATCH isn't in the http.sys verb enum) */
                wchar_t body[8192], gpu_id[512] = {0};
                int iv, gpu_mode = GPU_DEFAULT; BOOL has_gpu;
                HRESULT hr = S_OK;
                /* config is locked while building or running -- return a clean 409,
                   not the generic 500 that asb_vm_set_*'s E_ACCESSDENIED would
                   produce. The GUI likewise disables the edit control while a VM is
                   building or running (web/app.js makeIconCell !running && !bld). */
                if (inst->building_vhdx) {
                    send_err(req->RequestId, 409, "Conflict", "vm_building",
                             "cannot edit a VM while it is building/staging; wait for the build to finish");
                    return 0;
                }
                if (inst->running) {
                    send_err(req->RequestId, 409, "Conflict", "vm_running", "cannot edit a running VM; stop it first");
                    return 0;
                }
                if (!body_to_wide(req, body, ARRAYSIZE(body))) {
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                             "Request body must contain valid UTF-8 JSON without NUL bytes.");
                    return 0;
                }
                has_gpu = json_has_key(body, L"gpuMode") || json_has_key(body, L"gpuId");
                if (has_gpu) {
                    const wchar_t *verr = read_gpu_selection(body, &gpu_mode,
                                                            gpu_id, ARRAYSIZE(gpu_id));
                    if (verr) {
                        char message[512] = {0};
                        WideCharToMultiByte(CP_UTF8, 0, verr, -1, message, sizeof(message), NULL, NULL);
                        send_err(req->RequestId, 400, "Bad Request", "invalid_arg", message);
                        return 0;
                    }
                }
                /* name is the guest hostname -- fixed at create, NOT editable. */
                if (json_get_int(body, L"ramMb", &iv)) {
                    if (iv < 512 || iv % 2 != 0) { send_err(req->RequestId, 400, "Bad Request", "invalid_arg", "RAM must be 2 MB-aligned and at least 512 MB"); return 0; }
                    hr = asb_vm_set_ram(vm, (DWORD)iv);
                }
                if (json_get_int(body, L"cpuCores", &iv)) {
                    if (iv < 1) { send_err(req->RequestId, 400, "Bad Request", "invalid_arg", "CPU cores must be at least 1"); return 0; }
                    hr = asb_vm_set_cpu(vm, (DWORD)iv);
                }
                if (has_gpu) {
                    hr = asb_vm_set_gpu_selection(vm, gpu_mode, gpu_id);
                    if (FAILED(hr)) {
                        send_hr(req->RequestId, "editVm", nu, vm_id, hr);
                        return 0;
                    }
                }
                if (json_get_int(body, L"networkMode", &iv)) {
                    if (iv < 0 || iv > 3) { send_err(req->RequestId, 400, "Bad Request", "invalid_arg", "networkMode must be 0 (None), 1 (NAT), 2 (External), or 3 (Internal)"); return 0; }
                    hr = asb_vm_set_network(vm, iv);
                }
                asb_save();
                if (SUCCEEDED(hr)) {
                    append_vm_json(buf, sizeof(buf), 0, inst);
                    send_json(req->RequestId, 200, "OK", buf);
                } else {
                    send_hr(req->RequestId, "editVm", nu, vm_id, hr);
                }
                return 0;
            }
            send_err(req->RequestId, 405, "Method Not Allowed", "method", "unsupported method");
            return 0;
        }

        /* sub-routes */
        if (verb == HttpVerbPOST && wcscmp(sub, L"start") == 0) {
            /* Optional: boot from a chosen snapshot/branch -- mirrors the GUI's
               startVm (snapIndex/branchIndex/branchName). Starting from a
               snapshot with a branchName is how the GUI creates a new branch.
               With no body, boots the current state. */
            wchar_t body[512], bname[128] = {0}; int si = -1, bi = -1;
            body_to_wide(req, body, 512);
            json_get_int(body, L"snapIndex", &si);
            json_get_int(body, L"branchIndex", &bi);
            json_get_string(body, L"branchName", bname, 128);
            send_hr(req->RequestId, "start", nu, vm_id, asb_vm_start(vm, si, bi, bname[0] ? bname : NULL));
            return 0;
        }
        if (verb == HttpVerbPOST && wcscmp(sub, L"shutdown") == 0)
            { send_hr(req->RequestId, "shutdown", nu, vm_id, asb_vm_shutdown(vm)); return 0; }
        if (verb == HttpVerbPOST && wcscmp(sub, L"stop") == 0)
            { send_hr(req->RequestId, "stop", nu, vm_id, asb_vm_stop(vm)); return 0; }
        if (verb == HttpVerbPOST && wcscmp(sub, L"delete") == 0) {
            /* Refuse delete while the VHDX is building: the build worker holds the
               disk open, so removing the folder would orphan it. building_vhdx
               covers the whole build including its staging phase. */
            VmInstance *dv = asb_vm_instance(vm);
            if (dv && dv->building_vhdx) {
                send_err(req->RequestId, 409, "Conflict", "vm_building",
                         "cannot delete a VM while it is building/staging; "
                         "wait for the build to finish, then delete");
                return 0;
            }
            if (dv && g_gui_hwnd) ui_display_close(dv->unique_id);
            if (dv) display_drop(display_find(dv->unique_id));   /* close its display window first */
            send_hr(req->RequestId, "delete", nu, vm_id, asb_vm_delete(vm));
            return 0;
        }

        if (verb == HttpVerbGET && wcscmp(sub, L"sshInfo") == 0) {
            VmInstance *v = asb_vm_instance(vm);
            char user[256] = {0};
            int ssh_rep;
            if (!v) { send_err(req->RequestId, 404, "Not Found", "not_found", "no such VM"); return 0; }
            WideCharToMultiByte(CP_UTF8, 0, v->admin_user[0] ? v->admin_user : L"User", -1, user, sizeof(user), NULL, NULL);
            ssh_rep = (v->ssh_key_deployed && v->ssh_state == 2) ? 4 : v->ssh_state;
            sprintf_s(buf, sizeof(buf),
                "{\"host\":\"127.0.0.1\",\"port\":%lu,\"user\":\"%s\",\"sshState\":%d,\"enabled\":%s,"
                "\"keyDeployed\":%s}",
                (unsigned long)v->ssh_port, user, ssh_rep, v->ssh_enabled ? "true" : "false",
                v->ssh_key_deployed ? "true" : "false");
            send_json(req->RequestId, 200, "OK", buf);
            return 0;
        }

        if (wcscmp(sub, L"display") == 0) {
            VmInstance *v = asb_vm_instance(vm);
            if (!v) { send_err(req->RequestId, 404, "Not Found", "not_found", "no such VM"); return 0; }
            display_reap_stale(v->unique_id);   /* drop a self-closed (X) display first */
            if (verb == HttpVerbPOST) {
                DisplayEntry *e;
                if (!host_can_show_window()) {
                    send_err(req->RequestId, 409, "Conflict", "no_display",
                             "no local interactive desktop is available to show the window "
                             "(the daemon is in a non-interactive/service session)");
                    return 0;
                }
                if (!v->running) {
                    send_err(req->RequestId, 409, "Conflict", "not_running",
                             "VM is not running; start it before opening the display");
                    return 0;
                }
                /* Gate on display readiness -- a passive read of agent-online + the
                   agent's latched idd_status (display driver up). It never probes the
                   frame channel, so the check itself can't steal the single consumer
                   slot or blank the display the way a connect-test would. */
                if (!asb_vm_idd_ready(vm)) {
                    send_err(req->RequestId, 409, "Conflict", "display_not_ready",
                             "the VM's virtual display driver is not up yet; retry shortly");
                    return 0;
                }
                if (g_gui_hwnd) {
                    if (!ui_display_open(v->unique_id))
                        send_err(req->RequestId, 500, "Internal Server Error", "display_failed", "failed to open display");
                    else send_json(req->RequestId, 200, "OK", "{\"ok\":true,\"displayOpen\":true}");
                    return 0;
                }
                e = display_find(v->unique_id);   /* present => still open (a closed one was reaped above) */
                if (e) {
                    vm_display_idd_focus(e->disp);            /* already open -> bring to front */
                } else {
                    int slot; for (slot = 0; slot < ASB_MAX_VMS && g_displays[slot].vm_id; slot++) {}
                    if (slot == ASB_MAX_VMS) {
                        send_err(req->RequestId, 503, "Service Unavailable", "too_many",
                                 "no free display slot");
                        return 0;
                    }
                    e = &g_displays[slot];
                    e->disp = vm_display_idd_create(v, g_hinst, NULL);
                    if (!e->disp) {
                        e->vm_id = 0;
                        send_err(req->RequestId, 500, "Internal Server Error", "display_failed",
                                 "failed to create the display window");
                        return 0;
                    }
                    e->vm_id = v->unique_id;
                }
                send_json(req->RequestId, 200, "OK", "{\"ok\":true,\"displayOpen\":true}");
                return 0;
            }
            if (verb == HttpVerbDELETE) {
                if (g_gui_hwnd) ui_display_close(v->unique_id);
                else display_drop(display_find(v->unique_id));
                send_json(req->RequestId, 200, "OK", "{\"ok\":true,\"displayOpen\":false}");
                return 0;
            }
            if (verb == HttpVerbGET) {
                /* Pollable state -- NO window, and NO frame-channel contact. "ready" is
                   asb_vm_idd_ready: running + agent-online + the agent's latched
                   idd_status flag, so a client can poll this on an interval (it disturbs
                   nothing) and POST to open once ready. */
                char b[160];
                BOOL open = display_is_open(v->unique_id);
                BOOL ready = open || asb_vm_idd_ready(vm);
                sprintf_s(b, sizeof(b), "{\"open\":%s,\"ready\":%s}",
                          open ? "true" : "false", ready ? "true" : "false");
                send_json(req->RequestId, 200, "OK", b);
                return 0;
            }
            send_err(req->RequestId, 405, "Method Not Allowed", "method",
                     "use GET to poll state, POST to open the display, DELETE to close it");
            return 0;
        }

        /* snapshots (+ branches). Tree shape:
             GET    /v1/vms/{n}/snapshots                      list + current pointer
             POST   /v1/vms/{n}/snapshots                      take {name?}
             PUT    /v1/vms/{n}/snapshots/{s}                  rename {name, branchIndex?}
             DELETE /v1/vms/{n}/snapshots/{s}                  delete snapshot
             POST   /v1/vms/{n}/snapshots/{s}/branches         new branch
             DELETE /v1/vms/{n}/snapshots/{s}/branches/{b}     delete branch              */
        if (wcscmp(sub, L"snapshots") == 0) {
            if (verb == HttpVerbGET) {
                int sc = asb_snap_count(vm), s, b, cs = -1, cb = -1, semitted = 0, bemitted;
                asb_snap_get_current(vm, &cs, &cb);
                pos = sprintf_s(buf, sizeof(buf),
                    "{\"current\":{\"snapIndex\":%d,\"branchIndex\":%d},\"snapshots\":[", cs, cb);
                for (s = 0; s < sc; s++) {
                    AsbSnapshotInfo info;
                    if (pos > (int)sizeof(buf) - 512) break;   /* headroom; trees are tiny in practice */
                    if (!asb_snap_get_info(vm, s, &info)) continue;
                    if (semitted++) pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",");
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos, "{\"index\":%d,\"name\":", info.index);
                    pos  = append_wstr(buf, sizeof(buf), pos, info.name);
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",\"branchCount\":%d,\"branches\":[", info.branch_count);
                    bemitted = 0;
                    for (b = 0; b < info.branch_count; b++) {
                        AsbBranchInfo bi;
                        if (pos > (int)sizeof(buf) - 256) break;
                        if (!asb_snap_get_branch_info(vm, s, b, &bi)) continue;
                        if (bemitted++) pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",");
                        pos += sprintf_s(buf + pos, sizeof(buf) - pos, "{\"index\":%d,\"name\":", bi.index);
                        pos  = append_wstr(buf, sizeof(buf), pos, bi.name);
                        pos += sprintf_s(buf + pos, sizeof(buf) - pos, "}");
                    }
                    pos += sprintf_s(buf + pos, sizeof(buf) - pos, "]}");
                }
                /* base branches: forked directly off the base disk (snap_idx -2),
                   which asb_snap_get_info skips. Enumerate them like the GUI
                   (web/app.js base list) via the public base-branch count. */
                pos += sprintf_s(buf + pos, sizeof(buf) - pos, "],\"baseBranches\":[");
                {
                    int bbc = asb_snap_base_branch_count(vm), bb, bbemitted = 0;
                    for (bb = 0; bb < bbc; bb++) {
                        AsbBranchInfo bbi;
                        if (pos > (int)sizeof(buf) - 256) break;
                        if (!asb_snap_get_branch_info(vm, -2, bb, &bbi)) continue;
                        if (bbemitted++) pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",");
                        pos += sprintf_s(buf + pos, sizeof(buf) - pos, "{\"index\":%d,\"name\":", bbi.index);
                        pos  = append_wstr(buf, sizeof(buf), pos, bbi.name);
                        pos += sprintf_s(buf + pos, sizeof(buf) - pos, "}");
                    }
                }
                sprintf_s(buf + pos, sizeof(buf) - pos, "]}");
                send_json(req->RequestId, 200, "OK", buf);
                return 0;
            }
            if (verb == HttpVerbPOST) {
                wchar_t body[1024], sname[128] = {0};
                VmInstance *sv = asb_vm_instance(vm);
                if (sv && sv->running) {   /* clean 409, not the generic 500 from a running snapshot attempt */
                    send_err(req->RequestId, 409, "Conflict", "vm_running", "cannot snapshot a running VM; stop it first");
                    return 0;
                }
                body_to_wide(req, body, 1024);
                json_get_string(body, L"name", sname, 128);
                send_hr(req->RequestId, "snapTake", nu, vm_id, asb_snap_take(vm, sname[0] ? sname : NULL));
                return 0;
            }
        }
        if (wcsncmp(sub, L"snapshots/", 10) == 0 &&
            ((sub[10] >= L'0' && sub[10] <= L'9') ||
             (sub[10] == L'-' && sub[11] >= L'0' && sub[11] <= L'9'))) {   /* incl. base = -2 */
            const wchar_t *srest = sub + 10;       /* "{s}" | "{s}/branches" | "{s}/branches/{b}" */
            int sidx = _wtoi(srest);
            const wchar_t *br = wcsstr(srest, L"/branches");
            if (br) {
                const wchar_t *brest = br + 9;     /* expect "/{b}" -- delete one branch.
                   (No standalone "new branch": the GUI creates branches by starting
                    from a snapshot, exposed via POST .../start with a branchName.) */
                if (verb == HttpVerbDELETE && *brest == L'/') {
                    send_hr(req->RequestId, "snapDeleteBranch", nu, vm_id,
                            asb_snap_delete_branch(vm, sidx, _wtoi(brest + 1)));
                    return 0;
                }
            } else {
                if (verb == HttpVerbDELETE) {
                    if (sidx < 0) {   /* the base disk is not a deletable snapshot (asb_snap_delete
                                         also refuses it); only its branches are, via .../branches/{i} */
                        send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                                 "the base is not a deletable snapshot; delete a base branch via .../snapshots/-2/branches/{i}");
                        return 0;
                    }
                    send_hr(req->RequestId, "snapDelete", nu, vm_id, asb_snap_delete(vm, sidx));
                    return 0;
                }
                if (verb == HttpVerbPUT) {   /* rename; branchIndex optional (-1 = the snapshot itself) */
                    wchar_t body[512], nm[128] = {0}; int bidx = -1;
                    body_to_wide(req, body, 512);
                    json_get_int(body, L"branchIndex", &bidx);
                    if (!json_get_string(body, L"name", nm, 128)) {
                        send_err(req->RequestId, 400, "Bad Request", "invalid_arg", "name required");
                        return 0;
                    }
                    send_hr(req->RequestId, "snapRename", nu, vm_id, asb_snap_rename(vm, sidx, bidx, nm));
                    return 0;
                }
            }
        }

        send_err(req->RequestId, 404, "Not Found", "not_found", "no such route");
        return 0;
    }

    /* POST /v1/shutdown -> clean daemon stop. Mirror the GUI (which only allows
       minimize-to-tray while VMs run): refuse if any VM is running unless the
       caller passes {"force":true}, because daemon exit terminates every VM. */
    if (verb == HttpVerbPOST && wcscmp(path, L"/v1/shutdown") == 0) {
        wchar_t body[512]; BOOL force = FALSE;
        int running = 0, p2, cnt = asb_vm_count();
        char rbuf[2048];
        body_to_wide(req, body, 512);
        json_get_bool(body, L"force", &force);
        p2 = sprintf_s(rbuf, sizeof(rbuf),
            "{\"error\":{\"code\":\"vms_active\",\"message\":\"VMs are running or still "
            "building; stop/finish them first or POST {\\\"force\\\":true} to terminate "
            "them on shutdown.\",\"active\":[");
        for (i = 0; i < cnt; i++) {
            VmInstance *v = asb_vm_instance(asb_vm_get(i));
            /* A building VM has running=false but its worker holds the disk;
               shutting down mid-build would orphan it -- block that too. */
            if (v && (v->running || v->building_vhdx || v->starting)) {
                if (running) p2 += sprintf_s(rbuf + p2, sizeof(rbuf) - p2, ",");
                p2 = append_wstr(rbuf, sizeof(rbuf), p2, v->name);
                running++;
            }
        }
        sprintf_s(rbuf + p2, sizeof(rbuf) - p2, "]}}");
        if (running && !force) {
            send_json(req->RequestId, 409, "Conflict", rbuf);
            return 0;
        }
        send_json(req->RequestId, 200, "OK", "{\"ok\":true,\"stopping\":true}");
        return 1;
    }

    send_err(req->RequestId, 404, "Not Found", "not_found", "no such route");
    return 0;
}

/* ---- Discovery file + token ---- */

static void gen_token(void)
{
    unsigned int r; int i;
    for (i = 0; i + 8 <= (int)sizeof(g_token) - 1; i += 8) {
        rand_s(&r);
        sprintf_s(g_token + i, sizeof(g_token) - i, "%08x", r);
    }
    g_token[40] = 0;
}

static BOOL write_discovery(int port)
{
    wchar_t path[MAX_PATH], sddl[512];
    HANDLE token = NULL, file = INVALID_HANDLE_VALUE;
    TOKEN_USER *user = NULL;
    LPWSTR sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL dacl = NULL;
    BOOL present, defaulted, ok = FALSE;
    DWORD size = 0, written;
    BY_HANDLE_FILE_INFORMATION info;
    char json[512];
    int length;
    programdata_path(path, MAX_PATH, L"host.json");
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto done;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    user = malloc(size);
    if (!user || !GetTokenInformation(token, TokenUser, user, size, &size) ||
        !ConvertSidToStringSidW(user->User.Sid, &sid)) goto done;
    swprintf_s(sddl, ARRAYSIZE(sddl), L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;%s)", sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                             &descriptor, NULL) ||
        !GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) || !present)
        goto done;
    {
        SECURITY_ATTRIBUTES attributes = { sizeof(attributes), descriptor, FALSE };
        file = CreateFileW(path, GENERIC_WRITE | WRITE_DAC | WRITE_OWNER, 0, &attributes,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    }
    if (file == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        info.nNumberOfLinks != 1) goto done;
    if (SetSecurityInfo(file, SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        user->User.Sid, NULL, dacl, NULL) != ERROR_SUCCESS) goto done;
    length = sprintf_s(json, sizeof(json),
        "{\"endpoint\":\"http://127.0.0.1:%d\",\"port\":%d,\"token\":\"%s\","
        "\"pid\":%lu,\"version\":\"%s\",\"apiVersion\":\"%s\"}\n",
        port, port, g_token, GetCurrentProcessId(), ASB_PRODUCT_VER, ASB_API_VERSION);
    if (length < 0 || !SetEndOfFile(file) ||
        !WriteFile(file, json, (DWORD)length, &written, NULL) || written != (DWORD)length ||
        !FlushFileBuffers(file)) goto done;
    ok = TRUE;
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (token) CloseHandle(token);
    if (descriptor) LocalFree(descriptor);
    if (sid) LocalFree(sid);
    free(user);
    hlog(ok ? L"discovery file written: %s" : L"cannot securely write discovery file: %s", path);
    return ok;
}

static void delete_discovery(void)
{
    wchar_t path[MAX_PATH];
    programdata_path(path, MAX_PATH, L"host.json");
    DeleteFileW(path);
}

/* ---- http.sys setup ---- */

static int http_start(int *out_port)
{
    HTTPAPI_VERSION ver = HTTPAPI_VERSION_2;
    HTTP_BINDING_INFO binding; ULONG r; int port;

    r = HttpInitialize(ver, HTTP_INITIALIZE_SERVER, NULL);
    if (r) { hlog(L"HttpInitialize failed (%lu)", r); return 0; }
    r = HttpCreateServerSession(ver, &g_session, 0);
    if (r) { hlog(L"HttpCreateServerSession failed (%lu)", r); return 0; }
    r = HttpCreateUrlGroup(g_session, &g_url_group, 0);
    if (r) { hlog(L"HttpCreateUrlGroup failed (%lu)", r); return 0; }
    r = HttpCreateRequestQueue(ver, NULL, NULL, 0, &g_req_queue);
    if (r) { hlog(L"HttpCreateRequestQueue failed (%lu)", r); return 0; }

    RtlZeroMemory(&binding, sizeof(binding));
    binding.Flags.Present = 1;
    binding.RequestQueueHandle = g_req_queue;
    r = HttpSetUrlGroupProperty(g_url_group, HttpServerBindingProperty, &binding, sizeof(binding));
    if (r) { hlog(L"HttpSetUrlGroupProperty failed (%lu)", r); return 0; }

    for (port = DEFAULT_PORT; port < DEFAULT_PORT + 20; port++) {
        wchar_t url[64];
        swprintf_s(url, 64, L"http://127.0.0.1:%d/", port);
        r = HttpAddUrlToUrlGroup(g_url_group, url, 0, 0);
        if (r == NO_ERROR) { *out_port = port; hlog(L"listening on %s", url); return 1; }
        hlog(L"HttpAddUrlToUrlGroup %s failed (%lu); trying next port", url, r);
    }
    hlog(L"FATAL: no free port found.");
    return 0;
}

static void cli_fail(const wchar_t *msg)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    BOOL attached = FALSE;
    hlog(L"STARTUP ABORT: %s", msg);
    if (!h || h == INVALID_HANDLE_VALUE) {
        attached = AttachConsole(ATTACH_PARENT_PROCESS);
        h = GetStdHandle(STD_ERROR_HANDLE);
    }
    if (h && h != INVALID_HANDLE_VALUE) {
        char u[1024]; DWORD wr; int n;
        n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, u, sizeof(u), NULL, NULL);
        if (n > 1) WriteFile(h, u, (DWORD)(n - 1), &wr, NULL);
        WriteFile(h, "\r\n", 2, &wr, NULL);
    }
    if (attached) FreeConsole();
}

static BOOL is_elevated(void)
{
    HANDLE tok; TOKEN_ELEVATION el; DWORD sz; BOOL ok = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz))
            ok = el.TokenIsElevated;
        CloseHandle(tok);
    }
    return ok;
}

/* Windows 11 (build 22000+) is the minimum: the bundled virtual display (VDD)
   and audio (VAD) drivers are Windows-11-only -- their INFs target 10.0...22000
   and the VDD uses the UMDF reflector that only exists on Win11. RtlGetVersion is
   used because GetVersionEx lies without a manifest. */
static BOOL os_supported(wchar_t *why, int whycap)
{
    typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi; RtlGetVersionFn fn;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    ZeroMemory(&vi, sizeof(vi)); vi.dwOSVersionInfoSize = sizeof(vi);
    fn = nt ? (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion") : NULL;
    if (fn) fn(&vi);
    if (vi.dwMajorVersion < 10 || vi.dwBuildNumber < 22000) {
        swprintf_s(why, whycap, L"%lu.%lu build %lu (App Sandbox requires Windows 11, build 22000 or newer)",
                   vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        return FALSE;
    }
    return TRUE;
}

LRESULT asb_api_dispatch(void *opaque)
{
    ApiCall *call = opaque;
    if (g_stop_all || ui_command_active() || g_gui_call) {
        call->status = g_stop_all ? 503 : 409;
        strcpy_s(call->reason, sizeof(call->reason), g_stop_all ? "Service Unavailable" : "Conflict");
        call->response = _strdup("{\"error\":{\"code\":\"busy\",\"message\":\"Application is busy; retry the request.\"}}");
    } else {
        g_gui_call = call;
        call->stop = handle_request(call->request);
        g_gui_call = NULL;
        ui_refresh_from_core();
    }
    SetEvent(call->done);
    return 0;
}

static DWORD WINAPI api_thread(LPVOID unused)
{
    ULONG capacity = sizeof(HTTP_REQUEST) + 16384;
    PHTTP_REQUEST req = malloc(capacity);
    int stop = 0;
    (void)unused;
    while (req && !stop && !g_stop_all) {
        ULONG bytes = 0;
        HTTP_REQUEST_ID id = HTTP_NULL_ID;
        ULONG rc = HttpReceiveHttpRequest(g_req_queue, id, 0, req, capacity, &bytes, NULL);
        if (rc == ERROR_MORE_DATA) {
            PHTTP_REQUEST grown;
            id = req->RequestId;
            capacity = bytes;
            grown = realloc(req, capacity);
            if (!grown) break;
            req = grown;
            rc = HttpReceiveHttpRequest(g_req_queue, id, 0, req, capacity, &bytes, NULL);
        }
        if (rc != NO_ERROR) break;
        const wchar_t *path = req->CookedUrl.pAbsPath;
        if (!g_gui_hwnd || !auth_ok(req) ||
            (req->Verb == HttpVerbGET && path &&
             (!wcscmp(path, L"/v1/version") || !wcscmp(path, L"/v1/events")))) {
            stop = handle_request(req);
        } else {
            ApiCall call = {0};
            call.request = req;
            call.valid_body = body_to_wide(req, call.body, ARRAYSIZE(call.body));
            call.done = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (!call.done) break;
            if (PostMessageW(g_gui_hwnd, WM_ASB_API_REQUEST, 0, (LPARAM)&call)) {
                WaitForSingleObject(call.done, INFINITE);
                send_json(req->RequestId, call.status ? call.status : 500,
                          call.status ? call.reason : "Internal Server Error",
                          call.response ? call.response : "{}");
                stop = call.stop;
            }
            CloseHandle(call.done);
            free(call.response);
        }
    }
    free(req);
    if (stop && g_gui_hwnd) PostMessageW(g_gui_hwnd, WM_ASB_API_SHUTDOWN, 0, 0);
    return 0;
}

BOOL asb_api_start(HINSTANCE instance, HWND owner)
{
    int port;
    if (g_server_thread) return TRUE;
    if (!g_api_initialized) {
        open_log();
        InitializeCriticalSection(&g_ev_cs);
        InitializeConditionVariable(&g_ev_cv);
        g_api_initialized = TRUE;
    }
    g_stop_all = 0;
    g_hinst = instance;
    g_gui_hwnd = owner;
    g_gui_thread = owner ? GetCurrentThreadId() : 0;
    gen_token();
    if (!http_start(&port)) { asb_api_stop(); return FALSE; }
    g_api_subscription = asb_events_subscribe(api_event, NULL);
    if (!g_api_subscription) { asb_api_stop(); return FALSE; }
    g_server_thread = CreateThread(NULL, 0, api_thread, NULL, 0, NULL);
    if (!g_server_thread) { asb_api_stop(); return FALSE; }
    if (!write_discovery(port)) { asb_api_stop(); return FALSE; }
    return TRUE;
}

void asb_api_stop(void)
{
    if (!g_api_initialized) return;
    InterlockedExchange(&g_stop_all, 1);
    asb_events_unsubscribe(g_api_subscription);
    g_api_subscription = 0;
    EnterCriticalSection(&g_ev_cs);
    WakeAllConditionVariable(&g_ev_cv);
    LeaveCriticalSection(&g_ev_cs);
    delete_discovery();
    if (g_req_queue) HttpShutdownRequestQueue(g_req_queue);
    if (g_server_thread) {
        while (WaitForSingleObject(g_server_thread, 10) == WAIT_TIMEOUT) {
            MSG msg;
            while (PeekMessageW(&msg, g_gui_hwnd, WM_ASB_API_REQUEST, WM_ASB_API_REQUEST, PM_REMOVE))
                DispatchMessageW(&msg);
        }
        CloseHandle(g_server_thread); g_server_thread = NULL;
    }
    EnterCriticalSection(&g_ev_cs);
    while (g_sse_count) SleepConditionVariableCS(&g_ev_cv, &g_ev_cs, INFINITE);
    LeaveCriticalSection(&g_ev_cs);
    if (g_url_group) { HttpCloseUrlGroup(g_url_group); g_url_group = 0; }
    if (g_req_queue) { HttpCloseRequestQueue(g_req_queue); g_req_queue = NULL; }
    if (g_session) { HttpCloseServerSession(g_session); g_session = 0; }
    HttpTerminate(HTTP_INITIALIZE_SERVER, NULL);
    for (int i = 0; i < ASB_MAX_VMS; ++i) display_drop(&g_displays[i]);
}

/* ---- Entry ---- */

int run_headless(HINSTANCE hInst, const wchar_t *cmdline)
{
    HANDLE mtx, bt;

    (void)cmdline;
    open_log();
    InitializeCriticalSection(&g_ev_cs);
    InitializeConditionVariable(&g_ev_cv);
    g_api_initialized = TRUE;
    hlog(L"=== appsandbox --headless starting (pid %lu) ===", GetCurrentProcessId());

    /* ---- Fail fast (with a clear CLI message) if prerequisites aren't met ---- */
    if (!is_elevated()) {
        cli_fail(L"App Sandbox --headless must be run as Administrator -- creating/running VMs "
                 L"via HCS and binding the local API both require elevation.");
        if (g_log) { fclose(g_log); g_log = NULL; }
        return 3;
    }
    {
        wchar_t why[256], m[512];
        if (!os_supported(why, 256)) {
            swprintf_s(m, 512, L"App Sandbox --headless: unsupported OS -- detected %s.", why);
            cli_fail(m);
            if (g_log) { fclose(g_log); g_log = NULL; }
            return 3;
        }
    }
    if (!prereq_check_all()) {
        cli_fail(L"App Sandbox --headless requires the 'VirtualMachinePlatform' Windows feature, "
                 L"which is not enabled. Enable it and reboot, e.g.:\r\n"
                 L"    dism /online /Enable-Feature /FeatureName:VirtualMachinePlatform /All");
        if (g_log) { fclose(g_log); g_log = NULL; }
        return 3;
    }

    mtx = CreateMutexW(NULL, FALSE, L"Global\\AppSandboxCoreHost");
    if (!mtx) { hlog(L"FATAL: CreateMutex failed (%lu).", GetLastError()); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* cli_fail, not just hlog: the loser's open_log failed (the running
           daemon holds headless.log with share-deny), so a log-only message
           would vanish -- tell the launching console/user directly. */
        cli_fail(L"AppSandbox is already running (the app window or another "
                 L"headless daemon). Only one instance can run at a time.");
        return 2;
    }
    hlog(L"single-instance lock acquired.");

    asb_set_hinstance(hInst);
    g_hinst = hInst;

    bt = CreateThread(NULL, 0, bootstrap_thread, NULL, 0, NULL);
    if (!bt) { hlog(L"FATAL: bootstrap thread failed."); return 1; }
    while (!g_init_done) Sleep(25);
    hlog(L"core initialized + reconnected (%d VMs).", asb_vm_count());

    if (!asb_api_start(hInst, NULL)) { asb_cleanup(); return 1; }
    WaitForSingleObject(g_server_thread, INFINITE);
    asb_api_stop();

    asb_cleanup();
    hlog(L"cleaned up (VMs terminated). exit 0.");
    if (g_log) { fclose(g_log); g_log = NULL; }
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return 0;
}
