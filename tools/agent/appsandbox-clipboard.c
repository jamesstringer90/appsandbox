/*
 * appsandbox-clipboard.exe — Guest-side clipboard writer for AppSandbox.
 *
 * Runs as SYSTEM, spawned by the agent service. Listens on AF_HYPERV
 * socket (GUID :0005) for the host IDD display to connect. Handles the
 * host→guest clipboard direction using delayed rendering:
 *   - On FORMAT_LIST from host: stores format metadata, requests data
 *     via FORMAT_DATA_REQ one format at a time
 *   - On FORMAT_DATA_RESP from host: accumulates data, applies all
 *     formats to guest clipboard once complete
 *
 * Guest→host clipboard is handled separately by appsandbox-clipboard-reader.exe
 * (USER, GUID :0006).
 *
 * Logs to C:\Windows\AppSandbox\clipboard.log (beside agent.log).
 */

#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdarg.h>
#include <shellapi.h>
#include <shlobj.h>

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
static volatile SOCKET g_client_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_send_cs;

/* Message window (created on msg_window_thread) */
static HWND g_msg_hwnd = NULL;

/* ---- Logging ---- */

static void clip_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\clipboard.log", "a") != 0 || !f)
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

/* ---- Send helpers ---- */

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
            CloseHandle(user_token);
            CreateDirectoryW(g_clip_temp_dir, NULL);
            clip_log("Temp dir: %ls", g_clip_temp_dir);
            return;
        }
        CloseHandle(user_token);
    }

    /* Fallback: SYSTEM temp (should not normally happen) */
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_clip_temp_dir, MAX_PATH, L"%sAppSandboxClip", tmp);
    CreateDirectoryW(g_clip_temp_dir, NULL);
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
        wcscpy_s(ptr, MAX_PATH, paths[i]);
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

/* Send FORMAT_DATA_REQ for the next format, or post WM_CLIP_APPLY_FMTS if all done.
   Called from recv thread. */
