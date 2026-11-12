#include <windows.h>
#include <dbt.h>
#include <wtsapi32.h>
#include <stdlib.h>

#pragma comment(lib, "wtsapi32.lib")

static BOOL is_hyperv_display(const DISPLAYCONFIG_PATH_INFO *path)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {0};
    DISPLAY_DEVICEW device = {0};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path->sourceInfo.adapterId;
    source.header.id = path->sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) return 0;

    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &device, 0); i++) {
        if (_wcsicmp(device.DeviceName, source.viewGdiDeviceName) == 0) {
            return _wcsicmp(device.DeviceID, L"ROOT\\BasicDisplay") == 0 ||
                _wcsicmp(device.DeviceID,
                    L"VMBUS\\{DA0A7802-E377-4AAC-8E77-0558EB1073F8}") == 0;
        }
        device.cb = sizeof(device);
    }
    return 0;
}

static int disable_hyperv_outputs(void)
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
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS || !path_count || !mode_count) break;
        paths = calloc(path_count, sizeof(*paths));
        modes = calloc(mode_count, sizeof(*modes));
        if (!paths || !modes) {
            result = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths,
                                    &mode_count, modes, NULL);
        if (result != ERROR_INSUFFICIENT_BUFFER) break;
    }

    if (result == ERROR_SUCCESS && paths && modes) {
        UINT32 kept = 0;
        for (UINT32 i = 0; i < path_count; i++) {
            if (is_hyperv_display(&paths[i])) continue;
            paths[kept++] = paths[i];
        }
        if (kept && kept < path_count) {
            UINT32 source = paths[0].sourceInfo.modeInfoIdx;
            if (source >= mode_count || modes[source].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                result = ERROR_INVALID_DATA;
            } else {
                POINTL origin = modes[source].sourceMode.position;
                for (UINT32 i = 0; i < kept; i++) {
                    UINT32 index = paths[i].sourceInfo.modeInfoIdx;
                    if (index < mode_count && modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                        modes[index].sourceMode.position.x == 0 && modes[index].sourceMode.position.y == 0) {
                        origin = modes[index].sourceMode.position;
                        break;
                    }
                }
                for (UINT32 i = 0; i < mode_count; i++) {
                    if (modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                        modes[i].sourceMode.position.x -= origin.x;
                        modes[i].sourceMode.position.y -= origin.y;
                    }
                }
                result = SetDisplayConfig(kept, paths, mode_count, modes,
                    SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
            }
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

static HANDLE g_display_changed;
static HANDLE g_display_stop;

static DWORD WINAPI display_worker(LPVOID parameter)
{
    HWND window = (HWND)parameter;
    HANDLE events[] = {g_display_stop, g_display_changed};
    while (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        DWORD result = (DWORD)disable_hyperv_outputs();
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

static int watch_hyperv_outputs(void)
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
    g_display_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
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
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
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

int main(void)
{
    return watch_hyperv_outputs();
}
