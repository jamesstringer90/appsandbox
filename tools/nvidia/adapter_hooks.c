#include "adapter_hooks.h"
#include "adapter_identity.h"
#include <stdio.h>
#include <string.h>

#if defined(_M_X64) || defined(_M_IX86)
#if defined(_M_IX86)
#define NVIDIA_ICD_NAME "nvoglv32.dll"
#define NVIDIA_ICD_NAME_W L"nvoglv32.dll"
#else
#define NVIDIA_ICD_NAME "nvoglv64.dll"
#define NVIDIA_ICD_NAME_W L"nvoglv64.dll"
#endif

typedef NTSTATUS (NTAPI *EnumDisplayDevicesFn)(PUNICODE_STRING, DWORD,
                                             PDISPLAY_DEVICEW, DWORD);
typedef int (WINAPI *CudaInitFn)(unsigned);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static BOOL g_ready;
static volatile LONG g_hooks_enabled;
static NvidiaAdapterApi g_adapter;
static PFND3DKMT_OPENADAPTERFROMHDC g_open_hdc;
static EnumDisplayDevicesFn g_enum_displays;
static NvidiaAdapterIdentity *g_adapters;
static ULONG g_adapter_count;
static LUID g_luid;
static LUID g_icd_luid;
static wchar_t g_icd_path[MAX_PATH];
static DISPLAY_DEVICEW g_display;

