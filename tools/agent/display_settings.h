#ifndef ASB_GUEST_DISPLAY_SETTINGS_H
#define ASB_GUEST_DISPLAY_SETTINGS_H

#include <windows.h>
#include <ctype.h>
#include <string.h>
#include <wchar.h>

static __inline BOOL asb_display_settings_path(wchar_t *path, UINT capacity)
{
    static const wchar_t suffix[] = L"\\AppSandbox\\display_settings.cfg";
    UINT length = GetWindowsDirectoryW(path, capacity);
    if (!length || length >= capacity || ARRAYSIZE(suffix) > capacity - length) return FALSE;
    memcpy(path + length, suffix, sizeof(suffix));
    return TRUE;
}

static __inline BOOL asb_display_settings_enabled(void)
{
    static const char key[] = "automaticallyDisableExtraDisplays=";
    wchar_t path[MAX_PATH];
    char buffer[128];
    DWORD length = 0;
    if (!asb_display_settings_path(path, ARRAYSIZE(path))) return TRUE;
    HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return TRUE;
    BOOL read = ReadFile(file, buffer, sizeof(buffer) - 1, &length, NULL);
    CloseHandle(file);
    if (!read || length == sizeof(buffer) - 1) return TRUE;
    buffer[length] = '\0';
    const char *value = buffer;
    while (isspace((unsigned char)*value)) value++;
    if (strncmp(value, key, sizeof(key) - 1)) return TRUE;
    value += sizeof(key) - 1;
    while (isspace((unsigned char)*value)) value++;
    char enabled = *value++;
    if (enabled != '0' && enabled != '1') return TRUE;
    while (isspace((unsigned char)*value)) value++;
    return *value ? TRUE : enabled == '1';
}

static __inline DWORD asb_display_settings_save(BOOL enabled)
{
    wchar_t path[MAX_PATH], temporary[MAX_PATH + 4];
    const char *value = enabled ? "automaticallyDisableExtraDisplays=1\r\n" :
                                  "automaticallyDisableExtraDisplays=0\r\n";
    DWORD length = (DWORD)strlen(value), written = 0;
    if (!asb_display_settings_path(path, ARRAYSIZE(path)) ||
        swprintf_s(temporary, ARRAYSIZE(temporary), L"%ls.tmp", path) < 0)
        return ERROR_FILENAME_EXCED_RANGE;
    HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    BOOL saved = WriteFile(file, value, length, &written, NULL) && written == length &&
        FlushFileBuffers(file);
    DWORD result = saved ? ERROR_SUCCESS : GetLastError();
    if (!saved && result == ERROR_SUCCESS) result = ERROR_WRITE_FAULT;
    if (!CloseHandle(file) && result == ERROR_SUCCESS) result = GetLastError();
    if (result == ERROR_SUCCESS &&
        !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        result = GetLastError();
    if (result != ERROR_SUCCESS) DeleteFileW(temporary);
    return result;
}

#endif
