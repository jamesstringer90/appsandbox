/*
 * vm_clipboard.c -- Per-VM clipboard synchronization over Hyper-V sockets.
 *
 * Extracted from vm_display_idd.c. Manages two channels:
 *   :0005  Host->guest (writer) — sends FORMAT_LIST, serves DATA_REQ
 *   :0006  Guest->host (reader) — receives FORMAT_LIST, fetches data
 *
 * Focus-gated: sync is disabled by default. The display window toggles
 * it on focus/blur. When disabled, no clipboard traffic flows in either
 * direction, which prevents background VMs from corrupting the host
 * clipboard through format round-trip garbling.
 */

#include "vm_clipboard.h"
#include "hcs_vm.h"
#include <shlobj.h>
#include <ole2.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

/* ---- HV socket address ---- */

#ifndef AF_HYPERV
#define AF_HYPERV       34
#endif
#ifndef HV_PROTOCOL_RAW
#define HV_PROTOCOL_RAW 1
#endif

#pragma pack(push, 1)
typedef struct {
    unsigned short Family;
    unsigned short Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;
#pragma pack(pop)

/* Both superseded by hcs_service_guid(os_type, 5|6, ...) — kept for grep.
   Windows VMs reach byte-identical GUIDs via the helper. */
static const GUID CLIPBOARD_SERVICE_GUID =
    { 0xa5b0cafe, 0x0005, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

static const GUID CLIPBOARD_READER_SERVICE_GUID =
    { 0xa5b0cafe, 0x0006, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Wire protocol ---- */

#define CLIP_MAGIC          0x504C4341u
#define CLIP_READY_MAGIC    0x59444C43u

#define CLIP_MSG_FORMAT_LIST      1
#define CLIP_MSG_FORMAT_DATA_REQ  2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA        4
#define CLIP_MSG_FILE_LIST        5   /* host -> guest: file metadata for FileContents */
#define CLIP_MSG_FILE_DATA_REQ    6   /* guest -> host: request one file's bytes */
#define CLIP_MSG_PULL_LIST        7   /* host -> guest: re-send the current format list */
/* CLIP_MSG_SYNC_ENABLE = 12 (defined in header) */

/* Appended to FORMAT_LIST by a guest that will follow it with CLIP_MSG_FILE_LIST
   (guest->host lazy files).  Old hosts ignore the trailing bytes; new hosts use
   it to know they must wait for metadata instead of fetching files eagerly. */
#define CLIP_LAZY_MARKER          0x5A594C46u   /* "FLZY" */

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)
#define CLIP_FILE_CHUNK     (1024 * 1024)
#define CLIP_MAX_META       4096
/* How long to wait for FILE_LIST after a guest promised it.  The guest walks
   the copied tree (disk I/O) before it can send the metadata, so this is
   generous; the socket closing is the real failure signal. */
#define CLIP_META_WAIT_MS   30000
/* Watchdog budgets for ONE pasted stream.  A stream can spend minutes queued
   behind earlier pastes (the wire carries one file at a time, and an abandoned
   paste is drained at wire speed), so these clocks only start when the stream
   actually owns the wire slot.  Before the first byte arrives a reconnect may
   still be in progress; once bytes flow, a minute without a single 1 MiB chunk
   means the guest is gone. */
#define CLIP_START_WAIT_MS  120000
#define CLIP_STALL_MS       60000
/* How long the reader thread waits for the FILE_DATA reply header after sending
   a request.  Receiving it is a local guest operation; two minutes means the
   guest (or the link) is wedged and the connection should be rebuilt. */
#define CLIP_REPLY_WAIT_MS  120000
/* How long the "accept the next guest list even while unfocused" token stays
   valid after the host asked the guest for a fresh list.  A new guest answers
   at once; an old one ignores message type 7, and the token must not outlive
   that reply or the focus gate would stay bypassed for the whole connection. */
#define CLIP_PULL_WAIT_MS   10000
#define SMALL_MAX           64

#define WM_CLIP_READER_APPLY (WM_APP + 11)

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 msg_type;
    UINT32 format;
    UINT32 data_size;
} ClipHeader;

typedef struct {
    UINT32 path_len;
    UINT64 file_size;
    UINT8  is_directory;
} ClipFileInfo;
#pragma pack(pop)

typedef struct {
    UINT   local_id;
    UINT   remote_id;
    BYTE  *data;
    UINT32 data_size;
    BOOL   is_hdrop;
} ClipFmtEntry;

/* File metadata advertised to the host by the guest (FILE_LIST).  Same wire
   shape as the host->guest metadata: relative name, size, attributes, dir. */
typedef struct {
    wchar_t name[MAX_PATH];
    UINT64  size;
    DWORD   attrs;
    int     is_dir;
} ClipMeta;

/* One entry of the host->guest file list that was actually advertised with the
   last FORMAT_LIST/FILE_LIST.  The same snapshot answers every later
   FILE_DATA_REQ, so the index the guest picked from its FileGroupDescriptorW
   always maps to the exact file that was described, even if the source tree
   changes between copy and paste. */
typedef struct {
    wchar_t full[MAX_PATH];
    wchar_t rel[MAX_PATH];
    UINT64  size;
    DWORD   attrs;
    int     is_dir;
} ClipFileSnap;

/* ---- Guest->host lazy FileContents (OLE data object) ---- */

typedef struct {
    UINT    fmt;
    HGLOBAL h;
    SIZE_T  size;
} ClipSmall;

typedef struct ClipDataObject ClipDataObject;

typedef struct {
    IStreamVtbl *lpVtbl;
    LONG         ref;
    int          index;
    int          id;               /* unique per IStream, for log correlation */
    UINT64       declared_size;
    UINT64       pos;
    HANDLE       hFile;
    wchar_t      temp_path[MAX_PATH];
    wchar_t      name[MAX_PATH];   /* advertised name of this entry */
    LONG         gen;              /* request generation this stream owns */
    int          started;
    int          finished;
    int          abandoned;        /* paste released this stream (under xfer_cs) */
    struct VmClipboardData *clip;
} ClipFCS;

struct ClipDataObject {
    IDataObjectVtbl *lpVtbl;
    LONG             ref;
    ClipSmall        smalls[SMALL_MAX];
    int              small_count;
    ClipMeta        *meta;
    int              meta_count;
    struct VmClipboardData *clip;
};

/* ---- Instance data ---- */

struct VmClipboardData {
    GUID             runtime_id;
    wchar_t          os_type[32];  /* for picking the HV-socket service GUID variant */
    HWND             hwnd;
    volatile BOOL    stop;
    volatile BOOL    sync_enabled;

    VmClipboardLogFn log_fn;
    void            *log_ud;

    /* Writer channel (:0005 host->guest) */
    volatile SOCKET  writer_socket;
    volatile LONG    writer_suppress;
    HANDLE           writer_thread;
    CRITICAL_SECTION writer_cs;

    /* Push worker: every host->guest clipboard send happens on this thread so
       the UI thread never blocks on the socket. The socket is busy for the whole
       of a file transfer (the guest expects one un-interrupted byte stream), so
       a send from the UI thread would freeze the window for the length of the
       transfer - exactly when the user is waiting for a big file to arrive. */
    HANDLE           push_thread;
    HANDLE           push_event;      /* signalled when work is queued */
    volatile LONG    pending_sync;    /* -1 = nothing, 0 = disable, 1 = enable */
    volatile LONG    pending_push;    /* 1 = a FORMAT_LIST push is queued */

    /* GetClipboardSequenceNumber() of the clipboard content last pushed to the
       guest. An unchanged value means the guest already holds this content, so
       pushing it again would only make the guest delete its staged file and
       download it all over again. */
    volatile LONG    pushed_seq;
    volatile LONG    pushed_seq_valid;

    /* Paths captured while the last FORMAT_LIST was pushed. A guest that renders
       files lazily only asks for the bytes when the user pastes - possibly long
       after the list was sent. Streaming from this snapshot guarantees it gets
       the files it was told about, even if the host clipboard has changed since
       (and avoids re-reading somebody else's clipboard at paste time). */
    wchar_t          pushed_paths[CLIP_MAX_FORMATS][MAX_PATH];
    int              pushed_path_count;
    /* Flattened snapshot of the same push, in the exact order advertised to the
       guest.  Guards both fields. */
    ClipFileSnap    *file_snap;
    int              file_snap_count;
    CRITICAL_SECTION pushed_paths_cs;

    /* Reader channel (:0006 guest->host) */
    volatile SOCKET  reader_socket;
    /* 1 only after the ready handshake on a live socket.  A paste that is
       waiting for a reconnect must not be counted as a stalled transfer. */
    volatile LONG    reader_link_up;
    volatile LONG    reader_suppress;
    HANDLE           reader_thread;
    CRITICAL_SECTION reader_cs;
    ClipFmtEntry     reader_fmts[CLIP_MAX_FORMATS];
    int              reader_fmt_count;
    int              reader_fetch_idx;

    /* Guest->host lazy files: metadata sent right after FORMAT_LIST. */
    ClipMeta        *reader_meta;
    int              reader_meta_count;
    BOOL             reader_expect_meta;     /* guest promised a FILE_LIST */
    BOOL             reader_list_accepted;   /* last FORMAT_LIST was honoured */
    BOOL             reader_has_hdrop;
    BOOL             reader_files_requested; /* eager CF_HDROP fetch started */
    BOOL             reader_formats_done;
    ULONGLONG        reader_meta_deadline;   /* 0 = not waiting */
    volatile LONG    reader_want_eager;      /* OLE failed: fetch files eagerly */

    /* The advertised guest list no longer matches what the guest holds (a
       stale request was refused).  After the channel reconnects the host asks
       the guest to send its current list; otherwise every retry would keep
       failing against the old names/sizes until the user copied again. */
    volatile LONG    reader_pull_needed;
    volatile LONG    reader_pull_sent;       /* accept the next list even if
                                                sync is off (host asked for it) */
    ULONGLONG        reader_pull_deadline;   /* 0 = no request outstanding */

    /* OLE STA worker: owns the guest->host data object.  The IDD display
       thread is MTA, and OleSetClipboard() only works on an STA. */
    HANDLE           ole_thread;
    HANDLE           ole_event;
    HANDLE           ole_done;
    HANDLE           ole_ready_evt;
    volatile LONG    ole_stop;
    IDataObject     *ole_object;
    volatile LONG    ole_apply_ok;

    /* One lazy file transfer at a time on the wire; each IStream owns its own
       staging file and generation so pastes that overlap cannot read each
       other's bytes. */
    CRITICAL_SECTION xfer_cs;
    HANDLE           xfer_event;
    BOOL             xfer_active;
    BOOL             xfer_done;
    BOOL             xfer_error;
    BOOL             xfer_cancel;   /* paste abandoned: drain, don't write */
    UINT64           xfer_total;
    wchar_t          xfer_path[MAX_PATH];
    /* Duplicate of the requesting stream's staging-file handle.  The reader
       thread writes through this instead of reopening the path, so an early
       IStream::Release (cancel, or the shell abandoning a speculative stream
       during overlapping pastes) can delete the path without the writer losing
       its handle. */
    HANDLE           xfer_hFile;
    wchar_t          xfer_req_name[MAX_PATH];
    volatile LONG    xfer_gen;
    volatile LONG    file_req_pending;
    volatile LONG    file_req_idx;

    /* Lazy stream staging dir, kept out of temp_dir so a list reset cannot
       delete a file an in-flight paste is still reading. */
    wchar_t          lazy_dir[MAX_PATH];

    /* Temp directory for file transfers */
    wchar_t          temp_dir[MAX_PATH];
};

/* ---- Logging ---- */

static void clip_log(VmClipboard clip, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    if (!clip || !clip->log_fn) return;
    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);
    clip->log_fn(buf, clip->log_ud);
}

/* ---- Low-level I/O ---- */

static BOOL clip_recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

static BOOL clip_send_all(SOCKET s, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = send(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

static SOCKET clip_connect_hv(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family    = AF_HYPERV;
    addr.VmId      = *vm_runtime_id;
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(s, &wfds); FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));
    return s;
}

/* ---- Utility ---- */

/* Shell "file" formats.  A guest->host lazy list leaves these on the host
   clipboard; they must never be echoed back to the guest as ordinary formats
   (GetClipboardData cannot read them, and FileContents would re-download). */
static UINT g_cf_file_desc_w = 0;
static UINT g_cf_file_desc_a = 0;
static UINT g_cf_file_contents = 0;
static UINT g_cf_file_name_w = 0;
static UINT g_cf_file_name_a = 0;

static void clip_init_ole_formats(void)
{
    if (g_cf_file_desc_w) return;
    g_cf_file_desc_w   = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    g_cf_file_desc_a   = RegisterClipboardFormatW(L"FileGroupDescriptor");
    g_cf_file_contents = RegisterClipboardFormatW(L"FileContents");
    g_cf_file_name_w   = RegisterClipboardFormatW(L"FileNameW");
    g_cf_file_name_a   = RegisterClipboardFormatW(L"FileName");
}

static BOOL clip_should_skip(UINT fmt)
{
    if (fmt != 0 &&
        (fmt == g_cf_file_desc_w || fmt == g_cf_file_desc_a ||
         fmt == g_cf_file_contents ||
         fmt == g_cf_file_name_w || fmt == g_cf_file_name_a))
        return TRUE;

    switch (fmt) {
    case 0:
    case CF_BITMAP:
    case CF_PALETTE:
    case CF_OWNERDISPLAY:
    case CF_METAFILEPICT:
    case CF_ENHMETAFILE:
    case CF_DSPTEXT:
    case CF_DSPBITMAP:
    case CF_DSPMETAFILEPICT:
    case CF_DSPENHMETAFILE:
        return TRUE;
    default:
        return FALSE;
    }
}

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

/* ---- File transfer: host->guest (send files from host clipboard) ---- */