static void clip_request_next_format(void)
{
    ClipHeader hdr;

    if (g_clip_fetch_idx >= g_clip_fmt_count) {
        /* All formats fetched — tell msg window to apply */
        clip_log("All %d formats fetched, applying.", g_clip_fmt_count);
        if (g_msg_hwnd)
            PostMessageW(g_msg_hwnd, WM_CLIP_APPLY_FMTS, 0, 0);
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

/* Apply fetched format data to the clipboard.
   Called on the message window thread via WM_CLIP_APPLY_FMTS. */
static void clip_apply_format_list(void)
{
    int i;

    if (g_clip_fmt_count == 0) return;

    InterlockedExchange(&g_suppress, 1);

    if (!OpenClipboard(g_msg_hwnd)) {
        clip_log("clip_apply_format_list: OpenClipboard failed (%lu).", GetLastError());
        InterlockedExchange(&g_suppress, 0);
        clip_free_pending();
        return;
    }

    EmptyClipboard();

    for (i = 0; i < g_clip_fmt_count; i++) {
        if (g_clip_fmts[i].is_hdrop) {
            HGLOBAL hDrop = clip_build_hdrop_from_temp();
            if (hDrop)
                SetClipboardData(g_clip_fmts[i].local_id, hDrop);
        } else if (g_clip_fmts[i].data && g_clip_fmts[i].data_size > 0) {
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
    }

    CloseClipboard();
    clip_log("Applied %d formats to clipboard.", g_clip_fmt_count);
    clip_free_pending();
}

/* ---- Message window for clipboard monitoring ---- */

static LRESULT CALLBACK clip_msg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLIP_APPLY_FMTS:
        clip_log("WM_CLIP_APPLY_FMTS — applying fetched data.");
        clip_apply_format_list();
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

    clip_log("Message window created (write-only, no clipboard listener).");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(g_msg_hwnd);
    g_msg_hwnd = NULL;
    return 0;
}

/* Receive and write a single FILE_DATA message to disk. */
static BOOL clip_recv_file_data(SOCKET s, const ClipHeader *hdr)
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

/* ---- Recv thread: delayed rendering protocol ---- */

static DWORD WINAPI recv_thread_proc(LPVOID param)
{
    SOCKET s = (SOCKET)(UINT_PTR)param;
    ClipHeader hdr;

    clip_log("Recv thread started.");

    while (1) {
        if (!recv_exact(s, &hdr, sizeof(hdr)))
            break;

        if (hdr.magic != CLIP_MAGIC) {
            clip_log("Bad magic 0x%08X, disconnecting.", hdr.magic);
            break;
        }

        switch (hdr.msg_type) {

        case CLIP_MSG_SYNC_ENABLE: {
            UINT8 flag = 0;
            if (hdr.data_size >= 1) {
                if (!recv_exact(s, &flag, 1)) goto done;
                if (hdr.data_size > 1) {
                    BYTE skip[256];
                    UINT32 rem = hdr.data_size - 1;
                    while (rem > 0) {
                        int c = rem > 256 ? 256 : (int)rem;
                        if (!recv_exact(s, skip, c)) goto done;
                        rem -= c;
                    }
                }
            }
            InterlockedExchange(&g_sync_enabled, flag ? 1 : 0);
            if (flag && g_sync_event) SetEvent(g_sync_event);
            else if (!flag && g_sync_event) ResetEvent(g_sync_event);
            clip_log("SYNC_ENABLE %s", flag ? "ON" : "OFF");
            break;
        }

        case CLIP_MSG_FORMAT_LIST: {
            BYTE buf[16384];
            UINT32 count, i;
            int off;

            if (hdr.data_size > sizeof(buf)) goto done;
            if (!recv_exact(s, buf, (int)hdr.data_size)) goto done;

            if (!g_sync_enabled) {
                clip_log("FORMAT_LIST ignored (sync disabled).");
                break;
            }

            count = *(UINT32 *)buf;
            if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
            off = 4;

            clip_free_pending();
            g_clip_fmt_count = 0;
            for (i = 0; i < count && off + 8 <= (int)hdr.data_size; i++) {
                UINT32 fmt_id   = *(UINT32 *)(buf + off); off += 4;
                UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
                UINT local_id = fmt_id;

                if (name_len > 0 && name_len < 256 && off + (int)name_len <= (int)hdr.data_size) {
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
                g_clip_fmts[g_clip_fmt_count].is_hdrop = FALSE;
                g_clip_fmt_count++;
            }
            g_clip_fetch_idx = 0;

            clip_log("Received format list (%u formats), fetching data...", g_clip_fmt_count);

            /* Clean temp dir for potential file transfers */
            clip_remove_dir_recursive(g_clip_temp_dir);
            CreateDirectoryW(g_clip_temp_dir, NULL);

            /* Start fetching format data one at a time */
            clip_request_next_format();
            break;
        }

        case CLIP_MSG_FORMAT_DATA_RESP: {
            int idx = g_clip_fetch_idx;
            clip_log("Data response for format %u (%u bytes).", hdr.format, hdr.data_size);

            if (idx >= 0 && idx < g_clip_fmt_count) {
                if (hdr.format == CF_HDROP && hdr.data_size == 0) {
                    g_clip_fmts[idx].is_hdrop = TRUE;
                } else if (hdr.data_size > 0 && hdr.data_size <= CLIP_MAX_PAYLOAD) {
                    BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr.data_size);
                    if (data && recv_exact(s, data, (int)hdr.data_size)) {
                        g_clip_fmts[idx].data = data;
                        g_clip_fmts[idx].data_size = hdr.data_size;
                    } else {
                        if (data) HeapFree(GetProcessHeap(), 0, data);
                    }
                }
                g_clip_fetch_idx++;
                clip_request_next_format();
            }
            break;
        }

        case CLIP_MSG_FILE_DATA:
            CreateDirectoryW(g_clip_temp_dir, NULL);
            if (!clip_recv_file_data(s, &hdr)) goto done;
            break;

        default:
            clip_log("Unknown msg_type %u.", hdr.msg_type);
            if (hdr.data_size > 0) {
                BYTE skip[256];
                UINT32 remaining = hdr.data_size;
                while (remaining > 0) {
                    int chunk = remaining > 256 ? 256 : (int)remaining;
                    if (!recv_exact(s, skip, chunk)) goto done;
                    remaining -= chunk;
                }
            }
            break;
        }
    }

done:
    clip_log("Recv thread exiting.");
    return 0;
}

/* ---- Handle one client connection ---- */

static void handle_client(SOCKET s)
{
    UINT32 ready = CLIP_READY_MAGIC;
    HANDLE recv_th;

    /* Send ready signal */
    if (send(s, (const char *)&ready, sizeof(ready), 0) != sizeof(ready)) {
        clip_log("Failed to send ready signal (%d).", WSAGetLastError());
        return;
    }
    clip_log("Sent ready signal to host.");

    g_client_sock = s;

    /* Start recv thread */
    recv_th = CreateThread(NULL, 0, recv_thread_proc, (LPVOID)(UINT_PTR)s, 0, NULL);
    if (!recv_th) {
        clip_log("Failed to create recv thread (%lu).", GetLastError());
        g_client_sock = INVALID_SOCKET;
        return;
    }

    /* Wait for recv thread to finish (host disconnected) */
    WaitForSingleObject(recv_th, INFINITE);
    CloseHandle(recv_th);

    g_client_sock = INVALID_SOCKET;
    clip_free_pending();
    g_clip_fmt_count = 0;
    clip_remove_dir_recursive(g_clip_temp_dir);
    clip_log("Client disconnected.");
}

/* ---- Main ---- */

int main(void)
{
    WSADATA wsa;
    SOCKET listen_s;
    SOCKADDR_HV addr;
    HANDLE msg_th;

    clip_log("Starting (PID=%lu, session=%lu).",
             GetCurrentProcessId(),
             WTSGetActiveConsoleSessionId());

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        clip_log("WSAStartup failed (%d).", WSAGetLastError());
        return 1;
    }

    InitializeCriticalSection(&g_send_cs);
    g_clip_fetch_idx = 0;
    clip_init_temp_dir();

    g_sync_event = CreateEventW(NULL, TRUE, FALSE, CLIP_SYNC_EVENT_NAME);
    if (!g_sync_event)
        clip_log("Warning: failed to create sync event (%lu).", GetLastError());

    /* Start message window thread (clipboard listener) */
    msg_th = CreateThread(NULL, 0, msg_window_thread, NULL, 0, NULL);
    if (!msg_th) {
        clip_log("Failed to create message window thread (%lu).", GetLastError());
        DeleteCriticalSection(&g_send_cs);
        WSACleanup();
        return 1;
    }

    /* Wait a moment for message window to be created */
    Sleep(100);

    listen_s = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
    if (listen_s == INVALID_SOCKET) {
        clip_log("socket(AF_HYPERV) failed (%d).", WSAGetLastError());
        DeleteCriticalSection(&g_send_cs);
        WSACleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = HV_GUID_WILDCARD;
    addr.ServiceId = CLIPBOARD_SERVICE_GUID;

    {
        int bind_tries;
        for (bind_tries = 0; bind_tries < 10; bind_tries++) {
            if (bind(listen_s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
                break;
            clip_log("bind attempt %d failed (%d), retrying...", bind_tries + 1, WSAGetLastError());
            Sleep(500);
        }
        if (bind_tries == 10) {
            clip_log("bind failed after 10 attempts, exiting.");
            closesocket(listen_s);
            DeleteCriticalSection(&g_send_cs);
            WSACleanup();
            return 1;
        }
    }

    if (listen(listen_s, 2) != 0) {
        clip_log("listen failed (%d).", WSAGetLastError());
        closesocket(listen_s);
        DeleteCriticalSection(&g_send_cs);
        WSACleanup();
        return 1;
    }

    clip_log("Listening on GUID a5b0cafe-0005-4000-8000-000000000001.");

    /* Accept loop — one client at a time */
    for (;;) {
        SOCKET client_s = accept(listen_s, NULL, NULL);
        if (client_s == INVALID_SOCKET) {
            clip_log("accept failed (%d).", WSAGetLastError());
            Sleep(1000);
            continue;
        }
        clip_log("Host connected.");
        handle_client(client_s);
        closesocket(client_s);
    }
}
