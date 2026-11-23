#include "monitor_edid.h"
#include <setupapi.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "setupapi.lib")

static BOOL read_edid_value(HKEY key, const wchar_t *subkey, AsbEdid *edid)
{
    DWORD size = sizeof(edid->bytes);
    if (RegGetValueW(key, subkey, L"EDID", RRF_RT_REG_BINARY, NULL, edid->bytes, &size) != ERROR_SUCCESS ||
        size < 128) return FALSE;
    unsigned int declared = (1u + edid->bytes[126]) * 128;
    if (declared > size || !asb_edid_valid(edid->bytes, declared)) return FALSE;
    edid->size = declared;
    return TRUE;
}

BOOL read_monitor_edid(const wchar_t *path, AsbEdid *edid)
{
    SP_DEVICE_INTERFACE_DATA iface = { sizeof(iface) };
    SP_DEVINFO_DATA device = { sizeof(device) };
    HDEVINFO devices = SetupDiCreateDeviceInfoList(NULL, NULL);
    edid->size = 0;
    if (devices == INVALID_HANDLE_VALUE) return FALSE;
    if (SetupDiOpenDeviceInterfaceW(devices, path, 0, &iface)) {
        /* The size query also returns the device behind this exact interface. */
        DWORD required;
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &iface, NULL, 0, &required, &device) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key != INVALID_HANDLE_VALUE) {
                if (!read_edid_value(key, NULL, edid)) read_edid_value(key, L"Device Parameters", edid);
                RegCloseKey(key);
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return edid->size != 0;
}

void enum_monitor_edid_sources(MonitorVisitor visit, void *context)
{
    UINT32 npaths, nmodes;
    LONG result;
    do {
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npaths, &nmodes) != ERROR_SUCCESS ||
            !npaths || !nmodes) return;
        DISPLAYCONFIG_PATH_INFO *paths = calloc(npaths, sizeof(*paths));
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nmodes, sizeof(*modes));
        result = paths && modes ? QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npaths, paths,
            &nmodes, modes, NULL) : ERROR_OUTOFMEMORY;
        if (result == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < npaths; ++i) {
                DISPLAYCONFIG_TARGET_DEVICE_NAME target = {0};
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {0};
                AsbEdid edid;
                target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                target.header.size = sizeof(target);
                target.header.adapterId = paths[i].targetInfo.adapterId;
                target.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS ||
                    !read_monitor_edid(target.monitorDevicePath, &edid)) continue;
                source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source.header.size = sizeof(source);
                source.header.adapterId = paths[i].sourceInfo.adapterId;
                source.header.id = paths[i].sourceInfo.id;
                DisplayConfigGetDeviceInfo(&source.header);
                const wchar_t *display = wcsrchr(source.viewGdiDeviceName, L'\\');
                wchar_t name[128];
                swprintf_s(name, ARRAYSIZE(name), L"%s (%s)",
                    target.monitorFriendlyDeviceName[0] ? target.monitorFriendlyDeviceName : L"Monitor",
                    display ? display + 1 : L"Display");
                /* Mirrored monitors share a Windows display number. */
                for (UINT32 j = 0; j < npaths; ++j) {
                    const DISPLAYCONFIG_PATH_SOURCE_INFO *other = &paths[j].sourceInfo;
                    if (j != i && source.header.id == other->id &&
                        source.header.adapterId.LowPart == other->adapterId.LowPart &&
                        source.header.adapterId.HighPart == other->adapterId.HighPart) {
                        size_t used = wcslen(name);
                        swprintf_s(name + used, ARRAYSIZE(name) - used, L" - output %u", target.header.id);
                        break;
                    }
                }
                visit(target.monitorDevicePath, name, context);
            }
        }
        free(modes);
        free(paths);
    } while (result == ERROR_INSUFFICIENT_BUFFER);
}
