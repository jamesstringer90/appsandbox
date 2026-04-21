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
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

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
/* CLIP_MSG_SYNC_ENABLE = 12 (defined in header) */

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)
#define CLIP_FILE_CHUNK     (1024 * 1024)

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

/* ---- Instance data ---- */

struct VmClipboardData {
    GUID             runtime_id;
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

    /* Reader channel (:0006 guest->host) */
    volatile SOCKET  reader_socket;
    volatile LONG    reader_suppress;
    HANDLE           reader_thread;
    CRITICAL_SECTION reader_cs;
    ClipFmtEntry     reader_fmts[CLIP_MAX_FORMATS];
    int              reader_fmt_count;
    int              reader_fetch_idx;

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

static BOOL clip_should_skip(UINT fmt)
{
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
        wcscpy_s(ptr, MAX_PATH, paths[i]);
        ptr += wcslen(paths[i]) + 1;
    }
    *ptr = L'\0';

    GlobalUnlock(hMem);
    return hMem;
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

static void clip_send_format_list(VmClipboard clip)
{
    ClipHeader hdr;
    SOCKET s;
    UINT fmt;
    BYTE buf[16384];
    int offset = 4;
    UINT32 count = 0;

    s = clip->writer_socket;
    if (s == INVALID_SOCKET) return;

    if (!OpenClipboard(clip->hwnd)) return;

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

    clip_log(clip, L"CLIP: Sent format list (%u formats) to guest.", count);
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
        if (OpenClipboard(NULL)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)hData;
                UINT file_count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                UINT fi;
                EnterCriticalSection(&clip->writer_cs);
                for (fi = 0; fi < file_count; fi++) {
                    wchar_t path[MAX_PATH];
                    wchar_t *name;
                    DragQueryFileW(hDrop, fi, path, MAX_PATH);
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
            } else {
                CloseClipboard();
                goto send_empty;
            }
            CloseClipboard();
        } else {
            goto send_empty;
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

static BOOL clip_writer_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {
    case CLIP_MSG_FORMAT_DATA_REQ:
        clip_handle_data_request(clip, hdr->format);
        return TRUE;
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

        closesocket(clip->writer_socket);
        clip->writer_socket = INVALID_SOCKET;

        if (clip->stop) break;

        while (!clip->stop) {
            int wait;
            SOCKET ws;
            for (wait = 0; wait < 3000 && !clip->stop; wait += 500)
                Sleep(500);
            if (clip->stop) break;

            ws = clip_connect_hv(&clip->runtime_id, &CLIPBOARD_SERVICE_GUID, 2000);
            if (ws != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (clip_recv_exact(ws, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(ws, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    clip->writer_socket = ws;
                    clip_log(clip, L"CLIP: Writer reconnected (GUID :0005).");
                    if (clip->sync_enabled) {
                        clip_send_sync_enable(clip, TRUE);
                        clip_send_format_list(clip);
                    } else {
                        clip_send_sync_enable(clip, FALSE);
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

static void clip_reader_request_next(VmClipboard clip, SOCKET s)
{
    ClipHeader hdr;

    if (clip->reader_fetch_idx >= clip->reader_fmt_count) {
        if (clip->hwnd && IsWindow(clip->hwnd))
            PostMessageW(clip->hwnd, WM_CLIP_READER_APPLY, 0, 0);
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

/* ---- Reader channel: handle messages ---- */

static BOOL clip_reader_handle_message(VmClipboard clip, SOCKET s, const ClipHeader *hdr)
{
    switch (hdr->msg_type) {

    case CLIP_MSG_FORMAT_LIST: {
        BYTE *buf;
        UINT32 count, i;
        int off;

        if (hdr->data_size > 16384) return FALSE;
        buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr->data_size);
        if (!buf) return FALSE;
        if (!clip_recv_exact(s, buf, (int)hdr->data_size)) { HeapFree(GetProcessHeap(), 0, buf); return FALSE; }

        /* Echo suppression */
        if (InterlockedExchange(&clip->reader_suppress, 0)) {
            clip_log(clip, L"CLIP-R: FORMAT_LIST suppressed (echo from host->guest).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        /* Focus gate: discard if sync is off */
        if (!clip->sync_enabled) {
            clip_log(clip, L"CLIP-R: FORMAT_LIST discarded (sync disabled).");
            HeapFree(GetProcessHeap(), 0, buf);
            return TRUE;
        }

        count = *(UINT32 *)buf;
        if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
        off = 4;

        EnterCriticalSection(&clip->reader_cs);
        clip_reader_free_pending(clip);
        clip->reader_fmt_count = 0;
        for (i = 0; i < count && off + 8 <= (int)hdr->data_size; i++) {
            UINT32 fmt_id   = *(UINT32 *)(buf + off); off += 4;
            UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
            UINT local_id = fmt_id;

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
            clip->reader_fmts[clip->reader_fmt_count].is_hdrop = FALSE;
            clip->reader_fmt_count++;
        }
        clip->reader_fetch_idx = 0;
        LeaveCriticalSection(&clip->reader_cs);

        HeapFree(GetProcessHeap(), 0, buf);

        clip_remove_dir_recursive(clip->temp_dir);
        CreateDirectoryW(clip->temp_dir, NULL);

        clip_log(clip, L"CLIP-R: Received format list (%d formats), fetching data...",
                 clip->reader_fmt_count);
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
        }
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

            rs = clip_connect_hv(&clip->runtime_id, &CLIPBOARD_READER_SERVICE_GUID, 2000);
            if (rs != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (clip_recv_exact(rs, &ready_magic, sizeof(ready_magic)) &&
                    ready_magic == CLIP_READY_MAGIC) {
                    DWORD no_timeout = 0;
                    setsockopt(rs, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
                    clip->reader_socket = rs;
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
        while (!clip->stop) {
            if (!clip_recv_exact(s, &hdr, sizeof(hdr))) break;
            if (hdr.magic != CLIP_MAGIC) break;
            if (!clip_reader_handle_message(clip, s, &hdr)) break;
        }

        clip_reader_free_pending(clip);
        clip_remove_dir_recursive(clip->temp_dir);
        closesocket(clip->reader_socket);
        clip->reader_socket = INVALID_SOCKET;

        if (clip->stop) break;
    }

    clip->reader_thread = NULL;
    return 0;
}

/* ---- Public API ---- */

ASB_API VmClipboard vm_clipboard_create(const GUID *runtime_id, HWND hwnd,
                                        VmClipboardLogFn log_fn, void *log_ud)
{
    VmClipboard clip;
    wchar_t tmp[MAX_PATH];
    SOCKET writer_s;

    clip = (VmClipboard)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*clip));
    if (!clip) return NULL;

    clip->runtime_id = *runtime_id;
    clip->hwnd = hwnd;
    clip->log_fn = log_fn;
    clip->log_ud = log_ud;
    clip->stop = FALSE;
    clip->sync_enabled = FALSE;
    clip->writer_socket = INVALID_SOCKET;
    clip->reader_socket = INVALID_SOCKET;
    clip->writer_suppress = 0;
    clip->reader_suppress = 0;

    InitializeCriticalSection(&clip->writer_cs);
    InitializeCriticalSection(&clip->reader_cs);

    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(clip->temp_dir, MAX_PATH, L"%sAppSandboxClip", tmp);
    CreateDirectoryW(clip->temp_dir, NULL);

    /* Initial writer connection */
    writer_s = clip_connect_hv(runtime_id, &CLIPBOARD_SERVICE_GUID, 2000);
    if (writer_s != INVALID_SOCKET) {
        UINT32 ready_magic = 0;
        if (clip_recv_exact(writer_s, &ready_magic, sizeof(ready_magic)) &&
            ready_magic == CLIP_READY_MAGIC) {
            DWORD no_timeout = 0;
            setsockopt(writer_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
            clip->writer_socket = writer_s;
            clip_send_sync_enable(clip, FALSE);
        } else {
            closesocket(writer_s);
        }
    }

    clip->writer_thread = CreateThread(NULL, 0, clip_writer_thread_proc, clip, 0, NULL);
    clip->reader_thread = CreateThread(NULL, 0, clip_reader_thread_proc, clip, 0, NULL);

    return clip;
}

ASB_API void vm_clipboard_destroy(VmClipboard clip)
{
    if (!clip) return;

    clip->stop = TRUE;

    if (clip->writer_socket != INVALID_SOCKET) {
        closesocket(clip->writer_socket);
        clip->writer_socket = INVALID_SOCKET;
    }
    if (clip->reader_socket != INVALID_SOCKET) {
        closesocket(clip->reader_socket);
        clip->reader_socket = INVALID_SOCKET;
    }

    if (clip->writer_thread) {
        WaitForSingleObject(clip->writer_thread, 3000);
        CloseHandle(clip->writer_thread);
    }
    if (clip->reader_thread) {
        WaitForSingleObject(clip->reader_thread, 3000);
        CloseHandle(clip->reader_thread);
    }

    clip_reader_free_pending(clip);
    clip_remove_dir_recursive(clip->temp_dir);

    DeleteCriticalSection(&clip->writer_cs);
    DeleteCriticalSection(&clip->reader_cs);

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
    clip_send_sync_enable(clip, enabled);

    if (enabled) {
        clip_send_format_list(clip);
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

    clip_send_format_list(clip);
}

ASB_API void vm_clipboard_on_reader_apply(VmClipboard clip)
{
    int i;
    if (!clip) return;

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

    CloseClipboard();
    InterlockedExchange(&clip->writer_suppress, 0);
    clip_log(clip, L"CLIP-R: Applied %d formats to host clipboard.", clip->reader_fmt_count);
    clip_reader_free_pending(clip);
    LeaveCriticalSection(&clip->reader_cs);
}
