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

static volatile SOCKET g_client_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_send_cs;
static HWND g_msg_hwnd = NULL;
static char g_log_path[MAX_PATH];

#define WM_CLIP_DATA_REQ (WM_USER + 201)

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

static BOOL send_all(SOCKET s, const void *buf, int len)
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

static BOOL recv_exact(SOCKET s, void *buf, int len)
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

/* Send on socket (caller must hold g_send_cs) */
static BOOL send_all_locked(const void *buf, int len)
{
    SOCKET s = g_client_sock;
    const char *p = (const char *)buf;
    int remaining = len;
    if (s == INVALID_SOCKET) return FALSE;
    while (remaining > 0) {
        int n = send(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
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

/* ---- Send format list to host (clipboard changed locally) ---- */

static void clip_send_format_list(void)
{
    ClipHeader hdr;
    UINT fmt;
    BYTE buf[16384];
    int offset = 4;
    UINT32 count = 0;

    if (g_client_sock == INVALID_SOCKET) return;

    if (!OpenClipboard(g_msg_hwnd)) {
        reader_log("clip_send_format_list: OpenClipboard failed (%lu).", GetLastError());
        return;
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
        return;
    }

    *(UINT32 *)buf = count;

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_LIST;
    hdr.format = 0;
    hdr.data_size = (UINT32)offset;

    EnterCriticalSection(&g_send_cs);
    send_all_locked(&hdr, sizeof(hdr));
    send_all_locked(buf, offset);
    LeaveCriticalSection(&g_send_cs);

    reader_log("Sent format list (%u formats).", count);
}

/* ---- Handle FORMAT_DATA_REQ from host ---- */

static void clip_handle_data_request(UINT fmt)
{
    ClipHeader hdr;

    if (fmt == CF_HDROP) {
        if (OpenClipboard(g_msg_hwnd)) {
            HANDLE hData = GetClipboardData(CF_HDROP);
            if (hData) {
                HDROP hDrop = (HDROP)hData;
                UINT file_count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
                UINT fi;
                reader_log("Data request CF_HDROP — %u files.", file_count);
                EnterCriticalSection(&g_send_cs);
                for (fi = 0; fi < file_count; fi++) {
                    wchar_t path[MAX_PATH];
                    wchar_t *name;
                    DragQueryFileW(hDrop, fi, path, MAX_PATH);
                    name = wcsrchr(path, L'\\');
                    name = name ? name + 1 : path;
                    clip_send_file_entry(path, name);
                }
                hdr.magic = CLIP_MAGIC;
                hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
                hdr.format = CF_HDROP;
                hdr.data_size = 0;
                send_all_locked(&hdr, sizeof(hdr));
                LeaveCriticalSection(&g_send_cs);
            } else {
                reader_log("GetClipboardData(CF_HDROP) returned NULL.");
                goto send_empty;
            }
            CloseClipboard();
        } else {
            reader_log("OpenClipboard failed for CF_HDROP (%lu).", GetLastError());
            goto send_empty;
        }
        return;

    send_empty:
        hdr.magic = CLIP_MAGIC;
        hdr.msg_type = CLIP_MSG_FORMAT_DATA_RESP;
        hdr.format = fmt;
        hdr.data_size = 0;
        EnterCriticalSection(&g_send_cs);
        send_all_locked(&hdr, sizeof(hdr));
        LeaveCriticalSection(&g_send_cs);
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
    SOCKET s = (SOCKET)(UINT_PTR)param;
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
        reader_log("WM_CLIPBOARDUPDATE — sending format list to host.");
        clip_send_format_list();
        return 0;

    case WM_CLIP_DATA_REQ:
        clip_handle_data_request((UINT)wp);
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

static void handle_client(SOCKET s)
{
    UINT32 ready = CLIP_READY_MAGIC;
    HANDLE recv_th;

    /* Send ready signal */
    if (send(s, (const char *)&ready, sizeof(ready), 0) != sizeof(ready)) {
        reader_log("Failed to send ready signal (%d).", WSAGetLastError());
        return;
    }
    reader_log("Sent ready signal to host.");

    g_client_sock = s;

    /* Start recv thread */
    recv_th = CreateThread(NULL, 0, recv_thread_proc, (LPVOID)(UINT_PTR)s, 0, NULL);
    if (!recv_th) {
        reader_log("Failed to create recv thread (%lu).", GetLastError());
        g_client_sock = INVALID_SOCKET;
        return;
    }

    /* Wait for recv thread to finish (host disconnected) */
    WaitForSingleObject(recv_th, INFINITE);
    CloseHandle(recv_th);

    g_client_sock = INVALID_SOCKET;
    reader_log("Host disconnected.");
}

/* ---- Main ---- */

int main(void)
{
    WSADATA wsa;
    SOCKET listen_s;
    SOCKADDR_HV addr;
    HANDLE msg_th;
    char tmp[MAX_PATH];

    /* Set up log path in user temp dir */
    GetTempPathA(MAX_PATH, tmp);
    sprintf_s(g_log_path, MAX_PATH, "%sclipboard-reader.log", tmp);

    reader_log("Starting (PID=%lu, session=%lu).",
               GetCurrentProcessId(),
               WTSGetActiveConsoleSessionId());

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        reader_log("WSAStartup failed (%d).", WSAGetLastError());
        return 1;
    }

    InitializeCriticalSection(&g_send_cs);

    /* Start message window thread (clipboard listener) */
    msg_th = CreateThread(NULL, 0, msg_window_thread, NULL, 0, NULL);
    if (!msg_th) {
        reader_log("Failed to create message window thread (%lu).", GetLastError());
        DeleteCriticalSection(&g_send_cs);
        WSACleanup();
        return 1;
    }

    /* Wait a moment for message window to be created */
    Sleep(100);

    listen_s = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
    if (listen_s == INVALID_SOCKET) {
        reader_log("socket(AF_HYPERV) failed (%d).", WSAGetLastError());
        DeleteCriticalSection(&g_send_cs);
        WSACleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = HV_GUID_WILDCARD;
    addr.ServiceId = CLIPBOARD_READER_SERVICE_GUID;

    {
        int bind_tries;
        for (bind_tries = 0; bind_tries < 10; bind_tries++) {
            if (bind(listen_s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
                break;
            reader_log("bind attempt %d failed (%d), retrying...",
                        bind_tries + 1, WSAGetLastError());
            Sleep(500);
        }
        if (bind_tries == 10) {
            reader_log("bind failed after 10 attempts, exiting.");
            closesocket(listen_s);
            DeleteCriticalSection(&g_send_cs);
            WSACleanup();
            return 1;
        }
    }

    if (listen(listen_s, 2) != 0) {
        reader_log("listen failed (%d).", WSAGetLastError());
        closesocket(listen_s);
        DeleteCriticalSection(&g_send_cs);
        WSACleanup();
        return 1;
    }

    reader_log("Listening on GUID a5b0cafe-0006-4000-8000-000000000001.");

    /* Accept loop — one client at a time */
    for (;;) {
        SOCKET client_s = accept(listen_s, NULL, NULL);
        if (client_s == INVALID_SOCKET) {
            reader_log("accept failed (%d).", WSAGetLastError());
            Sleep(1000);
            continue;
        }
        reader_log("Host connected.");
        handle_client(client_s);
        closesocket(client_s);
    }
}