static BOOL clip_send_file_entry(SOCKET s, const wchar_t *full_path, const wchar_t *rel_path)
{
    ClipHeader hdr;
    ClipFileInfo fi;
    DWORD attrs = GetFileAttributesW(full_path);
    UINT32 path_bytes;

    if (attrs == INVALID_FILE_ATTRIBUTES) return TRUE;

    path_bytes = (UINT32)(wcslen(rel_path) * sizeof(wchar_t));
    fi.path_len = path_bytes;

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd;
        wchar_t search[MAX_PATH];
        HANDLE h;

        fi.file_size = 0;
        fi.is_directory = 1;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!clip_send_all(s, &hdr, sizeof(hdr))) return FALSE;
        if (!clip_send_all(s, &fi, sizeof(fi))) return FALSE;
        if (path_bytes > 0 && !clip_send_all(s, rel_path, (int)path_bytes)) return FALSE;

        swprintf_s(search, MAX_PATH, L"%s\\*", full_path);
        h = FindFirstFileW(search, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t child_full[MAX_PATH], child_rel[MAX_PATH];
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                swprintf_s(child_full, MAX_PATH, L"%s\\%s", full_path, fd.cFileName);
                swprintf_s(child_rel, MAX_PATH, L"%s\\%s", rel_path, fd.cFileName);
                if (!clip_send_file_entry(s, child_full, child_rel)) {
                    FindClose(h);
                    return FALSE;
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        HANDLE hFile = CreateFileW(full_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER size;
        BYTE *chunk;
        UINT64 remaining;

        if (hFile == INVALID_HANDLE_VALUE) return TRUE;

        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
        if (!chunk) { CloseHandle(hFile); return FALSE; }

        GetFileSizeEx(hFile, &size);
        fi.file_size = (UINT64)size.QuadPart;
        fi.is_directory = 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!clip_send_all(s, &hdr, sizeof(hdr)) ||
            !clip_send_all(s, &fi, sizeof(fi)) ||
            (path_bytes > 0 && !clip_send_all(s, rel_path, (int)path_bytes))) {
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
            return FALSE;
        }

        remaining = fi.file_size;
        while (remaining > 0) {
            DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
            DWORD bytes_read;
            if (!ReadFile(hFile, chunk, to_read, &bytes_read, NULL) || bytes_read == 0) {
                HeapFree(GetProcessHeap(), 0, chunk);
                CloseHandle(hFile);
                return FALSE;
            }
            if (!clip_send_all(s, chunk, (int)bytes_read)) {
                HeapFree(GetProcessHeap(), 0, chunk);
                CloseHandle(hFile);
                return FALSE;
            }
            remaining -= bytes_read;
        }
        HeapFree(GetProcessHeap(), 0, chunk);
        CloseHandle(hFile);
    }
    return TRUE;
}

/* ---- HDROP builder (reader: guest->host file transfer) ---- */

static HGLOBAL clip_build_hdrop_from_temp(const wchar_t *temp_dir)
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

    swprintf_s(pattern, MAX_PATH, L"%s\\*", temp_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(paths[count], MAX_PATH, L"%s\\%s", temp_dir, fd.cFileName);
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

/* ---- Push queue ----
 *
 * The UI thread never talks to the clipboard socket directly: it queues the
 * request here and the push worker performs the send. The worker can afford
 * to wait for the socket (it is a background thread); the UI thread cannot,
 * because the socket is held for the whole of a file transfer.
 */

static void clip_queue_sync_enable(VmClipboard clip, BOOL enabled)
{
    InterlockedExchange(&clip->pending_sync, enabled ? 1 : 0);
    if (clip->push_event) SetEvent(clip->push_event);
}

static void clip_queue_push(VmClipboard clip)
{
    InterlockedExchange(&clip->pending_push, 1);
    if (clip->push_event) SetEvent(clip->push_event);
}

/* ---- Writer channel: send SYNC_ENABLE ---- */

static void clip_send_sync_enable(VmClipboard clip, BOOL enabled)
{
    ClipHeader hdr;
    UINT8 flag;
    SOCKET s = clip->writer_socket;
    if (s == INVALID_SOCKET) return;

    flag = enabled ? 1 : 0;
    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_SYNC_ENABLE;
    hdr.format = 0;
    hdr.data_size = 1;

    EnterCriticalSection(&clip->writer_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    clip_send_all(s, &flag, 1);
    LeaveCriticalSection(&clip->writer_cs);

    clip_log(clip, L"CLIP: Sent SYNC_ENABLE(%d) to guest.", (int)flag);
}

/* ---- Writer channel: send FORMAT_LIST ---- */

/* Capture the files that the FORMAT_LIST we are about to send advertises.
   Called while this thread holds the clipboard open. */
static void clip_snapshot_pushed_paths(VmClipboard clip)
{
    wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
    int path_count = 0;
    HANDLE hData;

    hData = GetClipboardData(CF_HDROP);
    if (hData) {
        UINT file_count = DragQueryFileW((HDROP)hData, 0xFFFFFFFF, NULL, 0);
        UINT fi;
        if (file_count > CLIP_MAX_FORMATS) file_count = CLIP_MAX_FORMATS;
        for (fi = 0; fi < file_count; fi++)
            DragQueryFileW((HDROP)hData, fi, paths[path_count++], MAX_PATH);
    }

    EnterCriticalSection(&clip->pushed_paths_cs);
    if (path_count > 0)
        memcpy(clip->pushed_paths, paths, sizeof(paths[0]) * (size_t)path_count);
    clip->pushed_path_count = path_count;
    LeaveCriticalSection(&clip->pushed_paths_cs);
}

/* Flatten the pushed paths into a stable, fully-described list.  This is the
   single source of truth for both the FILE_LIST we advertise and the bodies we
   later stream on FILE_DATA_REQ, so an index can never point at a different
   file (even if the source tree changes between copy and paste).  Returns a
   heap array owned by the caller and stores its length in *out_count. */
static ClipFileSnap *clip_build_file_snap(VmClipboard clip, int *out_count)
{
    typedef struct { wchar_t full[MAX_PATH]; wchar_t rel[MAX_PATH]; } SItem;
    ClipFileSnap *out;
    SItem *stack;
    wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
    int path_count = 0;
    int sp = 0, count = 0, pi;

    *out_count = 0;

    EnterCriticalSection(&clip->pushed_paths_cs);
    path_count = clip->pushed_path_count;
    if (path_count > 0)
        memcpy(paths, clip->pushed_paths, sizeof(paths[0]) * (size_t)path_count);
    LeaveCriticalSection(&clip->pushed_paths_cs);
    if (path_count <= 0) return NULL;

    out = (ClipFileSnap *)HeapAlloc(GetProcessHeap(), 0,
                                    sizeof(ClipFileSnap) * CLIP_MAX_META);
    stack = (SItem *)HeapAlloc(GetProcessHeap(), 0, sizeof(SItem) * 1024);
    if (!out || !stack) {
        if (out) HeapFree(GetProcessHeap(), 0, out);
        if (stack) HeapFree(GetProcessHeap(), 0, stack);
        return NULL;
    }

    for (pi = 0; pi < path_count; pi++) {
        if (sp < 1024) {
            wchar_t *bn;
            wcscpy_s(stack[sp].full, MAX_PATH, paths[pi]);
            bn = wcsrchr(paths[pi], L'\\');
            bn = bn ? bn + 1 : paths[pi];
            wcscpy_s(stack[sp].rel, MAX_PATH, bn);
            sp++;
        }
    }

    while (sp > 0 && count < CLIP_MAX_META) {
        SItem cur;
        sp--;
        cur = stack[sp];
        {
            DWORD attrs = GetFileAttributesW(cur.full);
            if (attrs == INVALID_FILE_ATTRIBUTES) continue;
            if (count < CLIP_MAX_META) {
                ClipFileSnap *d = &out[count];
                wcscpy_s(d->full, MAX_PATH, cur.full);
                wcscpy_s(d->rel, MAX_PATH, cur.rel);
                d->attrs = attrs;
                d->is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                if (d->is_dir) {
                    d->size = 0;
                } else {
                    WIN32_FILE_ATTRIBUTE_DATA fad;
                    if (GetFileAttributesExW(cur.full, GetFileExInfoStandard, &fad))
                        d->size = ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                    else
                        d->size = 0;
                }
                count++;
            }
            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                WIN32_FIND_DATAW fd;
                wchar_t pattern[MAX_PATH];
                HANDLE h;
                _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", cur.full);
                h = FindFirstFileW(pattern, &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        wchar_t child_full[MAX_PATH], child_rel[MAX_PATH];
                        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                        if (sp + 1 >= 1024) break;
                        _snwprintf_s(child_full, MAX_PATH, _TRUNCATE, L"%s\\%s", cur.full, fd.cFileName);
                        if (wcslen(cur.rel) + 1 + wcslen(fd.cFileName) < MAX_PATH)
                            _snwprintf_s(child_rel, MAX_PATH, _TRUNCATE, L"%s\\%s", cur.rel, fd.cFileName);
                        else
                            wcscpy_s(child_rel, MAX_PATH, fd.cFileName);
                        wcscpy_s(stack[sp].full, MAX_PATH, child_full);
                        wcscpy_s(stack[sp].rel, MAX_PATH, child_rel);
                        sp++;
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, stack);
    *out_count = count;
    return out;
}

static void clip_send_format_list(VmClipboard clip)
{
    ClipHeader hdr;
    SOCKET s;
    UINT fmt;
    BYTE buf[16384];
    int offset = 4;
    UINT32 count = 0;
    DWORD seq;

    s = clip->writer_socket;
    if (s == INVALID_SOCKET) return;

    /* GetClipboardSequenceNumber() changes every time the clipboard content
       changes. When it has not changed since the last push, the guest already
       holds this content - including the file it staged in its temp folder -
       so pushing it again would make the guest delete the staged file and
       download it all over again. That happened on every alt-tab back into
       the VM: focus regains re-pushed the clipboard, the guest wiped its
       temp dir on the format list and re-downloaded the file from scratch. */
    seq = GetClipboardSequenceNumber();
    if (seq != 0 &&
        InterlockedCompareExchange(&clip->pushed_seq_valid, 0, 0) &&
        (LONG)seq == InterlockedCompareExchange(&clip->pushed_seq, 0, 0)) {
        clip_log(clip, L"CLIP: Clipboard unchanged (seq %u) - not re-pushing to guest.",
                 (unsigned)seq);
        return;
    }

    /* Another clipboard owner can hold the clipboard open for a moment (the
       UI thread applies guest content on the same clipboard). Retry briefly
       rather than dropping the push - the guest would not hear about this
       content until the next clipboard change. */
    {
        int attempt;
        for (attempt = 0; attempt < 10; attempt++) {
            if (OpenClipboard(clip->hwnd)) break;
            Sleep(20);
        }
        if (attempt == 10) return;
    }

    {
        BOOL has_hdrop = IsClipboardFormatAvailable(CF_HDROP);
        fmt = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0 && count < CLIP_MAX_FORMATS) {
            char name[256];
            int name_len = 0;

            if (clip_should_skip(fmt)) continue;
            if (has_hdrop && fmt != CF_HDROP) continue;

            if (fmt >= 0xC000)
                name_len = GetClipboardFormatNameA(fmt, name, sizeof(name));

            if (offset + 8 + name_len > 16384) break;

            *(UINT32 *)(buf + offset) = fmt;       offset += 4;
            *(UINT32 *)(buf + offset) = (UINT32)name_len; offset += 4;
            if (name_len > 0) {
                memcpy(buf + offset, name, name_len);
                offset += name_len;
            }
            count++;
        }
    }

    /* Re-read the sequence number now that we hold the clipboard: it now
       describes exactly the content we are about to push. Reading it before
       opening the clipboard left a window in which another application could
       have replaced the content, which would make us record a sequence
       number that does not match what we sent. */
    {
        DWORD cur = GetClipboardSequenceNumber();
        if (cur != 0) seq = cur;
    }

    /* Remember the advertised files while we still hold the clipboard. */
    clip_snapshot_pushed_paths(clip);

    CloseClipboard();

    if (count == 0) return;

    *(UINT32 *)buf = count;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_LIST;
    hdr.format = 0;
    hdr.data_size = (UINT32)offset;

    InterlockedExchange(&clip->reader_suppress, 1);

    EnterCriticalSection(&clip->writer_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    clip_send_all(s, buf, offset);
    LeaveCriticalSection(&clip->writer_cs);

    /* Remember what we pushed so the next focus change does not push it again. */
    InterlockedExchange(&clip->pushed_seq, (LONG)seq);
    InterlockedExchange(&clip->pushed_seq_valid, seq != 0);

    clip_log(clip, L"CLIP: Sent format list (%u formats) to guest.", count);

    /* Send file metadata (names + sizes) so a new guest can advertise the files
       as CFSTR_FILEDESCRIPTORW/CFSTR_FILECONTENTS and stream them on demand.
       Old guests ignore this message type. */
    {
        int entry_count = 0;
        ClipFileSnap *entries = clip_build_file_snap(clip, &entry_count);

        if (entries && entry_count > 0) {
            int bufcap = 1024 * 1024;
            BYTE *fbuf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bufcap);
            int off = 4;
            int i;
            int sent_count = 0;
            int committed = 0;

            if (fbuf) {
                *(UINT32 *)fbuf = (UINT32)entry_count;
                for (i = 0; i < entry_count && off < bufcap - 64; i++) {
                    UINT32 name_len = (UINT32)(wcslen(entries[i].rel) * sizeof(wchar_t));
                    if (off + 4 + (int)name_len + 8 + 4 + 1 > bufcap) break;
                    *(UINT32 *)(fbuf + off) = name_len; off += 4;
                    memcpy(fbuf + off, entries[i].rel, name_len); off += name_len;
                    *(UINT64 *)(fbuf + off) = entries[i].size; off += 8;
                    *(UINT32 *)(fbuf + off) = entries[i].attrs; off += 4;
                    fbuf[off++] = entries[i].is_dir ? 1 : 0;
                    sent_count = i + 1;
                }

                /* Publish exactly this list as the snapshot the request handler
                   serves from.  Committing it before the send means a request
                   that races the advertisement can never see the old list. */
                EnterCriticalSection(&clip->pushed_paths_cs);
                if (clip->file_snap) {
                    HeapFree(GetProcessHeap(), 0, clip->file_snap);
                    clip->file_snap = NULL;
                    clip->file_snap_count = 0;
                }
                if (sent_count > 0) {
                    clip->file_snap = entries;
                    clip->file_snap_count = sent_count;
                    entries = NULL;            /* owned by clip->file_snap */
                    committed = 1;
                }
                LeaveCriticalSection(&clip->pushed_paths_cs);

                if (committed) {
                    ClipHeader fhdr;
                    fhdr.magic = CLIP_MAGIC;
                    fhdr.msg_type = CLIP_MSG_FILE_LIST;
                    fhdr.format = 0;
                    fhdr.data_size = (UINT32)off;
                    EnterCriticalSection(&clip->writer_cs);
                    clip_send_all(s, &fhdr, sizeof(fhdr));
                    clip_send_all(s, fbuf, off);
                    LeaveCriticalSection(&clip->writer_cs);
                }
                HeapFree(GetProcessHeap(), 0, fbuf);
            }
        } else {
            /* No files pushed (or the walk failed): drop the old snapshot so a
               late FILE_DATA_REQ cannot be answered from a previous clipboard. */
            EnterCriticalSection(&clip->pushed_paths_cs);
            if (clip->file_snap) {
                HeapFree(GetProcessHeap(), 0, clip->file_snap);
                clip->file_snap = NULL;
                clip->file_snap_count = 0;
            }
            LeaveCriticalSection(&clip->pushed_paths_cs);
        }
        if (entries) HeapFree(GetProcessHeap(), 0, entries);
    }
}

/* ---- Push worker thread ---- */

static DWORD WINAPI clip_push_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;

    for (;;) {
        LONG sync;
        LONG push;

        WaitForSingleObject(clip->push_event, 250);
        if (clip->stop) break;

        /* Drain the queue. SYNC_ENABLE is always sent before the format list
           that depends on it, so the guest never sees a format list while
           its sync is still disabled. */
        for (;;) {
            sync = InterlockedExchange(&clip->pending_sync, -1);
            push = InterlockedExchange(&clip->pending_push, 0);
            if (sync < 0 && !push) break;

            if (sync >= 0) clip_send_sync_enable(clip, sync != 0);
            if (push)     clip_send_format_list(clip);
        }
    }

    clip->push_thread = NULL;
    return 0;
}

/* ---- Writer channel: handle DATA_REQ from guest ---- */

static void clip_handle_data_request(VmClipboard clip, UINT fmt)
{
    ClipHeader hdr;
    SOCKET s = clip->writer_socket;

    if (s == INVALID_SOCKET) return;

    clip_log(clip, L"CLIP: Data request from guest for format %u.", fmt);

    /* When sync is disabled, return empty — don't leak host clipboard. */
    if (!clip->sync_enabled) {
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&clip->writer_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        LeaveCriticalSection(&clip->writer_cs);
        return;
    }

    if (fmt == CF_HDROP) {
        wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
        int path_count = 0;

        /* Serve the files this guest was told about, not whatever happens to be
           on the clipboard now: a lazy guest only asks for the bytes when the
           user pastes, which can be long after the FORMAT_LIST was pushed. */
        EnterCriticalSection(&clip->pushed_paths_cs);
        path_count = clip->pushed_path_count;
        if (path_count > 0)
            memcpy(paths, clip->pushed_paths, sizeof(paths[0]) * (size_t)path_count);
        LeaveCriticalSection(&clip->pushed_paths_cs);

        /* No snapshot (for example a guest from an older build requested data
           without a preceding push): fall back to the live clipboard. Snapshot
           the dropped paths while the clipboard is open, then release it
           again. Holding OpenClipboard() across a multi-gigabyte transfer
           would lock every other application on the host out of the clipboard
           for the whole transfer. */
        if (path_count == 0 && OpenClipboard(NULL)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)hData;
                UINT file_count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                UINT fi;
                if (file_count > CLIP_MAX_FORMATS) file_count = CLIP_MAX_FORMATS;
                for (fi = 0; fi < file_count; fi++)
                    DragQueryFileW(hDrop, fi, paths[path_count++], MAX_PATH);
            }
            CloseClipboard();
        }

        if (path_count == 0) goto send_empty;

        {
            int i;
            EnterCriticalSection(&clip->writer_cs);
            for (i = 0; i < path_count; i++) {
                const wchar_t *path = paths[i];
                const wchar_t *name;
                name = wcsrchr(path, L'\\');
                name = name ? name + 1 : path;
                clip_send_file_entry(s, path, name);
            }
            hdr.magic = CLIP_MAGIC;
            hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
            hdr.format = CF_HDROP;
            hdr.data_size = 0;
            clip_send_all(s, &hdr, sizeof(hdr));
            LeaveCriticalSection(&clip->writer_cs);
        }
        return;

    send_empty:
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&clip->writer_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        LeaveCriticalSection(&clip->writer_cs);
        return;
    }

    if (OpenClipboard(NULL)) {
        HANDLE hData = GetClipboardData(fmt);
        void *ptr = hData ? GlobalLock(hData) : NULL;
        SIZE_T size = ptr ? GlobalSize(hData) : 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = (ptr && size <= CLIP_MAX_PAYLOAD) ? (UINT32)size : 0;

        EnterCriticalSection(&clip->writer_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        if (hdr.data_size > 0)
            clip_send_all(s, ptr, (int)hdr.data_size);
        LeaveCriticalSection(&clip->writer_cs);

        if (ptr) GlobalUnlock(hData);
        CloseClipboard();
    } else {
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&clip->writer_cs);
        clip_send_all(s, &hdr, sizeof(hdr));
        LeaveCriticalSection(&clip->writer_cs);
    }
}

