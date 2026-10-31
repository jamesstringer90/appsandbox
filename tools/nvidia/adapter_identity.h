#ifndef ASB_NVIDIA_ADAPTER_IDENTITY_H
#define ASB_NVIDIA_ADAPTER_IDENTITY_H

#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <dxgi.h>
#include <winternl.h>
#include <d3dkmthk.h>
#pragma warning(pop)

typedef struct {
    HMODULE module;
    PFND3DKMT_ENUMADAPTERS2 enumerate;
    PFND3DKMT_QUERYADAPTERINFO query;
    PFND3DKMT_OPENADAPTERFROMLUID open;
    PFND3DKMT_CLOSEADAPTER close;
} NvidiaAdapterApi;

typedef struct {
    LUID luid;
    D3DKMT_DEVICE_IDS ids;
    D3DKMT_ADAPTERADDRESS address;
    BOOL has_address;
} NvidiaAdapterIdentity;

typedef struct {
    int (WINAPI *get_count)(int *);
    int (WINAPI *get_device)(int *, int);
    int (WINAPI *get_pci)(char *, int, int);
    int (WINAPI *get_luid)(char *, unsigned *, int);
} NvidiaCudaApi;

BOOL nvidia_adapter_api_load(NvidiaAdapterApi *api);
BOOL nvidia_query_adapter(const NvidiaAdapterApi *api, D3DKMT_HANDLE adapter,
                          KMTQUERYADAPTERINFOTYPE type, void *data, UINT size);
BOOL nvidia_query_adapter_identity(const NvidiaAdapterApi *api, D3DKMT_HANDLE adapter,
                                   LUID luid, NvidiaAdapterIdentity *identity);
HRESULT nvidia_enum_dxgi_adapter(IDXGIFactory1 *factory, UINT *index,
                                 IDXGIAdapter1 **adapter, DXGI_ADAPTER_DESC1 *desc);
HRESULT nvidia_enum_guest_adapter(const NvidiaAdapterApi *api, IDXGIFactory1 *factory,
                                  UINT *index, IDXGIAdapter1 **adapter,
                                  NvidiaAdapterIdentity *identity);
BOOL nvidia_cuda_api_load(HMODULE module, NvidiaCudaApi *api);
BOOL nvidia_cuda_device_address(const NvidiaCudaApi *api, int device,
                                D3DKMT_ADAPTERADDRESS *address);
BOOL nvidia_cuda_find_device(const NvidiaCudaApi *api, const D3DKMT_ADAPTERADDRESS *address,
                             int *device);
BOOL nvidia_cuda_host_luid(const NvidiaCudaApi *api, const D3DKMT_ADAPTERADDRESS *address,
                           LUID *luid);

#endif
