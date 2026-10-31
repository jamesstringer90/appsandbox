#define COBJMACROS
#include <windows.h>
#include <string.h>
#include "cuda_opencl_adapter_hooks.h"

typedef void *(__cdecl *QueryInterfaceFn)(UINT id);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static QueryInterfaceFn g_query;

#if defined(_M_X64)
#include "adapter_identity.h"
#pragma warning(push)
#pragma warning(disable: 4201)
#include <d3d12.h>
#pragma warning(pop)

#define NVAPI_GET_PHYSICAL_GPU_FROM_GPUID 0x5380ad1a
#define NVAPI_OK 0
#define NVAPI_NOT_SUPPORTED (-104)
#define NVAPI_MAX_PHYSICAL_GPUS 64
#define NVAPI_LOGICAL_GPU_INFO 0x842b066e
#define NVAPI_PHYSICAL_GPU_ADAPTER 0x0ff07fde
#define NVAPI_D3D11_FATBIN 0x6086bd93
#define NVAPI_D3D12_FATBIN 0x70c07832

typedef struct {
    UINT version;
    void *adapter_id;
    UINT physical_count;
    void *physical_gpus[NVAPI_MAX_PHYSICAL_GPUS];
    UINT reserved[8];
} LogicalGpuInfo;

typedef int (__cdecl *EnumPhysicalFn)(void **gpus, UINT *count);
typedef int (__cdecl *LogicalInfoFn)(void *gpu, LogicalGpuInfo *info);
typedef int (__cdecl *PhysicalAdapterFn)(void *gpu, void *luid);
typedef int (__cdecl *FatbinFn)(IUnknown *device, BYTE *supported);

static LogicalInfoFn g_logical_info;
static PhysicalAdapterFn g_physical_adapter;
static FatbinFn g_d3d11_fatbin;
static FatbinFn g_d3d12_fatbin;
static SRWLOCK g_identity_lock = SRWLOCK_INIT;
static LUID g_device_luid;
static BOOL g_have_device_luid;

static BOOL find_nvidia_adapter(const LUID *wanted, LUID *luid)
{
    IDXGIFactory1 *factory = NULL;
    BOOL found = FALSE;
    UINT i;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return FALSE;
    for (i = 0; !found;) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;

        if (nvidia_enum_dxgi_adapter(factory, &i, &adapter, &desc) != S_OK) break;
        IDXGIAdapter1_Release(adapter);
        if (wanted && (wanted->LowPart != desc.AdapterLuid.LowPart ||
                       wanted->HighPart != desc.AdapterLuid.HighPart)) continue;
        if (luid) *luid = desc.AdapterLuid;
        found = TRUE;
    }
    IDXGIFactory1_Release(factory);
    return found;
}

static void capture_device_identity(IUnknown *device)
{
    IDXGIDevice *dxgi = NULL;
    ID3D12Device *d3d12 = NULL;
    LUID luid = {0};
    BOOL found = FALSE;

    if (!device) return;
    if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi))) {
        IDXGIAdapter *adapter = NULL;
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
                luid = desc.AdapterLuid;
                found = TRUE;
            }
            IDXGIAdapter_Release(adapter);
        }
        IDXGIDevice_Release(dxgi);
    } else if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_ID3D12Device, (void **)&d3d12))) {
        ID3D12Device_GetAdapterLuid(d3d12, &luid);
        ID3D12Device_Release(d3d12);
        found = TRUE;
    }
    if (!found || !find_nvidia_adapter(&luid, NULL)) return;
    AcquireSRWLockExclusive(&g_identity_lock);
    g_device_luid = luid;
    g_have_device_luid = TRUE;
    ReleaseSRWLockExclusive(&g_identity_lock);
}

