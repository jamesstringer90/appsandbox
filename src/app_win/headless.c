#define COBJMACROS

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
#include "headless_vm.h"
#include "headless_ssh.h"
#include <wincodec.h>
#include <wincrypt.h>
#include <string.h>
#include "api_json.h"
#include "api_operations.h"
#include "api_contract.h"
#include "mcp_server.h"
#include <errno.h>
#include <limits.h>

#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"crypt32.lib")
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
static LONG g_headless_count;
static LONG g_mcp_count;
static AsbMcp *g_mcp;
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
    if (e && e->disp && !vm_display_idd_is_active(e->disp))
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
    char ev[4096], idText[ASB_TEMPLATE_ID_TEXT_SIZE]; int pos;
    if (event->is_template) asb_template_id_format(event->vm_id,idText);
    else asb_vm_id_format(event->vm_id, idText);
    (void)context;
    if (g_stop_all) return;
    switch (event->type) {
    case CORE_VM_EVENT_LOG:
        hlog(L"%S", event->str_value ? event->str_value : "");
        break;
    case CORE_VM_EVENT_STATE_CHANGED:
    case CORE_VM_EVENT_PROGRESS:
        pos = sprintf_s(ev, sizeof(ev), "{\"event\":\"%s\",\"id\":\"%s\",\"name\":",
            event->is_template?(event->type==CORE_VM_EVENT_PROGRESS?"templateProgress":"templateStateChanged"):
                (event->type == CORE_VM_EVENT_PROGRESS ? "vmProgress" : "vmStateChanged"), idText);
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
    pos  = append_wstr(out, cap, pos, asb_api_guest_os_type(v->os_type));
    pos += sprintf_s(out + pos, cap - pos, ",\"osName\":");
    pos  = append_wstr(out, cap, pos, asb_api_guest_os_name(v->os_type));
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

static int append_template_json(char *out,int cap,int pos,AsbTemplateId id)
{
    int index=asb_template_index_by_id(id);
    VmInstance *builder=index<0?asb_find_vm_by_id(id):NULL;
    if (index<0 && (!builder || !builder->is_template)) return -1;
    char text[ASB_TEMPLATE_ID_TEXT_SIZE];asb_template_id_format(id,text);
    pos+=sprintf_s(out+pos,cap-pos,"{\"id\":\"%s\",\"name\":",text);
    pos=append_wstr(out,cap,pos,index>=0?asb_template_name(index):builder->name);
    pos+=sprintf_s(out+pos,cap-pos,",\"osType\":");
    pos=append_wstr(out,cap,pos,asb_api_guest_os_type(index>=0?asb_template_os_type(index):builder->os_type));
    pos+=sprintf_s(out+pos,cap-pos,",\"osName\":");
    pos=append_wstr(out,cap,pos,asb_api_guest_os_name(index>=0?asb_template_os_type(index):builder->os_type));
    pos+=sprintf_s(out+pos,cap-pos,",\"hddGb\":%lu,\"ready\":%s,\"state\":\"%s\",\"building\":%s,\"progress\":%d}",
        index>=0?asb_template_hdd_gb(index):builder->hdd_gb,index>=0?"true":"false",
        index>=0?"ready":"building",index>=0?"false":"true",index>=0?100:builder->vhdx_progress);
    return pos;
}

static int build_host_info(char *buf, int cap)
{
    MEMORYSTATUSEX ms; ULARGE_INTEGER freeB;
    const GpuList *gpus = asb_gpu_list();
    wchar_t pd[MAX_PATH];
    int i, count, pos, vmCores = 0, vmRamMb = 0, vmHddGb = 0;
    ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    asb_default_disk_directory(pd,MAX_PATH);
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
        (unsigned long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
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
    wchar_t body[65536];
    BOOL valid_body;
    USHORT status;
    char reason[64];
    char *response;
    int stop;
    HANDLE done;
    BOOL deferred;
} ApiCall;
static ApiCall *g_gui_call;
static const wchar_t *g_request_body;

BOOL asb_api_busy(void) { return GetCurrentThreadId() == g_gui_thread && g_gui_call != NULL; }

static const wchar_t *request_body(void)
{
    return asb_api_busy()?g_gui_call->body:g_request_body?g_request_body:L"{}";
}

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
    char *body=asb_api_error_json(code,msg);
    send_json(id,status,reason,body?body:"{\"error\":{\"code\":\"operation_failed\",\"message\":\"Operation failed.\"}}");
    free(body);
}

static void send_hr(HTTP_REQUEST_ID id, const char *action, const char *nu, AsbVmId vm_id, HRESULT hr)
{
    char b[512], idText[ASB_TEMPLATE_ID_TEXT_SIZE];
    if (strstr(action,"Template")) asb_template_id_format(vm_id,idText);
    else asb_vm_id_format(vm_id, idText);
    if (SUCCEEDED(hr)) {
        sprintf_s(b, sizeof(b),
            "{\"ok\":true,\"accepted\":true,\"action\":\"%s\",\"name\":\"%s\",\"id\":\"%s\"}",
            action, nu, idText);
        send_json(id, 202, "Accepted", b);
    } else {
        USHORT status=hr==E_INVALIDARG?400:hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)?404:
            hr==E_ACCESSDENIED || hr==HRESULT_FROM_WIN32(ERROR_BUSY)?409:hr==E_NOTIMPL?501:500;
        const char *code=status==400?"invalid_arg":status==404?"not_found":status==409?"conflict":status==501?"not_supported":"operation_failed";
        const char *message=status==400?"Invalid argument.":status==404?"Resource not found.":status==409?"Operation conflicts with the resource state.":
            status==501?"Operation is not supported by this host.":"Operation failed.";
        send_err(id,status,"Operation Failed",code,message);
    }
}

