#define COBJMACROS
#include <windows.h>
#include "cuda_opencl_adapter_hooks.h"

#if defined(_M_X64)
#include <intrin.h>
#include "adapter_identity.h"

typedef HRESULT (STDMETHODCALLTYPE *AdapterGetDescFn)(IDXGIAdapter *, DXGI_ADAPTER_DESC *);

typedef struct ComputeAdapter {
    struct ComputeAdapter *next;
    IDXGIAdapter1 *adapter;
    LUID guest_luid;
    D3DKMT_ADAPTERADDRESS address;
    LUID cuda_luid;
    BOOL have_cuda_luid;
    HMODULE opencl;
    LUID opencl_luid;
} ComputeAdapter;

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK g_luid_lock = SRWLOCK_INIT;
static ComputeAdapter *g_adapters;
static AdapterGetDescFn g_adapter_get_desc;
static __declspec(thread) BOOL g_resolving;

static BOOL cached_cuda_luid(HMODULE cuda, ComputeAdapter *adapter, LUID *luid)
{
    BOOL found;
    LUID resolved;
    NvidiaCudaApi api;

    AcquireSRWLockShared(&g_luid_lock);
    found = adapter->have_cuda_luid;
    if (found) *luid = adapter->cuda_luid;
    ReleaseSRWLockShared(&g_luid_lock);
    if (found) return TRUE;
    /* CUDA is already calling GetDesc. Reentering cuInit here can deadlock. */
    if (!nvidia_cuda_api_load(cuda, &api) ||
        !nvidia_cuda_host_luid(&api, &adapter->address, &resolved)) return FALSE;
    AcquireSRWLockExclusive(&g_luid_lock);
    if (!adapter->have_cuda_luid) {
        adapter->cuda_luid = resolved;
        adapter->have_cuda_luid = TRUE;
    }
    *luid = adapter->cuda_luid;
    ReleaseSRWLockExclusive(&g_luid_lock);
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE compute_adapter_get_desc(IDXGIAdapter *adapter, DXGI_ADAPTER_DESC *desc)
{
    HRESULT result = g_adapter_get_desc(adapter, desc);
    MEMORY_BASIC_INFORMATION caller;
    HMODULE cuda, implementation, loader;
    ComputeAdapter *entry;
    LUID luid;

    if (FAILED(result) || !desc || desc->VendorId != 0x10de || g_resolving)
        return result;
    cuda = GetModuleHandleW(L"nvcuda.dll");
    implementation = GetModuleHandleW(L"nvcuda64.dll");
    loader = GetModuleHandleW(L"appsandbox-nvcuda.dll");
    if (!VirtualQuery(_ReturnAddress(), &caller, sizeof(caller))) return result;

    /* Only CUDA/OpenCL driver callers need host LUIDs; applications need guest LUIDs. */
    for (entry = g_adapters; entry; entry = entry->next) {
        if (entry->guest_luid.LowPart == desc->AdapterLuid.LowPart &&
            entry->guest_luid.HighPart == desc->AdapterLuid.HighPart)
            break;
    }
    if (!entry) return result;
    AcquireSRWLockShared(&g_luid_lock);
    if (entry->opencl && caller.AllocationBase == entry->opencl) {
        desc->AdapterLuid = entry->opencl_luid;
        ReleaseSRWLockShared(&g_luid_lock);
        return result;
    }
    ReleaseSRWLockShared(&g_luid_lock);
    if (caller.AllocationBase != cuda && caller.AllocationBase != implementation &&
        caller.AllocationBase != loader) return result;
    g_resolving = TRUE;
    if (cached_cuda_luid(loader ? loader : cuda ? cuda : implementation, entry, &luid))
        desc->AdapterLuid = luid;
    g_resolving = FALSE;
    return result;
}

static void free_adapters(ComputeAdapter *adapters)
{
    while (adapters) {
        ComputeAdapter *next = adapters->next;
        if (adapters->adapter) IDXGIAdapter1_Release(adapters->adapter);
        HeapFree(GetProcessHeap(), 0, adapters);
        adapters = next;
    }
}

static BOOL CALLBACK install_compute_hook(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HMODULE pinned;
    NvidiaAdapterApi api = {0};
    IDXGIFactory1 *factory = NULL;
    ComputeAdapter *adapters = NULL, **tail = &adapters, *entry;
    BOOL installed = FALSE;
    UINT i;
    (void)once;
    (void)parameter;
    (void)context;

    if (!nvidia_adapter_api_load(&api)) return FALSE;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) goto failed;
    for (i = 0; ;) {
        IDXGIAdapter1 *adapter = NULL;
        NvidiaAdapterIdentity identity;
        HRESULT result = nvidia_enum_guest_adapter(&api, factory, &i, &adapter, &identity);

        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (result != S_OK) goto failed;
        entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*entry));
        if (!entry) {
            IDXGIAdapter1_Release(adapter);
            goto failed;
        }
        entry->adapter = adapter;
        entry->guest_luid = identity.luid;
        entry->address = identity.address;
        *tail = entry;
        tail = &entry->next;
    }
    IDXGIFactory1_Release(factory);
    factory = NULL;
    if (!adapters) {
        FreeLibrary(api.module);
        return TRUE;
    }

    g_adapters = adapters;
    for (entry = adapters; entry; entry = entry->next) {
        void *volatile *slot;
        void *original;
        DWORD protection, unused;

        slot = (void *volatile *)&entry->adapter->lpVtbl->GetDesc;
        original = *slot;
        if (original && original != (void *)compute_adapter_get_desc &&
            (!g_adapter_get_desc || original == (void *)g_adapter_get_desc) &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              (LPCWSTR)compute_adapter_get_desc, &pinned) &&
            VirtualProtect((void *)slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
            g_adapter_get_desc = (AdapterGetDescFn)original;
            if (InterlockedCompareExchangePointer(slot, (void *)compute_adapter_get_desc, original) == original)
                installed = TRUE;
            else if (!installed)
                g_adapter_get_desc = NULL;
            VirtualProtect((void *)slot, sizeof(*slot), protection, &unused);
        }
        IDXGIAdapter1_Release(entry->adapter);
        entry->adapter = NULL;
    }
    if (installed) return TRUE;
    g_adapters = NULL;
    g_adapter_get_desc = NULL;
