#include <windows.h>
#include <dbt.h>
#include <wtsapi32.h>
#include <stdlib.h>
#include <errno.h>

#pragma comment(lib, "wtsapi32.lib")

static HANDLE g_display_changed;
static HANDLE g_display_stop;

static BOOL is_appsandbox_display(const DISPLAYCONFIG_PATH_INFO *path)
{
    DISPLAYCONFIG_TARGET_DEVICE_NAME target = {0};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path->targetInfo.adapterId;
    target.header.id = path->targetInfo.id;
    return DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS &&
        !_wcsicmp(target.monitorFriendlyDeviceName, L"AppSandboxVDD");
}

static int disable_extra_displays(void)
{
    HDESK original_desktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK input_desktop = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 path_count = 0, mode_count = 0;
    LONG result = ERROR_INSUFFICIENT_BUFFER;

    if (!input_desktop || !SetThreadDesktop(input_desktop)) {
        result = (LONG)GetLastError();
        goto done;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        free(paths);
        free(modes);
        paths = NULL;
        modes = NULL;
        result = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS || !path_count) break;
        paths = calloc(path_count, sizeof(*paths));
        modes = calloc(mode_count ? mode_count : 1, sizeof(*modes));
        if (!paths || !modes) {
            result = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        result = QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths,
                                    &mode_count, modes, NULL);
        if (result != ERROR_INSUFFICIENT_BUFFER) break;
    }

    if (result == ERROR_SUCCESS && paths && modes) {
        UINT32 kept = 0, active = 0;
        BOOL activate = FALSE;
        for (UINT32 i = 0; i < path_count; i++) {
            if (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) active++;
            if (!paths[i].targetInfo.targetAvailable || !is_appsandbox_display(&paths[i])) continue;
            UINT32 j;
            for (j = 0; j < kept; j++) {
                if (paths[j].targetInfo.id == paths[i].targetInfo.id &&
                    paths[j].targetInfo.adapterId.LowPart == paths[i].targetInfo.adapterId.LowPart &&
                    paths[j].targetInfo.adapterId.HighPart == paths[i].targetInfo.adapterId.HighPart) break;
            }
            if (j < kept) continue;
            if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) {
                activate = TRUE;
                paths[i].flags |= DISPLAYCONFIG_PATH_ACTIVE;
            }
            paths[kept++] = paths[i];
        }
        if (kept && (activate || kept < active)) {
            POINTL origin = {0};
            BOOL have_origin = FALSE;
            for (UINT32 i = 0; i < kept; i++) {
                UINT32 index = paths[i].sourceInfo.modeInfoIdx;
                if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) continue;
                if (index >= mode_count || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    result = ERROR_INVALID_DATA;
                    goto done;
                }
                POINTL position = modes[index].sourceMode.position;
                if (!have_origin || (position.x == 0 && position.y == 0)) {
                    origin = position;
                    have_origin = TRUE;
                }
            }
            for (UINT32 i = 0; i < mode_count; i++) {
                if (modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    modes[i].sourceMode.position.x -= origin.x;
                    modes[i].sourceMode.position.y -= origin.y;
                }
            }
            if (WaitForSingleObject(g_display_stop, 0) != WAIT_OBJECT_0)
                result = SetDisplayConfig(kept, paths, mode_count, modes,
                    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
                    (activate ? SDC_ALLOW_CHANGES : 0));
        }
    }
done:
    free(modes);
    free(paths);
    if (input_desktop) {
        SetThreadDesktop(original_desktop);
        CloseDesktop(input_desktop);
    }
    return (int)result;
}

static DWORD WINAPI display_worker(LPVOID parameter)
{
    HWND window = (HWND)parameter;
    HANDLE events[] = {g_display_stop, g_display_changed};
    while (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        DWORD result = (DWORD)disable_extra_displays();
        if (result != ERROR_SUCCESS) {
            PostMessageW(window, WM_CLOSE, 0, 0);
            return result;
        }
    }
    return 0;
}

static LRESULT CALLBACK display_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_DISPLAYCHANGE:
    case WM_WTSSESSION_CHANGE:
        SetEvent(g_display_changed);
        return 0;
    case WM_DEVICECHANGE:
        if ((wparam == DBT_DEVICEARRIVAL || wparam == DBT_DEVICEREMOVECOMPLETE) &&
            lparam && ((DEV_BROADCAST_HDR *)lparam)->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
            SetEvent(g_display_changed);
        return 0;
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static int watch_hyperv_outputs(HANDLE stop_event, HANDLE parent)
{
    static const GUID monitor_interface =
        {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
    WNDCLASSW cls = {0};
    DEV_BROADCAST_DEVICEINTERFACE_W filter = {0};
    HWND window = NULL;
    HANDLE worker = NULL;
    HDEVNOTIFY notification = NULL;
    BOOL session_notifications = FALSE;
    DWORD result = 1;
    MSG message;

    g_display_changed = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_display_stop = stop_event ? stop_event : CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_display_changed || !g_display_stop) goto done;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpfnWndProc = display_window_proc;
    cls.lpszClassName = L"AppSandboxDisplayMonitor";
    if (!RegisterClassW(&cls)) goto done;
    window = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, cls.hInstance, NULL);
    if (!window) goto done;
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = monitor_interface;
    notification = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    session_notifications = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
    if (!notification) goto done;
    worker = CreateThread(NULL, 0, display_worker, window, 0, NULL);
    if (!worker) goto done;
    SetEvent(g_display_changed);
    for (;;) {
        HANDLE events[] = {g_display_stop, parent};
        DWORD count = parent ? ARRAYSIZE(events) : 1;
        DWORD wait = MsgWaitForMultipleObjects(count, events, FALSE, INFINITE, QS_ALLINPUT);
        if (wait != WAIT_OBJECT_0 + count) break;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) goto done;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
done:
    if (g_display_stop) SetEvent(g_display_stop);
    if (worker) {
        WaitForSingleObject(worker, INFINITE);
        GetExitCodeThread(worker, &result);
        CloseHandle(worker);
    }
    if (notification) UnregisterDeviceNotification(notification);
    if (session_notifications) WTSUnRegisterSessionNotification(window);
    if (window && IsWindow(window)) DestroyWindow(window);
    if (g_display_stop) CloseHandle(g_display_stop);
    if (g_display_changed) CloseHandle(g_display_changed);
    return (int)result;
}

int wmain(int argc, wchar_t **argv)
{
    HANDLE stop_event = NULL;
    HANDLE parent = NULL;
    if (argc != 1) {
        if (argc != 3 || wcscmp(argv[1], L"--stop-event")) return ERROR_INVALID_PARAMETER;
        wchar_t *end;
        errno = 0;
        DWORD parent_id = wcstoul(argv[2], &end, 10);
        if (errno || !parent_id || end == argv[2] || *end != ':') return ERROR_INVALID_PARAMETER;
        wchar_t *start = end + 1;
        unsigned long long value = _wcstoui64(start, &end, 10);
        if (errno || !value || start == end || *end || value != (ULONG_PTR)value) return ERROR_INVALID_PARAMETER;
        parent = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, parent_id);
        if (!parent) return (int)GetLastError();
        BOOL copied = DuplicateHandle(parent, (HANDLE)(ULONG_PTR)value, GetCurrentProcess(),
                                      &stop_event, SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, 0);
        DWORD error = copied ? ERROR_SUCCESS : GetLastError();
        if (!copied) {
            CloseHandle(parent);
            return (int)error;
        }
    }
    int result = watch_hyperv_outputs(stop_event, parent);
    if (parent) CloseHandle(parent);
    return result;
}
