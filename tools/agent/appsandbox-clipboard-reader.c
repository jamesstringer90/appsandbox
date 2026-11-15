/*
 * appsandbox-clipboard-reader.exe — Guest-side clipboard reader for AppSandbox.
 *
 * Runs as the interactive USER (spawned by agent via WTSQueryUserToken)
 * so it can fully enumerate and read the user's clipboard.
 *
 * Listens on AF_HYPERV socket (GUID :0006) for the host IDD display to
 * connect.  Handles the guest->host clipboard direction:
 *   - Monitors clipboard via AddClipboardFormatListener
 *   - On WM_CLIPBOARDUPDATE: enumerates formats, sends FORMAT_LIST to host
 *   - On FORMAT_DATA_REQ from host: reads clipboard data, sends
 *     FORMAT_DATA_RESP (including FILE_DATA for CF_HDROP)
 *
 * Host->guest clipboard is handled separately by appsandbox-clipboard.exe
 * (SYSTEM, GUID :0005).
 *
 * Logs to %TEMP%\clipboard-reader.log (user-writable).
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <shellapi.h>
#include <shlobj.h>
#include "../transport/asb_transport.h"   /* AF_HYPERV on PC, ivshmem on Mac */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

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

/* Clipboard reader channel: {A5B0CAFE-0006-4000-8000-000000000001} */
static const GUID CLIPBOARD_READER_SERVICE_GUID =
    { 0xa5b0cafe, 0x0006, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Clipboard protocol (must match host-side vm_display_idd.c) ---- */

#define CLIP_MAGIC          0x504C4341  /* "ACLP" little-endian */
#define CLIP_READY_MAGIC    0x59444C43  /* "CLDY" little-endian */

#define CLIP_MSG_FORMAT_LIST      1
#define CLIP_MSG_FORMAT_DATA_REQ  2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA        4
#define CLIP_MSG_FILE_LIST        5   /* guest -> host: file metadata for lazy files */
#define CLIP_MSG_FILE_DATA_REQ    6   /* host -> guest: request one file's bytes */
#define CLIP_MSG_PULL_LIST        7   /* host -> guest: re-send the current list */

/* Appended after the FORMAT_LIST entries when the guest will follow up with
   CLIP_MSG_FILE_LIST and wants files rendered lazily (on paste).  Old hosts
   stop parsing at the last entry and ignore the trailing bytes. */
#define CLIP_LAZY_MARKER          0x5A594C46u   /* "FLZY" */

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)  /* 64 MB */
#define CLIP_FILE_CHUNK     (1024 * 1024)        /* 1 MB */
#define CLIP_META_MAX       4096
#define CLIP_ROOT_MAX       64
#define CLIP_WALK_MAX_DEPTH 64

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 msg_type;
    UINT32 format;
    UINT32 data_size;
} ClipHeader;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    UINT32 path_len;     /* relative path length in bytes (UTF-16LE, no null) */
    UINT64 file_size;    /* file data bytes that follow (0 for directories) */
    UINT8  is_directory; /* 1 = directory entry, 0 = regular file */
} ClipFileInfo;
#pragma pack(pop)

/* ---- Globals ---- */

static AsbConn * volatile g_client_sock = NULL;
static CRITICAL_SECTION g_send_cs;
static HWND g_msg_hwnd = NULL;
static char g_log_path[MAX_PATH];

/* Focus-gated sync: the clipboard writer (SYSTEM, :0005) creates this
 * named event and sets/resets it when the host sends SYNC_ENABLE.
 * We check it before sending FORMAT_LIST to the host. */
static HANDLE g_sync_event = NULL;
#define CLIP_SYNC_EVENT_NAME L"Global\\AppSandboxClipSync"

static BOOL clip_sync_is_enabled(void)
{
    if (!g_sync_event) return TRUE;
    return WaitForSingleObject(g_sync_event, 0) == WAIT_OBJECT_0;
}