/* ---- Writer recv thread ---- */

/* Stream one file's bytes for a FileContents request. Flattens the pushed
   paths the same way the metadata list was flattened.  Replies are tagged
   with idx + 1 so the guest can tell this on-demand body apart from an eager
   CF_HDROP stream that may be interleaved on the same socket.  Returns FALSE
   when the connection must be torn down (cancel / send error). */
static BOOL clip_handle_file_data_request(VmClipboard clip, SOCKET s, UINT32 idx)
{
    ClipFileSnap e = {0};
    BOOL have = FALSE;

    /* Serve from the exact snapshot that was advertised with FILE_LIST.  Do not
       re-walk the filesystem here: a file added/removed/renamed between copy and
       paste (or a different FindFirstFile order) would shift the index and make
       the guest receive bytes for a different file, which fails its name/size
       check and tears the whole session - and every other queued paste - down. */
    EnterCriticalSection(&clip->pushed_paths_cs);
    if (!clip->file_snap && clip->pushed_path_count > 0) {
        /* Edge case: a request without an advertised list (for example a test
           harness, or a snapshot that failed to build).  Build it once and
           cache it so every index still maps consistently. */
        LeaveCriticalSection(&clip->pushed_paths_cs);
        {
            int n = 0;
            ClipFileSnap *snap = clip_build_file_snap(clip, &n);
            EnterCriticalSection(&clip->pushed_paths_cs);
            if (!clip->file_snap && snap && n > 0) {
                clip->file_snap = snap;
                clip->file_snap_count = n;
                snap = NULL;
            }
            LeaveCriticalSection(&clip->pushed_paths_cs);
            if (snap) HeapFree(GetProcessHeap(), 0, snap);
        }
        EnterCriticalSection(&clip->pushed_paths_cs);
    }
    if (clip->file_snap && (int)idx < clip->file_snap_count) {
        e = clip->file_snap[idx];
        have = TRUE;
    }
    LeaveCriticalSection(&clip->pushed_paths_cs);

    if (!have) {
        /* Send empty response. */
        ClipFileInfo fi;
        ClipHeader hdr;
        BOOL ok;
        fi.path_len = 0; fi.file_size = 0; fi.is_directory = 0;
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = idx + 1;
        hdr.data_size = (UINT32)sizeof(fi);
        EnterCriticalSection(&clip->writer_cs);
        ok = clip_send_all(s, &hdr, sizeof(hdr)) &&
             clip_send_all(s, &fi, sizeof(fi));
        LeaveCriticalSection(&clip->writer_cs);
        return ok;
    }

    if (e.is_dir) {
        ClipFileInfo fi;
        ClipHeader hdr;
        UINT32 name_bytes = (UINT32)(wcslen(e.rel) * sizeof(wchar_t));
        BOOL ok;
        fi.path_len = name_bytes;
        fi.file_size = 0;
        fi.is_directory = 1;
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = idx + 1;
        hdr.data_size = (UINT32)sizeof(fi) + name_bytes;
        EnterCriticalSection(&clip->writer_cs);
        ok = clip_send_all(s, &hdr, sizeof(hdr)) &&
             clip_send_all(s, &fi, sizeof(fi)) &&
             (name_bytes == 0 || clip_send_all(s, e.rel, (int)name_bytes));
        LeaveCriticalSection(&clip->writer_cs);
        return ok;
    } else {
        HANDLE hFile = CreateFileW(e.full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        BYTE *chunk;
        UINT64 remaining;
        ClipFileInfo fi;
        ClipHeader hdr;
        UINT32 name_bytes = (UINT32)(wcslen(e.rel) * sizeof(wchar_t));
        BOOL ok;
        if (hFile == INVALID_HANDLE_VALUE) {
            fi.path_len = 0; fi.file_size = 0; fi.is_directory = 0;
            hdr.magic = CLIP_MAGIC;
            hdr.msg_type = CLIP_MSG_FILE_DATA;
            hdr.format = idx + 1;
            hdr.data_size = (UINT32)sizeof(fi);
            EnterCriticalSection(&clip->writer_cs);
            ok = clip_send_all(s, &hdr, sizeof(hdr)) &&
                 clip_send_all(s, &fi, sizeof(fi));
            LeaveCriticalSection(&clip->writer_cs);
            return ok;
        }
        /* Advertise the size the guest was told about so its validation always
           matches, even if the file grew since the list was built.  A file that
           shrank is a real error and stops the transfer below. */
        fi.path_len = name_bytes;
        fi.file_size = e.size;
        fi.is_directory = 0;
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = idx + 1;
        hdr.data_size = (UINT32)sizeof(fi) + name_bytes;
        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
        remaining = fi.file_size;

        EnterCriticalSection(&clip->writer_cs);
        ok = clip_send_all(s, &hdr, sizeof(hdr)) &&
             clip_send_all(s, &fi, sizeof(fi)) &&
             (name_bytes == 0 || clip_send_all(s, e.rel, (int)name_bytes));
        while (ok && remaining > 0) {
            DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
            DWORD bytes_read;
            if (!ReadFile(hFile, chunk, to_read, &bytes_read, NULL) || bytes_read == 0) {
                clip_log(clip, L"CLIP: File %u local read failed at %llu/%llu "
                               L"bytes (error %lu).",
                         idx, (unsigned long long)(fi.file_size - remaining),
                         (unsigned long long)fi.file_size, GetLastError());
                ok = FALSE;
                break;
            }
            if (!clip_send_all(s, chunk, (int)bytes_read)) {
                clip_log(clip, L"CLIP: File %u body send failed at %llu/%llu "
                               L"bytes (guest closed the connection).",
                         idx, (unsigned long long)(fi.file_size - remaining),
                         (unsigned long long)fi.file_size);
                ok = FALSE;
                break;
            }
            remaining -= bytes_read;
        }
        LeaveCriticalSection(&clip->writer_cs);
        if (chunk) HeapFree(GetProcessHeap(), 0, chunk);
        CloseHandle(hFile);
        if (!ok) {
            /* Cancelled / disconnected: stop the writer thread so the guest is
               not left waiting for the rest of the body. */
            return FALSE;
        }
    }

    return TRUE;
}

/* ==== Guest->host lazy files: OLE data object ==========================
 *
 * The reader thread parses FILE_LIST and never downloads the file bytes.
 * This data object is published with OleSetClipboard() on a dedicated STA
 * thread; when an application pastes, its IStream::Read asks the reader
 * thread for exactly one file, which streams it into a temp file as it
 * arrives.  The shell's own copy dialog supplies the progress bar.
 * ======================================================================= */

static IStreamVtbl       g_clip_fcs_vtbl;
static IEnumFORMATETCVtbl g_clip_enum_vtbl;
static IDataObjectVtbl   g_clip_do_vtbl;

/* Distinguishes the IStream objects created for overlapping pastes of the same
   file: every paste gets its own stream, all sharing one file index. */
static LONG              g_next_fcs_id;

/* ---------- IStream ---------- */

static HRESULT STDMETHODCALLTYPE ClipFCS_QueryInterface(IStream *this, REFIID riid, void **ppv)
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

static ULONG STDMETHODCALLTYPE ClipFCS_AddRef(IStream *this)
{
    return (ULONG)InterlockedIncrement(&((ClipFCS *)this)->ref);
}

/* A paste that is abandoned (the shell releases its IStream, or cancels the
   copy) must not take the clipboard channel down with it: several pastes can
   share this connection, and tearing it down made one bad job fail all of the
   others too.  Instead the body of the abandoned request is drained and
   discarded, which keeps the wire in sync and frees the slot for the next
   paste.  See clip->xfer_cancel. */

static ULONG STDMETHODCALLTYPE ClipFCS_Release(IStream *this)
{
    ClipFCS *s = (ClipFCS *)this;
    ULONG r = (ULONG)InterlockedDecrement(&s->ref);
    if (r == 0) {
        int cancelled = 0;

        /* Released before all of its bytes were consumed: this paste was
           abandoned (or failed).  Ask the reader thread to stop writing and
           drain the rest of the body.  Deliberately does not require
           !finished: a stream that gave up after a stall still has a request
           in flight, and without the cancel the reader would download the
           whole body into a staging file nobody owns any more. */
        EnterCriticalSection(&s->clip->xfer_cs);
        s->abandoned = TRUE;
        if (s->started &&
            s->pos < s->declared_size &&
            s->gen == s->clip->xfer_gen &&
            s->clip->xfer_active && !s->clip->xfer_done) {
            s->clip->xfer_cancel = TRUE;
            cancelled = 1;
        }
        LeaveCriticalSection(&s->clip->xfer_cs);

        clip_log(s->clip, L"CLIP-R: Released stream #%d (idx %d) gen=%ld "
                          L"pos=%llu/%llu started=%d finished=%d cancel=%d.",
                 s->id, s->index, (long)s->gen,
                 (unsigned long long)s->pos,
                 (unsigned long long)s->declared_size,
                 s->started ? 1 : 0, s->finished ? 1 : 0, cancelled);

        if (cancelled)
            clip_log(s->clip, L"CLIP-R: Request %d abandoned; draining the rest "
                              L"of the body (connection stays up).", s->index);

        if (s->hFile && s->hFile != INVALID_HANDLE_VALUE) CloseHandle(s->hFile);
        if (s->temp_path[0]) DeleteFileW(s->temp_path);
        HeapFree(GetProcessHeap(), 0, s);
    }
    return r;
}

/* Pump posted messages for this thread.  The OLE data object lives in a
   single-threaded apartment, so while one stream's IStream::Read() waits we
   must keep dispatching COM calls for the other streams (otherwise both pastes
   freeze until the wire transfer finishes).  Never called while holding a lock. */

/* All ClipFCS calls arrive on the one OLE STA thread, so a plain counter is
   enough.  Read() pumps messages, and that pump can dispatch a second Read()
   for a queued stream: that nested call must NOT pump again, or it can block
   waiting for the single wire slot while the outer Read is itself blocked
   inside DispatchMessage -> an unbounded, unkillable stall.  One level of
   nesting is safe (the nested wait simply ends when the reader thread frees
   the slot), deeper is not. */
static int g_read_depth;

static void clip_pump_messages(void)
{
    MSG msg;

    if (g_read_depth > 1)
        return;

    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

/* Wait for the transfer event without starving the apartment: pump messages
   while waiting so that another stream's Read() can still be dispatched. */
static DWORD clip_wait_xfer(VmClipboard clip, DWORD ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;

    for (;;) {
        DWORD remain, r;
        ULONGLONG now = GetTickCount64();

        if (now >= deadline) return WAIT_TIMEOUT;
        remain = (DWORD)(deadline - now);
        r = MsgWaitForMultipleObjects(1, &clip->xfer_event, FALSE, remain, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        if (r == WAIT_TIMEOUT) return WAIT_TIMEOUT;
        if (r == WAIT_OBJECT_0 + 1) clip_pump_messages();
        else return r;
    }
}

/* Claim the single wire slot and create this stream's own staging file.
   Creating it here (not on the reader thread) means a newer request can never
   overwrite the path of an older stream that has not opened its file yet. */
static BOOL clip_lazy_start_request(VmClipboard clip, ClipFCS *s)
{
    wchar_t tmp[MAX_PATH];
    HANDLE hf;
    LONG gen;
    ULONGLONG deadline = GetTickCount64() + 600000;  /* bound a wedged guest */

    for (;;) {
        EnterCriticalSection(&clip->xfer_cs);
        /* A reentrant Read can reach this point after another frame on the
           same stream already consumed the whole body (the OLE STA dispatches
           calls while Read pumps).  Starting a second transfer for a stream
           that is already complete would download the file again into a file
           nobody will read, and hold the wire against the next paste. */
        if (s->pos >= s->declared_size) {
            LeaveCriticalSection(&clip->xfer_cs);
            return TRUE;
        }
        if (!clip->xfer_active) {
            /* The paste was abandoned while this stream was still queued:
               do not claim the wire and download a file nobody will read. */
            if (s->abandoned) {
                LeaveCriticalSection(&clip->xfer_cs);
                return FALSE;
            }
            clip->xfer_active = TRUE;
            clip->xfer_done   = FALSE;
            clip->xfer_error  = FALSE;
            clip->xfer_cancel = FALSE;
            clip->xfer_total  = s->declared_size;
            gen = InterlockedIncrement(&clip->xfer_gen);
            s->gen = gen;
            /* Mark it started while holding the lock so a concurrent Release
               cancels the transfer instead of racing the creation of the
               staging file. */
            s->started = 1;
            wcscpy_s(clip->xfer_req_name, MAX_PATH, s->name);
            LeaveCriticalSection(&clip->xfer_cs);
            break;
        }
        LeaveCriticalSection(&clip->xfer_cs);
        if (clip->stop || GetTickCount64() >= deadline) return FALSE;
        clip_wait_xfer(clip, 200);
    }

    CreateDirectoryW(clip->lazy_dir, NULL);
    {
        /* Only worth a line when the paste may actually run out of space;
           otherwise this repeats once per file of a multi-file paste. */
        ULARGE_INTEGER freeb;
        if (GetDiskFreeSpaceExW(clip->lazy_dir, &freeb, NULL, NULL) &&
            freeb.QuadPart < s->declared_size)
            clip_log(clip, L"CLIP-R: Lazy staging '%ls' has only %llu MB free, "
                           L"less than the %llu MB file being pasted.",
                     clip->lazy_dir,
                     (unsigned long long)(freeb.QuadPart / (1024 * 1024)),
                     (unsigned long long)(s->declared_size / (1024 * 1024)));
    }
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s\\_gc_%d_%ld.tmp",
                 clip->lazy_dir, s->index, (long)gen);
    hf = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&clip->xfer_cs);
        clip->xfer_error  = TRUE;
        clip->xfer_done   = TRUE;
        clip->xfer_active = FALSE;
        LeaveCriticalSection(&clip->xfer_cs);
        SetEvent(clip->xfer_event);
        return FALSE;
    }

    s->hFile = hf;
    wcscpy_s(s->temp_path, MAX_PATH, tmp);

    /* Released between the claim and the request: give the slot back now so
       the next paste does not wait for a body nobody asked for. */
    EnterCriticalSection(&clip->xfer_cs);
    if (s->abandoned) {
        LeaveCriticalSection(&clip->xfer_cs);
        CloseHandle(hf);
        s->hFile = INVALID_HANDLE_VALUE;
        s->temp_path[0] = L'\0';
        DeleteFileW(tmp);
        EnterCriticalSection(&clip->xfer_cs);
        clip->xfer_done   = TRUE;
        clip->xfer_active = FALSE;
        LeaveCriticalSection(&clip->xfer_cs);
        SetEvent(clip->xfer_event);
        return FALSE;
    }
    LeaveCriticalSection(&clip->xfer_cs);

    /* The reader thread needs its own *file object*, not a DuplicateHandle:
       a duplicate shares one file pointer with the stream, and the stream
       seeks around while the body is being written, so the writer would land
       at the reader's offset and corrupt the staged data.  Reopen by path
       instead.  Release deletes the path, but that only marks the file for
       deletion; this open handle keeps it usable until the request ends. */
    {
        HANDLE serve = CreateFileW(tmp, GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (serve == INVALID_HANDLE_VALUE) {
            clip_log(clip, L"CLIP-R: Stream %d: cannot reopen staging file "
                           L"'%ls' (%lu).", s->index, tmp, GetLastError());
            CloseHandle(hf);
            s->hFile = INVALID_HANDLE_VALUE;
            s->temp_path[0] = L'\0';
            DeleteFileW(tmp);
            EnterCriticalSection(&clip->xfer_cs);
            clip->xfer_error  = TRUE;
            clip->xfer_done   = TRUE;
            clip->xfer_active = FALSE;
            LeaveCriticalSection(&clip->xfer_cs);
            SetEvent(clip->xfer_event);
            return FALSE;
        }

        EnterCriticalSection(&clip->xfer_cs);
        if (clip->xfer_hFile && clip->xfer_hFile != INVALID_HANDLE_VALUE)
            CloseHandle(clip->xfer_hFile);
        clip->xfer_hFile = serve;
        wcscpy_s(clip->xfer_path, MAX_PATH, tmp);
        InterlockedExchange(&clip->file_req_idx, (LONG)s->index);
        InterlockedExchange(&clip->file_req_pending, 1);
        LeaveCriticalSection(&clip->xfer_cs);
    }

    return TRUE;
}

/* Hold an extra reference for the duration of Read so a cancel delivered
   while we are waiting cannot free the stream underneath the call. */
static HRESULT STDMETHODCALLTYPE ClipFCS_Read(IStream *this, void *pv, ULONG cb, ULONG *pcbRead);

static HRESULT STDMETHODCALLTYPE clip_fcs_read_impl(IStream *this, void *pv, ULONG cb, ULONG *pcbRead)
{
    ClipFCS *s = (ClipFCS *)this;
    VmClipboard clip = s->clip;
    DWORD got = 0;
    UINT64 last_size = 0;
    ULONGLONG last_progress;
    ULONGLONG last_log;

    if (pcbRead) *pcbRead = 0;
    if (cb == 0) return S_OK;

    if (s->declared_size == 0) {
        s->finished = 1;
        return S_FALSE;
    }

    if (!s->started) {
        if (!clip_lazy_start_request(clip, s)) { s->finished = 1; return E_FAIL; }
    }
    if (!s->hFile || s->hFile == INVALID_HANDLE_VALUE) { s->finished = 1; return E_FAIL; }

    /* The progress clock starts when this stream owns the wire slot, not when
       the caller entered Read.  A paste queued behind two 5 GB transfers can
       legitimately wait minutes in clip_lazy_start_request; starting the
       watchdog earlier made it report "stalled: pos=0" the instant it was
       finally served. */
    last_progress = GetTickCount64();
    last_log = last_progress;

    for (;;) {
        LARGE_INTEGER fsz;
        UINT64 avail;
        int done, error, abandoned;
        ULONGLONG now;

        /* The reader thread writes this stream's private file, so its size is
           the authoritative byte count; no shared progress counter. */
        if (!GetFileSizeEx(s->hFile, &fsz)) { s->finished = 1; return E_FAIL; }
        avail = (UINT64)fsz.QuadPart;

        EnterCriticalSection(&clip->xfer_cs);
        abandoned = s->abandoned;
        if (s->gen == clip->xfer_gen) {
            done  = clip->xfer_done;
            error = clip->xfer_error;
        } else {
            /* A newer request claimed the slot: this body already finished. */
            done  = 1;
            error = (avail < s->declared_size) ? 1 : 0;
        }
        LeaveCriticalSection(&clip->xfer_cs);

        if (s->pos < avail && s->pos < s->declared_size) {
            UINT64 n = avail - s->pos;
            LARGE_INTEGER off;
            if (n > cb) n = cb;
            off.QuadPart = (LONGLONG)s->pos;
            if (!SetFilePointerEx(s->hFile, off, NULL, FILE_BEGIN)) { s->finished = 1; return E_FAIL; }
            if (!ReadFile(s->hFile, pv, (DWORD)n, &got, NULL)) { s->finished = 1; return E_FAIL; }
            s->pos += got;
            if (pcbRead) *pcbRead = got;
            return got ? S_OK : E_FAIL;
        }

        if (s->pos >= s->declared_size) {
            s->finished = 1;
            return S_FALSE;
        }

        if (abandoned) {
            /* The paste was abandoned: the reader thread is draining the rest
               of the body, but nobody will read this stream again.  Leave now
               instead of waiting for the inactivity watchdog. */
            s->finished = 1;
            return S_FALSE;
        }

        if (done) {
            clip_log(clip, L"CLIP-R: Stream #%d (idx %d, gen %ld) ended: "
                           L"pos=%llu/%llu avail=%llu error=%d.",
                     s->id, s->index, (long)s->gen,
                     (unsigned long long)s->pos,
                     (unsigned long long)s->declared_size,
                     (unsigned long long)avail, error);
            s->finished = 1;
            return error ? E_FAIL : S_FALSE;
        }

        /* A paste must never hang: fail if the clipboard is shutting down or
           the guest stopped sending chunks (VM gone mid-transfer). */
        now = GetTickCount64();
        if (avail != last_size) {
            last_size = avail;
            last_progress = now;
        } else if (!clip->reader_link_up) {
            /* The link is being re-established; no request can be served
               until the handshake completes, so this is waiting, not a
               stalled transfer.  Keep the paste alive instead of failing it
               for the duration of a reconnect. */
            last_progress = now;
        } else {
            ULONGLONG limit = (avail == 0) ? CLIP_START_WAIT_MS : CLIP_STALL_MS;

            if (clip->stop || now - last_progress >= limit) {
                clip_log(clip, L"CLIP-R: Stream #%d (idx %d, gen %ld) stalled: "
                               L"pos=%llu avail=%llu declared=%llu link=%d stop=%d.",
                         s->id, s->index, (long)s->gen,
                         (unsigned long long)s->pos,
                         (unsigned long long)avail,
                         (unsigned long long)s->declared_size,
                         (int)InterlockedCompareExchange(&clip->reader_link_up, 0, 0),
                         clip->stop ? 1 : 0);
                s->finished = 1;
                return E_FAIL;
            }
        }

        /* A heartbeat, not a progress bar: the shell already shows the bytes.
           One line every 30 s keeps a wedged paste visible without flooding
           the UI log during a multi-minute transfer. */
        if (now - last_log >= 30000) {
            last_log = now;
            clip_log(clip, L"CLIP-R: Stream #%d (idx %d, gen %ld) waiting: "
                           L"pos=%llu avail=%llu declared=%llu done=%d error=%d.",
                     s->id, s->index, (long)s->gen,
                     (unsigned long long)s->pos,
                     (unsigned long long)avail,
                     (unsigned long long)s->declared_size, done, error);
        }

        clip_wait_xfer(clip, 1000);
    }
}

/* Hold an extra reference for the duration of Read so a cancel delivered
   while we are waiting cannot free the stream underneath the call. */
static HRESULT STDMETHODCALLTYPE ClipFCS_Read(IStream *this, void *pv, ULONG cb, ULONG *pcbRead)
{
    IStream *s = this;
    HRESULT hr;

    s->lpVtbl->AddRef(s);
    /* Nested Reads are expected and normal here: Read pumps messages while it
       waits, so a second overlapping paste starts its own Read inside the
       first one.  They are separate IStream objects and share nothing; logging
       every reentry filled the user-visible log with noise. */
    g_read_depth++;
    hr = clip_fcs_read_impl(this, pv, cb, pcbRead);
    g_read_depth--;
    s->lpVtbl->Release(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_Write(IStream *this, const void *pv, ULONG cb, ULONG *pcbWritten)
{
    (void)this; (void)pv; (void)cb;
    if (pcbWritten) *pcbWritten = 0;
    return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_Seek(IStream *this, LARGE_INTEGER dlibMove,
                                              DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition)
{
    ClipFCS *s = (ClipFCS *)this;
    LONGLONG base, np;

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

static HRESULT STDMETHODCALLTYPE ClipFCS_SetSize(IStream *this, ULARGE_INTEGER libNewSize)
{
    (void)this; (void)libNewSize;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_CopyTo(IStream *this, IStream *pstm, ULARGE_INTEGER cb,
                                                 ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten)
{
    ClipFCS *s = (ClipFCS *)this;
    BYTE buf[64 * 1024];
    UINT64 left = cb.QuadPart;
    UINT64 rd = 0, wr = 0;

    if (pcbRead) pcbRead->QuadPart = 0;
    if (pcbWritten) pcbWritten->QuadPart = 0;

    if (cb.QuadPart == 0) {
        if (s->declared_size > s->pos) left = s->declared_size - s->pos;
        else left = ~(UINT64)0;
    }

    while (left > 0) {
        ULONG want = left > sizeof(buf) ? (ULONG)sizeof(buf) : (ULONG)left;
        ULONG got = 0, written = 0;
        HRESULT rh = ClipFCS_Read(this, buf, want, &got);
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
    if (wr != rd)
        clip_log(s->clip, L"CLIP-R: Stream %d CopyTo wrote %llu of %llu bytes.",
                 s->index, (unsigned long long)wr, (unsigned long long)rd);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_Commit(IStream *this, DWORD grfCommitFlags)
{
    (void)this; (void)grfCommitFlags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_Revert(IStream *this)
{
    (void)this;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_LockRegion(IStream *this, ULARGE_INTEGER libOffset,
                                                    ULARGE_INTEGER cb, DWORD dwLockType)
{
    (void)this; (void)libOffset; (void)cb; (void)dwLockType;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_UnlockRegion(IStream *this, ULARGE_INTEGER libOffset,
                                                      ULARGE_INTEGER cb, DWORD dwLockType)
{
    (void)this; (void)libOffset; (void)cb; (void)dwLockType;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_Stat(IStream *this, STATSTG *pstatstg, DWORD grfStatFlag)
{
    ClipFCS *s = (ClipFCS *)this;
    (void)grfStatFlag;
    if (!pstatstg) return E_INVALIDARG;
    ZeroMemory(pstatstg, sizeof(*pstatstg));
    pstatstg->type = STGTY_STREAM;
    pstatstg->cbSize.QuadPart = s->declared_size;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ClipFCS_Clone(IStream *this, IStream **ppstm)
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
} ClipEnum;

static HRESULT STDMETHODCALLTYPE ClipEnum_QueryInterface(IEnumFORMATETC *this, REFIID riid, void **ppv)
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

static ULONG STDMETHODCALLTYPE ClipEnum_AddRef(IEnumFORMATETC *this)
{
    return (ULONG)InterlockedIncrement(&((ClipEnum *)this)->ref);
}

static ULONG STDMETHODCALLTYPE ClipEnum_Release(IEnumFORMATETC *this)
{
    ClipEnum *e = (ClipEnum *)this;
    ULONG r = (ULONG)InterlockedDecrement(&e->ref);
    if (r == 0) {
        if (e->items) HeapFree(GetProcessHeap(), 0, e->items);
        HeapFree(GetProcessHeap(), 0, e);
    }
    return r;
}

static HRESULT STDMETHODCALLTYPE ClipEnum_Next(IEnumFORMATETC *this, ULONG celt,
                                               FORMATETC *rgelt, ULONG *pceltFetched)
{
    ClipEnum *e = (ClipEnum *)this;
    ULONG n = 0;

    if (!rgelt) return E_INVALIDARG;
    while (n < celt && e->pos < e->count)
        rgelt[n++] = e->items[e->pos++];
    if (pceltFetched) *pceltFetched = n;
    return (n == celt) ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE ClipEnum_Skip(IEnumFORMATETC *this, ULONG celt)
{
    ClipEnum *e = (ClipEnum *)this;
    e->pos += (int)celt;
    if (e->pos > e->count) e->pos = e->count;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ClipEnum_Reset(IEnumFORMATETC *this)
{
    ((ClipEnum *)this)->pos = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ClipEnum_Clone(IEnumFORMATETC *this, IEnumFORMATETC **ppenum)
{
    ClipEnum *e = (ClipEnum *)this;
    ClipEnum *c;

    if (!ppenum) return E_INVALIDARG;
    *ppenum = NULL;
    c = (ClipEnum *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ClipEnum));
    if (!c) return E_OUTOFMEMORY;
    c->items = (FORMATETC *)HeapAlloc(GetProcessHeap(), 0, sizeof(FORMATETC) * (size_t)e->count);
    if (!c->items && e->count > 0) { HeapFree(GetProcessHeap(), 0, c); return E_OUTOFMEMORY; }
    if (e->count > 0) memcpy(c->items, e->items, sizeof(FORMATETC) * (size_t)e->count);
    c->lpVtbl = &g_clip_enum_vtbl;
    c->ref = 1;
    c->count = e->count;
    c->pos = e->pos;
    *ppenum = (IEnumFORMATETC *)c;
    return S_OK;
}

/* ---------- IDataObject ---------- */

static HRESULT STDMETHODCALLTYPE ClipDO_QueryInterface(IDataObject *this, REFIID riid, void **ppv)
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

static ULONG STDMETHODCALLTYPE ClipDO_AddRef(IDataObject *this)
{
    return (ULONG)InterlockedIncrement(&((ClipDataObject *)this)->ref);
}

static ULONG STDMETHODCALLTYPE ClipDO_Release(IDataObject *this)
{
    ClipDataObject *d = (ClipDataObject *)this;
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

static HRESULT STDMETHODCALLTYPE ClipDO_GetData(IDataObject *this, FORMATETC *pformatetc,
                                                STGMEDIUM *pmedium)
{
    ClipDataObject *d = (ClipDataObject *)this;
    int i;

    if (!pformatetc || !pmedium) return E_INVALIDARG;
    ZeroMemory(pmedium, sizeof(*pmedium));
    if (pformatetc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

    if (pformatetc->cfFormat == g_cf_file_desc_w && (pformatetc->tymed & TYMED_HGLOBAL)) {
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
        ClipFCS *s;
        i = pformatetc->lindex;
        if (i < 0 || i >= d->meta_count) return DV_E_LINDEX;
        clip_log(d->clip, L"CLIP-R: Shell asked for FileContents %d '%ls' (%llu bytes).",
                 i, d->meta[i].name, (unsigned long long)d->meta[i].size);
        s = (ClipFCS *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ClipFCS));
        if (!s) return E_OUTOFMEMORY;
        s->lpVtbl = &g_clip_fcs_vtbl;
        s->ref = 1;
        s->index = i;
        s->id = (int)InterlockedIncrement(&g_next_fcs_id);
        s->declared_size = d->meta[i].size;
        wcscpy_s(s->name, MAX_PATH, d->meta[i].name);
        s->hFile = INVALID_HANDLE_VALUE;
        s->clip = d->clip;
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

static HRESULT STDMETHODCALLTYPE ClipDO_GetDataHere(IDataObject *this, FORMATETC *pformatetc,
                                                    STGMEDIUM *pmedium)
{
    (void)this; (void)pformatetc; (void)pmedium;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ClipDO_QueryGetData(IDataObject *this, FORMATETC *pformatetc)
{
    ClipDataObject *d = (ClipDataObject *)this;
    int i;

    if (!pformatetc) return E_INVALIDARG;
    if (pformatetc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;

    if (pformatetc->cfFormat == g_cf_file_desc_w && (pformatetc->tymed & TYMED_HGLOBAL))
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

static HRESULT STDMETHODCALLTYPE ClipDO_GetCanonicalFormatEtc(IDataObject *this,
                                                              FORMATETC *pformatectIn,
                                                              FORMATETC *pformatetcOut)
{
    (void)this;
    if (!pformatectIn || !pformatetcOut) return E_INVALIDARG;
    *pformatetcOut = *pformatectIn;
    pformatetcOut->ptd = NULL;
    return DATA_S_SAMEFORMATETC;
}

static HRESULT STDMETHODCALLTYPE ClipDO_SetData(IDataObject *this, FORMATETC *pformatetc,
                                                STGMEDIUM *pmedium, BOOL fRelease)
{
    (void)this; (void)pformatetc; (void)pmedium; (void)fRelease;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ClipDO_EnumFormatEtc(IDataObject *this, DWORD dwDirection,
                                                      IEnumFORMATETC **ppenumFormatEtc)
{
    ClipDataObject *d = (ClipDataObject *)this;
    ClipEnum *e;
    FORMATETC *items;
    int total, i, n = 0;

    if (!ppenumFormatEtc) return E_INVALIDARG;
    *ppenumFormatEtc = NULL;
    if (dwDirection != DATADIR_GET) return E_NOTIMPL;

    total = 1 + d->meta_count + d->small_count;
    items = (FORMATETC *)HeapAlloc(GetProcessHeap(), 0, sizeof(FORMATETC) * (size_t)total);
    if (!items) return E_OUTOFMEMORY;

    ZeroMemory(&items[n], sizeof(FORMATETC));
    items[n].cfFormat = (CLIPFORMAT)g_cf_file_desc_w;
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

    e = (ClipEnum *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ClipEnum));
    if (!e) { HeapFree(GetProcessHeap(), 0, items); return E_OUTOFMEMORY; }
    e->lpVtbl = &g_clip_enum_vtbl;
    e->ref = 1;
    e->items = items;
    e->count = n;
    *ppenumFormatEtc = (IEnumFORMATETC *)e;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ClipDO_DAdvise(IDataObject *this, FORMATETC *pformatetc,
                                                DWORD advf, IAdviseSink *pAdvSink,
                                                DWORD *pdwConnection)
{
    (void)this; (void)pformatetc; (void)advf; (void)pAdvSink;
    if (pdwConnection) *pdwConnection = 0;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE ClipDO_DUnadvise(IDataObject *this, DWORD dwConnection)
{
    (void)this; (void)dwConnection;
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE ClipDO_EnumDAdvise(IDataObject *this, IEnumSTATDATA **ppenumAdvise)
{
    (void)this;
    if (ppenumAdvise) *ppenumAdvise = NULL;
    return OLE_E_ADVISENOTSUPPORTED;
}

static IStreamVtbl g_clip_fcs_vtbl = {
    ClipFCS_QueryInterface, ClipFCS_AddRef, ClipFCS_Release,
    ClipFCS_Read, ClipFCS_Write, ClipFCS_Seek, ClipFCS_SetSize, ClipFCS_CopyTo,
    ClipFCS_Commit, ClipFCS_Revert, ClipFCS_LockRegion, ClipFCS_UnlockRegion,
    ClipFCS_Stat, ClipFCS_Clone
};

static IEnumFORMATETCVtbl g_clip_enum_vtbl = {
    ClipEnum_QueryInterface, ClipEnum_AddRef, ClipEnum_Release,
    ClipEnum_Next, ClipEnum_Skip, ClipEnum_Reset, ClipEnum_Clone
};

static IDataObjectVtbl g_clip_do_vtbl = {
    ClipDO_QueryInterface, ClipDO_AddRef, ClipDO_Release,
    ClipDO_GetData, ClipDO_GetDataHere, ClipDO_QueryGetData,
    ClipDO_GetCanonicalFormatEtc, ClipDO_SetData, ClipDO_EnumFormatEtc,
    ClipDO_DAdvise, ClipDO_DUnadvise, ClipDO_EnumDAdvise
};

/* Build a data object from the current reader list.  Caller holds reader_cs. */
static ClipDataObject *clip_do_build(VmClipboard clip)
{
    ClipDataObject *d;
    int i;

    d = (ClipDataObject *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ClipDataObject));
    if (!d) return NULL;
    d->lpVtbl = &g_clip_do_vtbl;
    d->ref = 1;
    d->clip = clip;

    if (clip->reader_meta_count > 0) {
        d->meta = (ClipMeta *)HeapAlloc(GetProcessHeap(), 0,
                                        sizeof(ClipMeta) * (size_t)clip->reader_meta_count);
        if (!d->meta) { HeapFree(GetProcessHeap(), 0, d); return NULL; }
        memcpy(d->meta, clip->reader_meta,
               sizeof(ClipMeta) * (size_t)clip->reader_meta_count);
        d->meta_count = clip->reader_meta_count;
    }

    for (i = 0; i < clip->reader_fmt_count && d->small_count < SMALL_MAX; i++) {
        HGLOBAL h;
        void *p;
        if (clip->reader_fmts[i].is_hdrop) continue;
        if (!clip->reader_fmts[i].data || clip->reader_fmts[i].data_size == 0) continue;
        h = GlobalAlloc(GMEM_MOVEABLE, clip->reader_fmts[i].data_size);
        if (!h) continue;
        p = GlobalLock(h);
        if (!p) { GlobalFree(h); continue; }
        memcpy(p, clip->reader_fmts[i].data, clip->reader_fmts[i].data_size);
        GlobalUnlock(h);
        d->smalls[d->small_count].fmt  = clip->reader_fmts[i].local_id;
        d->smalls[d->small_count].h    = h;
        d->smalls[d->small_count].size = clip->reader_fmts[i].data_size;
        d->small_count++;
    }

    return d;
}

/* Runs on the OLE STA thread. */
static void clip_ole_apply_lazy(VmClipboard clip)
{
    ClipDataObject *d;
    IDataObject *obj;
    HRESULT hr;

    EnterCriticalSection(&clip->reader_cs);
    d = (clip->reader_meta_count > 0) ? clip_do_build(clip) : NULL;
    LeaveCriticalSection(&clip->reader_cs);

    if (!d) {
        InterlockedExchange(&clip->ole_apply_ok, 0);
        return;
    }

    obj = (IDataObject *)d;
    hr = OleSetClipboard(obj);
    if (SUCCEEDED(hr)) {
        if (clip->ole_object) clip->ole_object->lpVtbl->Release(clip->ole_object);
        clip->ole_object = obj;                 /* keep our reference */
        InterlockedExchange(&clip->ole_apply_ok, 1);
        clip_log(clip, L"CLIP-R: Advertised %d file(s) via FileContents (lazy).",
                 d->meta_count);
        {
            int li;
            for (li = 0; li < d->meta_count && li < 8; li++)
                clip_log(clip, L"CLIP-R:   [%d] '%ls' %llu bytes%s",
                         li, d->meta[li].name,
                         (unsigned long long)d->meta[li].size,
                         d->meta[li].is_dir ? L" (dir)" : L"");
            if (d->meta_count > 8)
                clip_log(clip, L"CLIP-R:   ... and %d more.", d->meta_count - 8);
        }
    } else {
        obj->lpVtbl->Release(obj);
        InterlockedExchange(&clip->ole_apply_ok, 0);
        clip_log(clip, L"CLIP-R: OleSetClipboard failed (0x%08lX); falling back to eager files.",
                 (unsigned long)hr);
    }
}

static DWORD WINAPI clip_ole_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;
    MSG msg;
    HRESULT hr;

    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);
    hr = OleInitialize(NULL);
    if (FAILED(hr))
        clip_log(clip, L"CLIP-R: OleInitialize failed (0x%08lX).", (unsigned long)hr);
    SetEvent(clip->ole_ready_evt);

    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &clip->ole_event, FALSE, INFINITE, QS_ALLINPUT);
        if (InterlockedCompareExchange(&clip->ole_stop, 0, 0)) break;

        if (r == WAIT_OBJECT_0) {
            clip_ole_apply_lazy(clip);
            SetEvent(clip->ole_done);
        } else if (r == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else {
            break;
        }
    }

    if (SUCCEEDED(hr)) {
        if (clip->ole_object) {
            if (OleIsCurrentClipboard(clip->ole_object) == S_OK)
                OleSetClipboard(NULL);
            clip->ole_object->lpVtbl->Release(clip->ole_object);
            clip->ole_object = NULL;
        }
        OleUninitialize();
    }
    return 0;
}

static BOOL clip_writer_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {
    case CLIP_MSG_FORMAT_DATA_REQ:
        clip_handle_data_request(clip, hdr->format);
        return TRUE;
    case CLIP_MSG_FILE_DATA_REQ: {
        /* Guest requests one file's bytes (FileContents). hdr->data_size carries
           the 4-byte index into the pushed (flattened) file list. */
        UINT32 idx = 0;
        if (hdr->data_size >= 4) {
            if (!clip_recv_exact(s, &idx, 4)) return FALSE;
        }
        if (!clip_handle_file_data_request(clip, s, idx)) return FALSE;
        return TRUE;
    }
    default:
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!clip_recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

static DWORD WINAPI clip_writer_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;
    SOCKET s;
    ClipHeader hdr;

    for (;;) {
        s = clip->writer_socket;

        while (!clip->stop) {
            if (!clip_recv_exact(s, &hdr, sizeof(hdr))) break;
            if (hdr.magic != CLIP_MAGIC) break;
            if (!clip_writer_handle_message(clip, s, &hdr)) break;
        }

        /* Atomically claim the socket so the writer thread and clip_stop()
           never double-close the same handle (App Verifier invalid-handle). */
        {
            SOCKET old = (SOCKET)InterlockedExchangePointer(
                (PVOID volatile *)&clip->writer_socket, (PVOID)INVALID_SOCKET);
            if (old != INVALID_SOCKET) closesocket(old);
        }

        if (clip->stop) break;

        while (!clip->stop) {
            int wait;
            SOCKET ws;
            for (wait = 0; wait < 3000 && !clip->stop; wait += 500)
                Sleep(500);
            if (clip->stop) break;

            { GUID svc; hcs_service_guid(clip->os_type, 5, &svc);
              ws = clip_connect_hv(&clip->runtime_id, &svc, 2000); }
            if (ws != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (clip_recv_exact(ws, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(ws, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    clip->writer_socket = ws;
                    clip_log(clip, L"CLIP: Writer reconnected (GUID :0005).");
                    /* The guest side restarted, so it lost its clipboard and
                       the file it had staged. Force a fresh push even if the
                       host clipboard has not changed since the last one. */
                    InterlockedExchange(&clip->pushed_seq_valid, 0);
                    if (clip->sync_enabled) {
                        clip_queue_sync_enable(clip, TRUE);
                        clip_queue_push(clip);
                    } else {
                        clip_queue_sync_enable(clip, FALSE);
                    }
                    break;
                }
                closesocket(ws);
            }
        }

        if (clip->stop) break;
    }

    clip->writer_thread = NULL;
    return 0;
}

/* ---- Reader channel: file data receive ---- */

static BOOL clip_reader_recv_file_data(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    ClipFileInfo fi;
    wchar_t rel_path[MAX_PATH], full_path[MAX_PATH];

    if (hdr->data_size < sizeof(fi)) return FALSE;
    if (!clip_recv_exact(s, &fi, sizeof(fi))) return FALSE;

    if (fi.path_len == 0 || fi.path_len > (MAX_PATH - 1) * sizeof(wchar_t)) {
        UINT32 skip_hdr = hdr->data_size - (UINT32)sizeof(fi);
        BYTE skip[4096];
        while (skip_hdr > 0) {
            int chunk = skip_hdr > 4096 ? 4096 : (int)skip_hdr;
            if (!clip_recv_exact(s, skip, chunk)) return FALSE;
            skip_hdr -= chunk;
        }
        { UINT64 rem = fi.file_size;
          while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!clip_recv_exact(s, skip, c)) return FALSE; rem -= c; } }
        return TRUE;
    }

    if (!clip_recv_exact(s, rel_path, (int)fi.path_len)) return FALSE;
    rel_path[fi.path_len / sizeof(wchar_t)] = L'\0';

    if (!clip_path_is_safe(rel_path)) {
        BYTE skip[4096];
        UINT64 rem = fi.file_size;
        while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!clip_recv_exact(s, skip, c)) return FALSE; rem -= c; }
        return TRUE;
    }

    swprintf_s(full_path, MAX_PATH, L"%s\\%s", clip->temp_dir, rel_path);

    if (fi.is_directory) {
        clip_ensure_parent_dir(full_path);
        CreateDirectoryW(full_path, NULL);
    } else {
        HANDLE hFile;
        clip_ensure_parent_dir(full_path);
        hFile = CreateFileW(full_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            UINT64 remaining = fi.file_size;
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            if (!chunk) { CloseHandle(hFile); return FALSE; }
            while (remaining > 0) {
                DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
                DWORD written;
                if (!clip_recv_exact(s, chunk, (int)to_read)) {
                    HeapFree(GetProcessHeap(), 0, chunk);
                    CloseHandle(hFile);
                    return FALSE;
                }
                WriteFile(hFile, chunk, to_read, &written, NULL);
                remaining -= to_read;
            }
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
        } else {
            UINT64 remaining = fi.file_size;
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            if (!chunk) return FALSE;
            while (remaining > 0) {
                int c = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (int)remaining;
                if (!clip_recv_exact(s, chunk, c)) { HeapFree(GetProcessHeap(), 0, chunk); return FALSE; }
                remaining -= c;
            }
            HeapFree(GetProcessHeap(), 0, chunk);
        }
    }
    return TRUE;
}

/* ---- Reader channel: request next format or apply ---- */

static void clip_reader_free_pending(VmClipboard clip)
{
    int i;
    for (i = 0; i < clip->reader_fmt_count; i++) {
        if (clip->reader_fmts[i].data) {
            HeapFree(GetProcessHeap(), 0, clip->reader_fmts[i].data);
            clip->reader_fmts[i].data = NULL;
        }
    }
}

/* Caller holds reader_cs. */
static void clip_reader_free_meta_locked(VmClipboard clip)
{
    if (clip->reader_meta) {
        HeapFree(GetProcessHeap(), 0, clip->reader_meta);
        clip->reader_meta = NULL;
    }
    clip->reader_meta_count = 0;
}

static int clip_wait_readable(SOCKET s, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;

    if (s == INVALID_SOCKET) return -1;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return select(0, &rfds, NULL, NULL, &tv);
}

static void clip_reader_post_apply(VmClipboard clip)
{
    if (clip->hwnd && IsWindow(clip->hwnd))
        PostMessageW(clip->hwnd, WM_CLIP_READER_APPLY, 0, 0);
}

/* Ask the guest for the file bytes the old (eager) way.  Used for guests
   that do not support FILE_LIST, or when OLE advertisement failed. */
static void clip_reader_request_files_eager(VmClipboard clip, SOCKET s)
{
    ClipHeader hdr;

    if (clip->reader_files_requested) return;
    clip->reader_files_requested = TRUE;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
    hdr.format = CF_HDROP;
    hdr.data_size = 0;

    EnterCriticalSection(&clip->reader_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    LeaveCriticalSection(&clip->reader_cs);

    clip_log(clip, L"CLIP-R: Requesting files eagerly.");
}

static void clip_reader_maybe_apply(VmClipboard clip, SOCKET s)
{
    (void)s;

    if (!clip->reader_formats_done) return;

    if (!clip->reader_has_hdrop) {
        clip->reader_meta_deadline = 0;
        clip_reader_post_apply(clip);
        return;
    }

    if (clip->reader_meta_count > 0) {
        clip->reader_meta_deadline = 0;
        /* Apply on the reader thread so the display thread (and the guest's
           rendered frame) is never blocked waiting for OleSetClipboard, which
           can stall on a slow/hung clipboard viewer. */
        InterlockedExchange(&clip->writer_suppress, 1);
        if (clip->ole_event && clip->ole_done) {
            ResetEvent(clip->ole_done);
            SetEvent(clip->ole_event);
            WaitForSingleObject(clip->ole_done, 30000);
            if (InterlockedCompareExchange(&clip->ole_apply_ok, 0, 0)) {
                DWORD seq = GetClipboardSequenceNumber();
                if (seq != 0) {
                    InterlockedExchange(&clip->pushed_seq, (LONG)seq);
                    InterlockedExchange(&clip->pushed_seq_valid, 1);
                }
            } else {
                clip_log(clip, L"CLIP-R: Lazy file advertise failed; fetching eagerly.");
                InterlockedExchange(&clip->reader_want_eager, 1);
            }
        } else {
            /* No OLE thread (should not happen): fall back to eager. */
            InterlockedExchange(&clip->reader_want_eager, 1);
        }
        return;
    }

    if (!clip->reader_expect_meta) {
        /* Old guest: it sent CF_HDROP as part of the normal fetch, so the
           files are already in temp_dir. */
        clip->reader_meta_deadline = 0;
        clip_reader_post_apply(clip);
        return;
    }

    /* New guest promised metadata.  The walk can take a while on big trees,
       so wait; the deadline only covers a guest that dies mid-walk. */
    if (clip->reader_meta_deadline == 0)
        clip->reader_meta_deadline = GetTickCount64() + CLIP_META_WAIT_MS;
}

static void clip_reader_request_next(VmClipboard clip, SOCKET s)
{
    ClipHeader hdr;

    while (clip->reader_fetch_idx < clip->reader_fmt_count &&
           clip->reader_fmts[clip->reader_fetch_idx].is_hdrop)
        clip->reader_fetch_idx++;

    if (clip->reader_fetch_idx >= clip->reader_fmt_count) {
        clip->reader_formats_done = TRUE;
        clip_reader_maybe_apply(clip, s);
        return;
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
    hdr.format = clip->reader_fmts[clip->reader_fetch_idx].remote_id;
    hdr.data_size = 0;

    EnterCriticalSection(&clip->reader_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    LeaveCriticalSection(&clip->reader_cs);
}

static BOOL clip_reader_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr);

/* Serve one CLIP_MSG_FILE_DATA_REQ from the paste's IStream.  Runs on the
   reader thread and writes into the staging file that stream created, so it
   can drain it (and show progress) concurrently.  Returns FALSE when the
   connection must be torn down (cancel, validation failure, I/O error). */
static BOOL clip_reader_serve_file_request(VmClipboard clip, SOCKET s)
{
    ClipHeader rhdr, r;
    ClipFileInfo fi;
    UINT32 idx;
    wchar_t expect_name[MAX_PATH];
    UINT64 expect_size;
    BOOL ok = TRUE;
    HANDLE hf = INVALID_HANDLE_VALUE;
    BYTE *chunk = NULL;
    UINT64 unread = 0;
    BOOL discarding = FALSE;
    BOOL drop = FALSE;
    ULONGLONG reply_deadline;

    ZeroMemory(&fi, sizeof(fi));
    ZeroMemory(&r, sizeof(r));

    /* Take the request out of the slot.  Clear pending while still holding the
       lock: a stream can only claim the (still busy) slot after we free it. */
    EnterCriticalSection(&clip->xfer_cs);
    idx         = (UINT32)clip->file_req_idx;
    expect_size = clip->xfer_total;
    wcscpy_s(expect_name, MAX_PATH, clip->xfer_req_name);
    /* Take ownership of the staging-file handle the stream duplicated for us.
       Opening by path here raced an early IStream::Release, which deletes the
       path (ERROR_FILE_NOT_FOUND) while the reply is already on the wire. */
    hf = clip->xfer_hFile;
    clip->xfer_hFile = NULL;
    drop = clip->xfer_cancel;
    InterlockedExchange(&clip->file_req_pending, 0);
    LeaveCriticalSection(&clip->xfer_cs);

    if (drop) {
        /* The paste released its IStream before its request went out.  Do not
           ask the guest for a body (possibly gigabytes) that nobody will read:
           nothing has been sent yet, so the slot can be freed immediately and
           the next queued paste starts without waiting for a drain. */
        if (hf && hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        EnterCriticalSection(&clip->xfer_cs);
        clip->xfer_done   = TRUE;
        clip->xfer_active = FALSE;
        LeaveCriticalSection(&clip->xfer_cs);
        SetEvent(clip->xfer_event);
        clip_log(clip, L"CLIP-R: Request %u dropped before it was sent "
                       L"(paste abandoned).", idx);
        return TRUE;
    }

    reply_deadline = GetTickCount64() + CLIP_REPLY_WAIT_MS;

    rhdr.magic = CLIP_MAGIC;
    rhdr.msg_type = CLIP_MSG_FILE_DATA_REQ;
    rhdr.format = 0;
    rhdr.data_size = 4;

    EnterCriticalSection(&clip->reader_cs);
    if (!clip_send_all(s, &rhdr, sizeof(rhdr)) || !clip_send_all(s, &idx, 4)) {
        clip_log(clip, L"CLIP-R: Request %u: could not send request "
                       L"(guest closed the connection).", idx);
        ok = FALSE;
    }
    LeaveCriticalSection(&clip->reader_cs);

    if (ok) {
        /* The guest may interleave a fresh FORMAT_LIST / FILE_LIST or an eager
           CF_HDROP stream if the user copies again mid-paste; fold those in
           until the reply tagged with our index arrives.  The tag keeps an
           eager file body from being mistaken for the pasted file. */
        for (;;) {
            int wr;

            if (clip->stop) { ok = FALSE; break; }
            wr = clip_wait_readable(s, 200);
            if (wr < 0) {
                clip_log(clip, L"CLIP-R: Request %u: connection lost while "
                               L"waiting for the file reply.", idx);
                ok = FALSE;
                break;
            }
            if (wr == 0) {
                /* Bound the wait: a guest that never answers would otherwise
                   block this thread and every paste queued behind it. */
                if (GetTickCount64() >= reply_deadline) {
                    clip_log(clip, L"CLIP-R: Request %u: no file reply after "
                                   L"%lu s; rebuilding the connection.",
                             idx, (unsigned long)(CLIP_REPLY_WAIT_MS / 1000));
                    ok = FALSE;
                    break;
                }
                continue;
            }
            if (!clip_recv_exact(s, &r, sizeof(r)) || r.magic != CLIP_MAGIC) {
                clip_log(clip, L"CLIP-R: Request %u: connection lost while "
                               L"waiting for the file reply.", idx);
                ok = FALSE;
                break;
            }
            if (r.msg_type == CLIP_MSG_FILE_DATA && r.format == idx + 1) break;
            if (!clip_reader_handle_message(clip, s, &r)) {
                ok = FALSE;
                break;
            }
        }
    }

    if (ok) {
        if (r.data_size < sizeof(fi) || !clip_recv_exact(s, &fi, sizeof(fi))) {
            ok = FALSE;
        } else {
            unread = fi.file_size;
        }
    }

    if (ok) {
        clip_log(clip, L"CLIP-R: Serving request %u ('%ls', %llu bytes).",
                 idx, expect_name, (unsigned long long)expect_size);
        /* The guest serves by index into its newest file snapshot, so after the
           user copies something else an old advertised entry can map to a
           different file.  Verify the name and size and fail the paste rather
           than return the wrong bytes. */
        wchar_t rel[MAX_PATH];
        BOOL named = FALSE;

        rel[0] = L'\0';
        if (fi.path_len > 0 && fi.path_len <= (MAX_PATH - 1) * sizeof(wchar_t)) {
            if (!clip_recv_exact(s, rel, (int)fi.path_len)) {
                ok = FALSE;
            } else {
                rel[fi.path_len / sizeof(wchar_t)] = L'\0';
                named = TRUE;
            }
        } else if (fi.path_len > 0) {
            ok = FALSE;   /* bogus length: never drain a huge body */
        }

        if (ok && (!named || wcscmp(rel, expect_name) != 0 ||
                   (!fi.is_directory && fi.file_size != expect_size))) {
            clip_log(clip, L"CLIP-R: File reply does not match request %u (want '%ls', "
                           L"%llu bytes; got '%ls', %llu%s); aborting paste.",
                     idx, expect_name, (unsigned long long)expect_size,
                     named ? rel : L"", (unsigned long long)fi.file_size,
                     fi.is_directory ? L", directory" : L"");
            /* The guest has copied something else since this list was
               advertised.  Refresh it after the reconnect so the next paste
               does not fail the same way. */
            InterlockedExchange(&clip->reader_pull_needed, 1);
            ok = FALSE;
        }
    }

    if (ok && !fi.is_directory) {
        /* Released before the first byte arrived: discard the whole body
           instead of writing a file nobody will ever read. */
        EnterCriticalSection(&clip->xfer_cs);
        if (clip->xfer_cancel) discarding = TRUE;
        LeaveCriticalSection(&clip->xfer_cs);

        if (!hf || hf == INVALID_HANDLE_VALUE) {
            /* The requesting stream was released before we got here.  It has
               already deleted its staging file; the request is dead, but we
               still drain the body to keep the connection usable. */
            clip_log(clip, L"CLIP-R: Request %u has no staging handle "
                           L"(stream released); draining the body.", idx);
            discarding = TRUE;
        }
        {
            if (hf && hf != INVALID_HANDLE_VALUE)
                SetFilePointer(hf, 0, NULL, FILE_BEGIN);
            chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            while (ok && unread > 0) {
                DWORD to_read = unread > CLIP_FILE_CHUNK
                                    ? CLIP_FILE_CHUNK : (DWORD)unread;
                DWORD written = 0;
                BOOL cancel_now;
                if (!chunk || !clip_recv_exact(s, chunk, (int)to_read)) {
                    clip_log(clip, L"CLIP-R: Request %u body receive failed after "
                                   L"%llu of %llu bytes (guest cancelled?).",
                             idx, (unsigned long long)(fi.file_size - unread),
                             (unsigned long long)fi.file_size);
                    ok = FALSE;
                    break;
                }

                /* Read the abandon flag under the slot lock so it can never be
                   confused with a newer transfer's flag. */
                EnterCriticalSection(&clip->xfer_cs);
                cancel_now = clip->xfer_cancel;
                LeaveCriticalSection(&clip->xfer_cs);
                if (cancel_now && !discarding) {
                    discarding = TRUE;
                    clip_log(clip, L"CLIP-R: Request %u abandoned by the paste; "
                                   L"discarding the remaining %llu bytes.",
                             idx, (unsigned long long)unread);
                    if (hf && hf != INVALID_HANDLE_VALUE) {
                        CloseHandle(hf);
                        hf = INVALID_HANDLE_VALUE;
                    }
                }

                if (!discarding) {
                    if (!WriteFile(hf, chunk, to_read, &written, NULL) ||
                        written != to_read) {
                        clip_log(clip, L"CLIP-R: Request %u staging write failed after "
                                       L"%llu of %llu bytes (error %lu).",
                                 idx, (unsigned long long)(fi.file_size - unread),
                                 (unsigned long long)fi.file_size, GetLastError());
                        ok = FALSE;
                        break;
                    }
                }
                unread -= to_read;
                SetEvent(clip->xfer_event);
            }
            if (chunk) HeapFree(GetProcessHeap(), 0, chunk);
            chunk = NULL;
        }
    }

    if (hf && hf != INVALID_HANDLE_VALUE) {
        CloseHandle(hf);
        hf = INVALID_HANDLE_VALUE;
    }

    /* Never drain a rejected body (it can be gigabytes): returning FALSE tears
       the connection down, which also stops the guest from sending it. */
    if (!ok) {
        EnterCriticalSection(&clip->xfer_cs);
        clip->xfer_error = TRUE;
        clip->xfer_done = TRUE;
        InterlockedExchange(&clip->file_req_pending, 0);
        clip->xfer_active = FALSE;
        clip->xfer_hFile = NULL;
        LeaveCriticalSection(&clip->xfer_cs);
        SetEvent(clip->xfer_event);
        return FALSE;
    }

    EnterCriticalSection(&clip->xfer_cs);
    clip->xfer_done = TRUE;
    InterlockedExchange(&clip->file_req_pending, 0);
    clip->xfer_active = FALSE;
    clip->xfer_hFile = NULL;
    LeaveCriticalSection(&clip->xfer_cs);
    SetEvent(clip->xfer_event);

    return TRUE;
}

/* ---- Reader channel: handle messages ---- */

static BOOL clip_reader_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {

    case CLIP_MSG_FORMAT_LIST: {
        BYTE *buf;
        UINT32 count, i;
        int off;
        BOOL pulled;

        /* Need at least the 4-byte leading count; reject short frames so the
           `*(UINT32 *)buf` read below cannot over-read the allocation. */
        if (hdr->data_size < 4 || hdr->data_size > 16384) return FALSE;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
        if (!buf) return FALSE;
        if (!clip_recv_exact(s, buf, (int)hdr->data_size)) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }

        /* A new guest sends FILE_LIST right after this frame.  Until the list
           below is actually accepted, that metadata must be ignored: the
           guest sends it even when we discard the list (sync off / echo), and
           applying it would publish files the focus gate said to ignore. */
        clip->reader_list_accepted = FALSE;

        /* The host asked for this list after a stale request was refused.  It
           must be honoured even if the window is unfocused (the paste that
           failed may have been into Explorer, which leaves sync off) and it
           takes precedence over a pending echo token.  The token expires so an
           old guest (which ignores the request) cannot keep the focus gate
           bypassed for the rest of the connection. */
        pulled = (InterlockedCompareExchange(&clip->reader_pull_sent, 0, 0) != 0 &&
                  GetTickCount64() < clip->reader_pull_deadline);
        if (pulled) {
            InterlockedExchange(&clip->reader_pull_sent, 0);
            InterlockedExchange(&clip->reader_suppress, 0);
            clip_log(clip, L"CLIP-R: Accepting the requested clipboard list.");
        }

        /* Echo suppression */
        if (!pulled && InterlockedExchange(&clip->reader_suppress, 0)) {
            clip_log(clip, L"CLIP-R: FORMAT_LIST suppressed (echo from host->guest).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        /* Focus gate: discard if sync is off */
        if (!pulled && !clip->sync_enabled) {
            clip_log(clip, L"CLIP-R: FORMAT_LIST discarded (sync disabled).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        count = *(UINT32 *)buf;
        if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
        off = 4;

        /* A new-style guest appends CLIP_LAZY_MARKER after the entries to say
           it will follow up with CLIP_MSG_FILE_LIST and wants the files
           rendered lazily (on paste).  Old guests simply stop after the last
           entry, which also means the eager CF_HDROP path below. */
        {
            int lazy = (hdr->data_size >= 8 &&
                        *(UINT32 *)(buf + hdr->data_size - 4) == CLIP_LAZY_MARKER);

            EnterCriticalSection(&clip->reader_cs);
            clip_reader_free_pending(clip);
            clip_reader_free_meta_locked(clip);
            clip->reader_fmt_count = 0;
            clip->reader_has_hdrop = FALSE;
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
                }
                off += (int)name_len;

                clip->reader_fmts[clip->reader_fmt_count].local_id = local_id;
                clip->reader_fmts[clip->reader_fmt_count].remote_id = fmt_id;
                clip->reader_fmts[clip->reader_fmt_count].data = NULL;
                clip->reader_fmts[clip->reader_fmt_count].data_size = 0;
                /* With the lazy marker the guest will never send the CF_HDROP
                   blob, so mark it now: skip fetching it and let the OLE data
                   object supply the files. */
                clip->reader_fmts[clip->reader_fmt_count].is_hdrop =
                    (lazy && local_id == CF_HDROP);
                if (local_id == CF_HDROP) clip->reader_has_hdrop = TRUE;
                clip->reader_fmt_count++;
            }
            clip->reader_fetch_idx   = 0;
            clip->reader_expect_meta = lazy;
            clip->reader_files_requested = FALSE;
            clip->reader_formats_done = FALSE;
            clip->reader_meta_deadline = 0;
            InterlockedExchange(&clip->reader_want_eager, 0);
            clip->reader_list_accepted = TRUE;
            LeaveCriticalSection(&clip->reader_cs);
        }

        HeapFree(GetProcessHeap(), 0, buf);

        clip_remove_dir_recursive(clip->temp_dir);
        CreateDirectoryW(clip->temp_dir, NULL);

        clip_log(clip, L"CLIP-R: Received format list (%d formats)%s, fetching data...",
                 clip->reader_fmt_count,
                 clip->reader_expect_meta ? L" [lazy files]" : L"");
        clip_reader_request_next(clip, s);
        return TRUE;
    }

    case CLIP_MSG_FORMAT_DATA_RESP: {
        int idx = clip->reader_fetch_idx;

        if (idx >= 0 && idx < clip->reader_fmt_count) {
            if (hdr->format == CF_HDROP && hdr->data_size == 0) {
                clip->reader_fmts[idx].is_hdrop = TRUE;
            } else if (hdr->data_size > 0 && hdr->data_size <= CLIP_MAX_PAYLOAD) {
                BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
                if (data && clip_recv_exact(s, data, (int)hdr->data_size)) {
                    clip->reader_fmts[idx].data = data;
                    clip->reader_fmts[idx].data_size = hdr->data_size;
                } else {
                    if (data) HeapFree(GetProcessHeap(), 0, data);
                    return FALSE;
                }
            }
            clip->reader_fetch_idx++;
            clip_reader_request_next(clip, s);
        } else if (hdr->format == CF_HDROP && hdr->data_size == 0 &&
                   clip->reader_files_requested) {
            /* Eager fallback finished: the guest has streamed the files into
               temp_dir and this empty CF_HDROP response closes the request. */
            clip_reader_post_apply(clip);
        }
        return TRUE;
    }

    case CLIP_MSG_FILE_LIST: {
        /* Guest->host lazy file metadata.  Wire shape matches the host->guest
           FILE_LIST: count(4), then per entry name_len(4) + UTF-16 name +
           size(8) + attrs(4) + is_dir(1). */
        BYTE *buf;
        UINT32 count, i;
        int off;
        ClipMeta *meta = NULL;
        int meta_count = 0;

        if (hdr->data_size < 4 || hdr->data_size > CLIP_MAX_PAYLOAD) return FALSE;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
        if (!buf) return FALSE;
        if (!clip_recv_exact(s, buf, (int)hdr->data_size)) {
            HeapFree(GetProcessHeap(), 0, buf);
            return FALSE;
        }

        /* Only metadata that belongs to the format list we just accepted may
           replace the current file set; see the reset in FORMAT_LIST. */
        if (!clip->reader_list_accepted) {
            HeapFree(GetProcessHeap(), 0, buf);
            clip_log(clip, L"CLIP-R: Ignored file metadata (no matching format list).");
            return TRUE;
        }

        count = *(UINT32 *)buf;
        if (count > CLIP_MAX_META) count = CLIP_MAX_META;
        if (count > 0) {
            meta = (ClipMeta *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         sizeof(ClipMeta) * (size_t)count);
            if (!meta) count = 0;
        }

        off = 4;
        for (i = 0; i < count; i++) {
            UINT32 name_len;
            UINT64 size;
            UINT32 attrs;
            int is_dir;

            if (off + 4 > (int)hdr->data_size) break;
            name_len = *(UINT32 *)(buf + off); off += 4;
            if (name_len > (UINT32)((int)hdr->data_size - off)) break;
            if (name_len >= MAX_PATH * sizeof(wchar_t)) break;
            if ((int)name_len + 8 + 4 + 1 > (int)hdr->data_size - off) break;

            memcpy(meta[meta_count].name, buf + off, name_len);
            meta[meta_count].name[name_len / sizeof(wchar_t)] = L'\0';
            off += (int)name_len;
            memcpy(&size, buf + off, 8); off += 8;
            memcpy(&attrs, buf + off, 4); off += 4;
            is_dir = buf[off++] ? 1 : 0;

            meta[meta_count].size   = size;
            meta[meta_count].attrs  = attrs;
            meta[meta_count].is_dir = is_dir;
            meta_count++;
        }

        HeapFree(GetProcessHeap(), 0, buf);

        EnterCriticalSection(&clip->reader_cs);
        clip_reader_free_meta_locked(clip);
        if (meta_count > 0) {
            clip->reader_meta = meta;
            clip->reader_meta_count = meta_count;
        } else if (meta) {
            HeapFree(GetProcessHeap(), 0, meta);
        }
        clip->reader_meta_deadline = 0;
        LeaveCriticalSection(&clip->reader_cs);

        clip_log(clip, L"CLIP-R: Received file metadata (%d entries).", meta_count);

        if (!clip->reader_files_requested) clip_reader_maybe_apply(clip, s);
        return TRUE;
    }

    case CLIP_MSG_FILE_DATA:
        CreateDirectoryW(clip->temp_dir, NULL);
        return clip_reader_recv_file_data(clip, s, hdr);

    default:
        if (hdr->data_size > 0) {
            BYTE skip[256];
            UINT32 remaining = hdr->data_size;
            while (remaining > 0) {
                int chunk = remaining > 256 ? 256 : (int)remaining;
                if (!clip_recv_exact(s, skip, chunk)) return FALSE;
                remaining -= chunk;
            }
        }
        return TRUE;
    }
}

/* ---- Reader recv thread ---- */

/* Ask the guest to send its current clipboard list.  Used after a stale
   request was refused: the host's advertisement no longer matches the guest,
   and the guest will not send a new list on its own while sync is off.  An
   old guest logs "Unknown msg_type 7" and skips the frame, so the connection
   stays usable. */
static void clip_reader_request_list(VmClipboard clip, SOCKET s)
{
    ClipHeader hdr;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_PULL_LIST;
    hdr.format = 0;
    hdr.data_size = 0;

    InterlockedExchange(&clip->reader_pull_sent, 1);
    clip->reader_pull_deadline = GetTickCount64() + CLIP_PULL_WAIT_MS;
    EnterCriticalSection(&clip->reader_cs);
    clip_send_all(s, &hdr, sizeof(hdr));
    LeaveCriticalSection(&clip->reader_cs);
    clip_log(clip, L"CLIP-R: Asked the guest to re-send its clipboard list "
                   L"(stale advertisement).");
}

static DWORD WINAPI clip_reader_thread_proc(LPVOID param)
{
    VmClipboard clip = (VmClipboard)param;
    SOCKET s;
    ClipHeader hdr;

    while (!clip->stop) {
        while (!clip->stop) {
            int wait;
            SOCKET rs;

            if (clip->reader_socket != INVALID_SOCKET) break;

            { GUID svc; hcs_service_guid(clip->os_type, 6, &svc);
              rs = clip_connect_hv(&clip->runtime_id, &svc, 2000); }
            if (rs != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (clip_recv_exact(rs, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(rs, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    clip->reader_socket = rs;
                    InterlockedExchange(&clip->reader_link_up, 1);
                    clip_log(clip, L"CLIP-R: Connected (GUID :0006).");
                    break;
                }
                closesocket(rs);
            }

            for (wait = 0; wait < 3000 && !clip->stop; wait += 500)
                Sleep(500);
        }

        if (clip->stop) break;

        s = clip->reader_socket;

        /* The previous connection was dropped by a refused (stale) request:
           ask the guest for its current list before serving anything else. */
        if (InterlockedExchange(&clip->reader_pull_needed, 0))
            clip_reader_request_list(clip, s);

        while (!clip->stop) {
            int r;

            /* An IStream on the paste side asked for one file: serve it before
               touching the socket for anything else. */
            if (InterlockedCompareExchange(&clip->file_req_pending, 1, 1)) {
                if (!clip_reader_serve_file_request(clip, s)) break;
                continue;
            }

            /* The OLE advertisement failed; the apply handler asked for the
               files to be fetched the eager way instead. */
            if (InterlockedCompareExchange(&clip->reader_want_eager, 0, 1)) {
                if (!clip->reader_files_requested)
                    clip_reader_request_files_eager(clip, s);
            }

            /* Guest promised FILE_LIST but never sent it (crashed mid-walk).  Do
               NOT fall back to an eager 10 GB transfer: that is what froze the
               guest.  Apply whatever non-file formats we have (if any) and drop
               the file list; a retry will re-send it. */
            if (clip->reader_meta_deadline != 0 &&
                GetTickCount64() >= clip->reader_meta_deadline) {
                clip->reader_meta_deadline = 0;
                clip->reader_expect_meta = FALSE;
                clip_log(clip, L"CLIP-R: Timed out waiting for file metadata; "
                               L"not fetching eagerly (lazy promise dropped).");
                clip_reader_post_apply(clip);
            }

            r = clip_wait_readable(s, 50);
            if (r < 0) break;
            if (r == 0) continue;

            if (!clip_recv_exact(s, &hdr, sizeof(hdr))) break;
            if (hdr.magic != CLIP_MAGIC) break;
            if (!clip_reader_handle_message(clip, s, &hdr)) break;
        }

        InterlockedExchange(&clip->reader_link_up, 0);
        InterlockedExchange(&clip->reader_pull_sent, 0);
        clip->reader_pull_deadline = 0;
        clip_reader_free_pending(clip);
        /* The reader thread is gone (or about to reconnect): no one can serve
           an outstanding request.  Close the duplicated staging handle and
           fail the active transfer so its IStream::Read returns immediately
           instead of waiting for the 120 s slot deadline.  Streams queued
           behind it can then claim the slot again after the reconnect. */
        EnterCriticalSection(&clip->xfer_cs);
        if (clip->xfer_hFile && clip->xfer_hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(clip->xfer_hFile);
            clip->xfer_hFile = NULL;
        }
        if (clip->xfer_active) {
            clip_log(clip, L"CLIP-R: Reader went away during a file transfer; "
                           L"failing it.");
            clip->xfer_error  = TRUE;
            clip->xfer_done   = TRUE;
            clip->xfer_active = FALSE;
            InterlockedExchange(&clip->file_req_pending, 0);
            SetEvent(clip->xfer_event);
        }
        LeaveCriticalSection(&clip->xfer_cs);
        clip_remove_dir_recursive(clip->temp_dir);
        /* Atomically claim the socket so the reader thread and clip_stop()
           never double-close the same handle (App Verifier invalid-handle). */
        {
            SOCKET old = (SOCKET)InterlockedExchangePointer(
                (PVOID volatile *)&clip->reader_socket, (PVOID)INVALID_SOCKET);
            if (old != INVALID_SOCKET) closesocket(old);
        }

        if (clip->stop) break;
    }

    clip->reader_thread = NULL;
    return 0;
}

/* ---- Public API ---- */

ASB_API VmClipboard vm_clipboard_create(const GUID *runtime_id,
                                        const wchar_t *os_type,
                                        HWND hwnd,
                                        VmClipboardLogFn log_fn, void *log_ud)
{
    VmClipboard clip;
    wchar_t tmp[MAX_PATH];
    SOCKET writer_s;
    GUID svc;

    clip = (VmClipboard)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*clip));
    if (!clip) return NULL;

    clip_init_ole_formats();

    clip->runtime_id = *runtime_id;
    if (os_type) wcscpy_s(clip->os_type, 32, os_type);
    clip->hwnd = hwnd;
    clip->log_fn = log_fn;
    clip->log_ud = log_ud;
    clip->stop = FALSE;
    clip->sync_enabled = FALSE;
    clip->writer_socket = INVALID_SOCKET;
    clip->reader_socket = INVALID_SOCKET;
    clip->writer_suppress = 0;
    clip->reader_suppress = 0;
    clip->pending_sync = -1;
    clip->pending_push = 0;
    clip->pushed_seq = 0;
    clip->pushed_seq_valid = 0;
    clip->pushed_path_count = 0;
    clip->reader_pull_needed = 0;
    clip->reader_pull_sent = 0;
    clip->reader_pull_deadline = 0;

    InitializeCriticalSection(&clip->writer_cs);
    InitializeCriticalSection(&clip->reader_cs);
    InitializeCriticalSection(&clip->pushed_paths_cs);
    InitializeCriticalSection(&clip->xfer_cs);

    clip->push_event    = CreateEventW(NULL, FALSE, FALSE, NULL);
    clip->xfer_event    = CreateEventW(NULL, FALSE, FALSE, NULL);
    clip->ole_event     = CreateEventW(NULL, FALSE, FALSE, NULL);
    clip->ole_done      = CreateEventW(NULL, FALSE, FALSE, NULL);
    clip->ole_ready_evt = CreateEventW(NULL, FALSE, FALSE, NULL);

    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(clip->temp_dir, MAX_PATH, L"%sAppSandboxClip", tmp);
    CreateDirectoryW(clip->temp_dir, NULL);

    /* Staging dir for lazily pasted guest files; separate from temp_dir so a
       new format list cannot delete a file an in-flight paste is reading. */
    swprintf_s(clip->lazy_dir, MAX_PATH, L"%sAppSandboxClipLazy", tmp);
    clip_remove_dir_recursive(clip->lazy_dir);
    CreateDirectoryW(clip->lazy_dir, NULL);

    /* Initial writer connection */
    hcs_service_guid(clip->os_type, 5, &svc);
    writer_s = clip_connect_hv(runtime_id, &svc, 2000);
    if (writer_s != INVALID_SOCKET) {
        UINT32 ready_magic = 0;
        if (clip_recv_exact(writer_s, &ready_magic, sizeof(ready_magic)) &&
            ready_magic == CLIP_READY_MAGIC) {
            DWORD no_timeout = 0;
            setsockopt(writer_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
            clip->writer_socket = writer_s;
        } else {
            closesocket(writer_s);
        }
    }

    clip->push_thread   = CreateThread(NULL, 0, clip_push_thread_proc, clip, 0, NULL);
    clip->writer_thread = CreateThread(NULL, 0, clip_writer_thread_proc, clip, 0, NULL);
    clip->ole_thread    = CreateThread(NULL, 0, clip_ole_thread_proc, clip, 0, NULL);
    clip->reader_thread = CreateThread(NULL, 0, clip_reader_thread_proc, clip, 0, NULL);

    /* Sync starts out disabled - tell the guest. */
    clip_queue_sync_enable(clip, FALSE);

    return clip;
}

ASB_API void vm_clipboard_destroy(VmClipboard clip)
{
    if (!clip) return;

    clip->stop = TRUE;

    /* Atomically claim each socket so only one side ever closes it; the writer/
       reader threads close their own via the same exchange (no double-close). */
    {
        SOCKET old = (SOCKET)InterlockedExchangePointer(
            (PVOID volatile *)&clip->writer_socket, (PVOID)INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);
    }
    {
        SOCKET old = (SOCKET)InterlockedExchangePointer(
            (PVOID volatile *)&clip->reader_socket, (PVOID)INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);
    }

    if (clip->writer_thread) {
        WaitForSingleObject(clip->writer_thread, 3000);
        CloseHandle(clip->writer_thread);
    }
    if (clip->reader_thread) {
        WaitForSingleObject(clip->reader_thread, 3000);
        CloseHandle(clip->reader_thread);
    }

    /* The OLE data object lives on its own STA thread; stop it only after the
       reader thread (which serves its file requests) is gone. */
    InterlockedExchange(&clip->ole_stop, 1);
    if (clip->ole_event) SetEvent(clip->ole_event);
    if (clip->ole_thread) {
        WaitForSingleObject(clip->ole_thread, 5000);
        CloseHandle(clip->ole_thread);
        clip->ole_thread = NULL;
    }
    /* The push worker waits on writer_cs while the writer thread streams a
       file, so it is joined after the writer thread has released the lock. */
    if (clip->push_event) SetEvent(clip->push_event);
    if (clip->push_thread) {
        WaitForSingleObject(clip->push_thread, 3000);
        CloseHandle(clip->push_thread);
    }
    if (clip->push_event) {
        CloseHandle(clip->push_event);
        clip->push_event = NULL;
    }
    if (clip->xfer_event) { CloseHandle(clip->xfer_event); clip->xfer_event = NULL; }
    if (clip->ole_event)  { CloseHandle(clip->ole_event);  clip->ole_event  = NULL; }
    if (clip->ole_done)   { CloseHandle(clip->ole_done);   clip->ole_done   = NULL; }
    if (clip->ole_ready_evt) { CloseHandle(clip->ole_ready_evt); clip->ole_ready_evt = NULL; }

    clip_reader_free_pending(clip);
    clip_reader_free_meta_locked(clip);
    if (clip->xfer_hFile && clip->xfer_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(clip->xfer_hFile);
        clip->xfer_hFile = NULL;
    }
    if (clip->file_snap) {
        HeapFree(GetProcessHeap(), 0, clip->file_snap);
        clip->file_snap = NULL;
        clip->file_snap_count = 0;
    }
    clip_remove_dir_recursive(clip->temp_dir);
    clip_remove_dir_recursive(clip->lazy_dir);

    DeleteCriticalSection(&clip->writer_cs);
    DeleteCriticalSection(&clip->reader_cs);
    DeleteCriticalSection(&clip->pushed_paths_cs);
    DeleteCriticalSection(&clip->xfer_cs);

    HeapFree(GetProcessHeap(), 0, clip);
}

ASB_API void vm_clipboard_set_sync_enabled(VmClipboard clip, BOOL enabled)
{
    if (!clip) return;
    if ((BOOL)clip->sync_enabled == enabled) return;

    clip->sync_enabled = enabled;
    clip_log(clip, L"CLIP: Window %s — clipboard sync %s.",
             enabled ? L"focused" : L"unfocused",
             enabled ? L"ON" : L"OFF");

    /* Queue rather than send: the UI thread must not block on the clipboard
       socket, which is held for the whole of a file transfer. SYNC_ENABLE is
       queued before the format list so the guest sees them in that order. */
    clip_queue_sync_enable(clip, enabled);

    if (enabled) {
        clip_queue_push(clip);
    }
}

ASB_API void vm_clipboard_on_clipboard_update(VmClipboard clip)
{
    if (!clip) return;

    /* Echo suppression */
    if (InterlockedExchange(&clip->writer_suppress, 0)) {
        clip_log(clip, L"CLIP: WM_CLIPBOARDUPDATE suppressed (echo).");
        return;
    }

    /* Focus gate */
    if (!clip->sync_enabled) return;

    clip_queue_push(clip);
}

ASB_API void vm_clipboard_on_reader_apply(VmClipboard clip)
{
    int i;
    BOOL lazy;

    if (!clip) return;

    EnterCriticalSection(&clip->reader_cs);
    if (clip->reader_fmt_count == 0) {
        LeaveCriticalSection(&clip->reader_cs);
        return;
    }
    lazy = (clip->reader_meta_count > 0 &&
            clip->reader_expect_meta &&
            !clip->reader_files_requested &&
            !InterlockedCompareExchange(&clip->reader_want_eager, 0, 0));
    LeaveCriticalSection(&clip->reader_cs);

    /* Guest->host files, lazy path: publish an OLE data object that streams
       each file from the guest only when an application pastes it.  The
       object must be set on an STA, which is the dedicated OLE thread. */
    if (lazy) {
        if (clip->ole_event && clip->ole_done) {
            /* Auto-reset; drop any stale signal from a previous timed-out
               request so this wait cannot return before the work is done. */
            ResetEvent(clip->ole_done);
            SetEvent(clip->ole_event);
            WaitForSingleObject(clip->ole_done, 30000);
            if (InterlockedCompareExchange(&clip->ole_apply_ok, 0, 0)) {
                DWORD seq = GetClipboardSequenceNumber();
                if (seq != 0) {
                    InterlockedExchange(&clip->pushed_seq, (LONG)seq);
                    InterlockedExchange(&clip->pushed_seq_valid, 1);
                }
            } else {
                clip_log(clip, L"CLIP-R: Lazy file advertise failed; fetching eagerly.");
                InterlockedExchange(&clip->reader_want_eager, 1);
            }
            return;
        }
        /* No OLE thread (should not happen): fall through to eager. */
        InterlockedExchange(&clip->reader_want_eager, 1);
        return;
    }

    EnterCriticalSection(&clip->reader_cs);
    if (clip->reader_fmt_count == 0) {
        LeaveCriticalSection(&clip->reader_cs);
        return;
    }

    InterlockedExchange(&clip->writer_suppress, 1);

    if (!OpenClipboard(clip->hwnd)) {
        InterlockedExchange(&clip->writer_suppress, 0);
        clip_reader_free_pending(clip);
        LeaveCriticalSection(&clip->reader_cs);
        return;
    }

    EmptyClipboard();

    for (i = 0; i < clip->reader_fmt_count; i++) {
        if (clip->reader_fmts[i].is_hdrop) {
            HGLOBAL hDrop = clip_build_hdrop_from_temp(clip->temp_dir);
            if (hDrop)
                SetClipboardData(clip->reader_fmts[i].local_id, hDrop);
        } else if (clip->reader_fmts[i].data && clip->reader_fmts[i].data_size > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, clip->reader_fmts[i].data_size);
            if (hMem) {
                void *ptr = GlobalLock(hMem);
                if (ptr) {
                    memcpy(ptr, clip->reader_fmts[i].data, clip->reader_fmts[i].data_size);
                    GlobalUnlock(hMem);
                    SetClipboardData(clip->reader_fmts[i].local_id, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
        }
    }

    /* The host clipboard now holds the guest's own content, and the guest
       already has it. Remember the new sequence number as "already pushed" so
       the next focus change does not push it straight back - that would make
       the guest delete and re-download its own file. Read it while the
       clipboard is still ours, so no other app can have changed it in
       between and made us record somebody else's content. */
    {
        DWORD seq = GetClipboardSequenceNumber();
        if (seq != 0) {
            InterlockedExchange(&clip->pushed_seq, (LONG)seq);
            InterlockedExchange(&clip->pushed_seq_valid, 1);
        }
    }

    CloseClipboard();
    InterlockedExchange(&clip->writer_suppress, 0);

    clip_log(clip, L"CLIP-R: Applied %d formats to host clipboard.", clip->reader_fmt_count);
    clip_reader_free_pending(clip);
    LeaveCriticalSection(&clip->reader_cs);
}
