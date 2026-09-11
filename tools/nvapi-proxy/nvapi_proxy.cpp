/*
 * nvapi_proxy.cpp — forwarding nvapi64.dll for NVIDIA GPU-PV guests.
 *
 * Why this exists
 * ---------------
 * In a paravirtualized guest there is no NVIDIA kernel-mode driver: the
 * adapter is Microsoft's Virtual Render Device wearing the host GPU's name.
 * NVIDIA's real NVAPI (nvapi64_impl.dll) works through the paravirtualized
 * escape channel for nearly everything — enumeration, arch info, DRS, the
 * D3D11/D3D12 cubin entry points DLSS is built on — but it cannot map a
 * physical GPU to its WDDM adapter LUID, so two calls answer
 * NVAPI_NOT_SUPPORTED (-104):
 *
 *   NvAPI_GPU_GetLogicalGpuInfo          (0x842B066E)  used by _nvngx.dll
 *   NvAPI_GPU_GetAdapterIdFromPhysicalGpu (0x0FF07FDE)  used by nvngx_dlss.dll
 *
 * NGX treats either failure as "unsupported hardware" and DLSS never shows
 * up in games. This DLL is installed by the agent as System32\nvapi64.dll
 * with NVIDIA's stub kept beside it as nvapi64_orig.dll. Every
 * nvapi_QueryInterface(id) is forwarded to the original; only when the
 * original answers -104 to one of the two calls above does the proxy fill in
 * the answer itself, using the LUID of the caller's D3D device (captured from
 * the NvAPI_D3D11/D3D12_IsFatbinPTXSupported call NGX makes just before).
 * Using the device's own LUID matters: a GPU-PV guest can expose several
 * VRD adapters with the same name and different LUIDs.
 *
 * Log: %SystemRoot%\AppSandbox\nvapi_proxy.log (init + emulated answers).
 * Set ASB_NVAPI_TRACE=1 in the game's environment to also log every
 * interface id requested.
 *
 * Built as a plain Win32 DLL with a static CRT; exports only
 * nvapi_QueryInterface (see exports.def).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdarg.h>

typedef int   NvAPI_Status;
typedef unsigned int NvU32;
typedef void *NvHandle;
#define NVAPI_OK             0
#define NVAPI_NOT_SUPPORTED  (-104)
#define NVAPI_MAX_PHYSICAL_GPUS 64

#pragma pack(push, 8)
struct NV_LOGICAL_GPU_DATA_V1 {
    NvU32    version;
    void    *pOSAdapterId;                              /* LUID* */
    NvU32    physicalGpuCount;
    NvHandle physicalGpuHandles[NVAPI_MAX_PHYSICAL_GPUS];
    NvU32    reserved[8];
};
#pragma pack(pop)

typedef void *(__cdecl *QI_t)(NvU32);
typedef NvAPI_Status (__cdecl *GetLogicalGpuInfo_t)(NvHandle, NV_LOGICAL_GPU_DATA_V1 *);
typedef NvAPI_Status (__cdecl *EnumPhysicalGPUs_t)(NvHandle *, NvU32 *);
typedef NvAPI_Status (__cdecl *IsFatbin_t)(IUnknown *, bool *);
typedef NvAPI_Status (__cdecl *GetAdapterId_t)(NvHandle, LUID *);

/* Interface ids: https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h */
static const NvU32 ID_EnumPhysicalGPUs            = 0xE5AC921F;
static const NvU32 ID_GetLogicalGpuInfo           = 0x842B066E;
static const NvU32 ID_GetAdapterIdFromPhysicalGpu = 0x0FF07FDE;
static const NvU32 ID_D3D11_IsFatbinPTXSupported  = 0x6086BD93;
static const NvU32 ID_D3D12_IsFatbinPTXSupported  = 0x70C07832;

static HMODULE g_orig;
static QI_t g_qi;
static GetLogicalGpuInfo_t g_realGetLogicalGpuInfo;
static GetAdapterId_t      g_realGetAdapterId;
static IsFatbin_t          g_realIsFatbin11, g_realIsFatbin12;
static CRITICAL_SECTION    g_cs;
static bool g_trace;
static LUID g_deviceLuid;
static bool g_haveDeviceLuid;