#define WM_CLIP_DATA_REQ (WM_USER + 201)
#define WM_CLIP_PUSH_LIST (WM_USER + 202)

/* Lazy file serving: the host asks for one file at a time by index into the
 * metadata most recently sent in CLIP_MSG_FILE_LIST.  g_serving defers a new
 * FORMAT_LIST while a file is streaming, so a list can never appear in the
 * middle of a file body on the wire. */
typedef struct {
    wchar_t rel[MAX_PATH];
    wchar_t full[MAX_PATH];
    UINT64  size;
    DWORD   attrs;
    int     is_dir;
} GFileMeta;

static GFileMeta * volatile g_reader_meta = NULL;
static volatile LONG g_reader_meta_count = 0;
static volatile LONG g_serving = 0;
static volatile LONG g_update_pending = 0;

/* ---- Logging ---- */

static void reader_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, g_log_path, "a") != 0 || !f)
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

/* ---- Reliable send/recv (socket) ---- */

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

/* Send on the connection (caller must hold g_send_cs) */
static BOOL send_all_locked(const void *buf, int len)
{
    AsbConn *s = g_client_sock;
    if (!s) return FALSE;
    return asb_send(s, buf, len) == len;
}

/* ---- Skip non-transferable formats ---- */

/* OLE "shell file" formats.  The guest clipboard writer advertises files with
 * FileGroupDescriptorW + FileContents (lazy, streamed on paste); those cannot
 * be read with GetClipboardData() and must never be echoed back to the host.
 * The normal guest->host file path is CF_HDROP. */
static UINT g_cf_file_desc_w = 0;
static UINT g_cf_file_desc_a = 0;
static UINT g_cf_file_contents = 0;
static UINT g_cf_file_name_w = 0;
static UINT g_cf_file_name_a = 0;

static BOOL should_skip_format(UINT fmt)
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

/* ---- Send file entry over socket ---- */

