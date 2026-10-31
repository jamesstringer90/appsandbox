#define COBJMACROS
#include "adapter_identity.h"
#include <string.h>

#define CUDA_ERROR_NOT_INITIALIZED 3
#define CUDA_ERROR_NOT_SUPPORTED 801

typedef int (WINAPI *GetProcFn)(const char *, void **, int, unsigned __int64);
typedef int (WINAPI *GetProcV2Fn)(const char *, void **, int, unsigned __int64, int *);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static NvidiaCudaApi g_cuda;
static NvidiaAdapterApi g_adapter;
static GetProcFn g_get_proc;
static GetProcV2Fn g_get_proc_v2;

int WINAPI cuDeviceGetLuid(char *luid, unsigned *node_mask, int device);
int WINAPI cuGetProcAddress(const char *symbol, void **function, int version,
                           unsigned __int64 flags);
int WINAPI cuGetProcAddress_v2(const char *symbol, void **function, int version,
                              unsigned __int64 flags, int *status);

static BOOL CALLBACK load_cuda(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE cuda;
    (void)once;
    (void)parameter;
    (void)context;

    cuda = LoadLibraryExW(L"appsandbox-nvcuda.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return TRUE;
    if (GetProcAddress(cuda, "appsandbox_cuda")) {
        FreeLibrary(cuda);
        return TRUE;
    }
    nvidia_cuda_api_load(cuda, &g_cuda);
    g_get_proc = (GetProcFn)GetProcAddress(cuda, "cuGetProcAddress");
    g_get_proc_v2 = (GetProcV2Fn)GetProcAddress(cuda, "cuGetProcAddress_v2");
    nvidia_adapter_api_load(&g_adapter);
    return TRUE;
}

static BOOL guest_device_luid(int device, LUID *luid)
{
    D3DKMT_ADAPTERADDRESS wanted;
    IDXGIFactory1 *factory = NULL;
    UINT index = 0, guest_matches = 0;
    LUID selected = {0};
    D3DKMT_DEVICE_IDS selected_ids = {0};
    BOOL complete = FALSE, ambiguous = FALSE;

    if (!g_adapter.module || !nvidia_cuda_device_address(&g_cuda, device, &wanted) ||
        !nvidia_cuda_find_device(&g_cuda, &wanted, &device) ||
        FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)))
        return FALSE;
    for (;;) {
        IDXGIAdapter1 *adapter = NULL;
        NvidiaAdapterIdentity identity;
        HRESULT result = nvidia_enum_guest_adapter(&g_adapter, factory, &index, &adapter, &identity);

        if (result == DXGI_ERROR_NOT_FOUND) {
            complete = TRUE;
            break;
        }
        if (result != S_OK) break;
        IDXGIAdapter1_Release(adapter);
        if (memcmp(&identity.address, &wanted, sizeof(wanted)) == 0) {
            if (!guest_matches) {
                selected = identity.luid;
                selected_ids = identity.ids;
            } else if (memcmp(&selected_ids, &identity.ids, sizeof(selected_ids)) != 0) {
                ambiguous = TRUE;
            }
            ++guest_matches;
        }
    }
    IDXGIFactory1_Release(factory);
    if (!complete || !guest_matches || ambiguous || (!selected.LowPart && !selected.HighPart))
        return FALSE;
    *luid = selected;
    return TRUE;
}

int WINAPI cuDeviceGetLuid(char *luid, unsigned *node_mask, int device)
{
    LUID guest;
    int result;
    InitOnceExecuteOnce(&g_once, load_cuda, NULL, NULL);
    if (!g_cuda.get_luid) return CUDA_ERROR_NOT_INITIALIZED;
    result = g_cuda.get_luid(luid, node_mask, device);
    if (result == 0 && luid && node_mask && *node_mask == 1 && guest_device_luid(device, &guest))
        memcpy(luid, &guest, sizeof(guest));
    return result;
}

static void wrap_function(const char *symbol, void **function, int version)
{
    if (!symbol || !function || !*function) return;
    if (strcmp(symbol, "cuDeviceGetLuid") == 0)
        *function = (void *)cuDeviceGetLuid;
    else if (strcmp(symbol, "cuGetProcAddress") == 0)
        *function = version >= 12000 ? (void *)cuGetProcAddress_v2 : (void *)cuGetProcAddress;
    else if (strcmp(symbol, "cuGetProcAddress_v2") == 0)
        *function = (void *)cuGetProcAddress_v2;
}

int WINAPI cuGetProcAddress(const char *symbol, void **function, int version,
                           unsigned __int64 flags)
{
    int result;
    InitOnceExecuteOnce(&g_once, load_cuda, NULL, NULL);
    if (!g_get_proc) return CUDA_ERROR_NOT_SUPPORTED;
    result = g_get_proc(symbol, function, version, flags);
    if (result == 0) wrap_function(symbol, function, version);
    return result;
}

int WINAPI cuGetProcAddress_v2(const char *symbol, void **function, int version,
                              unsigned __int64 flags, int *status)
{
    int result;
    InitOnceExecuteOnce(&g_once, load_cuda, NULL, NULL);
    if (!g_get_proc_v2) return CUDA_ERROR_NOT_SUPPORTED;
    result = g_get_proc_v2(symbol, function, version, flags, status);
    if (result == 0) wrap_function(symbol, function, version);
    return result;
}

DWORD __cdecl appsandbox_cuda(void)
{
    return 0x41534201;
}