/* Read the request entity body explicitly (reliable) -> wide string for json_get_*. */
static BOOL body_to_wide(PHTTP_REQUEST req, wchar_t *wout, int wcap)
{
    char body[ASB_API_BODY_MAX+1]; int pos = 0;
    wout[0] = 0;
    if (asb_api_busy()) {
        if (!g_gui_call->valid_body || wcslen(g_gui_call->body) >= (size_t)wcap) return FALSE;
        wcscpy_s(wout, wcap, g_gui_call->body);
        return TRUE;
    }
    if (g_request_body) {
        if (wcslen(g_request_body)>=(size_t)wcap) return FALSE;
        wcscpy_s(wout,wcap,g_request_body);
        return TRUE;
    }
    for (;;) {
        ULONG bytes = 0;
        ULONG r = HttpReceiveRequestEntityBody(g_req_queue, req->RequestId, 0,
                    body + pos, (ULONG)(sizeof(body) - pos - 1), &bytes, NULL);
        pos += (int)bytes;
        if (r==ERROR_HANDLE_EOF) break;
        if (r!=NO_ERROR || !bytes) return FALSE;
        if (pos >= ASB_API_BODY_MAX) {
            char extra;
            bytes=0;
            r=HttpReceiveRequestEntityBody(g_req_queue,req->RequestId,0,&extra,1,&bytes,NULL);
            if (bytes || r!=ERROR_HANDLE_EOF) return FALSE;
            break;
        }
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
    char path[8192];
    if (!req->CookedUrl.pAbsPath) return FALSE;
    int path_length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,req->CookedUrl.pAbsPath,
        req->CookedUrl.AbsPathLength/sizeof(wchar_t),path,sizeof(path)-1,NULL,NULL);
    if (!path_length) return FALSE;
    path[path_length]=0;
    const char *method=req->Verb==HttpVerbGET?"GET":req->Verb==HttpVerbPOST?"POST":
        req->Verb==HttpVerbPUT?"PUT":req->Verb==HttpVerbDELETE?"DELETE":"OTHER";
    return asb_api_validate_request(method,path,wout);
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

static uint64_t mcp_event_cursor(void *context)
{
    (void)context;
    EnterCriticalSection(&g_ev_cs);
    uint64_t cursor=(uint64_t)g_ev_seq;
    LeaveCriticalSection(&g_ev_cs);
    return cursor;
}

static int mcp_read_events(void *context,uint64_t *cursor,unsigned timeout,unsigned limit,char **json)
{
    (void)context;
    size_t capacity=64*8192+512;
    char *output=malloc(capacity);
    if (!output) return 500;
    ULONGLONG deadline=GetTickCount64()+timeout;
    unsigned count=0,dropped=0;
    size_t position=0;
    EnterCriticalSection(&g_ev_cs);
    while (*cursor==(uint64_t)g_ev_seq && !g_stop_all) {
        ULONGLONG now=GetTickCount64();
        if (now>=deadline) break;
        SleepConditionVariableCS(&g_ev_cv,&g_ev_cs,(DWORD)(deadline-now));
    }
    if ((uint64_t)g_ev_seq-*cursor>EV_CAP) {
        dropped=(unsigned)((uint64_t)g_ev_seq-*cursor-EV_CAP);*cursor=(uint64_t)g_ev_seq-EV_CAP;
    }
    position+=(size_t)sprintf_s(output,capacity,"{\"events\":[");
    while (*cursor<(uint64_t)g_ev_seq && count<limit) {
        const char *event=g_ev_ring[(*cursor)++%EV_CAP];
        if (!event) continue;
        size_t length=strlen(event);
        if (position+length+512>=capacity) { --*cursor;break; }
        if (count++) output[position++]=',';
        memcpy(output+position,event,length);position+=length;
    }
    sprintf_s(output+position,capacity-position,
        "],\"subscribed\":true,\"connected\":%s,\"streamEnded\":%s,\"streamError\":null,\"droppedEvents\":%u,\"pendingEvents\":%llu}",
        g_stop_all?"false":"true",g_stop_all?"true":"false",dropped,(unsigned long long)((uint64_t)g_ev_seq-*cursor));
    LeaveCriticalSection(&g_ev_cs);
    *json=output;return 200;
}

typedef struct {
    HTTP_REQUEST_ID request;char method[8],session[64],protocol[32];char *body;
} McpCall;

static int mcp_header(PHTTP_REQUEST request,const char *name,char *out,size_t capacity)
{
    int found=0;out[0]=0;
    for (USHORT i=0;i<request->Headers.UnknownHeaderCount;++i) {
        const HTTP_UNKNOWN_HEADER *header=&request->Headers.pUnknownHeaders[i];
        if (header->NameLength!=strlen(name) || _strnicmp(header->pName,name,header->NameLength)) continue;
        if (found++ || header->RawValueLength>=capacity) return 0;
        memcpy(out,header->pRawValue,header->RawValueLength);out[header->RawValueLength]=0;
    }
    return 1;
}

static void mcp_send(HTTP_REQUEST_ID id,const AsbMcpResponse *result)
{
    HTTP_RESPONSE response={0};HTTP_DATA_CHUNK chunk={0};HTTP_UNKNOWN_HEADER session={0};
    const char *body=result->json?result->json:"";char length[32];ULONG sent;
    response.StatusCode=(USHORT)result->status;response.pReason="MCP";response.ReasonLength=3;
    response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue="application/json";
    response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength=16;
    snprintf(length,sizeof(length),"%zu",strlen(body));
    response.Headers.KnownHeaders[HttpHeaderContentLength].pRawValue=length;
    response.Headers.KnownHeaders[HttpHeaderContentLength].RawValueLength=(USHORT)strlen(length);
    response.Headers.KnownHeaders[HttpHeaderCacheControl].pRawValue="no-store";
    response.Headers.KnownHeaders[HttpHeaderCacheControl].RawValueLength=8;
    if (result->session[0]) {
        session.pName="Mcp-Session-Id";session.NameLength=14;
        session.pRawValue=result->session;session.RawValueLength=(USHORT)strlen(result->session);
        response.Headers.UnknownHeaderCount=1;response.Headers.pUnknownHeaders=&session;
    }
    if (result->status==405) {
        response.Headers.KnownHeaders[HttpHeaderAllow].pRawValue="POST, DELETE";
        response.Headers.KnownHeaders[HttpHeaderAllow].RawValueLength=12;
    }
    if (body[0]) {
        chunk.DataChunkType=HttpDataChunkFromMemory;chunk.FromMemory.pBuffer=(void *)body;
        chunk.FromMemory.BufferLength=(ULONG)strlen(body);response.EntityChunkCount=1;response.pEntityChunks=&chunk;
    }
    HttpSendHttpResponse(g_req_queue,id,0,&response,NULL,&sent,NULL,0,NULL,NULL);
}

static DWORD WINAPI mcp_thread(void *context)
{
    McpCall *call=context;AsbMcpResponse response;
    asb_mcp_request(g_mcp,call->method,call->session,call->protocol,call->body,&response);
    mcp_send(call->request,&response);free(response.json);free(call->body);free(call);
    EnterCriticalSection(&g_ev_cs);--g_mcp_count;WakeAllConditionVariable(&g_ev_cv);LeaveCriticalSection(&g_ev_cs);
    return 0;
}

static void mcp_dispatch(PHTTP_REQUEST request)
{
    if (!auth_ok(request)) { send_err(request->RequestId,401,"Unauthorized","unauthorized","A valid API bearer token is required.");return; }
    char origin[256];
    McpCall *call=calloc(1,sizeof(*call));
    if (!call) { send_err(request->RequestId,503,"Unavailable","busy","MCP worker allocation failed.");return; }
    call->request=request->RequestId;
    if (!mcp_header(request,"Origin",origin,sizeof(origin)) ||
        !mcp_header(request,"Mcp-Session-Id",call->session,sizeof(call->session)) ||
        !mcp_header(request,"Mcp-Protocol-Version",call->protocol,sizeof(call->protocol))) {
        free(call);send_err(request->RequestId,400,"Bad Request","invalid_arg","Invalid MCP headers.");return;
    }
    if (!asb_mcp_origin_allowed(g_mcp,origin)) { free(call);send_err(request->RequestId,403,"Forbidden","invalid_origin","Origin is not allowed.");return; }
    const char *method=request->Verb==HttpVerbPOST?"POST":request->Verb==HttpVerbGET?"GET":request->Verb==HttpVerbDELETE?"DELETE":"OTHER";
    strcpy_s(call->method,sizeof(call->method),method);
    if (request->Verb==HttpVerbPOST) {
        wchar_t *body=calloc(65536,sizeof(*body));
        if (!body || !body_to_wide(request,body,65536)) {
            free(body);free(call);send_err(request->RequestId,400,"Bad Request","invalid_arg","Invalid MCP JSON body.");return;
        }
        int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,body,-1,NULL,0,NULL,NULL);
        call->body=length?malloc((size_t)length):NULL;
        if (call->body) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,body,-1,call->body,length,NULL,NULL);
        free(body);
        if (!call->body) { free(call);send_err(request->RequestId,500,"Error","allocation","MCP body allocation failed.");return; }
    }
    EnterCriticalSection(&g_ev_cs);
    if (g_mcp_count>=64 || g_stop_all) {
        LeaveCriticalSection(&g_ev_cs);free(call->body);free(call);
        send_err(request->RequestId,503,"Unavailable","busy","MCP request limit reached.");return;
    }
    ++g_mcp_count;LeaveCriticalSection(&g_ev_cs);
    HANDLE thread=CreateThread(NULL,0,mcp_thread,call,0,NULL);
    if (thread) CloseHandle(thread);
    else {
        free(call->body);free(call);EnterCriticalSection(&g_ev_cs);--g_mcp_count;LeaveCriticalSection(&g_ev_cs);
        send_err(request->RequestId,503,"Unavailable","thread","MCP worker creation failed.");
    }
}