static BOOL clip_send_file_entry(const wchar_t *full_path, const wchar_t *rel_path)
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

        if (!send_all_locked(&hdr, sizeof(hdr))) return FALSE;
        if (!send_all_locked(&fi, sizeof(fi))) return FALSE;
        if (path_bytes > 0 && !send_all_locked(rel_path, (int)path_bytes)) return FALSE;

        swprintf_s(search, MAX_PATH, L"%s\\*", full_path);
        h = FindFirstFileW(search, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t child_full[MAX_PATH], child_rel[MAX_PATH];
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                swprintf_s(child_full, MAX_PATH, L"%s\\%s", full_path, fd.cFileName);
                swprintf_s(child_rel, MAX_PATH, L"%s\\%s", rel_path, fd.cFileName);
                if (!clip_send_file_entry(child_full, child_rel)) {
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

        if (!send_all_locked(&hdr, sizeof(hdr)) ||
            !send_all_locked(&fi, sizeof(fi)) ||
            (path_bytes > 0 && !send_all_locked(rel_path, (int)path_bytes))) {
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
            if (!send_all_locked(chunk, (int)bytes_read)) {
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

/* ---- Lazy file metadata (guest -> host) ---- */

static void clip_meta_walk(GFileMeta *arr, int *count, int cap,
                           const wchar_t *full, const wchar_t *rel, int depth)
{
    GFileMeta *e;
    DWORD attrs;

    if (*count >= cap || depth > CLIP_WALK_MAX_DEPTH) return;

    attrs = GetFileAttributesW(full);
    if (attrs == INVALID_FILE_ATTRIBUTES) return;

    e = &arr[*count];
    wcscpy_s(e->rel, MAX_PATH, rel);
    wcscpy_s(e->full, MAX_PATH, full);
    e->attrs  = attrs;
    e->is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    e->size   = 0;
    if (!e->is_dir) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(full, GetFileExInfoStandard, &fad))
            e->size = ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    (*count)++;

    if (e->is_dir) {
        WIN32_FIND_DATAW fd;
        wchar_t pattern[MAX_PATH];
        HANDLE h;

        _snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", full);
        h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t child_full[MAX_PATH], child_rel[MAX_PATH];
                if (wcscmp(fd.cFileName, L".") == 0 ||
                    wcscmp(fd.cFileName, L"..") == 0) continue;
                _snwprintf_s(child_full, MAX_PATH, _TRUNCATE,
                             L"%s\\%s", full, fd.cFileName);
                _snwprintf_s(child_rel, MAX_PATH, _TRUNCATE,
                             L"%s\\%s", rel, fd.cFileName);
                clip_meta_walk(arr, count, cap, child_full, child_rel, depth + 1);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
}

/* Send the metadata for the snapshot just published as g_reader_meta.
   Caller holds g_send_cs. */
static void clip_send_file_list_locked(void)
{
    ClipHeader hdr;
    BYTE *buf;
    int i, off, total = 4;

    for (i = 0; i < (int)g_reader_meta_count; i++)
        total += 4 + (int)(wcslen(g_reader_meta[i].rel) * sizeof(wchar_t)) + 8 + 4 + 1;

    buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)total);
    if (!buf) {
        reader_log("clip_send_file_list: out of memory (%d bytes).", total);
        return;
    }

    *(UINT32 *)buf = (UINT32)g_reader_meta_count;
    off = 4;
    for (i = 0; i < (int)g_reader_meta_count; i++) {
        UINT32 name_len = (UINT32)(wcslen(g_reader_meta[i].rel) * sizeof(wchar_t));
        *(UINT32 *)(buf + off) = name_len; off += 4;
        memcpy(buf + off, g_reader_meta[i].rel, name_len); off += name_len;
        memcpy(buf + off, &g_reader_meta[i].size, 8); off += 8;
        *(UINT32 *)(buf + off) = g_reader_meta[i].attrs; off += 4;
        buf[off++] = g_reader_meta[i].is_dir ? 1 : 0;
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FILE_LIST;
    hdr.format = 0;
    hdr.data_size = (UINT32)off;

    send_all_locked(&hdr, sizeof(hdr));
    send_all_locked(buf, off);
    HeapFree(GetProcessHeap(), 0, buf);

    reader_log("FILE_LIST sent: %d entries.", (int)g_reader_meta_count);
    for (i = 0; i < (int)g_reader_meta_count && i < 8; i++)
        reader_log("  [%d] '%ls' %llu bytes%s", i, g_reader_meta[i].rel,
                   (unsigned long long)g_reader_meta[i].size,
                   g_reader_meta[i].is_dir ? " (dir)" : "");
    if ((int)g_reader_meta_count > 8)
        reader_log("  ... and %d more.", (int)g_reader_meta_count - 8);
}

/* Serve one file's bytes for a host paste (CLIP_MSG_FILE_DATA_REQ).  Runs on
   the recv thread: no clipboard access, only disk I/O on the snapshot. */
static void clip_serve_file_request(UINT32 idx)
{
    ClipHeader hdr;
    ClipFileInfo fi;
    GFileMeta *e = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;

    InterlockedIncrement(&g_serving);
    EnterCriticalSection(&g_send_cs);

    if (idx < (UINT32)g_reader_meta_count)
        e = &g_reader_meta[idx];

    ZeroMemory(&fi, sizeof(fi));
    if (e) {
        fi.path_len = (UINT32)(wcslen(e->rel) * sizeof(wchar_t));
        if (!e->is_dir) {
            LARGE_INTEGER size;
            /* Share everything: the guest app may still hold the source file
               open.  If it still cannot be opened, clear path_len so the host
               aborts the paste instead of saving an empty file. */
            hFile = CreateFileW(e->full, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile == INVALID_HANDLE_VALUE) {
                reader_log("Cannot open '%ls' for paste (error %lu); failing request.",
                           e->full, GetLastError());
                fi.path_len = 0;
            } else if (GetFileSizeEx(hFile, &size)) {
                fi.file_size = (UINT64)size.QuadPart;
            } else {
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;
                fi.path_len = 0;
            }
        }
    } else {
        reader_log("File data request for unknown index %u.", idx);
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FILE_DATA;
    /* Tag the reply with the requested index (1-based) so the host can tell it
       apart from an eager CF_HDROP file stream that may be interleaved on the
       wire; 0 means "eager stream". */
    hdr.format = idx + 1;
    hdr.data_size = (UINT32)sizeof(fi) + fi.path_len;

    if (e)
        reader_log("Serving idx=%u '%ls' (%llu bytes, dir=%d, open=%d).",
                   idx, e->rel, (unsigned long long)fi.file_size,
                   e->is_dir ? 1 : 0,
                   hFile != INVALID_HANDLE_VALUE ? 1 : 0);

    if (send_all_locked(&hdr, sizeof(hdr)) &&
        send_all_locked(&fi, sizeof(fi)) &&
        (fi.path_len == 0 || send_all_locked(e->rel, (int)fi.path_len))) {
        if (hFile != INVALID_HANDLE_VALUE) {
            BYTE *chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
            UINT64 remaining = fi.file_size;
            UINT64 total = fi.file_size;
            BOOL served = TRUE;

            while (chunk && remaining > 0) {
                DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
                DWORD bytes_read = 0;
                if (!ReadFile(hFile, chunk, to_read, &bytes_read, NULL) || bytes_read == 0) {
                    reader_log("Read of '%ls' failed at %llu/%llu bytes (error %lu).",
                               e->full,
                               (unsigned long long)(total - remaining),
                               (unsigned long long)total, GetLastError());
                    served = FALSE;
                    break;
                }
                if (!send_all_locked(chunk, (int)bytes_read)) {
                    /* The host released the paste (cancel) or the channel died. */
                    reader_log("Send of file %u failed at %llu/%llu bytes "
                               "(host closed the connection).",
                               idx,
                               (unsigned long long)(total - remaining),
                               (unsigned long long)total);
                    served = FALSE;
                    break;
                }
                remaining -= bytes_read;
            }
            if (chunk) HeapFree(GetProcessHeap(), 0, chunk);
            if (served)
                reader_log("Served file %u (%llu bytes).", idx,
                           (unsigned long long)fi.file_size);
        }
    } else {
        reader_log("Could not send the reply header for file %u (host gone).", idx);
    }

    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);

    LeaveCriticalSection(&g_send_cs);
    InterlockedDecrement(&g_serving);

    if (InterlockedExchange(&g_update_pending, 0) && g_msg_hwnd)
        PostMessageW(g_msg_hwnd, WM_CLIP_PUSH_LIST, 0, 0);
}

/* ---- Send format list to host (clipboard changed locally) ---- */

static void clip_send_format_list(void)
{
    ClipHeader hdr;
    UINT fmt;
    BYTE buf[16384];
    int offset = 4;
    UINT32 count = 0;
    BOOL has_hdrop = FALSE;
    wchar_t roots[CLIP_ROOT_MAX][MAX_PATH];
    int root_count = 0;
    GFileMeta *meta = NULL;
    int meta_count = 0;
    BOOL lazy = FALSE;
    BOOL lazy_sent = FALSE;

    if (g_client_sock == NULL) return;

    if (!OpenClipboard(g_msg_hwnd)) {
        reader_log("clip_send_format_list: OpenClipboard failed (%lu).", GetLastError());
        return;
    }

    {
        has_hdrop = IsClipboardFormatAvailable(CF_HDROP);

        fmt = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0 && count < CLIP_MAX_FORMATS) {
            char name[256];
            int name_len = 0;

            if (should_skip_format(fmt)) continue;
            if (has_hdrop && fmt != CF_HDROP) continue;

            if (fmt >= 0xC000)
                name_len = GetClipboardFormatNameA(fmt, name, sizeof(name));

            if (offset + 8 + name_len > (int)sizeof(buf)) break;

            *(UINT32 *)(buf + offset) = fmt;       offset += 4;
            *(UINT32 *)(buf + offset) = (UINT32)name_len; offset += 4;
            if (name_len > 0) {
                memcpy(buf + offset, name, name_len);
                offset += name_len;
            }
            count++;
        }

        /* Snapshot the dropped paths while the clipboard is open; walking the
           tree happens after CloseClipboard so other apps are not blocked. */
        if (has_hdrop) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                UINT n = DragQueryFileW((HDROP)hData, 0xFFFFFFFF, NULL, 0);
                UINT i;
                if (n > CLIP_ROOT_MAX) n = CLIP_ROOT_MAX;
                for (i = 0; i < n; i++) {
                    if (DragQueryFileW((HDROP)hData, i, roots[root_count], MAX_PATH) > 0)
                        root_count++;
                }
            }
        }
    }
    CloseClipboard();

    if (count == 0) {
        /* An empty list is still sent: the host pairs every push with one
           echoed FORMAT_LIST to suppress it, and a discarded echo would make
           the next genuine guest copy disappear.  A zero-format list is a
           no-op for old and new hosts. */
        reader_log("No transferable formats; sending empty list.");
    }

    /* Build the metadata snapshot for lazy FileContents. */
    if (root_count > 0) {
        int i;
        meta = (GFileMeta *)HeapAlloc(GetProcessHeap(), 0,
                                      sizeof(GFileMeta) * (size_t)CLIP_META_MAX);
        if (meta) {
            for (i = 0; i < root_count && meta_count < CLIP_META_MAX; i++) {
                const wchar_t *name = wcsrchr(roots[i], L'\\');
                name = name ? name + 1 : roots[i];
                clip_meta_walk(meta, &meta_count, CLIP_META_MAX, roots[i], name, 0);
            }
            lazy = (meta_count > 0);
            if (!lazy) {
                HeapFree(GetProcessHeap(), 0, meta);
                meta = NULL;
            }
        }
    }

    *(UINT32 *)buf = count;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_LIST;
    hdr.format = 0;

    /* Check before taking g_send_cs: the recv thread holds it for the whole
       file body, and blocking the window thread there would freeze the guest
       clipboard for the duration of a paste.  The serve posts
       WM_CLIP_PUSH_LIST when it finishes, so a deferred list is retried. */
    if (InterlockedCompareExchange(&g_serving, 0, 0) > 0) {
        InterlockedExchange(&g_update_pending, 1);
        if (meta) HeapFree(GetProcessHeap(), 0, meta);
        if (InterlockedCompareExchange(&g_serving, 0, 0) == 0 && g_msg_hwnd)
            PostMessageW(g_msg_hwnd, WM_CLIP_PUSH_LIST, 0, 0);
        reader_log("Format list deferred (file transfer in progress).");
        return;
    }

    EnterCriticalSection(&g_send_cs);

    /* Re-check under the lock: a serve may have started meanwhile. */
    if (InterlockedCompareExchange(&g_serving, 0, 0) > 0) {
        InterlockedExchange(&g_update_pending, 1);
        LeaveCriticalSection(&g_send_cs);
        if (meta) HeapFree(GetProcessHeap(), 0, meta);
        if (InterlockedCompareExchange(&g_serving, 0, 0) == 0 && g_msg_hwnd)
            PostMessageW(g_msg_hwnd, WM_CLIP_PUSH_LIST, 0, 0);
        reader_log("Format list deferred (file transfer in progress).");
        return;
    }

    if (lazy && offset + 4 <= (int)sizeof(buf)) {
        *(UINT32 *)(buf + offset) = CLIP_LAZY_MARKER;
        offset += 4;
        lazy_sent = TRUE;

        /* Publish the snapshot (safe: an in-flight serve holds g_send_cs). */
        if (g_reader_meta) HeapFree(GetProcessHeap(), 0, (void *)g_reader_meta);
        g_reader_meta = meta;
        g_reader_meta_count = meta_count;
        meta = NULL;
    }

    hdr.data_size = (UINT32)offset;
    send_all_locked(&hdr, sizeof(hdr));
    send_all_locked(buf, offset);

    if (lazy_sent && g_reader_meta_count > 0)
        clip_send_file_list_locked();

    LeaveCriticalSection(&g_send_cs);

    if (meta) HeapFree(GetProcessHeap(), 0, meta);

    reader_log("Sent format list (%u formats)%s.", count,
               lazy ? " [lazy files]" : "");
}