static BOOL current_device_luid(LUID *luid)
{
    typedef BOOL (WINAPI *AdapterLuidFn)(LUID *);
    wchar_t path[MAX_PATH];
    HMODULE module;
    UINT length;
    BOOL found;

    AcquireSRWLockShared(&g_identity_lock);
    found = g_have_device_luid;
    if (found) *luid = g_device_luid;
    ReleaseSRWLockShared(&g_identity_lock);
    if (found) return TRUE;

    length = GetSystemDirectoryW(path, ARRAYSIZE(path));
    if (length && length + ARRAYSIZE(L"\\opengl32.dll") <= ARRAYSIZE(path)) {
        wcscat_s(path, ARRAYSIZE(path), L"\\opengl32.dll");
        if (GetModuleHandleExW(0, path, &module)) {
            AdapterLuidFn get_luid = (AdapterLuidFn)GetProcAddress(module, "appsandbox_nvidia_adapter_luid");
            found = get_luid && get_luid(luid) && find_nvidia_adapter(luid, NULL);
            FreeLibrary(module);
            if (found) return TRUE;
        }
    }
    return find_nvidia_adapter(NULL, luid);
}

static int __cdecl logical_gpu_info_hook(void *gpu, LogicalGpuInfo *info)
{
    int result = g_logical_info ? g_logical_info(gpu, info) : NVAPI_NOT_SUPPORTED;
    EnumPhysicalFn enumerate;
    void *physical[NVAPI_MAX_PHYSICAL_GPUS] = {0};
    UINT count = 0;
    LUID luid;

    if (result == NVAPI_OK || !info ||
        (info->version & 0xffff) < sizeof(*info)) return result;
    enumerate = (EnumPhysicalFn)g_query(0xe5ac921f);
    if (!enumerate || enumerate(physical, &count) != NVAPI_OK || !count ||
        !current_device_luid(&luid)) return result;
    info->physical_count = 1;
    info->physical_gpus[0] = physical[0];
    if (info->adapter_id) memcpy(info->adapter_id, &luid, sizeof(luid));
    return NVAPI_OK;
}

static int __cdecl physical_gpu_adapter_hook(void *gpu, void *adapter_id)
{
    int result = g_physical_adapter ? g_physical_adapter(gpu, adapter_id) : NVAPI_NOT_SUPPORTED;
    LUID luid;

    if (result == NVAPI_OK || !adapter_id || !current_device_luid(&luid)) return result;
    memcpy(adapter_id, &luid, sizeof(luid));
    return NVAPI_OK;
}

static int __cdecl d3d11_fatbin_hook(IUnknown *device, BYTE *supported)
{
    capture_device_identity(device);
    return g_d3d11_fatbin ? g_d3d11_fatbin(device, supported) : NVAPI_NOT_SUPPORTED;
}

static int __cdecl d3d12_fatbin_hook(IUnknown *device, BYTE *supported)
{
    capture_device_identity(device);
    return g_d3d12_fatbin ? g_d3d12_fatbin(device, supported) : NVAPI_NOT_SUPPORTED;
}
#endif

static BOOL CALLBACK load_nvapi(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE module;
    (void)once;
    (void)parameter;
    (void)context;

    module = LoadLibraryExW(L"appsandbox-nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return TRUE;
    if (GetProcAddress(module, "appsandbox_nvapi")) {
        FreeLibrary(module);
        return TRUE;
    }
    g_query = (QueryInterfaceFn)GetProcAddress(module, "nvapi_QueryInterface");
    return TRUE;
}

void *__cdecl nvapi_QueryInterface(UINT id)
{
    void *original;

    InitOnceExecuteOnce(&g_once, load_nvapi, NULL, NULL);
    if (!g_query) return NULL;
#if defined(_M_X64)
    /* CUDA resolves this during initialization, before its DXGI comparisons. */
    if (id == NVAPI_GET_PHYSICAL_GPU_FROM_GPUID) nvidia_install_cuda_adapter_hook();
#endif
    original = g_query(id);
#if defined(_M_X64)
    switch (id) {
    case NVAPI_LOGICAL_GPU_INFO:
        g_logical_info = (LogicalInfoFn)original;
        return (void *)logical_gpu_info_hook;
    case NVAPI_PHYSICAL_GPU_ADAPTER:
        g_physical_adapter = (PhysicalAdapterFn)original;
        return (void *)physical_gpu_adapter_hook;
    case NVAPI_D3D11_FATBIN:
        g_d3d11_fatbin = (FatbinFn)original;
        return (void *)d3d11_fatbin_hook;
    case NVAPI_D3D12_FATBIN:
        g_d3d12_fatbin = (FatbinFn)original;
        return (void *)d3d12_fatbin_hook;
    }
#endif
    return original;
}

DWORD __cdecl appsandbox_nvapi(void)
{
    return 0x41534201;
}
