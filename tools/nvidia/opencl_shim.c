#define COBJMACROS
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include "adapter_identity.h"

#define CL_SUCCESS 0
#define CL_INVALID_VALUE (-30)
#define CL_INVALID_DEVICE (-33)
#define CL_PLATFORM_NOT_FOUND_KHR (-1001)
#define CL_DEVICE_TYPE_GPU 4
#define CL_DEVICE_VENDOR_ID 0x1001
#define CL_DEVICE_LUID_VALID_KHR 0x106c
#define CL_DEVICE_LUID_KHR 0x106d
#define CL_DEVICE_NODE_MASK_KHR 0x106e
#define CL_DEVICE_PCI_BUS_INFO_KHR 0x410f

typedef void *ClPlatform;
typedef void *ClDevice;
typedef int (WINAPI *PlatformsFn)(UINT, ClPlatform *, UINT *);
typedef int (WINAPI *PlatformInfoFn)(ClPlatform, UINT, size_t, void *, size_t *);
typedef int (WINAPI *DevicesFn)(ClPlatform, ULONGLONG, UINT, ClDevice *, UINT *);
typedef int (WINAPI *DeviceInfoFn)(ClDevice, UINT, size_t, void *, size_t *);
typedef void *(WINAPI *ExtensionFn)(const char *);
typedef void *(WINAPI *PlatformFunctionFn)(ClPlatform, const char *);
typedef int (WINAPI *PlatformDispatchFn)(ClPlatform, void *);
typedef BOOL (WINAPI *RegisterAdapterFn)(HMODULE, const LUID *, const LUID *);

/* The ICD ABI appends entries; the OpenCL 1.0 dispatch prefix is fixed. */
typedef struct {
    PlatformsFn get_platforms;
    PlatformInfoFn get_platform_info;
    DevicesFn get_devices;
    DeviceInfoFn get_device_info;
} ClDispatch;
C_ASSERT(offsetof(ClDispatch, get_device_info) == 3 * sizeof(void *));

typedef struct {
    UINT domain, bus, device, function;
} ClPciInfo;

typedef struct ClAdapter {
    struct ClAdapter *next;
    ClDevice device;
    ClDispatch *dispatch;
    DeviceInfoFn get_device_info;
    ClPciInfo pci;
    LUID host_luid;
    LUID guest_luid;
    BOOL mapped;
} ClAdapter;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_driver;
static PlatformsFn g_platforms;
static PlatformInfoFn g_platform_info;
static ExtensionFn g_extension;
static PlatformFunctionFn g_platform_function;
static PlatformDispatchFn g_platform_dispatch;
static ClAdapter *g_adapters;

int WINAPI clIcdGetPlatformIDsKHR(UINT count, ClPlatform *platforms, UINT *total);
void *WINAPI clGetExtensionFunctionAddress(const char *name);
void *WINAPI clIcdGetFunctionAddressForPlatformKHR(ClPlatform platform, const char *name);

static int WINAPI device_info_hook(ClDevice device, UINT name, size_t capacity,
                                   void *value, size_t *size)
{
    ClDispatch *dispatch;
    ClAdapter *entry;
    DeviceInfoFn function = NULL;
    int result;

    if (!device) return CL_INVALID_DEVICE;
    dispatch = *(ClDispatch **)device;
    for (entry = g_adapters; entry; entry = entry->next) {
        if (entry->device == device) {
            function = entry->get_device_info;
            break;
        }
        if (entry->dispatch == dispatch) function = entry->get_device_info;
    }
    if (!function) return CL_INVALID_VALUE;
    result = function(device, name, capacity, value, size);
    if (result == CL_SUCCESS && entry && entry->mapped && value &&
        name == CL_DEVICE_LUID_KHR && capacity >= sizeof(LUID))
        memcpy(value, &entry->guest_luid, sizeof(LUID));
    return result;
}