failed:
    if (factory) IDXGIFactory1_Release(factory);
    free_adapters(adapters);
    FreeLibrary(api.module);
    return FALSE;
}

void nvidia_install_cuda_adapter_hook(void)
{
    if (GetModuleHandleW(L"nvcuda.dll") || GetModuleHandleW(L"nvcuda64.dll") ||
        GetModuleHandleW(L"appsandbox-nvcuda.dll"))
        InitOnceExecuteOnce(&g_once, install_compute_hook, NULL, NULL);
}

BOOL WINAPI appsandbox_nvidia_register_opencl_adapter(HMODULE module, const LUID *guest, const LUID *host)
{
    ComputeAdapter *entry;
    HMODULE pinned;
    BOOL result = FALSE;

    if (!module || !guest || !host || (!host->LowPart && !host->HighPart) ||
        !InitOnceExecuteOnce(&g_once, install_compute_hook, NULL, NULL)) return FALSE;
    for (entry = g_adapters; entry; entry = entry->next) {
        if (entry->guest_luid.LowPart != guest->LowPart ||
            entry->guest_luid.HighPart != guest->HighPart) continue;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              (LPCWSTR)module, &pinned)) return FALSE;
        AcquireSRWLockExclusive(&g_luid_lock);
        if (!entry->opencl || (entry->opencl == module &&
            entry->opencl_luid.LowPart == host->LowPart && entry->opencl_luid.HighPart == host->HighPart)) {
            entry->opencl = module;
            entry->opencl_luid = *host;
            result = TRUE;
        }
        ReleaseSRWLockExclusive(&g_luid_lock);
        break;
    }
    return result;
}

#else

void nvidia_install_cuda_adapter_hook(void)
{
}

BOOL WINAPI appsandbox_nvidia_register_opencl_adapter(HMODULE module, const LUID *guest, const LUID *host)
{
    (void)module;
    (void)guest;
    (void)host;
    return FALSE;
}

#endif
