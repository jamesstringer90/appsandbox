/*
 * appsandbox-clipboard.exe — Guest-side clipboard writer for AppSandbox.
 *
 * Runs as the interactive user (spawned by the agent service on Winsta0\Default
 * with the console user's token). Listens on AF_HYPERV socket (GUID :0005) for
 * the host IDD display to connect. Handles the host→guest clipboard direction:
 *   - On FORMAT_LIST from host: stores format metadata. Small formats
 *     (text, HTML, bitmap, ...) are fetched immediately; files (CF_HDROP)
 *     are downloaded in the background and added when complete.
 *   - On WM_RENDERFORMAT (the user pasted): requests the files via
 *     FORMAT_DATA_REQ happens in the background after the list arrives.
 *     finished CF_HDROP to the clipboard.
 *
 * Guest→host clipboard is handled separately by appsandbox-clipboard-reader.exe
 * (USER, GUID :0006).
 *
 * Runs as the interactive user, so it logs to %TEMP%\clipboard.log (beside
 * clipboard-reader.log); C:\Windows\AppSandbox is not writable for that user.
 */

#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <shlwapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#include "../transport/asb_transport.h"   /* AF_HYPERV on PC, ivshmem on Mac */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Clipboard channel: {A5B0CAFE-0005-4000-8000-000000000001} */
static const GUID CLIPBOARD_SERVICE_GUID =
    { 0xa5b0cafe, 0x0005, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Clipboard protocol ---- */

#define CLIP_MAGIC          0x504C4341  /* "ACLP" little-endian */
#define CLIP_READY_MAGIC    0x59444C43  /* "CLDY" little-endian */

/* Protocol: delayed rendering (like MS-RDPECLIP) */
#define CLIP_MSG_FORMAT_LIST      1
#define CLIP_MSG_FORMAT_DATA_REQ  2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA        4
#define CLIP_MSG_SYNC_ENABLE      12
#define CLIP_MSG_FILE_LIST        5
#define CLIP_MSG_FILE_DATA_REQ    6

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)  /* 64 MB */

#define WM_CLIP_APPLY_FMTS (WM_USER + 200)

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 msg_type;
    UINT32 format;
    UINT32 data_size;
} ClipHeader;
#pragma pack(pop)

/* Format entry: maps local↔remote IDs and holds fetched data */
typedef struct {
    UINT   local_id;
    UINT   remote_id;
    BYTE  *data;
    UINT32 data_size;
    BOOL   is_hdrop;
} ClipFmtEntry;

/* File transfer for CF_HDROP */
#define CLIP_FILE_CHUNK     (1024 * 1024)  /* 1 MB — HV socket is basically memcpy */
static wchar_t g_clip_temp_dir[MAX_PATH];

/* Registered clipboard formats for on-demand file streaming. */
static UINT g_cf_file_desc = 0;   /* CFSTR_FILEDESCRIPTORW */
static UINT g_cf_file_contents = 0; /* CFSTR_FILECONTENTS */

#pragma pack(push, 1)
typedef struct {
    UINT32 path_len;     /* relative path length in bytes (UTF-16LE, no null) */
    UINT64 file_size;    /* file data bytes that follow (0 for directories) */
    UINT8  is_directory; /* 1 = directory entry, 0 = regular file */
} ClipFileInfo;
#pragma pack(pop)

/* Format data: filled by recv thread, applied by wndproc */
static ClipFmtEntry g_clip_fmts[CLIP_MAX_FORMATS];
static int          g_clip_fmt_count = 0;
static int          g_clip_fetch_idx = 0;

/* File metadata for FileContents (lazy streaming). */
#define META_MAX 4096
typedef struct {
    wchar_t name[MAX_PATH];
    UINT64  size;
    DWORD   attrs;
    int     is_dir;
} FileMeta;
static FileMeta g_file_meta[META_MAX];
static int      g_file_meta_count = 0;

/* On-demand file transfer for FileContents.  The network only carries one file
 * at a time, but several IStreams may exist at once (Explorer starts one per
 * file and can run a few in parallel).  Each stream owns its own staging file;
 * g_xfer_gen identifies which request the shared "currently streaming" state
 * belongs to.  A stream whose generation is no longer current knows its own
 * transfer already finished and reads its private file to EOF.  The recv
 * thread fills the temp file while the IStream reader drains it, so Explorer's
 * progress dialog advances during the transfer instead of waiting for the
 * whole file. */
struct FileContentsStream;   /* defined below; see g_active_stream */

static volatile LONG g_file_req_pending = 0;  /* recv thread must fetch */
static volatile LONG g_file_req_idx = 0;
static CRITICAL_SECTION g_xfer_cs;
static HANDLE g_slot_event = NULL;   /* manual-reset: wire slot is free */
static struct FileContentsStream *g_active_stream = NULL; /* slot owner */
static int      g_xfer_active = 0;
static int      g_xfer_done = 0;
static int      g_xfer_error = 0;
static volatile LONG g_xfer_cancel = 0; /* paste abandoned: drain, don't write */
static volatile LONG g_xfer_dead = 0;  /* recv thread is gone: no more bytes */
static UINT64   g_xfer_total = 0;
static wchar_t  g_xfer_path[MAX_PATH];    /* staging file of the active request */
static HANDLE   g_xfer_hFile = NULL;      /* duplicate for the recv thread */
static volatile LONG g_xfer_gen = 0;      /* generation of the active request */
static wchar_t  g_req_name[MAX_PATH];     /* expected relative name of it */

/* Lazy staging files live outside g_clip_temp_dir: a new FORMAT_LIST wipes
   that directory, which must never delete a file an in-flight paste reads. */
static wchar_t g_lazy_dir[MAX_PATH];

/* Echo suppression */
static volatile LONG g_suppress = 0;

/* Focus-gated sync: host sends SYNC_ENABLE(0/1) on :0005.
 * When disabled, we ignore FORMAT_LIST and don't apply to clipboard.
 * The named event is shared with appsandbox-clipboard-reader.exe so
 * it can gate guest->host FORMAT_LIST sending. */
static volatile LONG g_sync_enabled = 0;
static HANDLE g_sync_event = NULL;
#define CLIP_SYNC_EVENT_NAME L"Global\\AppSandboxClipSync"

/* Socket shared between threads */
static AsbConn * volatile g_client_sock = NULL;
static CRITICAL_SECTION g_send_cs;

/* Message window (created on msg_window_thread) */
static HWND g_msg_hwnd = NULL;

/* State for the background file fetch (Option 2, made safe): the guest
 * advertises files only once they have actually been downloaded into the temp
 * folder, so a paste never blocks on a multi-gigabyte transfer and no
 * clipboard probe (Explorer menus, clipboard history, ...) can trigger one.
 * g_list_gen is bumped for every FORMAT_LIST; g_fetch_gen records which list
 * the in-flight fetch belongs to so a stale response is ignored. */
static CRITICAL_SECTION g_state_cs;
static volatile LONG g_list_has_hdrop = 0;
static volatile LONG g_apply_pending = 0;
static ULONGLONG     g_meta_deadline = 0;
static volatile LONG g_list_gen = 0;
static volatile LONG g_fetch_gen = 0;
static volatile LONG g_file_fetch_active = 0;
static volatile LONG g_applied_gen = 0;
static volatile LONG g_applied_seq = 0;
static volatile LONG g_applied_has_nonfile = 0;
#define WM_CLIP_ADD_FILES (WM_USER + 202)

/* ---- Logging ---- */

/* The clipboard helper is started with CreateProcessAsUserW (see
   spawn_clipboard_in_session in agent.c), i.e. it runs as the logged-in user,
   not as SYSTEM.  The user cannot create files in C:\Windows\AppSandbox, so the
   old hard-coded log was silently never written.  Log next to
   clipboard-reader.log in the user's temp directory instead: that is where the
   reader already keeps its log, and it is always writable. */
static char g_log_path[MAX_PATH] = "";

static const char *clip_log_path(void)
{
    if (!g_log_path[0]) {
        char tmp[MAX_PATH];

        if (GetTempPathA(MAX_PATH, tmp) > 0 &&
            strlen(tmp) + strlen("clipboard.log") + 1 < MAX_PATH)
            sprintf_s(g_log_path, MAX_PATH, "%sclipboard.log", tmp);
        else
            lstrcpynA(g_log_path, "C:\\Windows\\AppSandbox\\clipboard.log",
                      MAX_PATH);
    }
    return g_log_path;
}

static void clip_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, clip_log_path(), "a") != 0 || !f)
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

/* ---- Reliable send/recv ---- */

static BOOL send_all(AsbConn *s, const void *buf, int len)
{
    return asb_send(s, buf, len) == len;
}

static BOOL recv_exact(AsbConn *s, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = asb_recv(s, (char *)buf + got, len - got);
        if (n <= 0) return FALSE;
        got += n;
    }
    return TRUE;
}

/* ---- Send helpers ---- */

static BOOL send_all_locked(const void *buf, int len)
{
    AsbConn *s = g_client_sock;
    if (!s) return FALSE;
    return asb_send(s, buf, len) == len;
}

/* ---- Send clipboard to host ---- */

static void clip_init_temp_dir(void)
{
    DWORD session_id = WTSGetActiveConsoleSessionId();
    HANDLE user_token = NULL;
    wchar_t tmp[MAX_PATH];

    /* Get the logged-in user's token so we can resolve their %TEMP% */
    if (session_id != 0xFFFFFFFF &&
        WTSQueryUserToken(session_id, &user_token)) {
        if (ExpandEnvironmentStringsForUserW(user_token, L"%TEMP%",
                                              tmp, MAX_PATH)) {
            swprintf_s(g_clip_temp_dir, MAX_PATH, L"%s\\AppSandboxClip", tmp);
            swprintf_s(g_lazy_dir, MAX_PATH, L"%s\\AppSandboxClipLazy", tmp);
            CloseHandle(user_token);
            CreateDirectoryW(g_clip_temp_dir, NULL);
            CreateDirectoryW(g_lazy_dir, NULL);
            clip_log("Temp dir: %ls", g_clip_temp_dir);
            return;
        }
        CloseHandle(user_token);
    }

    /* Fallback: SYSTEM temp (should not normally happen) */
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_clip_temp_dir, MAX_PATH, L"%sAppSandboxClip", tmp);
    swprintf_s(g_lazy_dir, MAX_PATH, L"%sAppSandboxClipLazy", tmp);
    CreateDirectoryW(g_clip_temp_dir, NULL);
    CreateDirectoryW(g_lazy_dir, NULL);
    clip_log("Temp dir (fallback): %ls", g_clip_temp_dir);
}

/* ---- File transfer helpers ---- */

static void clip_remove_dir_recursive(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t path[MAX_PATH];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            clip_remove_dir_recursive(path);
        else
            DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
}