typedef struct { USHORT status;char *json; } HeadlessResult;

typedef AsbApiInputAction Action;
enum { ACTION_TEXT=101 };

static char *utf8(const wchar_t *text)
{
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,NULL,0,NULL,NULL);
    char *result=size?malloc(size):NULL;
    if (result && !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,result,size,NULL,NULL)) { free(result);return NULL; }
    return result;
}

static void fail(HeadlessResult *result,DWORD error,int input_operation)
{
    AsbApiOperationError response=asb_api_operation_error(error,input_operation);
    result->status=(USHORT)response.status;
    result->json=asb_api_error_json(response.code,response.message);
}

typedef struct { const wchar_t *name;BOOL open,focused; } Presence;
static BOOL CALLBACK find_viewer(HWND hwnd,LPARAM parameter)
{
    Presence *p=(Presence *)parameter;DWORD pid=0;
    GetWindowThreadProcessId(hwnd,&pid);
    if (pid!=GetCurrentProcessId()) return TRUE;
    wchar_t cls[64],title[512];
    GetClassNameW(hwnd,cls,ARRAYSIZE(cls));GetWindowTextW(hwnd,title,ARRAYSIZE(title));
    const wchar_t *text=title;
    if (!wcsncmp(text,L"\U0001F507 ",3)) text+=3;
    size_t length=wcslen(p->name);
    if (!wcscmp(cls,L"AppSandboxIddDisplay") && !wcsncmp(text,p->name,length) &&
        (!wcsncmp(text+length,L" Display ",9) || !wcscmp(text+length,L" - IDD Display"))) {
        p->open=TRUE;p->focused=GetForegroundWindow()==hwnd;
    }
    return TRUE;
}

static char *status_json(const AsbHeadlessTarget *target,const AsbInputStatus *s)
{
    wchar_t json[8192],id[ASB_VM_ID_TEXT_SIZE];JsonBuilder b;
    asb_vm_id_format_w(target->id,id);
    Presence presence={target->name,FALSE,FALSE};EnumWindows(find_viewer,(LPARAM)&presence);
    jb_init(&b,json,ARRAYSIZE(json));jb_object_begin(&b);
    jb_string(&b,L"vmId",id);jb_string(&b,L"vm",target->name);
    jb_string(&b,L"state",s->flags&ASB_STATUS_AGENT_HAS_CONTROL?L"agent":s->flags&ASB_STATUS_USER_PAUSED?L"user":L"available");
    jb_int(&b,L"generation",(int)s->generation);jb_int(&b,L"work_id",(int)s->work_id);
    jb_bool(&b,L"agent_has_control",!!(s->flags&ASB_STATUS_AGENT_HAS_CONTROL));
    jb_bool(&b,L"agent_waiting_for_control",!!(s->flags&ASB_STATUS_AGENT_WAITING));
    jb_bool(&b,L"user_has_control",!!(s->flags&ASB_STATUS_USER_HAS_CONTROL));
    jb_bool(&b,L"agent_finished",!!(s->flags&ASB_STATUS_AGENT_FINISHED));
    jb_bool(&b,L"agent_cancelled",!!(s->flags&ASB_STATUS_AGENT_CANCELLED));
    jb_bool(&b,L"revoked",!!(s->flags&ASB_STATUS_USER_PAUSED));
    jb_bool(&b,L"viewer_open",presence.open);jb_bool(&b,L"viewer_focused",presence.focused);
    if (!(s->flags&ASB_STATUS_AGENT_FINISHED)) {
        jb_string(&b,L"identity",s->identity);jb_string(&b,L"vendor",s->vendor);
        jb_string(&b,L"model",s->model);jb_string(&b,L"purpose",s->purpose);
    }
    jb_append(&b,L",\"cursor\":");
    if (s->x==UINT32_MAX || s->y==UINT32_MAX) jb_append(&b,L"null");
    else {
        wchar_t cursor[160];
        swprintf_s(cursor,ARRAYSIZE(cursor),L"{\"xNormalized\":%u,\"yNormalized\":%u,\"range\":65535}",s->x,s->y);
        jb_append(&b,cursor);
    }
    jb_object_end(&b);
    return utf8(json);
}

static char *base64(const BYTE *data,DWORD bytes)
{
    if (!bytes) return _strdup("");
    DWORD size=0;
    if (!CryptBinaryToStringA(data,bytes,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,NULL,&size)) return NULL;
    char *result=malloc(size);
    if (result && !CryptBinaryToStringA(data,bytes,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,result,&size)) { free(result);return NULL; }
    return result;
}

static DWORD ssh_operation(const AsbHeadlessTarget *target,const wchar_t *operation,const ApiJson *json,HeadlessResult *result)
{
    DWORD error=0;
    char vm[ASB_VM_ID_TEXT_SIZE],id[24];
    asb_vm_id_format(target->id,vm);
    UINT64 session=target->ssh_session;
    if (!wcscmp(operation,L"ssh_open")) {
        wchar_t *command=calloc(16385,sizeof(wchar_t));
        if (!command) return ERROR_NOT_ENOUGH_MEMORY;
        int field=api_json_field(json,0,L"command");
        if (field!=-1 && !api_json_string(json,field,command,16385)) error=ERROR_INVALID_PARAMETER;
        if (!error) error=asb_ssh_open(target,command,&session);
        free(command);
    } else if (!wcscmp(operation,L"ssh_close")) error=asb_ssh_close(target->id,session);
    else if (!wcscmp(operation,L"ssh_read")) {
        int timeout=1000;
        int field=api_json_field(json,0,L"timeoutMs");
        if (field!=-1 && (!api_json_int(json,field,&timeout) || timeout<0 || timeout>10000)) return ERROR_INVALID_PARAMETER;
        AsbSshOutput output;
        error=asb_ssh_read(target->id,session,(DWORD)timeout,&output);
        if (error) return error;
        char *out=base64(output.stdout_bytes,output.stdout_size),*err=base64(output.stderr_bytes,output.stderr_size);
        if (!out || !err) error=ERROR_NOT_ENOUGH_MEMORY;
        else {
            size_t size=strlen(out)+strlen(err)+400;
            result->json=malloc(size);
            if (!result->json) error=ERROR_NOT_ENOUGH_MEMORY;
            else sprintf_s(result->json,size,"{\"vmId\":\"%s\",\"sshSessionId\":\"ssh-%016llx\",\"running\":%s,\"exitCode\":%lu,\"stdinError\":%lu,\"stdoutBase64\":\"%s\",\"stderrBase64\":\"%s\"}",
                vm,session,output.running?"true":"false",output.exit_code,output.stdin_error,out,err);
        }
        free(out);free(err);asb_ssh_output_free(&output);
        return error;
    } else if (!wcscmp(operation,L"ssh_write")) {
        int plain=api_json_field(json,0,L"stdin"),encoded=api_json_field(json,0,L"stdinBase64"),closed=api_json_field(json,0,L"closeStdin");
        BOOL close_stdin=FALSE;
        if ((plain!=-1 && encoded!=-1) || (closed!=-1 && !api_json_bool(json,closed,&close_stdin))) return ERROR_INVALID_PARAMETER;
        wchar_t *value=calloc(65536,sizeof(wchar_t));BYTE *bytes=NULL;DWORD size=0;
        if (!value) return ERROR_NOT_ENOUGH_MEMORY;
        if (plain!=-1 || encoded!=-1) {
            if (!api_json_string(json,plain!=-1?plain:encoded,value,65536)) error=ERROR_INVALID_PARAMETER;
            else if (plain!=-1) {
                char *text=utf8(value);
                if (!text) error=ERROR_INVALID_PARAMETER;
                else { bytes=(BYTE *)text;size=(DWORD)strlen(text); }
            } else if (value[0] && (!CryptStringToBinaryW(value,0,CRYPT_STRING_BASE64|CRYPT_STRING_STRICT,NULL,&size,NULL,NULL) || size>65536)) error=ERROR_INVALID_PARAMETER;
            else {
                bytes=malloc(size?size:1);
                if (!bytes) error=ERROR_NOT_ENOUGH_MEMORY;
                else if (size && !CryptStringToBinaryW(value,0,CRYPT_STRING_BASE64|CRYPT_STRING_STRICT,bytes,&size,NULL,NULL)) error=ERROR_INVALID_PARAMETER;
            }
        }
        if (!error) error=asb_ssh_write(target->id,session,bytes,size,close_stdin);
        free(value);free(bytes);
        if (error) return error;
        result->json=malloc(256);
        if (!result->json) return ERROR_NOT_ENOUGH_MEMORY;
        sprintf_s(result->json,256,"{\"vmId\":\"%s\",\"sshSessionId\":\"ssh-%016llx\",\"queuedBytes\":%lu,\"stdinClosing\":%s}",vm,session,size,close_stdin?"true":"false");
        return 0;
    } else return ERROR_INVALID_PARAMETER;
    if (error) return error;
    sprintf_s(id,sizeof(id),"ssh-%016llx",session);
    result->json=malloc(256);
    if (!result->json) return ERROR_NOT_ENOUGH_MEMORY;
    sprintf_s(result->json,256,"{\"vmId\":\"%s\",\"sshSessionId\":\"%s\",\"closed\":%s}",vm,id,!wcscmp(operation,L"ssh_close")?"true":"false");
    return 0;
}