/* ---- Handle FORMAT_DATA_REQ from host ---- */

static void clip_handle_data_request(UINT fmt)
{
    ClipHeader hdr;

    if (fmt == CF_HDROP) {
        wchar_t (*paths)[MAX_PATH] = NULL;
        int path_count = 0;
        int snap_failed = 0;

        /* Snapshot the dropped paths while the clipboard is open, then release
           it again. Holding OpenClipboard() across a multi-gigabyte transfer
           would lock every other application on the guest out of the clipboard
           for the whole transfer (the host side already does this). */
        if (OpenClipboard(g_msg_hwnd)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)hData;
                UINT file_count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                UINT fi;
                if (file_count > CLIP_MAX_FORMATS) file_count = CLIP_MAX_FORMATS;
                paths = (wchar_t (*)[MAX_PATH])HeapAlloc(
                    GetProcessHeap(), 0, sizeof(wchar_t[MAX_PATH]) * file_count);
                if (paths) {
                    for (fi = 0; fi < file_count; fi++)
                        DragQueryFileW(hDrop, fi, paths[path_count++], MAX_PATH);
                } else {
                    snap_failed = 1;
                }
                reader_log("Data request CF_HDROP - %u file(s).", file_count);
            } else {
                reader_log("GetClipboardData(CF_HDROP) returned NULL.");
            }
            CloseClipboard();
        } else {
            reader_log("OpenClipboard failed for CF_HDROP (%lu).", GetLastError());
        }

        if (path_count == 0 || snap_failed) {
            if (paths) HeapFree(GetProcessHeap(), 0, paths);
            hdr.magic = CLIP_MAGIC;
            hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
            hdr.format = CF_HDROP;
            hdr.data_size = 0;
            EnterCriticalSection(&g_send_cs);
            send_all_locked(&hdr, sizeof(hdr));
            LeaveCriticalSection(&g_send_cs);
            return;
        }

        /* Block new format lists from interleaving (counter, so a lazy serve
           on the recv thread is not clobbered). */
        InterlockedIncrement(&g_serving);
        EnterCriticalSection(&g_send_cs);
        {
            int i;
            for (i = 0; i < path_count; i++) {
                wchar_t *name = wcsrchr(paths[i], L'\\');
                name = name ? name + 1 : paths[i];
                clip_send_file_entry(paths[i], name);
            }
            hdr.magic = CLIP_MAGIC;
            hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
            hdr.format = CF_HDROP;
            hdr.data_size = 0;
            send_all_locked(&hdr, sizeof(hdr));
        }
        LeaveCriticalSection(&g_send_cs);
        InterlockedDecrement(&g_serving);

        if (paths) HeapFree(GetProcessHeap(), 0, paths);

        /* Drain a format list that was deferred while we streamed. */
        if (InterlockedExchange(&g_update_pending, 0) && g_msg_hwnd)
            PostMessageW(g_msg_hwnd, WM_CLIP_PUSH_LIST, 0, 0);
        return;
    }
    /* Regular format */
    if (OpenClipboard(g_msg_hwnd)) {
        HANDLE hData = GetClipboardData(fmt);
        void *ptr = hData ? GlobalLock(hData) : NULL;
        SIZE_T size = ptr ? GlobalSize(hData) : 0;

        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = (ptr && size <= CLIP_MAX_PAYLOAD) ? (UINT32)size : 0;

        EnterCriticalSection(&g_send_cs);
        send_all_locked(&hdr, sizeof(hdr));
        if (hdr.data_size > 0)
            send_all_locked(ptr, (int)hdr.data_size);
        LeaveCriticalSection(&g_send_cs);

        if (ptr) GlobalUnlock(hData);
        CloseClipboard();
        reader_log("Responded format %u (%u bytes).", fmt, hdr.data_size);
    } else {
        reader_log("OpenClipboard failed for format %u (%lu).", fmt, GetLastError());
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&g_send_cs);
        send_all_locked(&hdr, sizeof(hdr));
        LeaveCriticalSection(&g_send_cs);
    }
}

