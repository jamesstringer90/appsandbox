#ifndef HEADLESS_H
#define HEADLESS_H

#include <windows.h>

/* Run the AppSandbox headless daemon (appsandbox.exe --headless).
   Owns the core for the process lifetime and serves a local HTTP API.
   Returns the process exit code. */
int run_headless(HINSTANCE hInst, const wchar_t *cmdline);

#define WM_ASB_API_REQUEST (WM_APP + 30)
#define WM_ASB_API_SHUTDOWN (WM_APP + 31)
BOOL asb_api_start(HINSTANCE instance, HWND owner);
void asb_api_stop(void);
LRESULT asb_api_dispatch(void *request);
BOOL asb_api_busy(void);

#endif /* HEADLESS_H */
