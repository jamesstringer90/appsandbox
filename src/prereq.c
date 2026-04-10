#include "prereq.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

#define PREREQ_PIPE_BUF 8192
#define PREREQ_FEATURE  L"VirtualMachinePlatform"

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

/* Parse the most recent percentage from DISM progress output like "[==== 65.3% ====]" */
static float parse_dism_pct(const char *buf, int len)
{
    int i;
    /* Scan backwards to find the last '%' and parse the number before it */
    for (i = len - 1; i >= 1; i--) {
        if (buf[i] == '%') {
            /* Walk backwards past the number (digits and '.') */
            int end = i;
            int start = i - 1;
            while (start >= 0 && ((buf[start] >= '0' && buf[start] <= '9') || buf[start] == '.'))
                start--;
            start++;
            if (start < end) {
                float pct = (float)strtod(&buf[start], NULL);
                if (pct >= 0.0f && pct <= 100.0f)
                    return pct;
            }
        }
    }
    return -1.0f;
}

BOOL prereq_enable_feature(const wchar_t *feature_name, BOOL *reboot_required,
                            PrereqProgressCallback progress_cb, void *user_data)
{
    wchar_t cmd[512];
    HANDLE read_pipe = NULL, write_pipe = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;
    char buf[4096];
    DWORD total = 0, bytes_read = 0;
    float last_pct = -1.0f;

    *reboot_required = FALSE;

    _snwprintf_s(cmd, 512, _TRUNCATE,
        L"C:\\Windows\\System32\\dism.exe /online /Enable-Feature /FeatureName:%ls /All /NoRestart",
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

    /* Read output incrementally, parsing progress percentages */
    while (ReadFile(read_pipe, buf + total, (DWORD)(sizeof(buf) - 1 - total), &bytes_read, NULL)
           && bytes_read > 0)
    {
        total += bytes_read;
        buf[total] = '\0';

        if (progress_cb) {
            float pct = parse_dism_pct(buf, (int)total);
            if (pct >= 0.0f && pct != last_pct) {
                last_pct = pct;
                progress_cb(pct, user_data);
            }
        }

        /* Keep only the last 512 bytes to avoid overflow while preserving
           enough context to parse the most recent progress line */
        if (total > 3072) {
            memmove(buf, buf + total - 512, 512);
            total = 512;
        }
    }

    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_pipe);

    /* 0 = success, 3010 = success but reboot required */
    if (exit_code == 3010) {
        *reboot_required = TRUE;
        return TRUE;
    }

    return exit_code == 0;
}

BOOL prereq_check_all(void)
{
    return prereq_is_feature_enabled(PREREQ_FEATURE);
}

BOOL prereq_reboot(void)
{
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
    return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OPERATINGSYSTEM);
}