/* ---- Recv thread: handles FORMAT_DATA_REQ from host ---- */

static DWORD WINAPI recv_thread_proc(LPVOID param)
{
    AsbConn *s = (AsbConn *)param;
    ClipHeader hdr;

    reader_log("Recv thread started.");

    while (1) {
        if (!recv_exact(s, &hdr, sizeof(hdr)))
            break;

        if (hdr.magic != CLIP_MAGIC) {
            reader_log("Bad magic 0x%08X, disconnecting.", hdr.magic);
            break;
        }

        switch (hdr.msg_type) {
        case CLIP_MSG_FORMAT_DATA_REQ:
            reader_log("Data request for format %u.", hdr.format);
            /* Post to msg window thread so clipboard is accessed there */
            PostMessageW(g_msg_hwnd, WM_CLIP_DATA_REQ, (WPARAM)hdr.format, 0);
            break;

        case CLIP_MSG_FILE_DATA_REQ: {
            UINT32 idx = 0;
            UINT32 extra = hdr.data_size;

            if (extra >= 4) {
                if (!recv_exact(s, &idx, 4)) goto done;
                extra -= 4;
            }
            if (extra > 0) {
                BYTE skip[4096];
                UINT32 remaining = extra;
                while (remaining > 0) {
                    int chunk = remaining > 4096 ? 4096 : (int)remaining;
                    if (!recv_exact(s, skip, chunk)) goto done;
                    remaining -= chunk;
                }
            }
            clip_serve_file_request(idx);
            break;
        }

        case CLIP_MSG_PULL_LIST: {
            /* The host's advertised list is stale (a paste was refused).
               Re-send the current clipboard list; if a file body is being
               served, clip_send_format_list defers and posts again when the
               serve finishes.  Old hosts never send this. */
            UINT32 remaining = hdr.data_size;
            BYTE skip[256];
            while (remaining > 0) {
                int chunk = remaining > (UINT32)sizeof(skip)
                                ? (int)sizeof(skip) : (int)remaining;
                if (!recv_exact(s, skip, chunk)) goto done;
                remaining -= (UINT32)chunk;
            }
            reader_log("Host asked for the current clipboard list.");
            if (g_msg_hwnd) PostMessageW(g_msg_hwnd, WM_CLIP_PUSH_LIST, 0, 0);
            break;
        }
        default:
            reader_log("Unknown msg_type %u.", hdr.msg_type);
            if (hdr.data_size > 0) {
                BYTE skip[4096];
                UINT32 remaining = hdr.data_size;
                while (remaining > 0) {
                    int chunk = remaining > 4096 ? 4096 : (int)remaining;
                    if (!recv_exact(s, skip, chunk)) goto done;
                    remaining -= chunk;
                }
            }
            break;
        }
    }

done:
    reader_log("Recv thread exiting.");
    return 0;
}