static BOOL same_luid(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static BOOL resolve_adapter_icd_path(D3DKMT_HANDLE adapter, wchar_t *path, size_t capacity)
{
    D3DKMT_OPENGLINFO info = {0};
    const wchar_t *name, *source;
    wchar_t directory[MAX_PATH];
    DWORD attributes;

    if (!nvidia_query_adapter(&g_adapter, adapter, KMTQAITYPE_UMOPENGLINFO, &info, sizeof(info)))
        return FALSE;
    info.UmdOpenGlIcdFileName[MAX_PATH - 1] = L'\0';
    source = info.UmdOpenGlIcdFileName;
    name = wcsrchr(source, L'\\');
    name = name ? name + 1 : source;
    if (_wcsicmp(name, NVIDIA_ICD_NAME_W) != 0) return FALSE;

    if (wcsncmp(source, L"\\??\\", 4) == 0) source += 4;
    if (_wcsnicmp(source, L"\\SystemRoot\\", 12) == 0) {
        if (!GetWindowsDirectoryW(directory, MAX_PATH)) return FALSE;
        if (swprintf_s(path, capacity, L"%s\\%s", directory, source + 12) < 0)
            return FALSE;
    } else if (source[0] && source[1] == L':' && source[2] == L'\\') {
        if (wcslen(source) >= capacity) return FALSE;
        wcscpy_s(path, capacity, source);
    } else if (source == name) {
        if (!GetSystemDirectoryW(directory, MAX_PATH)) return FALSE;
        if (swprintf_s(path, capacity, L"%s\\%s", directory, source) < 0)
            return FALSE;
    } else {
        return FALSE;
    }
#if defined(_M_IX86)
    if (GetWindowsDirectoryW(directory, MAX_PATH)) {
        size_t length = wcslen(directory);
        if (_wcsnicmp(path, directory, length) == 0 &&
            _wcsnicmp(path + length, L"\\System32\\", 10) == 0 &&
            wcslen(path) + 1 < capacity) {
            wchar_t native[MAX_PATH];
            if (swprintf_s(native, ARRAYSIZE(native), L"%s\\Sysnative%s",
                           directory, path + length + 9) < 0)
                return FALSE;
            wcscpy_s(path, capacity, native);
        }
    }
#endif
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const wchar_t *store = path;
        wchar_t translated[MAX_PATH];
        while (*store && _wcsnicmp(store, L"\\DriverStore\\", 13) != 0) ++store;
        if (*store && wcslen(path) + 4 < capacity &&
            wcslen(path) + 4 < ARRAYSIZE(translated)) {
            if (swprintf_s(translated, ARRAYSIZE(translated), L"%.*s\\HostDriverStore%s",
                           (int)(store - path), path, store + 12) < 0)
                return FALSE;
            attributes = GetFileAttributesW(translated);
            if (attributes != INVALID_FILE_ATTRIBUTES)
                wcscpy_s(path, capacity, translated);
        }
    }
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL find_cuda_luid(const D3DKMT_ADAPTERADDRESS *address, LUID *luid)
{
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    CudaInitFn init;
    NvidiaCudaApi api;

    if (!cuda) return FALSE;
    if (GetProcAddress(cuda, "appsandbox_cuda")) {
        FreeLibrary(cuda);
        cuda = LoadLibraryExW(L"appsandbox-nvcuda.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!cuda) return FALSE;
    }
    init = (CudaInitFn)GetProcAddress(cuda, "cuInit");
    if (init && nvidia_cuda_api_load(cuda, &api) && init(0) == 0 &&
        nvidia_cuda_host_luid(&api, address, luid)) return TRUE;
    FreeLibrary(cuda);
    return FALSE;
}

static BOOL select_nvidia_adapter(void)
{
    D3DKMT_ENUMADAPTERS2 enumeration = {0};
    D3DKMT_ADAPTERINFO *adapters;
    NvidiaAdapterIdentity selected = {0};
    ULONG capacity, best_sources = 0;
    BOOL found = FALSE;

    if (g_adapter.enumerate(&enumeration) < 0 || !enumeration.NumAdapters)
        return FALSE;
    capacity = enumeration.NumAdapters;
    adapters = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                         capacity * sizeof(*adapters));
    g_adapters = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                           capacity * sizeof(*g_adapters));
    if (!adapters || !g_adapters) {
        if (adapters) HeapFree(GetProcessHeap(), 0, adapters);
        return FALSE;
    }
    enumeration.pAdapters = adapters;
    if (g_adapter.enumerate(&enumeration) >= 0 && enumeration.NumAdapters <= capacity) {
        for (ULONG i = 0; i < enumeration.NumAdapters; ++i) {
            wchar_t path[MAX_PATH];

            if (!nvidia_query_adapter_identity(&g_adapter, adapters[i].hAdapter,
                                               adapters[i].AdapterLuid, &g_adapters[g_adapter_count]))
                continue;
            ++g_adapter_count;
            if ((found && adapters[i].NumOfSources <= best_sources) ||
                !resolve_adapter_icd_path(adapters[i].hAdapter, path, MAX_PATH))
                continue;
            found = TRUE;
            best_sources = adapters[i].NumOfSources;
            g_luid = adapters[i].AdapterLuid;
            selected = g_adapters[g_adapter_count - 1];
            wcscpy_s(g_icd_path, MAX_PATH, path);
        }
    }
    for (ULONG i = 0; i < capacity; ++i) {
        if (adapters[i].hAdapter) {
            D3DKMT_CLOSEADAPTER close = {adapters[i].hAdapter};
            g_adapter.close(&close);
        }
    }
    HeapFree(GetProcessHeap(), 0, adapters);
    if (!found) return FALSE;

    capacity = g_adapter_count;
    g_adapter_count = 0;
    for (ULONG i = 0; i < capacity; ++i) {
        if (same_luid(g_adapters[i].luid, selected.luid) ||
            (selected.has_address && g_adapters[i].has_address &&
             memcmp(&g_adapters[i].ids, &selected.ids, sizeof(selected.ids)) == 0 &&
             memcmp(&g_adapters[i].address, &selected.address, sizeof(selected.address)) == 0))
            g_adapters[g_adapter_count++] = g_adapters[i];
    }
    g_icd_luid = g_luid;
    if (selected.has_address) find_cuda_luid(&selected.address, &g_icd_luid);
    swprintf_s(g_display.DeviceID, ARRAYSIZE(g_display.DeviceID),
               L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X",
               selected.ids.VendorID, selected.ids.DeviceID, selected.ids.SubSystemID,
               selected.ids.SubVendorID, selected.ids.RevisionID);
    wcscpy_s(g_display.DeviceString, ARRAYSIZE(g_display.DeviceString), L"NVIDIA");
    g_display.cb = sizeof(g_display);
    g_display.StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    return TRUE;
}