static int scalar_count(const wchar_t *text)
{
    int count=0;
    for (const wchar_t *p=text;*p;++p,++count) {
        if (*p>=0xd800 && *p<=0xdbff) {
            ++p;if (*p<0xdc00 || *p>0xdfff) return -1;
        } else if (*p>=0xdc00 && *p<=0xdfff) return -1;
    }
    return count;
}

static BOOL label(const ApiJson *json,const wchar_t *key,wchar_t *out,size_t capacity,int limit)
{
    if (!api_json_string(json,api_json_field(json,0,key),out,capacity)) return FALSE;
    size_t start=0,length=wcslen(out);
    while (start<length && wcschr(L" \r\n\t",out[start])) ++start;
    while (length>start && wcschr(L" \r\n\t",out[length-1])) --length;
    memmove(out,out+start,(length-start)*sizeof(*out));out[length-start]=0;
    int count=scalar_count(out);
    return count>0 && count<=limit;
}

static BOOL integer(const ApiJson *json,int object,const wchar_t *key,int *value,int fallback,BOOL required)
{
    int token=api_json_field(json,object,key);
    if (token==-1 && !required) { *value=fallback;return TRUE; }
    return api_json_int(json,token,value);
}

static DWORD parse_actions(const ApiJson *json,Action actions[128],int *count)
{
    DWORD error=asb_api_parse_input(json,actions,count);
    if (error) return error;
    for (int i=0;i<*count;++i) if (actions[i].type==ASB_API_TEXT)
        for (const wchar_t *p=actions[i].text;*p;++p)
            if (*p!='\r' && *p!='\n' && *p!='\t' && VkKeyScanW(*p)==-1) return ERROR_NOT_SUPPORTED;
    return 0;
}

static DWORD send_packet(AsbInputSession *session,UINT32 lease,UINT32 type,UINT32 a,UINT32 b,UINT32 c)
{
    InputPacket packet={INPUT_MAGIC,type,a,b,c};
    return asb_input_send(session,TRUE,lease,&packet);
}

static DWORD text_key(AsbInputSession *session,UINT32 lease,wchar_t character)
{
    SHORT mapping=character=='\n' || character=='\r'?VK_RETURN:character=='\t'?VK_TAB:VkKeyScanW(character);
    if (mapping==-1) return ERROR_NOT_SUPPORTED;
    UINT scan=MapVirtualKeyW((UINT)mapping&0xff,MAPVK_VK_TO_VSC_EX);
    if (!scan) return ERROR_NOT_SUPPORTED;
    const UINT modifiers[]={0x2a,0x1d,0x38};
    DWORD error=0;
    for (unsigned i=0;i<ARRAYSIZE(modifiers) && !error;++i)
        if ((mapping>>8)&(1<<i)) error=send_packet(session,lease,INPUT_KEY_PHYSICAL,0,modifiers[i],0);
    UINT flags=(scan&0xff00)==0xe000?INPUT_KEY_EXTENDED:0;
    if (!error) error=send_packet(session,lease,INPUT_KEY_PHYSICAL,0,scan&0xff,flags);
    if (!error) error=send_packet(session,lease,INPUT_KEY_PHYSICAL,0,scan&0xff,flags|INPUT_KEY_UP);
    for (int i=ARRAYSIZE(modifiers)-1;i>=0 && !error;--i)
        if ((mapping>>8)&(1<<i)) error=send_packet(session,lease,INPUT_KEY_PHYSICAL,0,modifiers[i],INPUT_KEY_UP);
    return error;
}

typedef struct { AsbInputSession *session;UINT32 lease; } InputExecution;

static uint32_t input_execute_send(void *context,const InputPacket *packet)
{
    InputExecution *e=context;return asb_input_send(e->session,TRUE,e->lease,packet);
}

static uint32_t input_execute_text(void *context,uint32_t scalar)
{
    InputExecution *e=context;return text_key(e->session,e->lease,(wchar_t)scalar);
}

static uint32_t input_execute_barrier(void *context)
{
    InputExecution *e=context;return asb_input_barrier(e->session,e->lease,10000);
}

static void input_execute_sleep(unsigned milliseconds) { Sleep(milliseconds); }

static DWORD perform(AsbInputSession *session,const Action *actions,int count,UINT32 lease)
{
    DWORD error=asb_input_begin_batch(session);
    if (error) return error;
    InputExecution context={session,lease};
    AsbApiInputOps ops={input_execute_send,input_execute_text,input_execute_barrier,input_execute_sleep};
    error=asb_api_perform_input(actions,count,&context,&ops);
    asb_input_end_batch(session);return error;
}