/* ---- Message window for clipboard monitoring ---- */

static LRESULT CALLBACK clip_msg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLIPBOARDUPDATE:
        if (!clip_sync_is_enabled()) {
            reader_log("WM_CLIPBOARDUPDATE ignored (sync disabled).");
            break;
        }
        reader_log("WM_CLIPBOARDUPDATE — sending format list to host.");
        clip_send_format_list();
        return 0;

    case WM_CLIP_DATA_REQ:
        clip_handle_data_request((UINT)wp);
        return 0;

    case WM_CLIP_PUSH_LIST:
        clip_send_format_list();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI msg_window_thread(LPVOID param)
{
    WNDCLASSEXW wc;
    MSG msg;
    (void)param;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = clip_msg_wnd_proc;
    wc.hInstance      = GetModuleHandleW(NULL);
    wc.lpszClassName  = L"AppSandboxClipReader";
    RegisterClassExW(&wc);

    g_msg_hwnd = CreateWindowExW(0, L"AppSandboxClipReader", NULL,
                                  0, 0, 0, 0, 0, HWND_MESSAGE,
                                  NULL, GetModuleHandleW(NULL), NULL);
    if (!g_msg_hwnd) {
        reader_log("Failed to create message window (%lu).", GetLastError());
        return 1;
    }

    AddClipboardFormatListener(g_msg_hwnd);
    reader_log("Message window created, clipboard listener active.");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveClipboardFormatListener(g_msg_hwnd);
    DestroyWindow(g_msg_hwnd);
    g_msg_hwnd = NULL;
    return 0;
}