static void logf(const char *fmt, ...)
{
    wchar_t path[MAX_PATH];
    FILE *f = NULL;
    SYSTEMTIME st;
    va_list ap;

    if (!GetWindowsDirectoryW(path, MAX_PATH)) return;
    wcscat_s(path, MAX_PATH, L"\\AppSandbox\\nvapi_proxy.log");
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d pid=%lu] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, GetCurrentProcessId());
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static bool load_orig(void)
{
    if (g_qi) return true;
    EnterCriticalSection(&g_cs);
    if (!g_qi) {
        wchar_t path[MAX_PATH], trace[8];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, MAX_PATH, L"\\nvapi64_orig.dll");
        g_orig = LoadLibraryW(path);
        if (g_orig) g_qi = (QI_t)GetProcAddress(g_orig, "nvapi_QueryInterface");
        g_trace = GetEnvironmentVariableW(L"ASB_NVAPI_TRACE", trace, 8) > 0 && trace[0] == L'1';
        logf("init: nvapi64_orig.dll=%p nvapi_QueryInterface=%p", g_orig, g_qi);
    }
    LeaveCriticalSection(&g_cs);
    return g_qi != NULL;
}

/* LUID of the first hardware NVIDIA adapter DXGI enumerates — fallback when
   no device has been seen yet (a caller asking for the mapping before
   creating a device). */
static bool nvidia_adapter_luid(LUID *out)
{
    typedef HRESULT (WINAPI *CreateFactory1_t)(REFIID, void **);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    CreateFactory1_t create;
    IDXGIFactory1 *factory = NULL;
    bool found = false;

    if (!dxgi) return false;
    create = (CreateFactory1_t)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory)
        return false;
    for (UINT i = 0; !found; i++) {
        IDXGIAdapter1 *ad = NULL;
        DXGI_ADAPTER_DESC1 d;
        if (factory->EnumAdapters1(i, &ad) != S_OK || !ad) break;
        ad->GetDesc1(&d);
        ad->Release();
        if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            *out = d.AdapterLuid;
            found = true;
        }
    }
    factory->Release();
    return found;
}

/* LUID of the adapter behind a D3D11 or D3D12 device. */
static bool luid_from_device(IUnknown *dev, LUID *out)
{
    IDXGIDevice *dxgiDev = NULL;
    ID3D12Device *d12 = NULL;

    if (!dev) return false;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev)) && dxgiDev) {
        IDXGIAdapter *ad = NULL;
        bool ok = false;
        if (SUCCEEDED(dxgiDev->GetAdapter(&ad)) && ad) {
            DXGI_ADAPTER_DESC d;
            ad->GetDesc(&d);
            *out = d.AdapterLuid;
            ad->Release();
            ok = true;
        }
        dxgiDev->Release();
        return ok;
    }
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device), (void **)&d12)) && d12) {
        *out = d12->GetAdapterLuid();
        d12->Release();
        return true;
    }
    return false;
}

static bool current_luid(LUID *out)
{
    if (g_haveDeviceLuid) { *out = g_deviceLuid; return true; }
    return nvidia_adapter_luid(out);
}

static NvAPI_Status __cdecl my_IsFatbin(IsFatbin_t real, const char *tag, IUnknown *dev, bool *sup)
{
    LUID l = {};
    NvAPI_Status r;

    if (luid_from_device(dev, &l)) { g_deviceLuid = l; g_haveDeviceLuid = true; }
    r = real ? real(dev, sup) : NVAPI_NOT_SUPPORTED;
    if (g_trace)
        logf("%s(dev=%p) = %d supported=%d luid=%08X%08X", tag, dev, r,
             (sup && r == NVAPI_OK) ? (int)*sup : -1, l.HighPart, l.LowPart);
    return r;
}
static NvAPI_Status __cdecl my_IsFatbin11(IUnknown *dev, bool *sup)
{ return my_IsFatbin(g_realIsFatbin11, "NvAPI_D3D11_IsFatbinPTXSupported", dev, sup); }
static NvAPI_Status __cdecl my_IsFatbin12(IUnknown *dev, bool *sup)
{ return my_IsFatbin(g_realIsFatbin12, "NvAPI_D3D12_IsFatbinPTXSupported", dev, sup); }

