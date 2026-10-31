#define COBJMACROS
#include "adapter_identity.h"
#include <stdio.h>
#include <string.h>

BOOL nvidia_adapter_api_load(NvidiaAdapterApi *api)
{
    api->module = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api->module) return FALSE;
    api->enumerate = (PFND3DKMT_ENUMADAPTERS2)GetProcAddress(api->module, "D3DKMTEnumAdapters2");
    api->query = (PFND3DKMT_QUERYADAPTERINFO)GetProcAddress(api->module, "D3DKMTQueryAdapterInfo");
    api->open = (PFND3DKMT_OPENADAPTERFROMLUID)GetProcAddress(api->module, "D3DKMTOpenAdapterFromLuid");
    api->close = (PFND3DKMT_CLOSEADAPTER)GetProcAddress(api->module, "D3DKMTCloseAdapter");
    if (api->query && api->open && api->close) return TRUE;
    FreeLibrary(api->module);
    ZeroMemory(api, sizeof(*api));
    return FALSE;
}

BOOL nvidia_query_adapter(const NvidiaAdapterApi *api, D3DKMT_HANDLE adapter,
                          KMTQUERYADAPTERINFOTYPE type, void *data, UINT size)
{
    D3DKMT_QUERYADAPTERINFO query = {0};
    query.hAdapter = adapter;
    query.Type = type;
    query.pPrivateDriverData = data;
    query.PrivateDriverDataSize = size;
    return api->query(&query) >= 0;
}

BOOL nvidia_query_adapter_identity(const NvidiaAdapterApi *api, D3DKMT_HANDLE adapter,
                                   LUID luid, NvidiaAdapterIdentity *identity)
{
    D3DKMT_ADAPTERTYPE type = {0};
    D3DKMT_QUERY_DEVICE_IDS ids = {0};
    if (!nvidia_query_adapter(api, adapter, KMTQAITYPE_ADAPTERTYPE_RENDER, &type, sizeof(type)) ||
        !type.Paravirtualized ||
        !nvidia_query_adapter(api, adapter, KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids)) ||
        ids.DeviceIds.VendorID != 0x10de) return FALSE;
    identity->luid = luid;
    identity->ids = ids.DeviceIds;
    identity->has_address = nvidia_query_adapter(api, adapter, KMTQAITYPE_ADAPTERADDRESS_RENDER,
                                                &identity->address, sizeof(identity->address));
    return TRUE;
}

HRESULT nvidia_enum_dxgi_adapter(IDXGIFactory1 *factory, UINT *index,
                                 IDXGIAdapter1 **adapter, DXGI_ADAPTER_DESC1 *desc)
{
    for (;;) {
        HRESULT result = IDXGIFactory1_EnumAdapters1(factory, (*index)++, adapter);
        if (result != S_OK) return result;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(*adapter, desc)) && desc->VendorId == 0x10de &&
            !(desc->Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return S_OK;
        IDXGIAdapter1_Release(*adapter);
        *adapter = NULL;
    }
}

HRESULT nvidia_enum_guest_adapter(const NvidiaAdapterApi *api, IDXGIFactory1 *factory,
                                  UINT *index, IDXGIAdapter1 **adapter,
                                  NvidiaAdapterIdentity *identity)
{
    for (;;) {
        DXGI_ADAPTER_DESC1 desc;
        D3DKMT_OPENADAPTERFROMLUID open = {0};
        BOOL found = FALSE;
        HRESULT result = nvidia_enum_dxgi_adapter(factory, index, adapter, &desc);
        if (result != S_OK) return result;
        open.AdapterLuid = desc.AdapterLuid;
        if (api->open(&open) >= 0) {
            D3DKMT_CLOSEADAPTER close = {open.hAdapter};
            found = nvidia_query_adapter_identity(api, open.hAdapter, desc.AdapterLuid, identity) &&
                identity->ids.DeviceID == desc.DeviceId && identity->has_address;
            api->close(&close);
        }
        if (found) return S_OK;
        IDXGIAdapter1_Release(*adapter);
        *adapter = NULL;
    }
}

BOOL nvidia_cuda_api_load(HMODULE module, NvidiaCudaApi *api)
{
    ZeroMemory(api, sizeof(*api));
    if (!module) return FALSE;
    if (GetProcAddress(module, "appsandbox_cuda")) {
        module = GetModuleHandleW(L"appsandbox-nvcuda.dll");
        if (!module) return FALSE;
    }
    api->get_count = (int (WINAPI *)(int *))GetProcAddress(module, "cuDeviceGetCount");
    api->get_device = (int (WINAPI *)(int *, int))GetProcAddress(module, "cuDeviceGet");
    api->get_pci = (int (WINAPI *)(char *, int, int))GetProcAddress(module, "cuDeviceGetPCIBusId");
    api->get_luid = (int (WINAPI *)(char *, unsigned *, int))GetProcAddress(module, "cuDeviceGetLuid");
    return api->get_count && api->get_device && api->get_pci && api->get_luid;
}

BOOL nvidia_cuda_device_address(const NvidiaCudaApi *api, int device,
                                D3DKMT_ADAPTERADDRESS *address)
{
    char pci[64] = {0}, trailing;
    unsigned domain;
    if (!api->get_pci || api->get_pci(pci, sizeof(pci), device) != 0) return FALSE;
    pci[sizeof(pci) - 1] = '\0';
    return sscanf_s(pci, "%x:%x:%x.%x%c", &domain, &address->BusNumber,
                    &address->DeviceNumber, &address->FunctionNumber, &trailing, 1u) == 4;
}

BOOL nvidia_cuda_find_device(const NvidiaCudaApi *api, const D3DKMT_ADAPTERADDRESS *address,
                             int *device)
{
    int count = 0, matches = 0;
    if (!api->get_count || !api->get_device || api->get_count(&count) != 0) return FALSE;
    /* KMT omits PCI domains, so bus/device/function must identify a single device. */
    for (int i = 0; i < count; ++i) {
        int candidate;
        D3DKMT_ADAPTERADDRESS current;
        if (api->get_device(&candidate, i) != 0 ||
            !nvidia_cuda_device_address(api, candidate, &current)) return FALSE;
        if (memcmp(&current, address, sizeof(current)) != 0) continue;
        if (++matches != 1) return FALSE;
        *device = candidate;
    }
    return matches == 1;
}

BOOL nvidia_cuda_host_luid(const NvidiaCudaApi *api, const D3DKMT_ADAPTERADDRESS *address,
                           LUID *luid)
{
    int device;
    unsigned node_mask = 0;
    LUID selected = {0};
    if (!api->get_luid || !nvidia_cuda_find_device(api, address, &device) ||
        api->get_luid((char *)&selected, &node_mask, device) != 0 || node_mask != 1 ||
        (!selected.LowPart && !selected.HighPart)) return FALSE;
    *luid = selected;
    return TRUE;
}
