/*
 * appsandbox-input.exe — Console-session input injector for AppSandbox.
 *
 * Runs in the interactive console session (Session 1+), spawned by the
 * agent service via CreateProcessAsUser. Listens on AF_HYPERV socket
 * (service GUID :0003) for InputPacket messages from the host and calls
 * SendInput to inject mouse/keyboard events into the active desktop.
 *
 * Logs to C:\Windows\AppSandbox\input.log (beside agent.log).
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

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

/* Input channel: {A5B0CAFE-0003-4000-8000-000000000001} */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Input protocol (must match vm_display_idd.c) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3
#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 type;
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;
#pragma pack(pop)

/* ---- Logging ---- */

static void input_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\input.log", "a") != 0 || !f)
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

/* ---- Desktop switching ---- */

static void switch_to_input_desktop(void)
{
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (desk) {
        SetThreadDesktop(desk);
        CloseDesktop(desk);
    }
}

/* ---- Input injection ---- */

static void inject_input(const InputPacket *pkt)
{
    INPUT inp;
    UINT result;
    ZeroMemory(&inp, sizeof(inp));
    switch_to_input_desktop();

    switch (pkt->type) {
    case INPUT_MOUSE_MOVE: {
        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0) screen_w = 1920;
        if (screen_h <= 0) screen_h = 1080;
        inp.type = INPUT_MOUSE;
        inp.mi.dx = (LONG)(pkt->param1 * 65535 / (UINT32)(screen_w - 1));
        inp.mi.dy = (LONG)(pkt->param2 * 65535 / (UINT32)(screen_h - 1));
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(MOUSE_MOVE) failed: %lu", GetLastError());
        break;
    }
    case INPUT_MOUSE_BUTTON: {
        inp.type = INPUT_MOUSE;
        switch (pkt->param1) {
        case INPUT_BTN_LEFT:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case INPUT_BTN_RIGHT:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case INPUT_BTN_MIDDLE:
            inp.mi.dwFlags = pkt->param2 ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        default:
            return;
        }
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(MOUSE_BUTTON btn=%u down=%u) failed: %lu",
                       pkt->param1, pkt->param2, GetLastError());
        break;
    }
    case INPUT_MOUSE_WHEEL: {
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        inp.mi.mouseData = (DWORD)(INT32)pkt->param1;
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(MOUSE_WHEEL delta=%d) failed: %lu",
                       (INT32)pkt->param1, GetLastError());
        break;
    }
    case INPUT_KEY: {
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = (WORD)pkt->param1;
        inp.ki.wScan = (WORD)pkt->param2;
        inp.ki.dwFlags = 0;
        if (pkt->param3 & 1) inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (pkt->param3 & 2) inp.ki.dwFlags |= KEYEVENTF_KEYUP;
        result = SendInput(1, &inp, sizeof(INPUT));
        if (result == 0)
            input_log("SendInput(KEY vk=0x%X scan=0x%X flags=0x%X) failed: %lu",
                       pkt->param1, pkt->param2, pkt->param3, GetLastError());
        break;
    }
    }
}

/* ---- Handle one client connection ---- */

#define INPUT_READY_MAGIC  0x59445249  /* "IRDY" little-endian */

static void handle_client(SOCKET s)
{
    InputPacket pkt;
    UINT pkt_count = 0;
    UINT32 ready = INPUT_READY_MAGIC;

    /* Tell the host we're ready to receive input */
    if (send(s, (const char *)&ready, sizeof(ready), 0) != sizeof(ready)) {
        input_log("Failed to send ready signal (%d).", WSAGetLastError());
        return;
    }
    input_log("Sent ready signal to host.");

    input_log("Entering recv loop.");

    while (1) {
        int total = 0;
        char *p = (char *)&pkt;
        int pkt_size = (int)sizeof(pkt);

        while (total < pkt_size) {
            int n = recv(s, p + total, pkt_size - total, 0);
            if (n <= 0) {
                if (n == 0) {
                    input_log("Client disconnected after %u packets.", pkt_count);
                    return;
                }
                input_log("recv error %d (n=%d) after %u packets.", WSAGetLastError(), n, pkt_count);
                return;
            }
            total += n;
        }

        if (pkt.magic != INPUT_MAGIC) {
            input_log("Bad magic 0x%08X, skipping.", pkt.magic);
            continue;
        }

        pkt_count++;
        if (pkt_count == 1)
            input_log("First packet: type=%u p1=%u p2=%u p3=%u",
                       pkt.type, pkt.param1, pkt.param2, pkt.param3);

        inject_input(&pkt);
    }
}

/* ---- Main: listen on AF_HYPERV, accept connections ---- */

int main(void)
{
    WSADATA wsa;
    SOCKET listen_s;
    SOCKADDR_HV addr;

    input_log("Starting (PID=%lu, session=%lu).",
              GetCurrentProcessId(),
              WTSGetActiveConsoleSessionId());

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        input_log("WSAStartup failed (%d).", WSAGetLastError());
        return 1;
    }

    listen_s = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
    if (listen_s == INVALID_SOCKET) {
        input_log("socket(AF_HYPERV) failed (%d).", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = HV_GUID_WILDCARD;
    addr.ServiceId = INPUT_SERVICE_GUID;

    if (bind(listen_s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        input_log("bind failed (%d).", WSAGetLastError());
        closesocket(listen_s);
        WSACleanup();
        return 1;
    }

    if (listen(listen_s, 2) != 0) {
        input_log("listen failed (%d).", WSAGetLastError());
        closesocket(listen_s);
        WSACleanup();
        return 1;
    }

    input_log("Listening on GUID a5b0cafe-0003-4000-8000-000000000001.");

    /* Accept loop — one client at a time */
    for (;;) {
        SOCKET client_s = accept(listen_s, NULL, NULL);
        if (client_s == INVALID_SOCKET) {
            input_log("accept failed (%d).", WSAGetLastError());
            Sleep(1000);
            continue;
        }
        input_log("Host connected.");
        handle_client(client_s);
        closesocket(client_s);
    }

}