static DWORD png(const AsbCapture *capture,char **encoded)
{
    IWICImagingFactory *factory=NULL;IWICBitmapEncoder *encoder=NULL;
    IWICBitmapFrameEncode *frame=NULL;IStream *stream=NULL;
    HRESULT initialized=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    HRESULT hr=CoCreateInstance(&CLSID_WICImagingFactory,NULL,CLSCTX_INPROC_SERVER,&IID_IWICImagingFactory,(void **)&factory);
    if (SUCCEEDED(hr)) hr=CreateStreamOnHGlobal(NULL,TRUE,&stream);
    if (SUCCEEDED(hr)) hr=IWICImagingFactory_CreateEncoder(factory,&GUID_ContainerFormatPng,NULL,&encoder);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_Initialize(encoder,stream,WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_CreateNewFrame(encoder,&frame,NULL);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Initialize(frame,NULL);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetSize(frame,capture->header.width,capture->header.height);
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_SetPixelFormat(frame,&format);
    if (SUCCEEDED(hr) && !IsEqualGUID(&format,&GUID_WICPixelFormat32bppBGRA)) hr=E_FAIL;
    for (UINT32 i=3;i<capture->header.data_size;i+=4) capture->pixels[i]=255;
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_WritePixels(frame,capture->header.height,capture->header.stride,capture->header.data_size,capture->pixels);
    if (SUCCEEDED(hr)) hr=IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr=IWICBitmapEncoder_Commit(encoder);
    if (SUCCEEDED(hr)) {
        STATSTG stat={0};HGLOBAL memory=NULL;
        hr=IStream_Stat(stream,&stat,STATFLAG_NONAME);
        if (SUCCEEDED(hr)) hr=GetHGlobalFromStream(stream,&memory);
        if (SUCCEEDED(hr) && stat.cbSize.HighPart) hr=E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            BYTE *bytes=GlobalLock(memory);DWORD size=0;
            if (!bytes || !CryptBinaryToStringA(bytes,stat.cbSize.LowPart,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,NULL,&size)) hr=E_FAIL;
            else {
                *encoded=malloc(size);
                if (!*encoded) hr=E_OUTOFMEMORY;
                else if (!CryptBinaryToStringA(bytes,stat.cbSize.LowPart,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,*encoded,&size)) hr=E_FAIL;
            }
            if (bytes) GlobalUnlock(memory);
        }
    }
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IStream_Release(stream);
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return SUCCEEDED(hr)?0:(DWORD)hr;
}

static void headless_execute(const AsbHeadlessTarget *target,const wchar_t *operation,const wchar_t *body,HeadlessResult *result)
{
    *result=(HeadlessResult){200,NULL};
    ApiJson json;ApiJsonToken *tokens=calloc(4096,sizeof(*tokens));
    Action actions[128]={0};int count=0,timeout=5000;
    AsbInputStatus context={0},status={0};
    DWORD error=0;AsbInputSession *session=NULL;
    if (!tokens) { fail(result,ERROR_NOT_ENOUGH_MEMORY,0);return; }
    if (!api_json_parse(&json,body,tokens,4096) || tokens[0].type!=API_JSON_OBJECT) { error=ERROR_INVALID_PARAMETER;goto done; }
    if (!wcsncmp(operation,L"ssh_",4)) { error=ssh_operation(target,operation,&json,result);goto done; }
    if (!wcscmp(operation,L"screenshot")) {
        if (!integer(&json,0,L"timeoutMs",&timeout,5000,FALSE) || timeout<1 || timeout>10000) { error=ERROR_INVALID_PARAMETER;goto done; }
        AsbCapture capture;
        ULONGLONG start=GetTickCount64();
        error=asb_capture(target,(DWORD)timeout,&capture);
        if (!error) {
            char *image=NULL;error=png(&capture,&image);
            if (!error) {
                char id[ASB_VM_ID_TEXT_SIZE],meta[1024],name[1024],os[128],os_name[128];
                asb_vm_id_format(target->id,id);
                WideCharToMultiByte(CP_UTF8,0,target->name,-1,name,sizeof(name),NULL,NULL);
                WideCharToMultiByte(CP_UTF8,0,asb_api_guest_os_type(target->os_type),-1,os,sizeof(os),NULL,NULL);
                WideCharToMultiByte(CP_UTF8,0,asb_api_guest_os_name(target->os_type),-1,os_name,sizeof(os_name),NULL,NULL);
                sprintf_s(meta,sizeof(meta),"{\"vmId\":\"%s\",\"vm\":\"%s\",\"osType\":\"%s\",\"osName\":\"%s\",\"width\":%u,\"height\":%u,\"stride\":%u,\"frameSeq\":%llu,\"gpuSharingActive\":%s,\"elapsedMs\":%llu}",
                    id,name,os,os_name,capture.header.width,capture.header.height,capture.header.stride,capture.header.frame_seq,capture.header.flags&ASB_SCREENSHOT_GPU_SHARED?"true":"false",GetTickCount64()-start);
                size_t size=strlen(image)+strlen(meta)+100;
                result->json=malloc(size);
                if (!result->json) error=ERROR_NOT_ENOUGH_MEMORY;
                else sprintf_s(result->json,size,"{\"metadata\":%s,\"image\":{\"mimeType\":\"image/png\",\"data\":\"%s\"}}",meta,image);
            }
            free(image);asb_capture_free(&capture);
        }
        goto done;
    }
    if (!wcscmp(operation,L"input")) {
        error=parse_actions(&json,actions,&count);
        if (error) goto done;
        for (int i=0;i<count;++i)
            if (actions[i].type==INPUT_UNICODE &&
                _wcsicmp(target->os_type,L"Windows") && _wcsicmp(target->os_type,L"macOS")) {
                error=ERROR_CALL_NOT_IMPLEMENTED;goto done;
            }
    }
    if (!wcscmp(operation,L"request_control") &&
        (!label(&json,L"identity",context.identity,ARRAYSIZE(context.identity),80) ||
         !label(&json,L"vendor",context.vendor,ARRAYSIZE(context.vendor),80) ||
         !label(&json,L"model",context.model,ARRAYSIZE(context.model),80) ||
         !label(&json,L"purpose",context.purpose,ARRAYSIZE(context.purpose),300))) { error=ERROR_INVALID_PARAMETER;goto done; }
    session=asb_input_session(target);
    if (!session) { error=ERROR_NOT_ENOUGH_MEMORY;goto done; }
    if (!wcscmp(operation,L"request_control")) error=asb_input_claim(session,&context);
    else if (!wcscmp(operation,L"finish")) error=asb_input_control(session,ASB_INPUT_AGENT_END,0);
    else if (!wcscmp(operation,L"input")) {
        asb_input_status(session,&status);
        error=status.flags&ASB_STATUS_AGENT_CANCELLED?ERROR_CANCELLED:
            !(status.flags&ASB_STATUS_AGENT_HAS_CONTROL)?ERROR_ACCESS_DENIED:perform(session,actions,count,status.generation);
    } else if (!wcscmp(operation,L"wait_control")) {
        if (!integer(&json,0,L"timeoutMs",&timeout,30000,FALSE) || timeout<1 || timeout>60000) { error=ERROR_INVALID_PARAMETER;goto done; }
        ULONGLONG until=GetTickCount64()+timeout;
        do {
            asb_input_status(session,&status);
            if (status.flags&(ASB_STATUS_AGENT_HAS_CONTROL|ASB_STATUS_AGENT_FINISHED)) break;
            Sleep(50);
        } while (GetTickCount64()<until);
    } else if (wcscmp(operation,L"input_status")) error=ERROR_INVALID_PARAMETER;
    if (!error) {
        asb_input_status(session,&status);
        char *state=status_json(target,&status);
        if (!state) error=ERROR_NOT_ENOUGH_MEMORY;
        else if (count) {
            size_t size=strlen(state)+128;result->json=malloc(size);
            if (!result->json) error=ERROR_NOT_ENOUGH_MEMORY;
            else sprintf_s(result->json,size,"{\"handledActions\":%d,\"acknowledgement\":\"injector_processed\",\"control\":%s}",count,state);
            free(state);
        } else result->json=state;
    }
done:
    if (session) asb_input_release(session);
    for (int i=0;i<count;++i) free(actions[i].text);
    free(tokens);
    if (error) { free(result->json);result->json=NULL;fail(result,error,wcscmp(operation,L"screenshot") && wcsncmp(operation,L"ssh_",4)); }
}

