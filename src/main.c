#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include "ui.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow)
{
    HWND hwnd;
    MSG msg;

    (void)hPrevInstance;
    (void)pCmdLine;

    /* Per-monitor DPI awareness (Windows 10 1703+) */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    /* Initialize OLE/COM (required for WebView2) */
    OleInitialize(NULL);

    /* Create main window */
    hwnd = ui_create_main_window(hInstance, nCmdShow);
    if (!hwnd) {
        MessageBoxW(NULL, L"Failed to create main window.", L"App Sandbox", MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    /* Message loop */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    OleUninitialize();
    return (int)msg.wParam;
}
