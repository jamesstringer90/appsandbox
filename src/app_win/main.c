#include <windows.h>
#include <ole2.h>
#include <stdio.h>
#include "ui.h"
#include "headless.h"

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

    /* Headless daemon mode: appsandbox.exe --headless
       (normal launches pass no args, so the GUI path below is unaffected). */
    if (pCmdLine && wcsstr(pCmdLine, L"-headless")) {
        int rc = run_headless(hInstance, pCmdLine);
        OleUninitialize();
        return rc;
    }

    /* Single-instance: one AppSandbox (GUI or headless) at a time. The headless
       daemon (headless.c) acquires this SAME mutex, so GUI + headless are mutually
       exclusive -- required because both touch the one %ProgramData%\AppSandbox\
       vms.cfg (no file locking) and the machine-global HCN networks that asb_init
       wipes (hcn_cleanup_stale_networks). Global\ name = machine-wide (any folder),
       fixed string = path-independent. OS auto-releases on crash.
       The handle is intentionally held (leaked) for the process lifetime. */
    {
        HANDLE inst_mutex = CreateMutexW(NULL, FALSE, L"Global\\AppSandboxCoreHost");
        if (inst_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(NULL,
                L"AppSandbox is already running (either the app window or the "
                L"headless service). Only one can run at a time.",
                L"App Sandbox", MB_ICONINFORMATION | MB_OK);
            OleUninitialize();
            return 0;
        }
    }

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