static void clip_ensure_parent_dir(const wchar_t *file_path)
{
    wchar_t dir[MAX_PATH];
    wchar_t *p;
    wcscpy_s(dir, MAX_PATH, file_path);
    p = wcsrchr(dir, L'\\');
    if (!p) return;
    *p = L'\0';
    for (p = dir; *p; p++) {
        if (*p == L'\\' && p > dir && *(p - 1) != L':') {
            *p = L'\0';
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(dir, NULL);
}

static BOOL clip_path_is_safe(const wchar_t *rel_path)
{
    if (wcsstr(rel_path, L"..")) return FALSE;
    if (rel_path[0] == L'\\' || rel_path[0] == L'/') return FALSE;
    if (wcslen(rel_path) > MAX_PATH - 50) return FALSE;
    return TRUE;
}

static HGLOBAL clip_build_hdrop_from_temp(void)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
    int count = 0;
    SIZE_T total_size;
    HGLOBAL hMem;
    DROPFILES *df;
    wchar_t *ptr;
    int i;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", g_clip_temp_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(paths[count], MAX_PATH, L"%s\\%s", g_clip_temp_dir, fd.cFileName);
        count++;
        if (count >= CLIP_MAX_FORMATS) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (count == 0) return NULL;

    total_size = sizeof(DROPFILES);
    for (i = 0; i < count; i++)
        total_size += (wcslen(paths[i]) + 1) * sizeof(wchar_t);
    total_size += sizeof(wchar_t);

    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_size);
    if (!hMem) return NULL;
    df = (DROPFILES *)GlobalLock(hMem);
    if (!df) { GlobalFree(hMem); return NULL; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    ptr = (wchar_t *)((BYTE *)df + sizeof(DROPFILES));
    for (i = 0; i < count; i++) {
        /* destsz must be this entry's reserved slice (wcslen+1), NOT MAX_PATH:
           the buffer is sized tightly per path (see total_size above), so passing
           MAX_PATH makes wcscpy_s's secure-CRT tail-fill memset run past the
           allocation -> heap overflow (caught by full page heap at memset). */
        wcscpy_s(ptr, wcslen(paths[i]) + 1, paths[i]);
        ptr += wcslen(paths[i]) + 1;
    }
    *ptr = L'\0';
    GlobalUnlock(hMem);
    return hMem;
}

/* Free all pending format data */
static void clip_free_pending(void)
{
    int i;
    for (i = 0; i < g_clip_fmt_count; i++) {
        if (g_clip_fmts[i].data) {
            HeapFree(GetProcessHeap(), 0, g_clip_fmts[i].data);
            g_clip_fmts[i].data = NULL;
        }
    }
}

/* Send FORMAT_DATA_REQ for the next eagerly fetched format, or post
   WM_CLIP_APPLY_FMTS if all of them are done. CF_HDROP entries are skipped:
   files are rendered lazily, when the user pastes. Called from recv thread. */
static void clip_request_next_format(void)
{
    ClipHeader hdr;

    /* Skip file entries - they are fetched in the background after apply. */
    while (g_clip_fetch_idx < g_clip_fmt_count &&
           g_clip_fmts[g_clip_fetch_idx].is_hdrop)
        g_clip_fetch_idx++;

    if (g_clip_fetch_idx >= g_clip_fmt_count) {
        /* Everything that needs to be here now has arrived - tell msg window
           to apply. If the list has files, wait until the metadata message
           (sent right after the format list) has been read, so the apply can
           advertise the files lazily instead of downloading them. */
        clip_log("Metadata formats fetched, applying.");
        if (g_list_has_hdrop) {
            InterlockedExchange(&g_apply_pending, 1);
        } else if (g_msg_hwnd) {
            PostMessageW(g_msg_hwnd, WM_CLIP_APPLY_FMTS, 0, 0);
        }
        return;
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
    hdr.format = g_clip_fmts[g_clip_fetch_idx].remote_id;
    hdr.data_size = 0;

    EnterCriticalSection(&g_send_cs);
    send_all_locked(&hdr, sizeof(hdr));
    LeaveCriticalSection(&g_send_cs);

    clip_log("Requesting format %u (remote=%u) [%d/%d].",
             g_clip_fmts[g_clip_fetch_idx].local_id,
             g_clip_fmts[g_clip_fetch_idx].remote_id,
             g_clip_fetch_idx + 1, g_clip_fmt_count);
}

/* Apply the format list to the clipboard.
   Small formats are applied with their fetched data; CF_HDROP is put on the
   clipboard with delayed rendering (SetClipboardData(fmt, NULL)) so that the
   files are only downloaded when the user actually pastes.
   Called on the message window thread via WM_CLIP_APPLY_FMTS. */
/* ---- COM IDataObject / IStream for on-demand FileContents ---- */

#define SMALL_MAX 64

typedef struct {
    UINT    fmt;
    HGLOBAL h;
    SIZE_T  size;
} SmallFmt;

typedef struct {
    IDataObjectVtbl *lpVtbl;
    LONG ref;
    SmallFmt smalls[SMALL_MAX];
    int small_count;
    FileMeta *meta;          /* snapshot of the pushed file list */
    int meta_count;
} AsbDO;

typedef struct FileContentsStream {
    IStreamVtbl *lpVtbl;
    LONG ref;
    int index;
    UINT64 declared_size;
    UINT64 pos;
    HANDLE hFile;
    wchar_t temp_path[MAX_PATH];
    wchar_t rel_name[MAX_PATH];   /* expected name from the advertised list */
    LONG gen;                     /* request generation this stream owns */
    int started;
    int finished;
    int abandoned;                /* paste released this stream (under g_xfer_cs) */
    DWORD thread_id;              /* STA thread that services this stream */
    HANDLE data_event;            /* auto-reset: bytes arrived or transfer done */
} FileContentsStream;

static IStreamVtbl FCS_Vtbl;
static IEnumFORMATETCVtbl EN_Vtbl;

/* ---------- IStream: reads straight from the growing temp file ---------- */

/* Abandoning a paste (the shell releases its IStream) must not take the whole
   clipboard channel down: several pastes can share this connection, and a
   teardown used to make one bad job fail every other one.  The body of the
   abandoned request is drained and discarded instead, which keeps the wire in
   sync and frees the slot for the next paste.  See g_xfer_cancel. */

static HRESULT STDMETHODCALLTYPE FCS_QueryInterface(IStream *this, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IStream)) {
        *ppv = this;
        this->lpVtbl->AddRef(this);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE FCS_AddRef(IStream *this)
{
    return (ULONG)InterlockedIncrement(&((FileContentsStream *)this)->ref);
}

static ULONG STDMETHODCALLTYPE FCS_Release(IStream *this)
{
    FileContentsStream *s = (FileContentsStream *)this;
    ULONG r = (ULONG)InterlockedDecrement(&s->ref);
    if (r == 0) {
        int cancelled = 0;
        HANDLE data_event;

        /* Released before its bytes arrived: this paste was abandoned.  Ask the
           recv thread to stop writing and drain the rest of the body. */
        EnterCriticalSection(&g_xfer_cs);
        if (g_active_stream == s) g_active_stream = NULL;
        s->abandoned = 1;
        if (s->started && !s->finished &&
            s->pos < s->declared_size &&
            s->gen == g_xfer_gen && g_xfer_active && !g_xfer_done) {
            InterlockedExchange(&g_xfer_cancel, 1);
            cancelled = 1;
        }
        data_event = s->data_event;
        s->data_event = NULL;
        LeaveCriticalSection(&g_xfer_cs);

        clip_log("Released stream %d gen %ld pos %llu/%llu started %d finished %d cancel %d.",
                 s->index, (long)s->gen,
                 (unsigned long long)s->pos,
                 (unsigned long long)s->declared_size,
                 s->started ? 1 : 0, s->finished ? 1 : 0, cancelled);

        if (cancelled)
            clip_log("Lazy stream %d abandoned; draining the rest of the body "
                     "(connection stays up).", s->index);

        if (data_event) CloseHandle(data_event);

        if (s->hFile && s->hFile != INVALID_HANDLE_VALUE) CloseHandle(s->hFile);
        if (s->temp_path[0]) DeleteFileW(s->temp_path);
        /* Stop this stream's STA message loop (if it is already exiting or
           running on this same thread, the posted WM_QUIT is harmless). */
        if (s->thread_id) PostThreadMessageW(s->thread_id, WM_QUIT, 0, 0);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return r;
}

/* Pump posted messages for this thread.  Each FileContents stream is serviced
   on its own STA (see fcs_thread_proc), so this only dispatches COM calls for
   that one stream; the pump is still required because otherwise a cancel
   (Release) could not be delivered while Read() is waiting for bytes. */
static void fcs_pump_messages(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* Wait for one event without starving the apartment. */
static DWORD fcs_wait_event(HANDLE ev, DWORD ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;

    if (!ev) return WAIT_TIMEOUT;
    for (;;) {
        DWORD remain, r;
        ULONGLONG now = GetTickCount64();

        if (now >= deadline) return WAIT_TIMEOUT;
        remain = (DWORD)(deadline - now);
        r = MsgWaitForMultipleObjects(1, &ev, FALSE, remain, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        if (r == WAIT_TIMEOUT) return WAIT_TIMEOUT;
        if (r == WAIT_OBJECT_0 + 1) fcs_pump_messages();
        else return r;
    }
}

/* Wake the stream that currently owns the wire slot.  Called by the recv
   thread after it writes bytes.  The pointer is protected by g_xfer_cs so a
   concurrent stream release cannot free it underneath us. */
static void fcs_signal_data(void)
{
    EnterCriticalSection(&g_xfer_cs);
    if (g_active_stream && g_active_stream->data_event)
        SetEvent(g_active_stream->data_event);
    LeaveCriticalSection(&g_xfer_cs);
}

/* Finish the active transfer, wake its reader and free the wire slot. */
static void fcs_finish_transfer(int error)
{
    EnterCriticalSection(&g_xfer_cs);
    if (error) g_xfer_error = 1;
    g_xfer_done = 1;
    if (g_active_stream && g_active_stream->data_event)
        SetEvent(g_active_stream->data_event);
    g_active_stream = NULL;
    InterlockedExchange(&g_file_req_pending, 0);
    g_xfer_active = 0;
    SetEvent(g_slot_event);   /* manual-reset: stays free until claimed */
    LeaveCriticalSection(&g_xfer_cs);
}

/* Claim the single network transfer slot and create this stream's own staging
   file.  Creating the file here (rather than in the recv thread) means a newer
   request can never overwrite the path of an older stream that has not opened
   its file yet. */
static BOOL fcs_start_request(FileContentsStream *s)
{
    wchar_t tmp[MAX_PATH];
    HANDLE hf;
    LONG gen;
    ULONGLONG last_wait_log = GetTickCount64();

    /* This stream's own wake-up event must exist before the recv thread can
       signal bytes, and is separate from the slot event so waiters never
       consume each other's notifications. */
    if (!s->data_event) {
        s->data_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!s->data_event) {
            clip_log("Lazy stream %d: CreateEvent failed (%lu).",
                     s->index, GetLastError());
            return FALSE;
        }
    }

    /* Several files can be pasted at once; only one travels on the wire at a
       time, so waiting for the slot is normal and must not be bounded by the
       per-transfer inactivity timeout.  Only a dead recv thread aborts it. */
    for (;;) {
        EnterCriticalSection(&g_xfer_cs);
        if (g_xfer_dead) {
            LeaveCriticalSection(&g_xfer_cs);
            clip_log("Lazy stream %d: connection closed while queued.", s->index);
            return FALSE;
        }
        if (!g_xfer_active) {
            /* The paste was abandoned while this stream was still queued: do
               not claim the wire and download a file nobody will read. */
            if (s->abandoned) {
                LeaveCriticalSection(&g_xfer_cs);
                clip_log("Lazy stream %d: abandoned while queued; not requesting.",
                         s->index);
                return FALSE;
            }
            g_xfer_active  = 1;
            g_xfer_done    = 0;
            g_xfer_error   = 0;
            InterlockedExchange(&g_xfer_cancel, 0);
            g_xfer_total   = s->declared_size;
            gen = ++g_xfer_gen;
            s->gen = gen;
            /* Mark it started while holding the lock so a concurrent Release
               cancels the transfer instead of racing the staging file. */
            s->started = 1;
            wcscpy_s(g_req_name, MAX_PATH, s->rel_name);
            g_file_req_idx = s->index;
            g_active_stream = s;
            ResetEvent(s->data_event);
            ResetEvent(g_slot_event);
            LeaveCriticalSection(&g_xfer_cs);
            break;
        }
        LeaveCriticalSection(&g_xfer_cs);
        /* Log occasionally so a genuinely wedged queue is visible in the log. */
        if (GetTickCount64() - last_wait_log >= 30000) {
            last_wait_log = GetTickCount64();
            clip_log("Lazy stream %d: still waiting for the wire (gen %ld).",
                     s->index, (long)g_xfer_gen);
        }
        fcs_wait_event(g_slot_event, 200);
    }

    CreateDirectoryW(g_lazy_dir, NULL);
    {
        /* Only worth a line when the paste may actually run out of space;
           otherwise this repeats once per file of a multi-file paste. */
        ULARGE_INTEGER freeb;
        if (GetDiskFreeSpaceExW(g_lazy_dir, &freeb, NULL, NULL) &&
            freeb.QuadPart < s->declared_size)
            clip_log("Lazy staging '%ls' has only %llu MB free, less than the "
                     "%llu MB file being pasted.",
                     g_lazy_dir,
                     (unsigned long long)(freeb.QuadPart / (1024 * 1024)),
                     (unsigned long long)(s->declared_size / (1024 * 1024)));
    }
    swprintf_s(tmp, MAX_PATH, L"%s\\_fc_%ld_%d.tmp",
               g_lazy_dir, (long)gen, s->index);
    hf = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        clip_log("Lazy stream %d: cannot create staging file '%ls' (%lu).",
                 s->index, tmp, GetLastError());
        fcs_finish_transfer(1);
        return FALSE;
    }

    s->hFile = hf;
    wcscpy_s(s->temp_path, MAX_PATH, tmp);

    /* Released between the claim and the request: give the slot back now so
       the next paste does not wait for a body nobody asked for. */
    EnterCriticalSection(&g_xfer_cs);
    if (s->abandoned) {
        LeaveCriticalSection(&g_xfer_cs);
        CloseHandle(hf);
        s->hFile = INVALID_HANDLE_VALUE;
        s->temp_path[0] = L'\0';
        DeleteFileW(tmp);
        fcs_finish_transfer(0);
        clip_log("Lazy stream %d: abandoned before the request; slot released.",
                 s->index);
        return FALSE;
    }
    LeaveCriticalSection(&g_xfer_cs);

    /* Give the recv thread its own *file object* (a fresh open, not a
       DuplicateHandle: a duplicate shares the one file pointer with the
       stream, and the stream seeks around while the body is being written, so
       the writer would land at the reader's offset and corrupt the staged
       data).  Release deletes the path, which must not pull the file out from
       under a request that is already being served (ERROR_FILE_NOT_FOUND);
       the open handle keeps the file usable until it is closed. */
    {
        HANDLE serve = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (serve == INVALID_HANDLE_VALUE) {
            clip_log("Lazy stream %d: cannot reopen staging file '%ls' (%lu).",
                     s->index, tmp, GetLastError());
            CloseHandle(hf);
            s->hFile = INVALID_HANDLE_VALUE;
            DeleteFileW(tmp);
            fcs_finish_transfer(1);
            return FALSE;
        }
        EnterCriticalSection(&g_xfer_cs);
        if (g_xfer_hFile && g_xfer_hFile != INVALID_HANDLE_VALUE)
            CloseHandle(g_xfer_hFile);
        g_xfer_hFile = serve;
        wcscpy_s(g_xfer_path, MAX_PATH, tmp);
        InterlockedExchange(&g_file_req_pending, 1);
        LeaveCriticalSection(&g_xfer_cs);
    }

    return TRUE;
}

static HRESULT fcs_read_impl(IStream *this, void *pv, ULONG cb, ULONG *pcbRead)
{
    FileContentsStream *s = (FileContentsStream *)this;
    DWORD got = 0;
    UINT64 last_size = 0;
    ULONGLONG last_progress;
    ULONGLONG last_log = GetTickCount64();

    if (pcbRead) *pcbRead = 0;
    if (cb == 0) return S_OK;

    if (s->declared_size == 0) {
        s->finished = 1;
        return S_FALSE;
    }

    if (!s->started) {
        if (!fcs_start_request(s)) {
            clip_log("Lazy stream %d (%ls): could not start request.",
                     s->index, s->rel_name);
            s->finished = 1;
            return E_FAIL;
        }
    }
    if (!s->hFile || s->hFile == INVALID_HANDLE_VALUE) {
        clip_log("Lazy stream %d (%ls): invalid staging handle.", s->index, s->rel_name);
        s->finished = 1;
        return E_FAIL;
    }

    /* The inactivity watchdog must measure real transfer progress.  Waiting in
       the queue above can legitimately take a long time when several files are
       pasted together, and must not count as "no progress". */
    last_progress = GetTickCount64();

    for (;;) {
        LARGE_INTEGER fsz;
        UINT64 avail;
        int done, error, abandoned;

        /* The recv thread writes this stream's private file.  Its current size
           is the authoritative byte count; no shared progress counter. */
        if (!GetFileSizeEx(s->hFile, &fsz)) {
            clip_log("Lazy stream %d (%ls): GetFileSizeEx failed (%lu).",
                     s->index, s->rel_name, GetLastError());
            s->finished = 1;
            return E_FAIL;
        }
        avail = (UINT64)fsz.QuadPart;

        EnterCriticalSection(&g_xfer_cs);
        abandoned = s->abandoned;
        if (s->gen == g_xfer_gen) {
            done  = g_xfer_done;
            error = g_xfer_error;
        } else {
            /* A newer request started: this one has already finished (the
               slot is only released after the body was fully read/written). */
            done  = 1;
            error = (avail < s->declared_size) ? 1 : 0;
        }
        LeaveCriticalSection(&g_xfer_cs);

        if (s->pos < avail && s->pos < s->declared_size) {
            UINT64 n = avail - s->pos;
            LARGE_INTEGER off;
            if (n > cb) n = cb;
            off.QuadPart = (LONGLONG)s->pos;
            if (!SetFilePointerEx(s->hFile, off, NULL, FILE_BEGIN)) {
                clip_log("Lazy stream %d (%ls): SetFilePointerEx failed (%lu).",
                         s->index, s->rel_name, GetLastError());
                s->finished = 1;
                return E_FAIL;
            }
            if (!ReadFile(s->hFile, pv, (DWORD)n, &got, NULL)) {
                clip_log("Lazy stream %d (%ls): ReadFile failed (%lu).",
                         s->index, s->rel_name, GetLastError());
                s->finished = 1;
                return E_FAIL;
            }
            s->pos += got;
            if (pcbRead) *pcbRead = got;
            return got ? S_OK : E_FAIL;
        }

        if (s->pos >= s->declared_size) {
            s->finished = 1;
            return S_FALSE;          /* clean end of stream */
        }

        if (abandoned) {
            /* The paste was abandoned: the recv thread is draining the rest of
               the body, but nobody will read this stream again.  Leave now
               instead of waiting for the inactivity watchdog. */
            s->finished = 1;
            return S_FALSE;
        }

        if (done) {
            clip_log("Stream %d ended: pos=%llu/%llu avail=%llu error=%d.",
                     s->index, (unsigned long long)s->pos,
                     (unsigned long long)s->declared_size,
                     (unsigned long long)avail, error);
            s->finished = 1;
            return error ? E_FAIL : S_FALSE;
        }

        /* A paste must never hang: fail if the peer stopped sending. */
        if (avail != last_size) {
            last_size = avail;
            last_progress = GetTickCount64();
        } else if (GetTickCount64() - last_progress >= 60000) {
            clip_log("Lazy stream %d (%ls): no data for 60 s "
                     "(gen %ld, pos %llu/%llu).",
                     s->index, s->rel_name, (long)s->gen,
                     (unsigned long long)s->pos,
                     (unsigned long long)s->declared_size);
            s->finished = 1;
            return E_FAIL;
        }

        /* A heartbeat, not a progress bar: the shell already shows the bytes.
           One line every 30 s keeps a wedged paste visible without flooding
           the log during a multi-minute transfer. */
        if (GetTickCount64() - last_log >= 30000) {
            last_log = GetTickCount64();
            clip_log("Stream %d waiting: pos=%llu avail=%llu declared=%llu "
                     "done=%d error=%d.",
                     s->index, (unsigned long long)s->pos,
                     (unsigned long long)avail,
                     (unsigned long long)s->declared_size, done, error);
        }

        fcs_wait_event(s->data_event, 1000);
    }
}

/* Hold an extra reference for the duration of Read so a cancel delivered
   while we are waiting cannot free the stream underneath the call. */
static HRESULT STDMETHODCALLTYPE FCS_Read(IStream *this, void *pv, ULONG cb, ULONG *pcbRead)
{
    FileContentsStream *s = (FileContentsStream *)this;
    HRESULT hr;

    InterlockedIncrement(&s->ref);
    hr = fcs_read_impl(this, pv, cb, pcbRead);
    s->lpVtbl->Release(this);
    return hr;
}

static HRESULT STDMETHODCALLTYPE FCS_Write(IStream *this, const void *pv, ULONG cb, ULONG *pcbWritten)
{
    (void)this; (void)pv; (void)cb;
    if (pcbWritten) *pcbWritten = 0;
    return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE FCS_Seek(IStream *this, LARGE_INTEGER dlibMove,
                                          DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition)
{
    FileContentsStream *s = (FileContentsStream *)this;
    LONGLONG base;
    LONGLONG np;

    switch (dwOrigin) {
    case STREAM_SEEK_SET: base = 0; break;
    case STREAM_SEEK_CUR: base = (LONGLONG)s->pos; break;
    case STREAM_SEEK_END: base = (LONGLONG)s->declared_size; break;
    default: return STG_E_INVALIDFUNCTION;
    }

    np = base + dlibMove.QuadPart;
    if (np < 0) return STG_E_INVALIDFUNCTION;

    s->pos = (UINT64)np;
    if (plibNewPosition) plibNewPosition->QuadPart = (ULONGLONG)np;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FCS_SetSize(IStream *this, ULARGE_INTEGER libNewSize)
{
    (void)this; (void)libNewSize;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE FCS_CopyTo(IStream *this, IStream *pstm, ULARGE_INTEGER cb,
                                            ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten)
{
    FileContentsStream *s = (FileContentsStream *)this;
    BYTE buf[64 * 1024];
    UINT64 left = cb.QuadPart;
    UINT64 rd = 0, wr = 0;

    if (pcbRead) pcbRead->QuadPart = 0;
    if (pcbWritten) pcbWritten->QuadPart = 0;

    /* cb == 0 means "copy to the end of the stream". */
    if (cb.QuadPart == 0) {
        if (s->declared_size > s->pos) left = s->declared_size - s->pos;
        else left = ~(UINT64)0;   /* size unknown/streaming: read until S_FALSE */
    }

    while (left > 0) {
        ULONG want = left > sizeof(buf) ? (ULONG)sizeof(buf) : (ULONG)left;
        ULONG got = 0, written = 0;
        HRESULT rh = FCS_Read(this, buf, want, &got);
        if (FAILED(rh)) return rh;
        if (got == 0) break;
        rd += got;
        {
            HRESULT wh = pstm->lpVtbl->Write(pstm, buf, got, &written);
            if (FAILED(wh)) return wh;
        }
        wr += written;
        left -= got;
        /* The read result, not the write result, says whether the source
           ended.  A short read that is not S_FALSE just means "more to come". */
        if (rh == S_FALSE) break;
    }

    if (pcbRead) pcbRead->QuadPart = rd;
    if (pcbWritten) pcbWritten->QuadPart = wr;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FCS_Commit(IStream *this, DWORD grfCommitFlags)
{
    (void)this; (void)grfCommitFlags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FCS_Revert(IStream *this)
{
    (void)this;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE FCS_LockRegion(IStream *this, ULARGE_INTEGER libOffset,
                                                ULARGE_INTEGER cb, DWORD dwLockType)
{
    (void)this; (void)libOffset; (void)cb; (void)dwLockType;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE FCS_UnlockRegion(IStream *this, ULARGE_INTEGER libOffset,
                                                  ULARGE_INTEGER cb, DWORD dwLockType)
{
    (void)this; (void)libOffset; (void)cb; (void)dwLockType;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE FCS_Stat(IStream *this, STATSTG *pstatstg, DWORD grfStatFlag)
{
    FileContentsStream *s = (FileContentsStream *)this;
    (void)grfStatFlag;
    if (!pstatstg) return E_INVALIDARG;
    ZeroMemory(pstatstg, sizeof(*pstatstg));
    pstatstg->type = STGTY_STREAM;
    pstatstg->cbSize.QuadPart = s->declared_size;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE FCS_Clone(IStream *this, IStream **ppstm)
{
    (void)this;
    if (ppstm) *ppstm = NULL;
    return E_NOTIMPL;
}

/* ---------- IEnumFORMATETC ---------- */

typedef struct {
    IEnumFORMATETCVtbl *lpVtbl;
    LONG ref;
    FORMATETC *items;
    int count;
    int pos;
} AsbEnum;

static HRESULT STDMETHODCALLTYPE EN_QueryInterface(IEnumFORMATETC *this, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC)) {
        *ppv = this;
        this->lpVtbl->AddRef(this);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE EN_AddRef(IEnumFORMATETC *this)
{
    return (ULONG)InterlockedIncrement(&((AsbEnum *)this)->ref);
}

static ULONG STDMETHODCALLTYPE EN_Release(IEnumFORMATETC *this)
{
    AsbEnum *e = (AsbEnum *)this;
    ULONG r = (ULONG)InterlockedDecrement(&e->ref);
    if (r == 0) {
        if (e->items) HeapFree(GetProcessHeap(), 0, e->items);
        HeapFree(GetProcessHeap(), 0, e);
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE EN_Next(IEnumFORMATETC *this, ULONG celt,
                                         FORMATETC *rgelt, ULONG *pceltFetched)
{
    AsbEnum *e = (AsbEnum *)this;
    ULONG n = 0;

    if (!rgelt) return E_INVALIDARG;
    while (n < celt && e->pos < e->count)
        rgelt[n++] = e->items[e->pos++];
    if (pceltFetched) *pceltFetched = n;
    return (n == celt) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE EN_Skip(IEnumFORMATETC *this, ULONG celt)
{
    AsbEnum *e = (AsbEnum *)this;
    e->pos += (int)celt;
    if (e->pos > e->count) e->pos = e->count;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE EN_Reset(IEnumFORMATETC *this)
{
    ((AsbEnum *)this)->pos = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE EN_Clone(IEnumFORMATETC *this, IEnumFORMATETC **ppenum)
{
    AsbEnum *e = (AsbEnum *)this;
    AsbEnum *c;

    if (!ppenum) return E_INVALIDARG;
    *ppenum = NULL;
    c = (AsbEnum *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AsbEnum));
    if (!c) return E_OUTOFMEMORY;
    c->items = (FORMATETC *)HeapAlloc(GetProcessHeap(), 0, sizeof(FORMATETC) * e->count);
    if (!c->items) { HeapFree(GetProcessHeap(), 0, c); return E_OUTOFMEMORY; }
    memcpy(c->items, e->items, sizeof(FORMATETC) * e->count);
    c->lpVtbl = &EN_Vtbl;
    c->ref = 1;
    c->count = e->count;
    c->pos = e->pos;
    *ppenum = (IEnumFORMATETC *)c;
    return S_OK;
}

/* ---------- IDataObject ---------- */

static HRESULT STDMETHODCALLTYPE DO_QueryInterface(IDataObject *this, REFIID riid, void **ppv)
{
    if (!ppv) return E_INVALIDARG;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject)) {
        *ppv = this;
        this->lpVtbl->AddRef(this);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE DO_AddRef(IDataObject *this)
{
    return (ULONG)InterlockedIncrement(&((AsbDO *)this)->ref);
}

static ULONG STDMETHODCALLTYPE DO_Release(IDataObject *this)
{
    AsbDO *d = (AsbDO *)this;
    ULONG r = (ULONG)InterlockedDecrement(&d->ref);
    if (r == 0) {
        int i;
        for (i = 0; i < d->small_count; i++)
            if (d->smalls[i].h) GlobalFree(d->smalls[i].h);
        if (d->meta) HeapFree(GetProcessHeap(), 0, d->meta);
        HeapFree(GetProcessHeap(), 0, d);
    }
    return r;
}

/* ---- Per-stream STA -----------------------------------------------------
   Explorer can start several FileContents streams at the same time.  If they
   all lived on the clipboard owner's single STA, a Read() waiting for the
   wire slot (a second paste while the first file is still streaming) would
   block every other stream in that apartment, freezing both progress bars.
   Each stream is therefore created on its own STA and the clipboard thread
   receives a marshalled proxy, so one blocked stream never stalls another. */
typedef struct {
    int index;
    UINT64 size;
    wchar_t rel_name[MAX_PATH];
    HANDLE ready;         /* worker -> clipboard thread: stream is marshalled */
    HANDLE unmarshalled;  /* clipboard thread -> worker: proxy has been made */
    IStream *marshal;     /* CoMarshalInterThreadInterfaceInStream result */
    HRESULT hr;
} FcsCreateParams;

static DWORD WINAPI fcs_thread_proc(LPVOID param)
{
    FcsCreateParams *p = (FcsCreateParams *)param;
    FileContentsStream *s;
    HANDLE ready = p->ready;
    HANDLE unmarshalled = p->unmarshalled;
    HRESULT hr = E_FAIL;
    MSG msg;

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
        p->hr = E_FAIL;
        SetEvent(ready);
        WaitForSingleObject(unmarshalled, INFINITE);
        CloseHandle(ready);
        CloseHandle(unmarshalled);
        HeapFree(GetProcessHeap(), 0, p);
        return 0;
    }

    s = (FileContentsStream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s) {
        p->hr = E_OUTOFMEMORY;
        SetEvent(ready);
        WaitForSingleObject(unmarshalled, INFINITE);
        CloseHandle(ready);
        CloseHandle(unmarshalled);
        HeapFree(GetProcessHeap(), 0, p);
        CoUninitialize();
        return 0;
    }

    s->lpVtbl = &FCS_Vtbl;
    s->ref = 1;
    s->index = p->index;
    s->declared_size = p->size;
    wcscpy_s(s->rel_name, MAX_PATH, p->rel_name);
    s->hFile = INVALID_HANDLE_VALUE;
    s->thread_id = GetCurrentThreadId();
    s->data_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    hr = CoMarshalInterThreadInterfaceInStream(&IID_IStream, (IUnknown *)s,
                                               &p->marshal);
    p->hr = hr;
    SetEvent(ready);

    /* Keep our own reference until the clipboard thread has unmarshalled the
       proxy.  The caller must not touch *p after SetEvent(unmarshalled); this
       thread owns and frees it from here on. */
    WaitForSingleObject(unmarshalled, INFINITE);
    s->lpVtbl->Release((IStream *)s);
    CloseHandle(ready);
    CloseHandle(unmarshalled);
    HeapFree(GetProcessHeap(), 0, p);

    if (SUCCEEDED(hr)) {
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return 0;
}

static HRESULT STDMETHODCALLTYPE DO_GetData(IDataObject *this, FORMATETC *pformatetc,
                                            STGMEDIUM *pmedium)
{
    AsbDO *d = (AsbDO *)this;
    int i;

    if (!pformatetc || !pmedium) return E_INVALIDARG;
    ZeroMemory(pmedium, sizeof(*pmedium));
    if (pformatetc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

    if (pformatetc->cfFormat == g_cf_file_desc && (pformatetc->tymed & TYMED_HGLOBAL)) {
        SIZE_T size = sizeof(FILEGROUPDESCRIPTORW) +
                      (d->meta_count > 0
                           ? (SIZE_T)(d->meta_count - 1) * sizeof(FILEDESCRIPTORW)
                           : 0);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        FILEGROUPDESCRIPTORW *fgd;

        if (!h) return E_OUTOFMEMORY;
        fgd = (FILEGROUPDESCRIPTORW *)GlobalLock(h);
        if (!fgd) { GlobalFree(h); return E_OUTOFMEMORY; }
        fgd->cItems = (UINT)d->meta_count;
        for (i = 0; i < d->meta_count; i++) {
            FILEDESCRIPTORW *fd = &fgd->fgd[i];
            fd->dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI |
                          (d->meta[i].is_dir ? 0 : FD_FILESIZE);
            fd->dwFileAttributes = d->meta[i].is_dir
                                       ? FILE_ATTRIBUTE_DIRECTORY
                                       : (d->meta[i].attrs & 0xFFFF);
            fd->nFileSizeLow  = (DWORD)(d->meta[i].size & 0xFFFFFFFF);
            fd->nFileSizeHigh = (DWORD)(d->meta[i].size >> 32);
            lstrcpynW(fd->cFileName, d->meta[i].name, ARRAYSIZE(fd->cFileName));
        }
        GlobalUnlock(h);
        pmedium->tymed = TYMED_HGLOBAL;
        pmedium->hGlobal = h;
        return S_OK;
    }

    if (pformatetc->cfFormat == g_cf_file_contents && (pformatetc->tymed & TYMED_ISTREAM)) {
        FileContentsStream *s;
        FcsCreateParams *p;
        HANDLE th;
        IStream *proxy = NULL;
        HRESULT hr2 = E_FAIL;

        i = pformatetc->lindex;
        if (i < 0 || i >= d->meta_count) return DV_E_LINDEX;
        clip_log("Shell asked for FileContents %d '%ls' (%llu bytes).",
                 i, d->meta[i].name, (unsigned long long)d->meta[i].size);

        p = (FcsCreateParams *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         sizeof(*p));
        if (!p) return E_OUTOFMEMORY;
        p->index = i;
        p->size = d->meta[i].size;
        wcscpy_s(p->rel_name, MAX_PATH, d->meta[i].name);
        p->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
        p->unmarshalled = CreateEventW(NULL, FALSE, FALSE, NULL);

        if (p->ready && p->unmarshalled) {
            th = CreateThread(NULL, 0, fcs_thread_proc, p, 0, NULL);
            if (th) {
                CloseHandle(th);
                if (WaitForSingleObject(p->ready, 5000) == WAIT_OBJECT_0 &&
                    SUCCEEDED(p->hr) && p->marshal) {
                    hr2 = CoGetInterfaceAndReleaseStream(p->marshal, &IID_IStream,
                                                         (void **)&proxy);
                    SetEvent(p->unmarshalled);   /* worker owns p now */
                    if (SUCCEEDED(hr2) && proxy) {
                        pmedium->tymed = TYMED_ISTREAM;
                        pmedium->pstm = proxy;
                        return S_OK;
                    }
                } else {
                    SetEvent(p->unmarshalled);   /* worker owns p now */
                }
                /* fall through to the direct-stream fallback */
            } else {
                CloseHandle(p->ready);
                CloseHandle(p->unmarshalled);
                HeapFree(GetProcessHeap(), 0, p);
            }
        } else {
            if (p->ready) CloseHandle(p->ready);
            if (p->unmarshalled) CloseHandle(p->unmarshalled);
            HeapFree(GetProcessHeap(), 0, p);
        }

        /* Fallback (thread or marshal failed): create the stream on this STA. */
        s = (FileContentsStream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                            sizeof(FileContentsStream));
        if (!s) return E_OUTOFMEMORY;
        s->lpVtbl = &FCS_Vtbl;
        s->ref = 1;
        s->index = i;
        s->declared_size = d->meta[i].size;
        wcscpy_s(s->rel_name, MAX_PATH, d->meta[i].name);
        s->hFile = INVALID_HANDLE_VALUE;
        pmedium->tymed = TYMED_ISTREAM;
        pmedium->pstm = (IStream *)s;
        return S_OK;
    }

    for (i = 0; i < d->small_count; i++) {
        if (d->smalls[i].fmt == pformatetc->cfFormat && (pformatetc->tymed & TYMED_HGLOBAL)) {
            SIZE_T sz = d->smalls[i].size;
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sz);
            void *src, *dst;

            if (!h) return E_OUTOFMEMORY;
            src = GlobalLock(d->smalls[i].h);
            dst = GlobalLock(h);
            if (src && dst) memcpy(dst, src, sz);
            if (src) GlobalUnlock(d->smalls[i].h);
            if (dst) GlobalUnlock(h);
            if (!src || !dst) { GlobalFree(h); return E_OUTOFMEMORY; }
            pmedium->tymed = TYMED_HGLOBAL;
            pmedium->hGlobal = h;
            return S_OK;
        }
    }

    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE DO_GetDataHere(IDataObject *this, FORMATETC *pformatetc,
                                                STGMEDIUM *pmedium)
{
    (void)this; (void)pformatetc; (void)pmedium;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE DO_QueryGetData(IDataObject *this, FORMATETC *pformatetc)
{
    AsbDO *d = (AsbDO *)this;
    int i;

    if (!pformatetc) return E_INVALIDARG;
    if (pformatetc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

    if (pformatetc->cfFormat == g_cf_file_desc && (pformatetc->tymed & TYMED_HGLOBAL))
        return S_OK;

    if (pformatetc->cfFormat == g_cf_file_contents && (pformatetc->tymed & TYMED_ISTREAM)) {
        if (pformatetc->lindex < 0 || pformatetc->lindex >= d->meta_count)
            return DV_E_LINDEX;
        return S_OK;
    }

    for (i = 0; i < d->small_count; i++) {
        if (pformatetc->cfFormat == d->smalls[i].fmt && (pformatetc->tymed & TYMED_HGLOBAL))
            return S_OK;
    }

    return DV_E_FORMATETC;
}

static HRESULT STDMETHODCALLTYPE DO_GetCanonicalFormatEtc(IDataObject *this,
                                                          FORMATETC *pformatectIn,
                                                          FORMATETC *pformatetcOut)
{
    (void)this;
    if (!pformatectIn || !pformatetcOut) return E_INVALIDARG;
    *pformatetcOut = *pformatectIn;
    pformatetcOut->ptd = NULL;
    return DATA_S_SAMEFORMATETC;
}

static HRESULT STDMETHODCALLTYPE DO_SetData(IDataObject *this, FORMATETC *pformatetc,
                                            STGMEDIUM *pmedium, BOOL fRelease)
{
    (void)this; (void)pformatetc; (void)pmedium; (void)fRelease;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE DO_EnumFormatEtc(IDataObject *this, DWORD dwDirection,
                                                  IEnumFORMATETC **ppenumFormatEtc)
{
    AsbDO *d = (AsbDO *)this;
    AsbEnum *e;
    FORMATETC *items;
    int total, i, n = 0;

    if (!ppenumFormatEtc) return E_INVALIDARG;
    *ppenumFormatEtc = NULL;
    if (dwDirection != DATADIR_GET) return E_NOTIMPL;

    total = 1 + d->meta_count + d->small_count;
    items = (FORMATETC *)HeapAlloc(GetProcessHeap(), 0, sizeof(FORMATETC) * total);
    if (!items) return E_OUTOFMEMORY;

    ZeroMemory(&items[n], sizeof(FORMATETC));
    items[n].cfFormat = (CLIPFORMAT)g_cf_file_desc;
    items[n].dwAspect = DVASPECT_CONTENT;
    items[n].lindex   = -1;
    items[n].tymed    = TYMED_HGLOBAL;
    n++;

    for (i = 0; i < d->meta_count; i++) {
        ZeroMemory(&items[n], sizeof(FORMATETC));
        items[n].cfFormat = (CLIPFORMAT)g_cf_file_contents;
        items[n].dwAspect = DVASPECT_CONTENT;
        items[n].lindex   = i;
        items[n].tymed    = TYMED_ISTREAM;
        n++;
    }

    for (i = 0; i < d->small_count; i++) {
        ZeroMemory(&items[n], sizeof(FORMATETC));
        items[n].cfFormat = (CLIPFORMAT)d->smalls[i].fmt;
        items[n].dwAspect = DVASPECT_CONTENT;
        items[n].lindex   = -1;
        items[n].tymed    = TYMED_HGLOBAL;
        n++;
    }

    e = (AsbEnum *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AsbEnum));
    if (!e) { HeapFree(GetProcessHeap(), 0, items); return E_OUTOFMEMORY; }
    e->lpVtbl = &EN_Vtbl;
    e->ref = 1;
    e->items = items;
    e->count = n;
    *ppenumFormatEtc = (IEnumFORMATETC *)e;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE DO_DAdvise(IDataObject *this, FORMATETC *pformatetc,
                                            DWORD advf, IAdviseSink *pAdvSink,
                                            DWORD *pdwConnection)
{
    (void)this; (void)pformatetc; (void)advf; (void)pAdvSink;
    if (pdwConnection) *pdwConnection = 0;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE DO_DUnadvise(IDataObject *this, DWORD dwConnection)
{
    (void)this; (void)dwConnection;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE DO_EnumDAdvise(IDataObject *this, IEnumSTATDATA **ppenumAdvise)
{
    (void)this;
    if (ppenumAdvise) *ppenumAdvise = NULL;
    return OLE_E_ADVISENOTSUPPORTED;
}

static IStreamVtbl FCS_Vtbl = {
    FCS_QueryInterface, FCS_AddRef, FCS_Release,
    FCS_Read, FCS_Write, FCS_Seek, FCS_SetSize, FCS_CopyTo, FCS_Commit, FCS_Revert,
    FCS_LockRegion, FCS_UnlockRegion, FCS_Stat, FCS_Clone
};

static IEnumFORMATETCVtbl EN_Vtbl = {
    EN_QueryInterface, EN_AddRef, EN_Release,
    EN_Next, EN_Skip, EN_Reset, EN_Clone
};

static IDataObjectVtbl DO_Vtbl = {
    DO_QueryInterface, DO_AddRef, DO_Release,
    DO_GetData, DO_GetDataHere, DO_QueryGetData, DO_GetCanonicalFormatEtc,
    DO_SetData, DO_EnumFormatEtc, DO_DAdvise, DO_DUnadvise, DO_EnumDAdvise
};

/* Build a data object from the metadata that came with the current list. */
static AsbDO *clip_build_data_object(void)
{
    AsbDO *d;
    int i;

    d = (AsbDO *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AsbDO));
    if (!d) return NULL;
    d->lpVtbl = &DO_Vtbl;
    d->ref = 1;

    d->meta = (FileMeta *)HeapAlloc(GetProcessHeap(), 0,
                                    sizeof(FileMeta) * (size_t)g_file_meta_count);
    if (!d->meta) { HeapFree(GetProcessHeap(), 0, d); return NULL; }
    memcpy(d->meta, g_file_meta, sizeof(FileMeta) * (size_t)g_file_meta_count);
    d->meta_count = g_file_meta_count;

    for (i = 0; i < g_clip_fmt_count && d->small_count < SMALL_MAX; i++) {
        if (g_clip_fmts[i].is_hdrop) continue;
        if (!g_clip_fmts[i].data || g_clip_fmts[i].data_size == 0) continue;
        {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, g_clip_fmts[i].data_size);
            void *p;
            if (!h) continue;
            p = GlobalLock(h);
            if (!p) { GlobalFree(h); continue; }
            memcpy(p, g_clip_fmts[i].data, g_clip_fmts[i].data_size);
            GlobalUnlock(h);
            d->smalls[d->small_count].fmt = g_clip_fmts[i].local_id;
            d->smalls[d->small_count].h = h;
            d->smalls[d->small_count].size = g_clip_fmts[i].data_size;
            d->small_count++;
        }
    }

    return d;
}

static void clip_apply_format_list(void)
{
    int i;
    BOOL has_nonfile = FALSE;
    LONG gen;

    if (g_clip_fmt_count == 0) return;

    /* The recv thread may be parsing a newer list; keep the shared format
       table stable while it is applied. */
    EnterCriticalSection(&g_state_cs);

    gen = g_list_gen;

    InterlockedExchange(&g_suppress, 1);

    /* Wipe the staging folder for this list before the file fetch starts. */
    clip_remove_dir_recursive(g_clip_temp_dir);
    CreateDirectoryW(g_clip_temp_dir, NULL);

#ifndef ASB_HARNESS
    /* Lazy path: advertise the files natively (FileGroupDescriptorW +
       FileContents) so nothing is downloaded until the user pastes.  The data
       object also carries the small formats, so the win32 path below is only
       used when there is no file metadata (old host) or OLE fails. */
    if (g_file_meta_count > 0) {
        AsbDO *d = clip_build_data_object();
        if (d) {
            HRESULT hr = OleSetClipboard((IDataObject *)d);
            d->lpVtbl->Release((IDataObject *)d);   /* OLE keeps its own ref */

            if (SUCCEEDED(hr)) {
                g_applied_gen = gen;
                g_applied_seq = (LONG)GetClipboardSequenceNumber();
                g_applied_has_nonfile = 1;
                clip_log("Advertised %d file(s) via FileContents (lazy).",
                         g_file_meta_count);
                {
                    int li;
                    for (li = 0; li < g_file_meta_count && li < 8; li++)
                        clip_log("  [%d] '%ls' %llu bytes%s",
                                 li, g_file_meta[li].name,
                                 (unsigned long long)g_file_meta[li].size,
                                 g_file_meta[li].is_dir ? " (dir)" : "");
                    if (g_file_meta_count > 8)
                        clip_log("  ... and %d more.", g_file_meta_count - 8);
                }
                clip_free_pending();
                LeaveCriticalSection(&g_state_cs);
                return;
            }
            clip_log("OleSetClipboard failed (0x%08lX); falling back to eager files.",
                     (unsigned long)hr);
        } else {
            clip_log("Failed to build file data object; falling back to eager files.");
        }
    }
#endif

    /* Eager path: apply the small (non-file) formats now; the file list is
       added by clip_add_files_to_clipboard once it has downloaded. */
    for (i = 0; i < g_clip_fmt_count; i++) {
        if (g_clip_fmts[i].is_hdrop) continue;
        if (g_clip_fmts[i].data && g_clip_fmts[i].data_size > 0) {
            if (!has_nonfile) {
                if (!OpenClipboard(g_msg_hwnd)) {
                    clip_log("clip_apply_format_list: OpenClipboard failed (%lu).", GetLastError());
                    break;
                }
                EmptyClipboard();
            }
            {
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, g_clip_fmts[i].data_size);
                if (hMem) {
                    void *ptr = GlobalLock(hMem);
                    if (ptr) {
                        memcpy(ptr, g_clip_fmts[i].data, g_clip_fmts[i].data_size);
                        GlobalUnlock(hMem);
                        SetClipboardData(g_clip_fmts[i].local_id, hMem);
                    } else {
                        GlobalFree(hMem);
                    }
                }
            }
            has_nonfile = TRUE;
        }
    }
    if (has_nonfile) CloseClipboard();

    g_applied_gen = gen;
    g_applied_seq = (LONG)GetClipboardSequenceNumber();
    g_applied_has_nonfile = has_nonfile ? 1 : 0;

    clip_log("Applied %d formats to clipboard (files pending download).",
             g_clip_fmt_count);
    clip_free_pending();
    LeaveCriticalSection(&g_state_cs);

    /* Eager fallback (old host, or OLE failed): prefetch CF_HDROP in the
       background. No clipboard is held open while the files download. */
    for (i = 0; i < g_clip_fmt_count; i++) {
        if (g_clip_fmts[i].is_hdrop) {
            ClipHeader hdr;
            BOOL sent;
            g_fetch_gen = gen;
            g_file_fetch_active = 1;
            hdr.magic = CLIP_MAGIC;
            hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
            hdr.format = g_clip_fmts[i].remote_id;
            hdr.data_size = 0;
            EnterCriticalSection(&g_send_cs);
            sent = send_all_locked(&hdr, sizeof(hdr));
            LeaveCriticalSection(&g_send_cs);
            if (sent)
                clip_log("Fetching files in background (deferred download started).");
            else
                clip_log("File fetch failed: host socket is closed.");
            break;
        }
    }
}

/* Files for the current list have finished downloading: hand them to the
   clipboard now that they are complete, so a paste never blocks. */
static void clip_add_files_to_clipboard(void)
{
    HGLOBAL hDrop;
    LONG gen;

    EnterCriticalSection(&g_state_cs);

    gen = g_fetch_gen;
    if (gen != g_list_gen) {
        clip_log("File fetch completed for a superseded list - ignoring.");
        LeaveCriticalSection(&g_state_cs);
        return;
    }
    g_file_fetch_active = 0;

    if ((LONG)GetClipboardSequenceNumber() != g_applied_seq) {
        clip_log("Clipboard changed while files were downloading - not overwriting.");
        LeaveCriticalSection(&g_state_cs);
        return;
    }

    hDrop = clip_build_hdrop_from_temp();
    if (hDrop) {
        if (OpenClipboard(g_msg_hwnd)) {
            if (!g_applied_has_nonfile) EmptyClipboard();
            SetClipboardData(CF_HDROP, hDrop);
            CloseClipboard();
            g_applied_seq = (LONG)GetClipboardSequenceNumber();
            clip_log("Files ready in clipboard (CF_HDROP added).");
        } else {
            clip_log("clip_add_files: OpenClipboard failed (%lu).", GetLastError());
            GlobalFree(hDrop);
        }
    } else {
        clip_log("File fetch completed but produced no files.");
    }

    LeaveCriticalSection(&g_state_cs);
}

/* ---- Message window for clipboard monitoring ---- */

static LRESULT CALLBACK clip_msg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLIP_APPLY_FMTS:
        clip_log("WM_CLIP_APPLY_FMTS — applying fetched data.");
        clip_apply_format_list();
        return 0;
    case WM_CLIP_ADD_FILES:
        clip_log("WM_CLIP_ADD_FILES — adding downloaded files.");
        clip_add_files_to_clipboard();
        return 0;
    case WM_RENDERFORMAT:
        /* Kept for compatibility; files are no longer delayed. */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void switch_to_input_desktop(void)
{
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (desk) {
        SetThreadDesktop(desk);
        CloseDesktop(desk);
    }
}

static DWORD WINAPI msg_window_thread(LPVOID param)
{
    WNDCLASSEXW wc;
    MSG msg;
    (void)param;

    /* Must be on the interactive desktop to access the user's clipboard */
    switch_to_input_desktop();
    OleInitialize(NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = clip_msg_wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AppSandboxClipHelper";
    RegisterClassExW(&wc);

    g_msg_hwnd = CreateWindowExW(0, L"AppSandboxClipHelper", NULL,
                                  0, 0, 0, 0, 0, HWND_MESSAGE,
                                  NULL, GetModuleHandleW(NULL), NULL);
    if (!g_msg_hwnd) {
        clip_log("Failed to create message window (%lu).", GetLastError());
        return 1;
    }

    clip_log("Message window created (delayed-render clipboard owner).");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(g_msg_hwnd);
    g_msg_hwnd = NULL;
    return 0;
}

/* Receive and write a single FILE_DATA message to disk. */
static BOOL clip_recv_file_data(AsbConn *s, const ClipHeader *hdr)
{
    ClipFileInfo fi;
    wchar_t rel_path[MAX_PATH], full_path[MAX_PATH];

    if (hdr->data_size < sizeof(fi)) return FALSE;
    if (!recv_exact(s, &fi, sizeof(fi))) return FALSE;

    if (fi.path_len == 0 || fi.path_len > (MAX_PATH - 1) * sizeof(wchar_t)) {
        UINT32 skip_hdr = hdr->data_size - (UINT32)sizeof(fi);
        BYTE skip[4096];
        while (skip_hdr > 0) {
            int chunk = skip_hdr > 4096 ? 4096 : (int)skip_hdr;
            if (!recv_exact(s, skip, chunk)) return FALSE;
            skip_hdr -= chunk;
        }
        { UINT64 rem = fi.file_size;
          while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!recv_exact(s, skip, c)) return FALSE; rem -= c; } }
        return TRUE;
    }

    if (!recv_exact(s, rel_path, (int)fi.path_len)) return FALSE;
    rel_path[fi.path_len / sizeof(wchar_t)] = L'\0';

    if (!clip_path_is_safe(rel_path)) {
        BYTE skip[4096];
        UINT64 rem = fi.file_size;
        clip_log("Rejected unsafe path.");
        while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!recv_exact(s, skip, c)) return FALSE; rem -= c; }
        return TRUE;
    }

    swprintf_s(full_path, MAX_PATH, L"%s\\%s", g_clip_temp_dir, rel_path);

    if (fi.is_directory) {
        clip_ensure_parent_dir(full_path);
        CreateDirectoryW(full_path, NULL);
    } else {
        HANDLE hFile;
        BYTE *chunk;
        clip_ensure_parent_dir(full_path);
        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
        if (!chunk) return FALSE;
        hFile = CreateFileW(full_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            UINT64 remaining = fi.file_size;
            while (remaining > 0) {
                DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
                DWORD written;
                if (!recv_exact(s, chunk, (int)to_read)) { HeapFree(GetProcessHeap(), 0, chunk); CloseHandle(hFile); return FALSE; }
                WriteFile(hFile, chunk, to_read, &written, NULL);
                remaining -= to_read;
            }
            CloseHandle(hFile);
        } else {
            UINT64 remaining = fi.file_size;
            while (remaining > 0) {
                int c = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (int)remaining;
                if (!recv_exact(s, chunk, c)) { HeapFree(GetProcessHeap(), 0, chunk); return FALSE; }
                remaining -= c;
            }
        }
        HeapFree(GetProcessHeap(), 0, chunk);
    }
    return TRUE;
}

/* Handle one message from the host.  Called from the recv loop, and also from
   the on-demand file wait below: a FORMAT_LIST pushed by the host while we are
   waiting for a FILE_DATA reply can appear in between and must not be parsed
   as the reply.  Returns FALSE when the connection must be torn down. */
static BOOL clip_handle_host_message(AsbConn *s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {

    case CLIP_MSG_SYNC_ENABLE: {
        UINT8 flag = 0;
        if (hdr->data_size >= 1) {
            if (!recv_exact(s, &flag, 1)) return FALSE;
            if (hdr->data_size > 1) {
                BYTE skip[256];
                UINT32 rem = hdr->data_size - 1;
                while (rem > 0) {
                    int c = rem > 256 ? 256 : (int)rem;
                    if (!recv_exact(s, skip, c)) return FALSE;
                    rem -= c;
                }
            }
        }
        InterlockedExchange(&g_sync_enabled, flag ? 1 : 0);
        if (flag && g_sync_event) SetEvent(g_sync_event);
        else if (!flag && g_sync_event) ResetEvent(g_sync_event);
        clip_log("SYNC_ENABLE %s", flag ? "ON" : "OFF");
        return TRUE;
    }

    case CLIP_MSG_FORMAT_LIST: {
        BYTE buf[16384];
        UINT32 count, i;
        int off;

        if (hdr->data_size > sizeof(buf)) return FALSE;
        if (hdr->data_size < 4) return FALSE;
        if (!recv_exact(s, buf, (int)hdr->data_size)) return FALSE;

        if (!g_sync_enabled) {
            clip_log("FORMAT_LIST ignored (sync disabled).");
            return TRUE;
        }

        count = *(UINT32 *)buf;
        if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
        off = 4;

        /* A new list supersedes any in-flight file fetch. */
        g_file_fetch_active = 0;

        /* Serialize with apply/add-files on the window thread. */
        EnterCriticalSection(&g_state_cs);
        clip_free_pending();
        g_clip_fmt_count = 0;
        for (i = 0; i < count && off + 8 <= (int)hdr->data_size; i++) {
            UINT32 fmt_id   = *(UINT32 *)(buf + off); off += 4;
            UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
            UINT local_id = fmt_id;

            /* off is in [8, data_size] here; reject a name_len that runs
               past the buffer (or whose signed cast would corrupt off)
               before the unconditional `off += name_len` advance below. */
            if (name_len > (UINT32)((int)hdr->data_size - off)) break;

            if (name_len > 0 && name_len < 256 && off + (int)name_len <= (int)hdr->data_size) {
                char name[256];
                memcpy(name, buf + off, name_len);
                name[name_len] = '\0';
                local_id = RegisterClipboardFormatA(name);
                clip_log("Format '%s' remote=%u local=%u.", name, fmt_id, local_id);
            }
            off += (int)name_len;

            g_clip_fmts[g_clip_fmt_count].local_id = local_id;
            g_clip_fmts[g_clip_fmt_count].remote_id = fmt_id;
            g_clip_fmts[g_clip_fmt_count].data = NULL;
            g_clip_fmts[g_clip_fmt_count].data_size = 0;
            /* CF_HDROP is the file list: fetched in the background after apply. */
            g_clip_fmts[g_clip_fmt_count].is_hdrop =
                (local_id == CF_HDROP) ? TRUE : FALSE;
            g_clip_fmt_count++;
        }
        /* A new list always starts with no file metadata: the host sends
           it as a separate FILE_LIST right after this message. */
        g_file_meta_count = 0;
        g_list_has_hdrop = 0;
        for (i = 0; i < (UINT32)g_clip_fmt_count; i++) {
            if (g_clip_fmts[i].is_hdrop) { g_list_has_hdrop = 1; break; }
        }
        /* Old hosts never send FILE_LIST; do not wait for them forever. */
        g_meta_deadline = GetTickCount64() + 1000;

        g_list_gen++;
        g_clip_fetch_idx = 0;
        LeaveCriticalSection(&g_state_cs);

        clip_log("Received format list (%u formats); fetching non-file formats...",
                 g_clip_fmt_count);

        /* Temp files are cleaned when the list is applied (or when a
           render starts), never here: a lazy list must not delete the
           files a pending paste might still need. */

        /* Fetch the small formats; CF_HDROP entries are skipped. */
        clip_request_next_format();
        return TRUE;
    }

    case CLIP_MSG_FORMAT_DATA_RESP: {
        int idx = g_clip_fetch_idx;

        /* Completion of the background file fetch: the host streams
           FILE_DATA messages and terminates with an empty FORMAT_DATA_RESP
           for CF_HDROP. Wake the window thread to add the files. */
        if (hdr->format == CF_HDROP && g_file_fetch_active) {
            clip_log("Data response for background file fetch (format %u, %u bytes).",
                     hdr->format, hdr->data_size);
            if (hdr->data_size > 0) {
                BYTE skip[4096];
                UINT32 rem = hdr->data_size;
                while (rem > 0) {
                    int c = rem > 4096 ? 4096 : (int)rem;
                    if (!recv_exact(s, skip, c)) return FALSE;
                    rem -= c;
                }
            }
            if (g_msg_hwnd)
                PostMessageW(g_msg_hwnd, WM_CLIP_ADD_FILES, 0, 0);
            clip_log("File fetch complete.");
            return TRUE;
        }

        clip_log("Data response for format %u (%u bytes).", hdr->format, hdr->data_size);

        if (idx >= 0 && idx < g_clip_fmt_count) {
            if (hdr->data_size > 0 && hdr->data_size <= CLIP_MAX_PAYLOAD) {
                BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
                if (data && recv_exact(s, data, (int)hdr->data_size)) {
                    g_clip_fmts[idx].data = data;
                    g_clip_fmts[idx].data_size = hdr->data_size;
                } else {
                    if (data) HeapFree(GetProcessHeap(), 0, data);
                }
            }
            g_clip_fetch_idx++;
            clip_request_next_format();
        }
        return TRUE;
    }

    case CLIP_MSG_FILE_DATA:
        CreateDirectoryW(g_clip_temp_dir, NULL);
        return clip_recv_file_data(s, hdr);

    case CLIP_MSG_FILE_LIST: {
        /* File metadata for FileContents. Parse into g_file_meta. */
        BYTE *buf = NULL;
        UINT32 count, i, off = 0;
        if (hdr->data_size < 4) return TRUE;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
        if (!buf) return TRUE;
        if (!recv_exact(s, buf, (int)hdr->data_size)) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }
        count = *(UINT32 *)buf; off = 4;
        if (count > META_MAX) count = META_MAX;
        EnterCriticalSection(&g_state_cs);
        g_file_meta_count = 0;
        for (i = 0; i < count && off + 4 <= (int)hdr->data_size; i++) {
            UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
            if (name_len > (MAX_PATH - 1) * sizeof(wchar_t) || off + (int)name_len + 8 + 4 + 1 > (int)hdr->data_size) break;
            if (name_len > 0) {
                memcpy(g_file_meta[g_file_meta_count].name, buf + off, name_len);
                g_file_meta[g_file_meta_count].name[name_len / sizeof(wchar_t)] = L'\0';
            } else {
                g_file_meta[g_file_meta_count].name[0] = L'\0';
            }
            off += name_len;
            g_file_meta[g_file_meta_count].size = *(UINT64 *)(buf + off); off += 8;
            g_file_meta[g_file_meta_count].attrs = *(UINT32 *)(buf + off); off += 4;
            g_file_meta[g_file_meta_count].is_dir = buf[off++] ? 1 : 0;
            g_file_meta_count++;
        }
        LeaveCriticalSection(&g_state_cs);
        HeapFree(GetProcessHeap(), 0, buf);
        clip_log("Received file metadata (%d entries).", g_file_meta_count);
        return TRUE;
    }

    case CLIP_MSG_FILE_DATA_REQ: {
        /* Guest sends FILE_DATA_REQ; nothing to do when receiving one. */
        if (hdr->data_size > 0) {
            BYTE skip[256]; UINT32 rem = hdr->data_size;
            while (rem > 0) { int c = rem > 256 ? 256 : (int)rem; if (!recv_exact(s, skip, c)) return FALSE; rem -= c; }
        }
        return TRUE;
    }

    default:
        clip_log("Unknown msg_type %u.", hdr->msg_type);
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

/* ---- Recv thread: delayed rendering protocol ---- */

static DWORD WINAPI recv_thread_proc(LPVOID param)
{
    AsbConn *s = (AsbConn *)param;
    ClipHeader hdr;

    clip_log("Recv thread started.");

    while (1) {
        /* If the apply is waiting only for file metadata, release it as soon
           as the metadata arrives (or the old-host timeout expires). */
        if (g_apply_pending &&
            (g_file_meta_count > 0 || GetTickCount64() >= g_meta_deadline)) {
            InterlockedExchange(&g_apply_pending, 0);
            if (g_msg_hwnd)
                PostMessageW(g_msg_hwnd, WM_CLIP_APPLY_FMTS, 0, 0);
        }

        /* Serve a pending on-demand file request (FileContents paste).
           The bytes are written into the staging file the requesting IStream
           created; that stream drains it, so the shell's progress dialog
           advances during the transfer. */
        if (InterlockedCompareExchange(&g_file_req_pending, 1, 1)) {
            ClipHeader rhdr;
            ClipHeader r;
            ClipFileInfo fi;
            UINT32 idx;
            wchar_t expect_name[MAX_PATH];
            UINT64 expect_size;
            BOOL ok = TRUE;
            HANDLE hf = INVALID_HANDLE_VALUE;
            BOOL discarding = FALSE;

            ZeroMemory(&r, sizeof(r));
            ZeroMemory(&fi, sizeof(fi));

            /* Take this request out of the shared slot.  Clear pending while
               holding the lock: a stream can only claim the (still busy) slot
               after we free it below, so it cannot be erased by this clear. */
            EnterCriticalSection(&g_xfer_cs);
            idx         = (UINT32)g_file_req_idx;
            expect_size = g_xfer_total;
            wcscpy_s(expect_name, MAX_PATH, g_req_name);
            /* Own handle to the staging file, duplicated by the stream.  Do not
               reopen g_xfer_path: an early Release deletes it while our reply
               is already in flight. */
            hf = g_xfer_hFile;
            g_xfer_hFile = NULL;
            InterlockedExchange(&g_file_req_pending, 0);
            LeaveCriticalSection(&g_xfer_cs);

            rhdr.magic = CLIP_MAGIC;
            rhdr.msg_type = CLIP_MSG_FILE_DATA_REQ;
            rhdr.format = 0;
            rhdr.data_size = 4;
            EnterCriticalSection(&g_send_cs);
            if (!send_all_locked(&rhdr, sizeof(rhdr)) ||
                !send_all_locked(&idx, 4))
                ok = FALSE;
            LeaveCriticalSection(&g_send_cs);

            if (ok)
                clip_log("Lazy request %u: serving '%ls' (%llu bytes).",
                         idx, expect_name, (unsigned long long)expect_size);

            /* Wait for our reply, folding any genuine host push (FORMAT_LIST /
               FILE_LIST) that was sent while the request was outstanding.  A
               new host tags the reply with idx+1 so an eager file stream is
               never mistaken for the pasted file. */
            if (ok) {
                for (;;) {
                    if (!recv_exact(s, &r, sizeof(r)) || r.magic != CLIP_MAGIC) {
                        ok = FALSE;
                        break;
                    }
                    if (r.msg_type == CLIP_MSG_FILE_DATA && r.format == idx + 1)
                        break;
                    if (!clip_handle_host_message(s, &r)) {
                        ok = FALSE;
                        break;
                    }
                }
            }

            if (ok &&
                (r.data_size < sizeof(fi) || !recv_exact(s, &fi, sizeof(fi))))
                ok = FALSE;

            if (ok) {
                wchar_t rel[MAX_PATH];
                BOOL named = FALSE;

                rel[0] = L'\0';
                if (fi.path_len > 0 &&
                    fi.path_len <= (MAX_PATH - 1) * sizeof(wchar_t)) {
                    if (recv_exact(s, rel, (int)fi.path_len)) {
                        rel[fi.path_len / sizeof(wchar_t)] = L'\0';
                        named = TRUE;
                    } else {
                        ok = FALSE;
                    }
                } else if (fi.path_len > 0) {
                    ok = FALSE;   /* bogus length: never drain a huge body */
                }

                if (ok && (!named || wcscmp(rel, expect_name) != 0 ||
                           (!fi.is_directory && fi.file_size != expect_size))) {
                    clip_log("File reply does not match request %u (want '%ls', "
                             "%llu bytes; got '%ls', %llu%s); aborting.",
                             idx, expect_name,
                             (unsigned long long)expect_size,
                             named ? rel : L"", (unsigned long long)fi.file_size,
                             fi.is_directory ? L", directory" : L"");
                    ok = FALSE;
                }
            }

            if (ok) {
                if (hf == NULL || hf == INVALID_HANDLE_VALUE) {
                    clip_log("Request %u has no staging handle "
                             "(stream released); failing request.", idx);
                    ok = FALSE;
                } else {
                    BYTE *chunk = NULL;
                    UINT64 remaining = fi.is_directory ? 0 : fi.file_size;

                    /* Released before the first byte arrived: discard the whole
                       body instead of writing a file nobody will ever read. */
                    if (InterlockedCompareExchange(&g_xfer_cancel, 1, 1)) {
                        discarding = TRUE;
                        clip_log("Lazy request %u abandoned before the body; "
                                 "discarding %llu bytes.",
                                 idx, (unsigned long long)remaining);
                        CloseHandle(hf);
                        hf = INVALID_HANDLE_VALUE;
                    }

                    if (hf != NULL && hf != INVALID_HANDLE_VALUE)
                        SetFilePointer(hf, 0, NULL, FILE_BEGIN);
                    if (remaining > 0)
                        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
                    while (ok && remaining > 0) {
                        DWORD to_read = remaining > CLIP_FILE_CHUNK
                                            ? CLIP_FILE_CHUNK
                                            : (DWORD)remaining;
                        DWORD written = 0;

                        if (!chunk || !recv_exact(s, chunk, (int)to_read)) {
                            clip_log("Lazy request %u: receive failed after %llu "
                                     "of %llu bytes.",
                                     idx,
                                     (unsigned long long)(fi.file_size - remaining),
                                     (unsigned long long)fi.file_size);
                            ok = FALSE;
                            break;
                        }

                        /* The paste was abandoned: keep draining the body so the
                           wire stays in sync, but stop writing and keep the
                           connection up for the other pastes. */
                        if (!discarding &&
                            InterlockedCompareExchange(&g_xfer_cancel, 1, 1)) {
                            discarding = TRUE;
                            clip_log("Lazy request %u abandoned by the paste; "
                                     "discarding the remaining %llu bytes.",
                                     idx, (unsigned long long)remaining);
                            if (hf != NULL && hf != INVALID_HANDLE_VALUE) {
                                CloseHandle(hf);
                                hf = INVALID_HANDLE_VALUE;
                            }
                        }

                        if (!discarding &&
                            (!WriteFile(hf, chunk, to_read, &written, NULL) ||
                             written != to_read)) {
                            clip_log("Lazy request %u: staging write failed after "
                                     "%llu of %llu bytes (error %lu).",
                                     idx,
                                     (unsigned long long)(fi.file_size - remaining),
                                     (unsigned long long)fi.file_size,
                                     GetLastError());
                            ok = FALSE;
                            break;
                        }
                        remaining -= to_read;
                        if (!discarding) fcs_signal_data();
                    }
                    if (chunk) HeapFree(GetProcessHeap(), 0, chunk);
                }
            }

            if (hf != NULL && hf != INVALID_HANDLE_VALUE) {
                CloseHandle(hf);
                hf = INVALID_HANDLE_VALUE;
            }

            /* A broken connection (socket error, validation failure) must tear
               down this session so the host stops sending the old body and the
               next paste starts immediately. */
            if (!ok) goto done;

            /* Publish completion, then free the slot.  Clearing pending while
               still holding the lock means a stream that claims the slot on
               the wake-up cannot have its fresh request erased. */
            clip_log("Lazy request %u %s (%llu bytes).", idx,
                     discarding ? "drained after cancel" : "complete",
                     (unsigned long long)expect_size);
            InterlockedExchange(&g_xfer_cancel, 0);
            fcs_finish_transfer(0);
            continue;
        }

        /* Poll instead of blocking in recv(): a paste can ask for a file at any
           moment, and only this thread can serve it. */
        {
            int pr = asb_poll(s, 50);
            if (pr < 0) break;
            if (pr == 0) continue;
        }

        if (!recv_exact(s, &hdr, sizeof(hdr)))
            break;

        if (hdr.magic != CLIP_MAGIC) {
            clip_log("Bad magic 0x%08X, disconnecting.", hdr.magic);
            break;
        }

        if (!clip_handle_host_message(s, &hdr))
            break;
    }

done:
    /* Detach the socket before waking a waiting paste: a render that starts
       after this point sends nothing (g_client_sock is NULL) instead of
       blocking for the full timeout on a socket nobody reads. */
    EnterCriticalSection(&g_send_cs);
    g_client_sock = NULL;
    LeaveCriticalSection(&g_send_cs);

    /* Drop any in-flight file fetch if the host goes away. */
    g_file_fetch_active = 0;
    InterlockedExchange(&g_apply_pending, 0);

    /* Unblock a paste that is waiting for bytes we can no longer deliver.
       Mark the channel dead first so streams queued behind the slot fail
       immediately instead of waiting for a request nobody will serve. */
    {
        int had_active;

        EnterCriticalSection(&g_xfer_cs);
        had_active = g_xfer_active;
        LeaveCriticalSection(&g_xfer_cs);

        InterlockedExchange(&g_xfer_dead, 1);
        fcs_finish_transfer(1);
        clip_log("Connection lost (%s); cancelling in-flight transfer.",
                 had_active ? "transfer active" : "no transfer active");
    }
    EnterCriticalSection(&g_xfer_cs);
    if (g_xfer_hFile && g_xfer_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_xfer_hFile);
        g_xfer_hFile = NULL;
    }
    LeaveCriticalSection(&g_xfer_cs);

    clip_log("Recv thread exiting.");
    return 0;
}

/* ---- Handle one client connection ---- */

static void handle_client(AsbConn *s)
{
    UINT32 ready = CLIP_READY_MAGIC;
    HANDLE recv_th;

    /* Send ready signal */
    if (asb_send(s, &ready, sizeof(ready)) != (int)sizeof(ready)) {
        clip_log("Failed to send ready signal.");
        return;
    }
    clip_log("Sent ready signal to host.");

    g_client_sock = s;
    InterlockedExchange(&g_xfer_dead, 0);

    /* Start recv thread */
    recv_th = CreateThread(NULL, 0, recv_thread_proc, (LPVOID)s, 0, NULL);
    if (!recv_th) {
        clip_log("Failed to create recv thread (%lu).", GetLastError());
        g_client_sock = NULL;
        return;
    }

    /* Wait for recv thread to finish (host disconnected) */
    WaitForSingleObject(recv_th, INFINITE);
    CloseHandle(recv_th);

    g_client_sock = NULL;

    /* Serialize with apply/add-files on the window thread before tearing
       down. */
    EnterCriticalSection(&g_state_cs);
    clip_free_pending();
    g_clip_fmt_count = 0;
    g_file_fetch_active = 0;
    g_file_meta_count = 0;
    g_list_has_hdrop = 0;
    clip_remove_dir_recursive(g_clip_temp_dir);
    LeaveCriticalSection(&g_state_cs);
    clip_log("Client disconnected.");
}

/* ---- Main ---- */

int main(void)
{
    HANDLE msg_th;
    AsbListener *l;

    clip_log("Starting (PID=%lu, session=%lu). Log: %s",
             GetCurrentProcessId(),
             WTSGetActiveConsoleSessionId(),
             clip_log_path());

    InitializeCriticalSection(&g_send_cs);
    InitializeCriticalSection(&g_state_cs);
    InitializeCriticalSection(&g_xfer_cs);
    g_slot_event = CreateEventW(NULL, TRUE, TRUE, NULL);
    g_clip_fetch_idx = 0;
    g_list_gen = 0;
    g_file_fetch_active = 0;

    clip_init_temp_dir();

    /* Stale staging files from a previous run: nothing can still be reading
       them, and they are never touched by the per-list temp cleanup. */
    clip_remove_dir_recursive(g_lazy_dir);
    CreateDirectoryW(g_lazy_dir, NULL);

    g_cf_file_desc = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    g_cf_file_contents = RegisterClipboardFormatW(L"FileContents");

    g_sync_event = CreateEventW(NULL, TRUE, FALSE, CLIP_SYNC_EVENT_NAME);
    if (!g_sync_event)
        clip_log("Warning: failed to create sync event (%lu).", GetLastError());

    /* Start message window thread (clipboard listener) */
    msg_th = CreateThread(NULL, 0, msg_window_thread, NULL, 0, NULL);
    if (!msg_th) {
        clip_log("Failed to create message window thread (%lu).", GetLastError());
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }

    /* Wait a moment for message window to be created */
    Sleep(100);

    if (asb_transport_init() != 0) {
        clip_log("asb_transport_init failed.");
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }

    l = asb_listen(ASB_CH_CLIPBOARD);
    if (!l) {
        clip_log("asb_listen(ASB_CH_CLIPBOARD) failed.");
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }
    clip_log("Listening on clipboard channel (transport=%s).",
             asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    /* Accept loop — one client at a time */
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        clip_log("Host connected.");
        handle_client(c);
        asb_close(c);
    }
}