static BOOL enumerate_devices(void)
{
    ClPlatform *platforms = NULL;
    UINT count = 0, i;
    BOOL result = FALSE;

    if (g_platforms(0, NULL, &count) != CL_SUCCESS || !count) return FALSE;
    platforms = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*platforms));
    if (!platforms || g_platforms(count, platforms, NULL) != CL_SUCCESS) goto done;
    for (i = 0; i < count; ++i) {
        ClDispatch *dispatch = *(ClDispatch **)platforms[i];
        ClDevice *devices;
        DevicesFn get_devices = dispatch->get_devices;
        DeviceInfoFn get_info = dispatch->get_device_info;
        UINT device_count = 0, j;

        if (g_platform_function) {
            get_devices = (DevicesFn)g_platform_function(platforms[i], "clGetDeviceIDs");
            get_info = (DeviceInfoFn)g_platform_function(platforms[i], "clGetDeviceInfo");
        }
        if (!get_devices || !get_info ||
            get_devices(platforms[i], CL_DEVICE_TYPE_GPU, 0, NULL, &device_count) != CL_SUCCESS ||
            !device_count) continue;
        devices = HeapAlloc(GetProcessHeap(), 0, device_count * sizeof(*devices));
        if (!devices) goto done;
        if (get_devices(platforms[i], CL_DEVICE_TYPE_GPU, device_count, devices, NULL) != CL_SUCCESS) {
            HeapFree(GetProcessHeap(), 0, devices);
            goto done;
        }
        for (j = 0; j < device_count; ++j) {
            UINT vendor = 0, valid = 0, node_mask = 0;
            ClPciInfo pci;
            LUID luid;
            ClAdapter *entry;

            if (get_info(devices[j], CL_DEVICE_VENDOR_ID, sizeof(vendor), &vendor, NULL) || vendor != 0x10de ||
                get_info(devices[j], CL_DEVICE_LUID_VALID_KHR, sizeof(valid), &valid, NULL) || !valid ||
                get_info(devices[j], CL_DEVICE_NODE_MASK_KHR, sizeof(node_mask), &node_mask, NULL) || node_mask != 1 ||
                get_info(devices[j], CL_DEVICE_PCI_BUS_INFO_KHR, sizeof(pci), &pci, NULL) ||
                get_info(devices[j], CL_DEVICE_LUID_KHR, sizeof(luid), &luid, NULL) ||
                (!luid.LowPart && !luid.HighPart)) continue;
            entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
            if (!entry) {
                HeapFree(GetProcessHeap(), 0, devices);
                goto done;
            }
            entry->device = devices[j];
            entry->dispatch = *(ClDispatch **)devices[j];
            entry->get_device_info = get_info;
            entry->pci = pci;
            entry->host_luid = luid;
            entry->next = g_adapters;
            g_adapters = entry;
        }
        HeapFree(GetProcessHeap(), 0, devices);
    }
    result = TRUE;
done:
    if (platforms) HeapFree(GetProcessHeap(), 0, platforms);
    return result;
}

static void map_adapters(void)
{
    NvidiaAdapterApi api = {0};
    BOOL have_adapter_api = nvidia_adapter_api_load(&api);
    HMODULE nvapi = LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    IDXGIFactory1 *factory = NULL;
    RegisterAdapterFn register_adapter = nvapi ?
        (RegisterAdapterFn)GetProcAddress(nvapi, "appsandbox_nvidia_register_opencl_adapter") : NULL;
    ClAdapter *entry;
    UINT i;

    if (!have_adapter_api ||
        FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) goto done;
    for (i = 0; ;) {
        IDXGIAdapter1 *adapter = NULL;
        NvidiaAdapterIdentity identity;
        ClAdapter *match = NULL;
        UINT matches = 0;

        if (nvidia_enum_guest_adapter(&api, factory, &i, &adapter, &identity) != S_OK) break;
        IDXGIAdapter1_Release(adapter);
        for (entry = g_adapters; entry; entry = entry->next) {
            if (entry->pci.bus == identity.address.BusNumber &&
                entry->pci.device == identity.address.DeviceNumber &&
                entry->pci.function == identity.address.FunctionNumber) {
                match = entry;
                ++matches;
            }
        }
        if (matches != 1) continue;
        if (!match->mapped) {
            match->guest_luid = identity.luid;
            match->mapped = TRUE;
        }
        if (register_adapter) register_adapter(g_driver, &identity.luid, &match->host_luid);
    }
done:
    if (factory) IDXGIFactory1_Release(factory);
    if (api.module) FreeLibrary(api.module);
    if (nvapi) FreeLibrary(nvapi);
}