typedef struct {
    HTTP_REQUEST_ID request;
    AsbHeadlessTarget target;
    const wchar_t *operation;
    wchar_t body[65536];
} AutomationCall;

static DWORD WINAPI headless_http_thread(void *context)
{
    AutomationCall *call=context;
    HeadlessResult result;
    headless_execute(&call->target,call->operation,call->body,&result);
    send_json(call->request,result.status,"Automation Result",result.json?result.json:"{}");
    free(result.json);free(call);
    EnterCriticalSection(&g_ev_cs);
    --g_headless_count;
    WakeAllConditionVariable(&g_ev_cv);
    LeaveCriticalSection(&g_ev_cs);
    return 0;
}

static void defer_headless(PHTTP_REQUEST request,VmInstance *vm,const wchar_t *operation,UINT64 session_id)
{
    if ((!vm->running || vm->shutdown_requested) && wcscmp(operation,L"input_status")) {
        send_err(request->RequestId,409,"Conflict","not_running","VM is not running.");return;
    }
    AutomationCall *call=calloc(1,sizeof(*call));
    if (!call) { send_err(request->RequestId,500,"Internal Server Error","memory","Allocation failed.");return; }
    if (!body_to_wide(request,call->body,ARRAYSIZE(call->body))) {
        free(call);send_err(request->RequestId,400,"Bad Request","invalid_arg","Invalid or oversized request body.");return;
    }
    if (!call->body[0]) wcscpy_s(call->body,ARRAYSIZE(call->body),L"{}");
    call->target.id=vm->unique_id;call->target.runtime=vm->runtime_id;
    wcscpy_s(call->target.name,ARRAYSIZE(call->target.name),vm->name);
    wcscpy_s(call->target.os_type,ARRAYSIZE(call->target.os_type),vm->os_type);
    wcscpy_s(call->target.user,ARRAYSIZE(call->target.user),vm->admin_user);
    call->target.ssh_port=vm->ssh_port;
    call->target.ssh_session=session_id;
    call->request=request->RequestId;call->operation=operation;
    EnterCriticalSection(&g_ev_cs);
    if (g_headless_count>=8) {
        LeaveCriticalSection(&g_ev_cs);free(call);
        send_err(request->RequestId,503,"Service Unavailable","busy","Too many active headless requests.");return;
    }
    ++g_headless_count;
    LeaveCriticalSection(&g_ev_cs);
    HANDLE thread=CreateThread(NULL,0,headless_http_thread,call,CREATE_SUSPENDED,NULL);
    if (!thread) {
        free(call);
        EnterCriticalSection(&g_ev_cs);--g_headless_count;LeaveCriticalSection(&g_ev_cs);
        send_err(request->RequestId,500,"Internal Server Error","thread","Worker creation failed.");return;
    }
    if (asb_api_busy()) g_gui_call->deferred=TRUE;
    ResumeThread(thread);CloseHandle(thread);
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
    if (api_json_has_key(body, L"gpuId") &&
        !api_json_get_string(body, L"gpuId", id, id_cap))
        return id[0] ? L"gpuId is too long (maximum 511 characters)."
                     : L"gpuId must be a valid JSON string without NUL characters.";
    if (!api_json_get_int(body, L"gpuMode", mode) && id[0]) *mode = GPU_DEFAULT;
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
    if (!is_linux && !is_win) return "Only Windows 11 and Ubuntu 26.04 LTS guests are supported on a Windows host.";
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

    const char *resource_error=ram_mb?asb_api_validate_ram_mb(ram_mb):NULL;
    if (!resource_error && hdd_gb) resource_error=asb_api_validate_hdd_gb(hdd_gb,is_linux);
    if (resource_error) return resource_error;
    if (cpu_cores != 0 && cpu_cores < 1) return "CPU cores must be at least 1.";
    if (gpu_mode < 0 || gpu_mode > 1)    return "gpuMode must be 0 (None) or 1 (Default GPU).";
    if (net_mode < 0 || net_mode > 3)    return "networkMode must be 0 (None), 1 (NAT), 2 (External), or 3 (Internal).";

    return NULL;
}