static BOOL find_desktop_display_name(void)
{
    DISPLAY_DEVICEW display = {0};

    for (DWORD i = 0; ; ++i) {
        display.cb = sizeof(display);
        if (g_enum_displays(NULL, i, &display, 0) < 0) break;
        if (display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            wcscpy_s(g_display.DeviceName, ARRAYSIZE(g_display.DeviceName), display.DeviceName);
            return TRUE;
        }
        if (!g_display.DeviceName[0] &&
            (display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            wcscpy_s(g_display.DeviceName, ARRAYSIZE(g_display.DeviceName), display.DeviceName);
    }
    return g_display.DeviceName[0] != L'\0';
}

static BOOL call_stack_contains_nvidia_icd(void)
{
    void *frames[24];
    USHORT count = CaptureStackBackTrace(1, ARRAYSIZE(frames), frames, NULL);

    /* Discovery runs inside the ICD's DllMain, so do not acquire the loader lock. */
    for (USHORT i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION memory;
        if (!VirtualQuery(frames[i], &memory, sizeof(memory)) || memory.Type != MEM_IMAGE)
            continue;
        __try {
            BYTE *base = memory.AllocationBase;
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
            IMAGE_NT_HEADERS *nt;
            IMAGE_DATA_DIRECTORY directory;
            IMAGE_EXPORT_DIRECTORY *exports;
            DWORD size;

            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) continue;
            nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
            size = nt->OptionalHeader.SizeOfImage;
            directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!directory.VirtualAddress || size < sizeof(*exports) ||
                directory.VirtualAddress > size - sizeof(*exports))
                continue;
            exports = (IMAGE_EXPORT_DIRECTORY *)(base + directory.VirtualAddress);
            if (exports->Name && exports->Name <= size - sizeof(NVIDIA_ICD_NAME) &&
                _stricmp((const char *)(base + exports->Name), NVIDIA_ICD_NAME) == 0)
                return TRUE;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return FALSE;
}

static NTSTATUS NTAPI enum_display_devices_hook(PUNICODE_STRING device, DWORD index,
                                   PDISPLAY_DEVICEW display, DWORD flags)
{
    if (InterlockedCompareExchange(&g_hooks_enabled, 0, 0) &&
        (!device || !device->Buffer || !device->Length) && display &&
        display->cb >= sizeof(*display) && call_stack_contains_nvidia_icd()) {
        if (index == 0) {
            DWORD size = display->cb;
            *display = g_display;
            display->cb = size;
            return 0;
        }
        --index;
    }
    return g_enum_displays(device, index, display, flags);
}

static NTSTATUS APIENTRY open_adapter_from_hdc_hook(D3DKMT_OPENADAPTERFROMHDC *adapter)
{
    NTSTATUS status = g_open_hdc(adapter);

    if (status >= 0 && adapter && InterlockedCompareExchange(&g_hooks_enabled, 0, 0) &&
        call_stack_contains_nvidia_icd()) {
        if (!same_luid(adapter->AdapterLuid, g_luid)) {
            D3DKMT_OPENADAPTERFROMLUID replacement = {0};
            replacement.AdapterLuid = g_luid;
            if (g_adapter.open(&replacement) >= 0) {
                D3DKMT_CLOSEADAPTER close = {adapter->hAdapter};
                if (g_adapter.close(&close) >= 0) {
                    adapter->hAdapter = replacement.hAdapter;
                    adapter->AdapterLuid = replacement.AdapterLuid;
                    adapter->VidPnSourceId = 0;
                } else {
                    close.hAdapter = replacement.hAdapter;
                    g_adapter.close(&close);
                }
            }
        }
        if (same_luid(adapter->AdapterLuid, g_luid)) adapter->AdapterLuid = g_icd_luid;
    }
    return status;
}

static void *volatile *find_win32u_import_slot(HMODULE module, const char *name)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imports;
    DWORD rva;

    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return NULL;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return NULL;
    imports = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    for (; imports->Name; ++imports) {
        IMAGE_THUNK_DATA *names, *addresses;
        if (_stricmp((const char *)(base + imports->Name), "win32u.dll") != 0 ||
            !imports->OriginalFirstThunk || !imports->FirstThunk)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + imports->OriginalFirstThunk);
        addresses = (IMAGE_THUNK_DATA *)(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            IMAGE_IMPORT_BY_NAME *entry;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            entry = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)entry->Name, name) == 0)
                return (void *volatile *)&addresses->u1.Function;
        }
    }
    return NULL;
}

