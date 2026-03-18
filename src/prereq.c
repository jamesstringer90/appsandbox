#include "prereq.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

#define PREREQ_PIPE_BUF 8192
#define PREREQ_FEATURE  L"HypervisorPlatform"

BOOL prereq_is_feature_enabled(const wchar_t *feature_name)
{
    BOOL enabled = FALSE;
    wchar_t cmd[512];
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char buf[PREREQ_PIPE_BUF];
    DWORD total = 0;
    DWORD bytes_read = 0;

    _snwprintf_s(cmd, 512, _TRUNCATE,
        L"C:\\Windows\\System32\\dism.exe /online /Get-FeatureInfo /FeatureName:%ls",
        feature_name);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return FALSE;

    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.wShowWindow = SW_HIDE;

    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return FALSE;
    }

    CloseHandle(write_pipe);

    ZeroMemory(buf, sizeof(buf));
    while (total < PREREQ_PIPE_BUF - 1 &&
        ReadFile(read_pipe, buf + total, PREREQ_PIPE_BUF - 1 - total, &bytes_read, NULL) &&
        bytes_read > 0)
    {
        total += bytes_read;
    }
    buf[total] = '\0';

    WaitForSingleObject(pi.hProcess, 10000);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_pipe);

    if (strstr(buf, "State : Enabled") || strstr(buf, "State : Enable Pending"))
        enabled = TRUE;

    return enabled;
}

BOOL prereq_enable_feature(const wchar_t *feature_name, BOOL *reboot_required)
{
    wchar_t cmd[512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;

    *reboot_required = FALSE;

    _snwprintf_s(cmd, 512, _TRUNCATE,
        L"C:\\Windows\\System32\\dism.exe /online /Enable-Feature /FeatureName:%ls /All /NoRestart",
        feature_name);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;

    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* 0 = success, 3010 = success but reboot required */
    if (exit_code == 3010) {
        *reboot_required = TRUE;
        return TRUE;
    }

    return exit_code == 0;
}

BOOL prereq_check_all(void)
{
    HMODULE hcs;
    BOOL reboot_required = FALSE;

    /* Quick check: if computecore.dll loads, HCS is available */
    hcs = LoadLibraryW(L"computecore.dll");
    if (hcs) {
        FreeLibrary(hcs);
        return TRUE;
    }

    /* HCS not available -- check if the feature is enabled */
    if (prereq_is_feature_enabled(PREREQ_FEATURE)) {
        ui_log(L"HypervisorPlatform is enabled but computecore.dll failed to load.");
        return FALSE;
    }

    /* Feature not enabled -- we already have admin (manifest), enable it */
    ui_log(L"HypervisorPlatform not enabled. Enabling...");

    if (!prereq_enable_feature(PREREQ_FEATURE, &reboot_required)) {
        MessageBoxW(NULL,
            L"Failed to enable HypervisorPlatform.\n\n"
            L"Please enable it manually via:\n"
            L"Settings > Apps > Optional Features > More Windows features > Hyper-V Platform",
            L"App Sandbox - Error",
            MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (reboot_required) {
        int result = MessageBoxW(NULL,
            L"HypervisorPlatform has been enabled successfully.\n\n"
            L"A reboot is required for the change to take effect. Reboot now?",
            L"App Sandbox - Reboot Required",
            MB_YESNO | MB_ICONINFORMATION);

        if (result == IDYES) {
            HANDLE token = NULL;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
                TOKEN_PRIVILEGES tp;
                ZeroMemory(&tp, sizeof(tp));
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
                AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
                CloseHandle(token);
            }
            ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OPERATINGSYSTEM);
        }

        return FALSE;
    }

    ui_log(L"HypervisorPlatform enabled successfully.");
    return TRUE;
}