static BOOL validate_body(const wchar_t *body)
{
    return asb_api_validate_body(body);
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
            "\"capabilities\":%s}",
            ASB_PRODUCT_VER, ASB_API_VERSION,asb_api_capabilities_json(1));
        send_json(req->RequestId, 200, "OK", buf);
        return 0;
    }
    if (!auth_ok(req)) {
        send_err(req->RequestId, 401, "Unauthorized", "unauthorized", "missing or bad bearer token");
        return 0;
    }

    if (verb==HttpVerbGET && !wcscmp(path,L"/v1/operations")) {
        send_json(req->RequestId,200,"OK",asb_api_operations_json());return 0;
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
            pos=append_template_json(buf,sizeof(buf),pos,asb_template_id(i));
        }
        sprintf_s(buf + pos, sizeof(buf) - pos, "]}");
        send_json(req->RequestId, 200, "OK", buf);
        return 0;
    }
    if (!wcsncmp(path,L"/v1/templates/",14) || !wcsncmp(path,L"/v1/template-ids/",17)) {
        BOOL by_id=!wcsncmp(path,L"/v1/template-ids/",17);
        AsbTemplateId template_id=by_id?asb_template_id_parse_w(path+17):asb_template_id_by_name(path+14);
        int length=append_template_json(buf,sizeof(buf),0,template_id);
        if (length<0) { send_err(req->RequestId,404,"Not Found","not_found","no such template");return 0; }
        if (verb==HttpVerbGET) { send_json(req->RequestId,200,"OK",buf);return 0; }
        if (verb!=HttpVerbDELETE) { send_err(req->RequestId,405,"Method Not Allowed","method","use GET or DELETE");return 0; }
        wchar_t name[256];
        int index=asb_template_index_by_id(template_id);
        wcscpy_s(name,ARRAYSIZE(name),index>=0?asb_template_name(index):asb_find_vm_by_id(template_id)->name);
        HRESULT hr=asb_template_delete_by_id(template_id);
        if (hr == HRESULT_FROM_WIN32(ERROR_BUSY)) {
            send_err(req->RequestId, 409, "Conflict", "template_in_use",
                     "This template is still required by an existing VM or an active disk copy.");
            return 0;
        }
        char nu[256] = {0}; WideCharToMultiByte(CP_UTF8,0,name,-1,nu,sizeof(nu),NULL,NULL);
        send_hr(req->RequestId, "deleteTemplate", nu, template_id, hr);
        return 0;
    }

    /* ---- /v1/vms (collection) ---- */
    if (wcscmp(path, L"/v1/vms") == 0 ||
        (wcscmp(path,L"/v1/templates")==0 && verb==HttpVerbPOST)) {
        if (verb == HttpVerbGET) {
            int count = asb_vm_count(), emitted = 0;
            pos = sprintf_s(buf, sizeof(buf), "{\"vms\":[");
            for (i = 0; i < count; i++) {
                VmInstance *v = asb_vm_instance(asb_vm_get(i));
                if (!v || v->is_template) continue;
                if (emitted++) pos += sprintf_s(buf + pos, sizeof(buf) - pos, ",");
                pos = append_vm_json(buf, sizeof(buf), pos, v);
            }
            sprintf_s(buf + pos, sizeof(buf) - pos, "]}");
            send_json(req->RequestId, 200, "OK", buf);
            return 0;
        }
        if (verb == HttpVerbPOST) {
            /* create */
            const wchar_t *body=request_body();
            AsbVmConfig cfg; int iv; BOOL bv;
            wchar_t name[256]={0}, os[32]={0}, img[MAX_PATH]={0}, tpl[256]={0};
            wchar_t user[128]={0}, pass[256]={0}, adapter[256]={0};
            wchar_t disk_directory[MAX_PATH + 1]={0};
            wchar_t gpu_id[512]={0};
            char nu[256]={0};
            if (!validate_body(body)) {
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                         "Request body must contain valid UTF-8 JSON without NUL bytes.");
                return 0;
            }
            ZeroMemory(&cfg, sizeof(cfg));
            cfg.network_mode=ASB_API_DEFAULT_NETWORK_MODE;
            api_json_get_string(body, L"name", name, 256);
            api_json_get_string(body, L"osType", os, 32);
            const AsbApiGuestOs *guest=asb_api_guest_os(os);
            if (guest) wcscpy_s(os,ARRAYSIZE(os),guest->backend);
            api_json_get_string(body, L"imagePath", img, MAX_PATH);
            api_json_get_string(body, L"templateName", tpl, 256);
            wchar_t template_id_text[ASB_TEMPLATE_ID_TEXT_SIZE]={0};
            AsbTemplateId template_id=0;
            if (api_json_has_key(body,L"templateId")) {
                if (tpl[0] || !api_json_get_string(body,L"templateId",template_id_text,ARRAYSIZE(template_id_text)) ||
                    !(template_id=asb_template_id_parse_w(template_id_text))) {
                    send_err(req->RequestId,400,"Bad Request","invalid_arg","Specify templateName or a valid templateId.");return 0;
                }
                int index=asb_template_index_by_id(template_id);
                if (index<0) { send_err(req->RequestId,404,"Not Found","not_found","no such template");return 0; }
                wcscpy_s(tpl,ARRAYSIZE(tpl),asb_template_name(index));
            }
            if (!api_json_get_string(body, L"adminUser", user, ARRAYSIZE(user))) {
                const char *error = !api_json_has_key(body, L"adminUser")
                    ? "Username is required."
                    : user[0]
                        ? (_wcsicmp(os, L"Linux") == 0 && !tpl[0]
                            ? "Username cannot exceed 32 characters (Linux limit)."
                            : "Username cannot exceed 20 characters.")
                        : "adminUser must be a valid JSON string without NUL characters.";
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg", error);
                return 0;
            }
            if (!api_json_get_string(body, L"adminPass", pass, ARRAYSIZE(pass))) {
                const char *error = !api_json_has_key(body, L"adminPass")
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
            api_json_get_string(body, L"netAdapter", adapter, 256);
            if (!api_json_get_string(body, L"diskDirectory", disk_directory, MAX_PATH + 1) &&
                api_json_has_key(body, L"diskDirectory")) {
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                         disk_directory[0] ? "Disk storage path is too long." : "diskDirectory must be a string without NUL characters.");
                return 0;
            }
            trim_ws(disk_directory);
            trim_ws(name); trim_ws(user);   /* match the GUI's .value.trim() */
            cfg.name = name; cfg.os_type = os; cfg.image_path = img;
            cfg.template_name=template_id?NULL:tpl;cfg.template_id=template_id;cfg.username = user; cfg.password = pass;
            cfg.net_adapter = adapter;
            cfg.disk_directory = disk_directory;
            cfg.gpu_id = gpu_id;
            if (api_json_get_int(body, L"ramMb", &iv)) cfg.ram_mb = (DWORD)iv;
            if (api_json_get_int(body, L"hddGb", &iv)) cfg.hdd_gb = (DWORD)iv;
            if (api_json_get_int(body, L"cpuCores", &iv)) cfg.cpu_cores = (DWORD)iv;
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
            if (api_json_get_int(body, L"networkMode", &iv)) cfg.network_mode = iv;
            if (api_json_get_bool(body, L"testMode", &bv)) cfg.test_mode = bv;
            if (api_json_get_bool(body, L"sshEnabled", &bv)) cfg.ssh_enabled = bv;
            if (api_json_get_bool(body, L"sshDeployKey", &bv)) cfg.ssh_deploy_key = bv;
            if (api_json_get_bool(body, L"isTemplate", &bv)) cfg.is_template = bv;
            if (!wcscmp(path,L"/v1/templates")) cfg.is_template=TRUE;
            if (asb_api_guest_support(1,os,cfg.is_template)==ERROR_CALL_NOT_IMPLEMENTED) {
                SecureZeroMemory(pass,sizeof(pass));
                send_err(req->RequestId,501,"Not Implemented","not_supported","Operation is not supported by this host.");return 0;
            }
            if (cfg.ssh_deploy_key && !cfg.ssh_enabled) {
                send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                         "sshDeployKey requires sshEnabled");
                return 0;
            }
            {
                const wchar_t *password_os = template_id?asb_template_os_type(asb_template_index_by_id(template_id)):os;
                const wchar_t *verr;
                for (i = 0; !template_id && tpl[0] && i < asb_template_count(); i++) {
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
                const wchar_t *verr = template_id?NULL:asb_validate_template_disk_size(tpl, cfg.hdd_gb);
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
                send_hr(req->RequestId,cfg.is_template?"createTemplate":"createVm",nu,created_id,hr);
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
        if (!vm || asb_vm_instance(vm)->is_template) { send_err(req->RequestId, 404, "Not Found", "not_found", "no such VM"); return 0; }
        vm_id = asb_vm_instance(vm)->unique_id;
        if (verb != HttpVerbGET && asb_vm_instance(vm)->starting) {
            send_err(req->RequestId, 409, "Conflict", "vm_starting", "VM start is in progress; retry once it completes");
            return 0;
        }
        WideCharToMultiByte(CP_UTF8, 0, asb_vm_name(vm), -1, nu, sizeof(nu), NULL, NULL);
        int snapshot_index=-1,branch_index=-1;
        const char *method=verb==HttpVerbGET?"GET":verb==HttpVerbPOST?"POST":verb==HttpVerbDELETE?"DELETE":verb==HttpVerbPUT?"PUT":"";
        if (sub && asb_api_snapshot_route(method,sub,&snapshot_index,&branch_index)<0) {
            send_err(req->RequestId,400,"Bad Request","invalid_arg","Invalid snapshot index or path.");return 0;
        }

        if (sub) {
            UINT64 session_id=0;
            const wchar_t *operation=asb_api_headless_route(method,sub,&session_id);
            if (operation) { defer_headless(req,asb_vm_instance(vm),operation,session_id);return 0; }
        }

        if (sub == NULL) {
            VmInstance *inst = asb_vm_instance(vm);
            if (!inst) { send_err(req->RequestId, 404, "Not Found", "not_found", "no such VM"); return 0; }
            if (verb == HttpVerbGET) {
                append_vm_json(buf, sizeof(buf), 0, inst);
                send_json(req->RequestId, 200, "OK", buf);
                return 0;
            }
            if (verb == HttpVerbPUT) {   /* edit (PUT instead of PATCH: PATCH isn't in the http.sys verb enum) */
                const wchar_t *body=request_body();
                wchar_t gpu_id[512] = {0};
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
                if (!validate_body(body)) {
                    send_err(req->RequestId, 400, "Bad Request", "invalid_arg",
                             "Request body must contain valid UTF-8 JSON without NUL bytes.");
                    return 0;
                }
                has_gpu = api_json_has_key(body, L"gpuMode") || api_json_has_key(body, L"gpuId");
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
                if (!validate_body(body)) {
                    send_err(req->RequestId,400,"Bad Request","invalid_arg","Invalid field types or JSON body.");return 0;
                }
                if (api_json_get_int(body,L"ramMb",&iv)) {
                    const char *error=asb_api_validate_ram_mb(iv);
                    if (error) { send_err(req->RequestId,400,"Bad Request","invalid_arg",error);return 0; }
                }
                if ((api_json_get_int(body,L"cpuCores",&iv) && iv<1) ||
                    (api_json_get_int(body,L"networkMode",&iv) && (iv<0 || iv>3))) {
                    send_err(req->RequestId,400,"Bad Request","invalid_arg","Invalid RAM, CPU, or network setting.");return 0;
                }
                if (api_json_get_int(body, L"ramMb", &iv)) {
                    hr = asb_vm_set_ram(vm, (DWORD)iv);
                }
                if (api_json_get_int(body, L"cpuCores", &iv)) {
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
                if (api_json_get_int(body, L"networkMode", &iv)) {
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
            const wchar_t *body=request_body();
            wchar_t bname[128] = {0}; int si = -1, bi = -1;
            if (!asb_api_validate_start(body) ||
                (api_json_has_key(body,L"branchName") && !api_json_get_string(body,L"branchName",bname,ARRAYSIZE(bname)))) {
                send_err(req->RequestId,400,"Bad Request","invalid_arg","Invalid start request.");return 0;
            }
            api_json_get_int(body, L"snapIndex", &si);
            api_json_get_int(body, L"branchIndex", &bi);
            api_json_get_string(body, L"branchName", bname, 128);
            HRESULT hr = asb_vm_start(vm, si, bi, bname[0] ? bname : NULL);
            BOOL open_display;
            if (SUCCEEDED(hr) && api_json_get_bool(body, L"openDisplay", &open_display) && !open_display)
                asb_vm_set_display_suppressed(vm, TRUE);
            send_hr(req->RequestId, "start", nu, vm_id, hr);
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
                const wchar_t *body=request_body();
                wchar_t sname[128] = {0};
                VmInstance *sv = asb_vm_instance(vm);
                if (sv && sv->running) {   /* clean 409, not the generic 500 from a running snapshot attempt */
                    send_err(req->RequestId, 409, "Conflict", "vm_running", "cannot snapshot a running VM; stop it first");
                    return 0;
                }
                if (api_json_has_key(body,L"name") && !api_json_get_string(body,L"name",sname,ARRAYSIZE(sname))) {
                    send_err(req->RequestId,400,"Bad Request","invalid_arg","Invalid snapshot request.");return 0;
                }
                send_hr(req->RequestId, "snapTake", nu, vm_id, asb_snap_take(vm, sname[0] ? sname : NULL));
                return 0;
            }
        }
        if (wcsncmp(sub, L"snapshots/", 10) == 0 &&
            ((sub[10] >= L'0' && sub[10] <= L'9') ||
             (sub[10] == L'-' && sub[11] >= L'0' && sub[11] <= L'9'))) {   /* incl. base = -2 */
            const wchar_t *srest = sub + 10;       /* "{s}" | "{s}/branches" | "{s}/branches/{b}" */
            int sidx=snapshot_index;
            const wchar_t *index_end=wcschr(srest,L'/');
            if (!index_end) index_end=srest+wcslen(srest);
            const wchar_t *br=*index_end?index_end:NULL;
            if (br) {
                const wchar_t *brest = br + 9;     /* expect "/{b}" -- delete one branch.
                   (No standalone "new branch": the GUI creates branches by starting
                    from a snapshot, exposed via POST .../start with a branchName.) */
                if (verb == HttpVerbDELETE && *brest == L'/') {
                    send_hr(req->RequestId, "snapDeleteBranch", nu, vm_id,
                            asb_snap_delete_branch(vm, sidx, branch_index));
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
                    const wchar_t *body=request_body();
                    wchar_t nm[128] = {0}; int bidx = -1;
                    api_json_get_int(body, L"branchIndex", &bidx);
                    if (bidx < -1 || !api_json_get_string(body, L"name", nm, 128)) {
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
        const wchar_t *body=request_body();
        BOOL force = FALSE;
        int running = 0, p2, cnt = asb_vm_count();
        char rbuf[2048];
        api_json_get_bool(body, L"force", &force);
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
        "\"pid\":%lu,\"version\":\"%s\",\"apiVersion\":\"%s\",\"mcpEndpoint\":\"http://127.0.0.1:%d/mcp\",\"mcpTransport\":\"streamable-http\"}\n",
        port, port, g_token, GetCurrentProcessId(), ASB_PRODUCT_VER, ASB_API_VERSION,port);
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
    int preferred = asb_get_api_port();
    if (preferred < 1 || preferred > 65535) {
        hlog(L"FATAL: ApiPort must be an integer from 1 to 65535 in vms.cfg [Settings].");
        return 0;
    }

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

    for (port = preferred; port <= 65535; port++) {
        wchar_t url[64];
        swprintf_s(url, 64, L"http://127.0.0.1:%d/", port);
        r = HttpAddUrlToUrlGroup(g_url_group, url, 0, 0);
        if (r == NO_ERROR) { *out_port = port; return 1; }
        if (r != ERROR_ALREADY_EXISTS && r != ERROR_SHARING_VIOLATION) {
            hlog(L"HttpAddUrlToUrlGroup %s failed (%lu)", url, r);return 0;
        }
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
        if (path && !wcscmp(path,L"/mcp")) { mcp_dispatch(req);continue; }
        if (!auth_ok(req) ||
            (req->Verb == HttpVerbGET && path &&
             (!wcscmp(path, L"/v1/version") || !wcscmp(path, L"/v1/events")))) {
            stop = handle_request(req);
        } else {
            ApiCall call = {0};
            call.request = req;
            call.valid_body = body_to_wide(req, call.body, ARRAYSIZE(call.body));
            if (!call.valid_body) {
                send_err(req->RequestId,400,"Bad Request","invalid_arg","Request body must be a valid JSON object with valid field types within the API size limits.");
                continue;
            }
            if (!g_gui_hwnd) {
                g_request_body=call.body;
                stop=handle_request(req);
                g_request_body=NULL;
                continue;
            }
            call.done = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (!call.done) break;
            if (PostMessageW(g_gui_hwnd, WM_ASB_API_REQUEST, 0, (LPARAM)&call)) {
                WaitForSingleObject(call.done, INFINITE);
                if (!call.deferred) send_json(req->RequestId, call.status ? call.status : 500,
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
    AsbMcpEvents mcp_events={NULL,mcp_event_cursor,mcp_read_events};
    g_mcp=asb_mcp_create((unsigned)port,g_token,ASB_PRODUCT_VER,&mcp_events);
    if (!g_mcp) { asb_api_stop();return FALSE; }
    g_api_subscription = asb_events_subscribe(api_event, NULL);
    if (!g_api_subscription) { asb_api_stop(); return FALSE; }
    g_server_thread = CreateThread(NULL, 0, api_thread, NULL, 0, NULL);
    if (!g_server_thread) { asb_api_stop(); return FALSE; }
    if (!write_discovery(port)) { asb_api_stop(); return FALSE; }
    asb_api_log_endpoints((unsigned)port);
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
    while (g_sse_count || g_headless_count || g_mcp_count) SleepConditionVariableCS(&g_ev_cv, &g_ev_cs, INFINITE);
    LeaveCriticalSection(&g_ev_cs);
    asb_mcp_destroy(g_mcp);g_mcp=NULL;
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