static void install_device_info_hooks(void)
{
    ClAdapter *entry;
    HMODULE pinned;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)device_info_hook, &pinned)) return;
    for (entry = g_adapters; entry; entry = entry->next) {
        void *volatile *slot = (void *volatile *)&entry->dispatch->get_device_info;
        DWORD protection, unused;
        if (!entry->mapped || *slot == (void *)device_info_hook) continue;
        if (VirtualProtect((void *)slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
            InterlockedCompareExchangePointer(slot, (void *)device_info_hook, (void *)entry->get_device_info);
            VirtualProtect((void *)slot, sizeof(*slot), protection, &unused);
        }
    }
}

static BOOL CALLBACK load_opencl(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE self;
    wchar_t path[MAX_PATH], *name;
    DWORD length;
    (void)once;
    (void)parameter;
    (void)context;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)load_opencl, &self)) return TRUE;
    length = GetModuleFileNameW(self, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return TRUE;
    name = wcsrchr(path, L'\\');
    if (!name) return TRUE;
    wcscpy_s(name + 1, ARRAYSIZE(path) - (size_t)(name + 1 - path), L"appsandbox-nvopencl64.dll");
    g_driver = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_driver || g_driver == self) return TRUE;
    g_extension = (ExtensionFn)GetProcAddress(g_driver, "clGetExtensionFunctionAddress");
    g_platform_info = (PlatformInfoFn)GetProcAddress(g_driver, "clGetPlatformInfo");
    if (!g_extension || !g_platform_info) return TRUE;
    g_platforms = (PlatformsFn)g_extension("clIcdGetPlatformIDsKHR");
    g_platform_function = (PlatformFunctionFn)g_extension("clIcdGetFunctionAddressForPlatformKHR");
    g_platform_dispatch = (PlatformDispatchFn)g_extension("clIcdSetPlatformDispatchDataKHR");
    if (g_platforms && enumerate_devices()) {
        map_adapters();
        install_device_info_hooks();
    }
    return TRUE;
}

int WINAPI clIcdGetPlatformIDsKHR(UINT count, ClPlatform *platforms, UINT *total)
{
    InitOnceExecuteOnce(&g_once, load_opencl, NULL, NULL);
    return g_platforms ? g_platforms(count, platforms, total) : CL_PLATFORM_NOT_FOUND_KHR;
}

int WINAPI clGetPlatformInfo(ClPlatform platform, UINT name, size_t capacity, void *value, size_t *size)
{
    InitOnceExecuteOnce(&g_once, load_opencl, NULL, NULL);
    return g_platform_info ? g_platform_info(platform, name, capacity, value, size) : CL_INVALID_VALUE;
}

void *WINAPI clIcdGetFunctionAddressForPlatformKHR(ClPlatform platform, const char *name)
{
    void *function;
    InitOnceExecuteOnce(&g_once, load_opencl, NULL, NULL);
    if (!name || !g_platform_function) return NULL;
    function = g_platform_function(platform, name);
    if (function && strcmp(name, "clGetDeviceInfo") == 0 && g_adapters)
        return (void *)device_info_hook;
    return function;
}

int WINAPI clIcdSetPlatformDispatchDataKHR(ClPlatform platform, void *data)
{
    InitOnceExecuteOnce(&g_once, load_opencl, NULL, NULL);
    return g_platform_dispatch ? g_platform_dispatch(platform, data) : CL_INVALID_VALUE;
}

void *WINAPI clGetExtensionFunctionAddress(const char *name)
{
    InitOnceExecuteOnce(&g_once, load_opencl, NULL, NULL);
    if (!name || !g_extension) return NULL;
    if (strcmp(name, "clIcdGetPlatformIDsKHR") == 0 && g_platforms)
        return (void *)clIcdGetPlatformIDsKHR;
    if (strcmp(name, "clIcdGetFunctionAddressForPlatformKHR") == 0 && g_platform_function)
        return (void *)clIcdGetFunctionAddressForPlatformKHR;
    if (strcmp(name, "clIcdSetPlatformDispatchDataKHR") == 0 && g_platform_dispatch)
        return (void *)clIcdSetPlatformDispatchDataKHR;
    return g_extension(name);
}

BOOL WINAPI appsandbox_opencl(void)
{
    return TRUE;
}
