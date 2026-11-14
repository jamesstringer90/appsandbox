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

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)  /* 64 MB */
#define CLIP_FILE_CHUNK     (1024 * 1024)        /* 1 MB */

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

static volatile LONG g_pending_formats;
static HWND g_msg_hwnd = NULL;
static char g_log_path[MAX_PATH];

/* Focus-gated sync: the clipboard writer (SYSTEM, :0005) creates this
 * named event and sets/resets it when the host sends SYNC_ENABLE.
 * We check it before sending FORMAT_LIST to the host. */
static HANDLE g_sync_event = NULL;
#define CLIP_SYNC_EVENT_NAME L"Global\\AppSandboxClipSync"

static BOOL clip_sync_is_enabled(void)
{
    if (!g_sync_event) {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, CLIP_SYNC_EVENT_NAME);
        if (event && InterlockedCompareExchangePointer((PVOID volatile *)&g_sync_event, event, NULL))
            CloseHandle(event);
    }
    if (!g_sync_event) return TRUE;
    return WaitForSingleObject(g_sync_event, 0) == WAIT_OBJECT_0;
}

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

/* ---- Skip non-transferable formats ---- */

static BOOL should_skip_format(UINT fmt)
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

/* ---- Send file entry over socket ---- */

static BOOL clip_send_file_entry(AsbConn *s, const wchar_t *full_path, const wchar_t *rel_path)
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

        if (!send_all(s, &hdr, sizeof(hdr))) return FALSE;
        if (!send_all(s, &fi, sizeof(fi))) return FALSE;
        if (path_bytes > 0 && !send_all(s, rel_path, (int)path_bytes)) return FALSE;

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
        if (!GetFileSizeEx(hFile, &size)) {
            HeapFree(GetProcessHeap(), 0, chunk);
            CloseHandle(hFile);
            return FALSE;
        }
        fi.file_size = (UINT64)size.QuadPart;
        fi.is_directory = 0;
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FILE_DATA;
        hdr.format = 0;
        hdr.data_size = (UINT32)sizeof(fi) + path_bytes;

        if (!send_all(s, &hdr, sizeof(hdr)) ||
            !send_all(s, &fi, sizeof(fi)) ||
            (path_bytes > 0 && !send_all(s, rel_path, (int)path_bytes))) {
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
            if (!send_all(s, chunk, (int)bytes_read)) {
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

/* ---- Send format list to host (clipboard changed locally) ---- */

static BOOL clip_send_format_list(AsbConn *s)
{
    ClipHeader hdr;
    UINT fmt;
    BYTE buf[16384];
    int offset = 4;
    UINT32 count = 0;

    if (!OpenClipboard(g_msg_hwnd)) {
        reader_log("clip_send_format_list: OpenClipboard failed (%lu).", GetLastError());
        InterlockedExchange(&g_pending_formats, 1);
        return TRUE;
    }

    {
        BOOL has_hdrop = IsClipboardFormatAvailable(CF_HDROP);

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
    }
    CloseClipboard();

    if (count == 0) {
        reader_log("No formats to send.");
        return TRUE;
    }

    *(UINT32 *)buf = count;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_LIST;
    hdr.format = 0;
    hdr.data_size = (UINT32)offset;

    if (!send_all(s, &hdr, sizeof(hdr)) || !send_all(s, buf, offset)) return FALSE;

    reader_log("Sent format list (%u formats).", count);
    return TRUE;
}

/* ---- Handle FORMAT_DATA_REQ from host ---- */

static BOOL clip_handle_data_request(AsbConn *s, UINT fmt)
{
    ClipHeader hdr = { CLIP_MAGIC, CLIP_MSG_FORMAT_DATA_RESP, fmt, 0 };
    BYTE *data = NULL;
    wchar_t (*paths)[MAX_PATH] = NULL;
    UINT path_count = 0;
    BOOL sent = TRUE;

    if (OpenClipboard(g_msg_hwnd)) {
        HANDLE hData = GetClipboardData(fmt);
        if (fmt == CF_HDROP && hData) {
            UINT count = DragQueryFileW((HDROP)hData, 0xFFFFFFFF, NULL, 0);
            if (count && count <= CLIP_MAX_PAYLOAD / sizeof(*paths))
                paths = (wchar_t (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), 0, (SIZE_T)count * sizeof(*paths));
            if (paths) {
                for (path_count = 0; path_count < count; path_count++) {
                    UINT length = DragQueryFileW((HDROP)hData, path_count, NULL, 0);
                    if (!length || length >= MAX_PATH ||
                        DragQueryFileW((HDROP)hData, path_count, paths[path_count], MAX_PATH) != length) {
                        path_count = 0;
                        break;
                    }
                }
            }
        } else if (hData) {
            void *ptr = GlobalLock(hData);
            SIZE_T size = ptr ? GlobalSize(hData) : 0;
            if (size && size <= CLIP_MAX_PAYLOAD)
                data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
            if (data) {
                memcpy(data, ptr, size);
                hdr.data_size = (UINT32)size;
            }
            if (ptr) GlobalUnlock(hData);
        }
        CloseClipboard();
    } else {
        reader_log("OpenClipboard failed for format %u (%lu).", fmt, GetLastError());
    }

    for (UINT i = 0; i < path_count && sent; i++) {
        const wchar_t *name = wcsrchr(paths[i], L'\\');
        sent = clip_send_file_entry(s, paths[i], name ? name + 1 : paths[i]);
    }
    if (sent) sent = send_all(s, &hdr, sizeof(hdr));
    if (sent && data) sent = send_all(s, data, (int)hdr.data_size);
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (data) HeapFree(GetProcessHeap(), 0, data);
    reader_log("Responded format %u (%u bytes, %u paths, sent=%d).", fmt, hdr.data_size, path_count, sent);
    return sent;
}

/* ---- Recv thread: handles FORMAT_DATA_REQ from host ---- */

static DWORD WINAPI recv_thread_proc(LPVOID param)
{
    AsbConn *s = (AsbConn *)param;
    ClipHeader hdr;

    reader_log("Recv thread started.");

    while (1) {
        int ready;
        if (InterlockedExchange(&g_pending_formats, 0) && clip_sync_is_enabled() &&
            !clip_send_format_list(s)) break;
        ready = asb_poll(s, 100);
        if (ready < 0) break;
        if (!ready) continue;
        if (!recv_exact(s, &hdr, sizeof(hdr)))
            break;

        if (hdr.magic != CLIP_MAGIC) {
            reader_log("Bad magic 0x%08X, disconnecting.", hdr.magic);
            break;
        }

        switch (hdr.msg_type) {
        case CLIP_MSG_FORMAT_DATA_REQ:
            reader_log("Data request for format %u.", hdr.format);
            if (!clip_handle_data_request(s, hdr.format)) goto done;
            break;

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
        InterlockedExchange(&g_pending_formats, 1);
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

    asb_set_timeout(s, 5000, 10000);

    /* Start recv thread */
    recv_th = CreateThread(NULL, 0, recv_thread_proc, (LPVOID)s, 0, NULL);
    if (!recv_th) {
        reader_log("Failed to create recv thread (%lu).", GetLastError());
        return;
    }

    /* Wait for recv thread to finish (host disconnected) */
    WaitForSingleObject(recv_th, INFINITE);
    CloseHandle(recv_th);

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

    g_sync_event = OpenEventW(SYNCHRONIZE, FALSE, CLIP_SYNC_EVENT_NAME);
    if (!g_sync_event)
        reader_log("Warning: could not open sync event (writer not running yet?).");

    /* Start message window thread (clipboard listener) */
    msg_th = CreateThread(NULL, 0, msg_window_thread, NULL, 0, NULL);
    if (!msg_th) {
        reader_log("Failed to create message window thread (%lu).", GetLastError());
        return 1;
    }

    /* Wait a moment for message window to be created */
    Sleep(100);

    if (asb_transport_init() != 0) {
        reader_log("asb_transport_init failed.");
        return 1;
    }

    l = asb_listen(ASB_CH_CLIPBOARD_READER);
    if (!l) {
        reader_log("asb_listen(ASB_CH_CLIPBOARD_READER) failed.");
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