static NvAPI_Status __cdecl my_GetAdapterIdFromPhysicalGpu(NvHandle hPhys, LUID *pLuid)
{
    NvAPI_Status r = g_realGetAdapterId ? g_realGetAdapterId(hPhys, pLuid) : NVAPI_NOT_SUPPORTED;
    LUID l;

    if (r == NVAPI_OK || !pLuid) return r;
    if (!current_luid(&l)) {
        logf("GetAdapterIdFromPhysicalGpu: real=%d and no adapter LUID available", r);
        return r;
    }
    *pLuid = l;
    logf("GetAdapterIdFromPhysicalGpu(gpu=%p): real=%d -> emulated luid=%08X%08X (%s)",
         hPhys, r, l.HighPart, l.LowPart, g_haveDeviceLuid ? "device" : "dxgi");
    return NVAPI_OK;
}

static NvAPI_Status __cdecl my_GetLogicalGpuInfo(NvHandle hLogical, NV_LOGICAL_GPU_DATA_V1 *p)
{
    NvAPI_Status r = g_realGetLogicalGpuInfo ? g_realGetLogicalGpuInfo(hLogical, p) : NVAPI_NOT_SUPPORTED;
    EnumPhysicalGPUs_t enumPhys;
    NvHandle hs[NVAPI_MAX_PHYSICAL_GPUS] = {};
    NvU32 n = 0, structSize;
    NvAPI_Status re;
    LUID l;

    if (r == NVAPI_OK || !p) return r;
    structSize = p->version & 0xFFFF;
    if (structSize < sizeof(NV_LOGICAL_GPU_DATA_V1)) {
        logf("GetLogicalGpuInfo: real=%d, struct v%u size %u too small — not emulated",
             r, p->version >> 16, structSize);
        return r;
    }
    enumPhys = (EnumPhysicalGPUs_t)g_qi(ID_EnumPhysicalGPUs);
    re = enumPhys ? enumPhys(hs, &n) : NVAPI_NOT_SUPPORTED;
    if (re != NVAPI_OK || n == 0) {
        logf("GetLogicalGpuInfo: real=%d, EnumPhysicalGPUs=%d count=%u — not emulated", r, re, n);
        return r;
    }
    if (!current_luid(&l)) {
        logf("GetLogicalGpuInfo: real=%d and no adapter LUID available", r);
        return r;
    }
    p->physicalGpuCount = 1;
    p->physicalGpuHandles[0] = hs[0];
    if (p->pOSAdapterId) *(LUID *)p->pOSAdapterId = l;
    logf("GetLogicalGpuInfo(logical=%p): real=%d -> emulated gpu=%p luid=%08X%08X (%s)",
         hLogical, r, hs[0], l.HighPart, l.LowPart, g_haveDeviceLuid ? "device" : "dxgi");
    return NVAPI_OK;
}

extern "C" __declspec(dllexport) void *__cdecl nvapi_QueryInterface(NvU32 id)
{
    void *real;

    if (!load_orig()) return NULL;
    real = g_qi(id);
    if (g_trace) logf("QueryInterface(0x%08X) -> %p", id, real);
    switch (id) {
    case ID_GetLogicalGpuInfo:
        g_realGetLogicalGpuInfo = (GetLogicalGpuInfo_t)real;
        return (void *)my_GetLogicalGpuInfo;
    case ID_GetAdapterIdFromPhysicalGpu:
        g_realGetAdapterId = (GetAdapterId_t)real;
        return (void *)my_GetAdapterIdFromPhysicalGpu;
    case ID_D3D11_IsFatbinPTXSupported:
        g_realIsFatbin11 = (IsFatbin_t)real;
        return (void *)my_IsFatbin11;
    case ID_D3D12_IsFatbinPTXSupported:
        g_realIsFatbin12 = (IsFatbin_t)real;
        return (void *)my_IsFatbin12;
    }
    return real;
}

/* Lets the agent recognise a deployed proxy (LoadLibraryEx + GetProcAddress)
   so it never mistakes it for NVIDIA's stub when refreshing nvapi64_orig.dll. */
extern "C" __declspec(dllexport) unsigned int __cdecl appsandbox_nvapi_proxy(void)
{
    return 0x41534201;  /* 'ASB' + interface version 1 */
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
    }
    return TRUE;
}