/* ---- Handle one host connection ---- */

static void handle_client(AsbConn *s)
{
    UINT32 ready = CLIP_READY_MAGIC;
    HANDLE recv_th;

    /* Send ready signal */
    if (asb_send(s, &ready, sizeof(ready)) != (int)sizeof(ready)) {
        reader_log("Failed to send ready signal.");
        return;
    }
    reader_log("Sent ready signal to host.");

    g_client_sock = s;

    /* Start recv thread */
    recv_th = CreateThread(NULL, 0, recv_thread_proc, (LPVOID)s, 0, NULL);
    if (!recv_th) {
        reader_log("Failed to create recv thread (%lu).", GetLastError());
        g_client_sock = NULL;
        return;
    }

    /* Wait for recv thread to finish (host disconnected) */
    WaitForSingleObject(recv_th, INFINITE);
    CloseHandle(recv_th);

    g_client_sock = NULL;
    reader_log("Host disconnected.");
}

/* ---- Main ---- */

int main(void)
{
    HANDLE msg_th;
    AsbListener *l;
    char tmp[MAX_PATH];

    /* Set up log path in user temp dir */
    GetTempPathA(MAX_PATH, tmp);
    sprintf_s(g_log_path, MAX_PATH, "%sclipboard-reader.log", tmp);

    reader_log("Starting (PID=%lu, session=%lu).",
               GetCurrentProcessId(),
               WTSGetActiveConsoleSessionId());

    InitializeCriticalSection(&g_send_cs);

    g_cf_file_desc_w   = RegisterClipboardFormatW(L"FileGroupDescriptorW");
    g_cf_file_desc_a   = RegisterClipboardFormatW(L"FileGroupDescriptor");
    g_cf_file_contents = RegisterClipboardFormatW(L"FileContents");
    g_cf_file_name_w   = RegisterClipboardFormatW(L"FileNameW");
    g_cf_file_name_a   = RegisterClipboardFormatW(L"FileName");

    g_sync_event = OpenEventW(SYNCHRONIZE, FALSE, CLIP_SYNC_EVENT_NAME);
    if (!g_sync_event)
        reader_log("Warning: could not open sync event (writer not running yet?).");

    /* Start message window thread (clipboard listener) */
    msg_th = CreateThread(NULL, 0, msg_window_thread, NULL, 0, NULL);
    if (!msg_th) {
        reader_log("Failed to create message window thread (%lu).", GetLastError());
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }

    /* Wait a moment for message window to be created */
    Sleep(100);

    if (asb_transport_init() != 0) {
        reader_log("asb_transport_init failed.");
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }

    l = asb_listen(ASB_CH_CLIPBOARD_READER);
    if (!l) {
        reader_log("asb_listen(ASB_CH_CLIPBOARD_READER) failed.");
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }
    reader_log("Listening on clipboard-reader channel (transport=%s).",
               asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    /* Accept loop — one client at a time */
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        reader_log("Host connected.");
        handle_client(c);
        asb_close(c);
    }
}