static BOOL install_win32u_import_hooks(HMODULE user32, HMODULE gdi32)
{
    void *volatile *display_slot = find_win32u_import_slot(user32, "NtUserEnumDisplayDevices");
    void *volatile *adapter_slot = find_win32u_import_slot(gdi32, "NtGdiDdDDIOpenAdapterFromHdc");
    void *display_original, *adapter_original;
    DWORD display_protection, adapter_protection, unused;
    HMODULE pinned;
    BOOL installed = FALSE;

    if (!display_slot || !adapter_slot || !*display_slot || !*adapter_slot)
        return FALSE;
    display_original = *display_slot;
    adapter_original = *adapter_slot;
    if (display_original == (void *)enum_display_devices_hook || adapter_original == (void *)open_adapter_from_hdc_hook)
        return FALSE;
    g_enum_displays = (EnumDisplayDevicesFn)display_original;
    g_open_hdc = (PFND3DKMT_OPENADAPTERFROMHDC)adapter_original;

    if (!VirtualProtect((void *)display_slot, sizeof(*display_slot), PAGE_READWRITE,
                        &display_protection))
        return FALSE;
    if (!VirtualProtect((void *)adapter_slot, sizeof(*adapter_slot), PAGE_READWRITE,
                        &adapter_protection)) {
        VirtualProtect((void *)display_slot, sizeof(*display_slot), display_protection, &unused);
        return FALSE;
    }
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          (LPCWSTR)nvidia_install_adapter_hooks, &pinned) &&
        InterlockedCompareExchangePointer(display_slot, (void *)enum_display_devices_hook,
                                          display_original) == display_original) {
        if (InterlockedCompareExchangePointer(adapter_slot, (void *)open_adapter_from_hdc_hook,
                                              adapter_original) == adapter_original) {
            InterlockedExchange(&g_hooks_enabled, TRUE);
            installed = TRUE;
        } else {
            InterlockedCompareExchangePointer(display_slot, display_original, (void *)enum_display_devices_hook);
        }
    }
    VirtualProtect((void *)adapter_slot, sizeof(*adapter_slot), adapter_protection, &unused);
    VirtualProtect((void *)display_slot, sizeof(*display_slot), display_protection, &unused);
    return installed;
}

static BOOL CALLBACK initialize_adapter_hooks(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE user32, gdi32;
    (void)once;
    (void)parameter;
    (void)context;

    user32 = LoadLibraryExW(L"user32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    gdi32 = nvidia_adapter_api_load(&g_adapter) ? g_adapter.module : NULL;
    if (!user32 || !gdi32) goto done;
    if (GetModuleHandleW(NVIDIA_ICD_NAME_W)) goto done;
    if (g_adapter.enumerate) {
        void *volatile *slot = find_win32u_import_slot(user32, "NtUserEnumDisplayDevices");
        if (slot) g_enum_displays = (EnumDisplayDevicesFn)*slot;
        if (g_enum_displays && select_nvidia_adapter() && !GetModuleHandleW(NVIDIA_ICD_NAME_W) &&
            find_desktop_display_name())
            g_ready = install_win32u_import_hooks(user32, gdi32);
    }
done:
    if (!g_ready) {
        if (g_adapters) HeapFree(GetProcessHeap(), 0, g_adapters);
        g_adapters = NULL;
        g_adapter_count = 0;
        if (user32) FreeLibrary(user32);
        if (gdi32) FreeLibrary(gdi32);
    }
    return TRUE;
}

BOOL nvidia_install_adapter_hooks(void)
{
    InitOnceExecuteOnce(&g_once, initialize_adapter_hooks, NULL, NULL);
    return g_ready;
}

BOOL WINAPI appsandbox_nvidia_adapter_luid(LUID *luid)
{
    if (!luid || !InterlockedCompareExchange(&g_hooks_enabled, 0, 0)) return FALSE;
    *luid = g_luid;
    return TRUE;
}

BOOL nvidia_get_icd_path(wchar_t *path, size_t capacity)
{
    if (!path || !capacity) return FALSE;
    path[0] = L'\0';
    if (!nvidia_install_adapter_hooks() || wcslen(g_icd_path) >= capacity) return FALSE;
    wcscpy_s(path, capacity, g_icd_path);
    return TRUE;
}

BOOL nvidia_map_luid_to_icd(LUID *luid)
{
    if (!luid || !nvidia_install_adapter_hooks()) return FALSE;
    for (ULONG i = 0; i < g_adapter_count; ++i) {
        if (same_luid(*luid, g_adapters[i].luid)) {
            *luid = g_icd_luid;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL nvidia_map_luid_to_guest(LUID *luid)
{
    if (!luid || !nvidia_install_adapter_hooks() || !same_luid(*luid, g_icd_luid)) return FALSE;
    *luid = g_luid;
    return TRUE;
}

#else

BOOL nvidia_install_adapter_hooks(void)
{
    return FALSE;
}

BOOL WINAPI appsandbox_nvidia_adapter_luid(LUID *luid)
{
    (void)luid;
    return FALSE;
}

BOOL nvidia_get_icd_path(wchar_t *path, size_t capacity)
{
    if (path && capacity) path[0] = L'\0';
    return FALSE;
}

BOOL nvidia_map_luid_to_icd(LUID *luid)
{
    (void)luid;
    return FALSE;
}

BOOL nvidia_map_luid_to_guest(LUID *luid)
{
    (void)luid;
    return FALSE;
}

#endif
