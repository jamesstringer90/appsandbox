/*++

Module Name:

    vdd.cpp

Abstract:

    AppSandbox Virtual Display Driver - Single 1920x1080 monitor with
    direct HvSocket frame transport to the host (no agent middleman).

    IddCx lifecycle:
    - DllMain
    - DriverEntry with WDF_OBJECT_ATTRIBUTES_INIT + EvtDriverUnload
    - DeviceAdd with PnP callbacks, IddCx config, EvtCleanupCallback,
      heap-allocated context (IndirectDeviceContextWrapper)
    - D0Entry -> InitAdapter (context type on adapter object)
    - AdapterInitFinished -> FinishInit -> CreateMonitor
    - ParseMonitorDescription, GetDefaultModes, QueryTargetModes
    - AssignSwapChain with direct HvSocket frame transport to host
    - UnassignSwapChain with cleanup

Environment:

    Windows User-Mode Driver Framework 2

--*/

#include "vdd.h"

#include <stdlib.h>  /* malloc, free */
#include <stdarg.h>  /* va_list, va_start, va_end */
#include <stdio.h>   /* _snprintf, _vsnprintf */
#include <cfgmgr32.h> /* CM_Locate_DevNode, CM_Disable_DevNode */
#include <d3dkmthk.h>  /* D3DKMTSetProcessSchedulingPriorityClass */
#include <setupapi.h>  /* SetupDiGetClassDevs, SetupDiEnumDeviceInfo */
#include "../transport/asb_transport.h"  /* ivshmem frame channel (Windows-VMs-on-Mac path only) */

/* ========================================================================= */
/*  File-based logging (persists across crashes, no debugger needed)         */
/* ========================================================================= */

#define VDD_LOG_PATH L"C:\\ProgramData\\AppSandboxVDD.log"

static void VddLog(const char* fmt, ...)
{
    HANDLE hFile = CreateFileW(VDD_LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;

    /* Timestamp */
    SYSTEMTIME sysTime;
    GetLocalTime(&sysTime);
    char buf[2048];
    int pos = sprintf_s(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                        sysTime.wYear, sysTime.wMonth, sysTime.wDay,
                        sysTime.wHour, sysTime.wMinute, sysTime.wSecond, sysTime.wMilliseconds);
    if (pos < 0) pos = 0;

    /* Format message */
    va_list args;
    va_start(args, fmt);
    int added = vsprintf_s(buf + pos, sizeof(buf) - (size_t)pos - 4, fmt, args);
    va_end(args);
    if (added > 0) pos += added;

    buf[pos++] = '\r';
    buf[pos++] = '\n';

    DWORD written;
    WriteFile(hFile, buf, (DWORD)pos, &written, NULL);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    /* Also send to debug output for good measure */
    buf[pos - 2] = '\n';
    buf[pos - 1] = '\0';
    OutputDebugStringA(buf);
}

/* ========================================================================= */
/*  EDID with computed checksum                                              */
/* ========================================================================= */

static void VddChecksum(BYTE* data, UINT size)
{
    BYTE sum = 0;
    for (UINT i = 0; i + 1 < size; ++i) sum += data[i];
    data[size - 1] = (BYTE)(0 - sum);
}

static void VddWrite16(BYTE* data, UINT value)
{
    data[0] = (BYTE)value;
    data[1] = (BYTE)(value >> 8);
}

static UINT VddBuildEdid(BYTE* edid, const ResolutionEntry* modes, UINT count)
{
    UINT blocks = (count + 4) / 5;
    UINT width = modes[0].width, height = modes[0].height;
    UINT clock = (UINT)((UINT64)(width + 160) * (height + 30) * 60 / 10000);
    ZeroMemory(edid, VDD_EDID_BYTES);
    memcpy(edid, VDD_EDID, sizeof(VDD_EDID));
    BYTE* dtd = edid + 54;
    ZeroMemory(dtd, 18);
    if (width <= 4095 && height <= 4095 && clock <= 65535) {
        VddWrite16(dtd, clock);
        dtd[2] = (BYTE)width;
        dtd[3] = 160;
        dtd[4] = (BYTE)((width >> 8) << 4);
        dtd[5] = (BYTE)height;
        dtd[6] = 30;
        dtd[7] = (BYTE)((height >> 8) << 4);
        dtd[8] = 48;
        dtd[9] = 32;
        dtd[10] = 0x35;
        dtd[17] = 0x1e;
    } else {
        dtd[3] = 0x10;
        edid[24] &= ~2;
    }
    edid[126] = (BYTE)blocks;
    VddChecksum(edid, 128);
    for (UINT block = 0, index = 0; block < blocks; ++block) {
        BYTE* ext = edid + (block + 1) * 128;
        UINT entries = min(5u, count - index);
        ext[0] = 0x70;
        ext[1] = 0x13;
        ext[2] = (BYTE)(3 + entries * 20);
        ext[3] = block ? 0 : 3;
        ext[4] = block ? 0 : (BYTE)(blocks - 1);
        ext[5] = 3;
        ext[7] = (BYTE)(entries * 20);
        for (UINT n = 0; n < entries; ++n, ++index) {
            BYTE* timing = ext + 8 + n * 20;
            width = modes[index].width;
            height = modes[index].height;
            clock = (UINT)((UINT64)(width + 160) * (height + 30) * 60 / 10000) - 1;
            VddWrite16(timing, clock);
            timing[2] = (BYTE)(clock >> 16);
            timing[3] = index ? 0 : 0x80;
            VddWrite16(timing + 4, width - 1);
            VddWrite16(timing + 6, 159);
            VddWrite16(timing + 8, 47);
            VddWrite16(timing + 10, 31);
            VddWrite16(timing + 12, height - 1);
            VddWrite16(timing + 14, 29);
            VddWrite16(timing + 16, 2);
            VddWrite16(timing + 18, 4);
        }
        VddChecksum(ext + 1, 5 + ext[2]);
        VddChecksum(ext, 128);
    }
    return (blocks + 1) * 128;
}

static UINT VddReadModes(const IDDCX_MONITOR_DESCRIPTION* description, ResolutionEntry* modes)
{
    if (!description->pData || description->DataSize < 256) return 0;
    const BYTE* edid = (const BYTE*)description->pData;
    if ((UINT)(edid[126] + 1) * 128 > description->DataSize) return 0;
    UINT count = 0;
    for (UINT block = 0; block < edid[126]; ++block) {
        const BYTE* ext = edid + (block + 1) * 128;
        if (ext[0] != 0x70 || ext[5] != 3 || ext[7] > 100 || ext[7] % 20) return 0;
        for (UINT offset = 8; offset < 8u + ext[7]; offset += 20) {
            if (count == VDD_MAX_MODES) return 0;
            UINT width = 1 + ext[offset + 4] + ((UINT)ext[offset + 5] << 8);
            UINT height = 1 + ext[offset + 12] + ((UINT)ext[offset + 13] << 8);
            if (width > DISPLAY_MAX_WIDTH || height > DISPLAY_MAX_HEIGHT) return 0;
            modes[count++] = { width, height };
        }
    }
    return count;
}

/* ========================================================================= */
/*  Monitor mode helper                                                      */
/* ========================================================================= */

static void VddCreateMonitorMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, UINT vSyncDivider,
    UINT width, UINT height)
{
    /* totalSize == activeSize, pixelRate = refresh * w * h.
       IddCx validates these relationships; using actual blanking values causes
       STATUS_INVALID_PARAMETER from IddCxMonitorArrival. */
    sig->totalSize.cx                           = width;
    sig->totalSize.cy                           = height;
    sig->activeSize.cx                          = width;
    sig->activeSize.cy                          = height;
    sig->AdditionalSignalInfo.vSyncFreqDivider  = vSyncDivider;
    sig->AdditionalSignalInfo.videoStandard     = 255;
    sig->vSyncFreq.Numerator                    = 60;
    sig->vSyncFreq.Denominator                  = 1;
    sig->hSyncFreq.Numerator                    = 60 * height;
    sig->hSyncFreq.Denominator                  = 1;
    sig->scanLineOrdering                       = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    sig->pixelRate                               = (UINT64)60 * width * height;
}

static UINT VddTargetResolutions(IDDCX_MONITOR monitor, ResolutionEntry* modes)
{
    MonitorContextWrapper* context = WdfObjectGet_MonitorContextWrapper(monitor);
    BOOL full = InterlockedCompareExchange(&context->pContext->fullModes, 0, 0) != 0;
    UINT count = 0;
    for (UINT i = 0; i < context->modeCount; ++i) {
        ResolutionEntry mode = context->modes[i];
        if (full || (mode.width <= VDD_WIDTH && mode.height <= VDD_HEIGHT))
            modes[count++] = mode;
    }
    if (!count) modes[count++] = { VDD_WIDTH, VDD_HEIGHT };
    return count;
}

static void VddNotifyGpuMode(VDD_SWAP_PROC* proc, BOOL available)
{
    VDD_DEVICE_CONTEXT* ctx = proc->pOwner;
    if (!ctx || !ctx->hDisplayWork || proc->bStopNetwork ||
        InterlockedCompareExchange(&ctx->stopping, 0, 0)) return;
    if (!available && InterlockedCompareExchange(&ctx->reconfiguring, 0, 0)) return;
    if (available && !proc->gpuResizeSupported) return;
    if (available) InterlockedExchange(&ctx->reconfiguring, 0);
    AcquireSRWLockExclusive(&ctx->displayLock);
    if (available) ctx->gpuModeSession = proc->gpuSession;
    else if (ctx->gpuModeSession == proc->gpuSession) ctx->gpuModeSession = 0;
    ReleaseSRWLockExclusive(&ctx->displayLock);
    WdfWorkItemEnqueue(ctx->hDisplayWork);
}

/* ========================================================================= */
/*  Host frame transport                                                     */
/*                                                                           */
/*  The frame channel (listen/accept/send on the ch2 service a5b0cafe-0002)  */
/*  goes through asb_transport: asb_listen(ASB_CH_DISPLAY)/asb_accept/       */
/*  asb_send resolve to AF_HYPERV HvSocket on a Windows host and to the      */
/*  ivshmem ch2 stream slot on a macOS host.                                 */
/* ========================================================================= */

/* Send exactly len bytes over the transport. Returns 0 on success, -1 on failure
   (asb_send loops until all bytes are sent on both backends). */
static int VddSendAll(AsbConn* c, const char* data, int len)
{
    return asb_send(c, data, len) == len ? 0 : -1;
}

/* Close a client connection and clear *pc. PC: graceful shutdown(SD_BOTH) then closesocket; ivshmem:
   asb_close marks the slot CLOSING. */
static void VddCloseConn(AsbConn* volatile* pc)
{
    AsbConn* c = *pc;
    if (!c) return;
    {
        unsigned long long s = asb_conn_socket_u64(c);
        if (s != ~0ull) shutdown((SOCKET)s, SD_BOTH);   /* PC only; ivshmem returns ~0 */
    }
    asb_close(c);
    *pc = NULL;
}

/* ========================================================================= */
/*  Network listener thread (accepts host connections over HvSocket)         */
/* ========================================================================= */

/* One-shot transport backend selection (PC AF_HYPERV vs Mac ivshmem). Done off the loader lock
   (in the network thread, never in DllMain) since the ivshmem probe enumerates device interfaces. */
static volatile LONG g_transportInited = 0;
static void VddEnsureTransportInit(void)
{
    if (InterlockedCompareExchange(&g_transportInited, 1, 0) == 0)
        asb_transport_init();   /* selects ivshmem if the host published the directory, else AF_HYPERV */
}

static DWORD WINAPI VddNetworkThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    AsbListener* l = NULL;

    VddLog("Network: starting");
    VddEnsureTransportInit();
    if (asb_transport_is_ivshmem() && proc->pOwner->hDisplayWork &&
        !InterlockedCompareExchange(&proc->pOwner->stopping, 0, 0))
        WdfWorkItemEnqueue(proc->pOwner->hDisplayWork);

    while (!proc->bStopNetwork) {
        /* (Re)create the listener. PC: socket(AF_HYPERV)+bind(ch2 GUID, wildcard)+listen;
           Mac: bind the ivshmem ch2 slot. */
        l = asb_listen(ASB_CH_DISPLAY);
        if (!l) {
            VddLog("Network: asb_listen(ch2) failed, retrying in 3s (transport=%s)",
                   asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");
            for (int w = 0; w < 3000 && !proc->bStopNetwork; w += 500)
                Sleep(500);
            continue;
        }

        VddLog("Network: listening for host connections (transport=%s)",
               asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

        int acceptTimeout = asb_transport_is_ivshmem() ? 100 : 1000;
        while (!proc->bStopNetwork) {
            AsbConn* nc = asb_accept(l, acceptTimeout);
            if (!nc)
                continue;

            VddLog("Network: host connected");

            /* If a previous pending connection wasn't picked up yet, close it. */
            {
                AsbConn* old = (AsbConn*)InterlockedExchangePointer(
                    (PVOID volatile*)&proc->hPendingConn, nc);
                if (old) {
                    VddCloseConn(&old);
                    VddLog("Network: replaced unclaimed pending connection");
                }
            }
        } /* end accept loop */

        asb_close_listener(l);
        l = NULL;
    } /* end retry loop */

    /* Close any pending connection. */
    {
        AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
            (PVOID volatile*)&proc->hPendingConn, NULL);
        if (pending) VddCloseConn(&pending);
    }

    if (l) asb_close_listener(l);

    VddLog("Network: stopped");
    return 0;
}

/* ========================================================================= */
/*  D3D11 device — cached in VDD_DEVICE_CONTEXT across swap chain transitions
 *  to avoid destroying the D3D device during IddCx's WdfObjectDelete
 *  teardown (which crashes WUDFHost.exe).                                    */
/* ========================================================================= */

static void VddPreferSoftwareRenderAdapter(IDDCX_ADAPTER adapterObject)
{
#pragma warning(suppress: 4127)
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) return;
    IDXGIFactory4* factory = NULL;
    IDXGIAdapter1* adapter = NULL;
    DXGI_ADAPTER_DESC1 desc = {};
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (hr == S_OK) hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    if (hr == S_OK) hr = adapter->GetDesc1(&desc);
    if (hr == S_OK) {
        IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
        args.PreferredRenderAdapter = desc.AdapterLuid;
        IddCxAdapterSetRenderAdapter(adapterObject, &args);
    }
    if (adapter) adapter->Release();
    if (factory) factory->Release();
}

static HRESULT VddGetOrCreateD3DDevice(VDD_DEVICE_CONTEXT* ctx, LUID adapterLuid,
                                        ID3D11Device** ppDevice, ID3D11DeviceContext** ppCtx)
{
    /* Check if cached device matches the requested LUID and is still healthy */
    if (ctx->pCachedDevice &&
        ctx->cachedDeviceLuid.HighPart == adapterLuid.HighPart &&
        ctx->cachedDeviceLuid.LowPart == adapterLuid.LowPart)
    {
        HRESULT reason = ctx->pCachedDevice->GetDeviceRemovedReason();
        if (reason == S_OK)
        {
            VddLog("GetOrCreateD3DDevice: reusing cached device (LUID=%08X:%08X)",
                   adapterLuid.HighPart, adapterLuid.LowPart);
            *ppDevice = ctx->pCachedDevice;
            *ppCtx = ctx->pCachedCtx;
            return S_OK;
        }
        VddLog("GetOrCreateD3DDevice: cached device removed (reason=0x%08X), recreating",
               reason);
        ctx->pCachedCtx->Release();
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
        ctx->pCachedCtx = nullptr;
    }

    /* Release any stale cached device with a different LUID */
    if (ctx->pCachedDevice)
    {
        VddLog("GetOrCreateD3DDevice: releasing stale cached device (LUID mismatch)");
        ctx->pCachedCtx->Release();
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
        ctx->pCachedCtx = nullptr;
    }

    VddLog("GetOrCreateD3DDevice: creating new device (LUID=%08X:%08X)",
           adapterLuid.HighPart, adapterLuid.LowPart);

    IDXGIFactory5* pFactory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory5), (void**)&pFactory);
    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: CreateDXGIFactory2 FAILED hr=0x%08X", hr);
        return hr;
    }

    IDXGIAdapter1* pAdapter = nullptr;
    hr = pFactory->EnumAdapterByLuid(adapterLuid, __uuidof(IDXGIAdapter1), (void**)&pAdapter);
    pFactory->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: EnumAdapterByLuid FAILED hr=0x%08X", hr);
        return hr;
    }

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pDevCtx = nullptr;

    hr = D3D11CreateDevice(
        pAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &pDevice,
        nullptr,
        &pDevCtx);

    pAdapter->Release();

    if (FAILED(hr))
    {
        VddLog("GetOrCreateD3DDevice: D3D11CreateDevice FAILED hr=0x%08X", hr);
        return hr;
    }

    VddLog("GetOrCreateD3DDevice: D3D11 device created successfully");

    /* Cache in device context */
    ctx->pCachedDevice = pDevice;
    ctx->pCachedCtx = pDevCtx;
    ctx->cachedDeviceLuid = adapterLuid;

    *ppDevice = pDevice;
    *ppCtx = pDevCtx;
    return S_OK;
}

static HRESULT VddCreateStagingTexture(VDD_SWAP_PROC* proc, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width              = width;
    desc.Height             = height;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.Usage              = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    return proc->pDevice->CreateTexture2D(&desc, nullptr, &proc->pStagingTex);
}

static BOOL VddGpuTextureCompatible(VDD_SWAP_PROC* proc, ID3D11Texture2D* texture)
{
    static_assert(sizeof(DisplayGpuPacket) == 68, "GPU display packet layout");
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    return desc.Width == proc->width && desc.Height == proc->height &&
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && desc.SampleDesc.Count == 1 &&
        desc.SampleDesc.Quality == 0 && desc.ArraySize == 1 && desc.MipLevels == 1;
}

static BOOL VddHandleResize(VDD_SWAP_PROC* proc, AsbConn* connection, DisplayGpuPacket request)
{
    VDD_DEVICE_CONTEXT* ctx = proc->pOwner;
    HRESULT status = E_INVALIDARG;
    if (request.session == proc->gpuSession && proc->gpuResizeSupported && ctx->hDisplayWork &&
        !InterlockedCompareExchange(&ctx->stopping, 0, 0) &&
        request.slot <= DISPLAY_SIZE_WINDOW && request.width && request.height &&
        request.width <= DISPLAY_MAX_WIDTH && request.height <= DISPLAY_MAX_HEIGHT &&
        (request.slot == DISPLAY_SIZE_GUEST ||
         (request.width >= DISPLAY_MIN_WIDTH && request.height >= DISPLAY_MIN_HEIGHT))) {
        status = S_OK;
    }
    DisplayGpuPacket reply = request;
    reply.type = DISPLAY_GPU_RESIZED;
    reply.status = status;
    if (VddSendAll(connection, (const char*)&reply, sizeof(reply)) != 0) return FALSE;
    if (status == S_OK) {
        AcquireSRWLockExclusive(&ctx->displayLock);
        ctx->displayRequest = request;
        ReleaseSRWLockExclusive(&ctx->displayLock);
        WdfWorkItemEnqueue(ctx->hDisplayWork);
    }
    return TRUE;
}

static DWORD WINAPI VddGpuIoThread(LPVOID parameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)parameter;
    const UINT capacity = sizeof(VDD_WIRE_CURSOR_HEADER) + 256 * 256 * 4 * 2;
    BYTE* packet = (BYTE*)malloc(capacity);
    HANDLE events[] = { proc->hGpuIoStop, proc->hGpuIoWake };
    if (!packet) {
        InterlockedExchange(&proc->gpuIoFailed, 1);
        return 1;
    }
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, 100);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT) {
            InterlockedExchange(&proc->gpuIoFailed, 1);
            break;
        }
        UINT bytes = 0;
        AcquireSRWLockExclusive(&proc->gpuCursorLock);
        if (proc->bGpuCursorPending) {
            bytes = proc->gpuCursorBytes;
            memcpy(packet, proc->pGpuCursorPacket, bytes);
            proc->bGpuCursorPending = FALSE;
        }
        ReleaseSRWLockExclusive(&proc->gpuCursorLock);
        if (bytes && VddSendAll(proc->hGpuIoConn, (const char*)packet, bytes) != 0) {
            InterlockedExchange(&proc->gpuIoFailed, 1);
            break;
        }
        int ready = asb_poll(proc->hGpuIoConn, 0);
        if (ready < 0) {
            InterlockedExchange(&proc->gpuIoFailed, 1);
            break;
        }
        if (!ready) continue;
        int received = asb_recv(proc->hGpuIoConn, (char*)&proc->gpuControl + proc->gpuControlBytes,
            sizeof(proc->gpuControl) - proc->gpuControlBytes);
        if (received <= 0) {
            InterlockedExchange(&proc->gpuIoFailed, 1);
            break;
        }
        proc->gpuControlBytes += received;
        if (proc->gpuControlBytes != sizeof(proc->gpuControl)) continue;
        DisplayGpuPacket request = proc->gpuControl;
        proc->gpuControlBytes = 0;
        if (request.magic != DISPLAY_GPU_MAGIC || request.version != DISPLAY_GPU_VERSION ||
            request.type != DISPLAY_GPU_RESIZE || request.session != proc->gpuSession) {
            InterlockedExchange(&proc->gpuIoFailed, 1);
            break;
        }
        if (!VddHandleResize(proc, proc->hGpuIoConn, request)) {
            InterlockedExchange(&proc->gpuIoFailed, 1);
            break;
        }
    }
    free(packet);
    return 0;
}

static void VddStopGpuIo(VDD_SWAP_PROC* proc, BOOL keepConnection)
{
    if (proc->hGpuIoStop) SetEvent(proc->hGpuIoStop);
    if (!keepConnection && proc->hGpuIoConn) {
        unsigned long long socket = asb_conn_socket_u64(proc->hGpuIoConn);
        if (socket != ~0ull) shutdown((SOCKET)socket, SD_BOTH);
    }
    if (proc->hGpuIoThread) {
        WaitForSingleObject(proc->hGpuIoThread, INFINITE);
        CloseHandle(proc->hGpuIoThread);
    }
    proc->hGpuIoThread = NULL;
    if (proc->hGpuIoConn) {
        if (keepConnection && !InterlockedCompareExchange(&proc->gpuIoFailed, 0, 0))
            proc->hClientConn = proc->hGpuIoConn;
        else
            VddCloseConn(&proc->hGpuIoConn);
        proc->hGpuIoConn = NULL;
    }
    if (proc->hGpuIoStop) CloseHandle(proc->hGpuIoStop);
    if (proc->hGpuIoWake) CloseHandle(proc->hGpuIoWake);
    proc->hGpuIoStop = NULL;
    proc->hGpuIoWake = NULL;
    free(proc->pGpuCursorPacket);
    proc->pGpuCursorPacket = NULL;
    proc->bGpuCursorPending = FALSE;
}

static BOOL VddStartGpuIo(VDD_SWAP_PROC* proc)
{
    proc->hGpuIoStop = CreateEvent(NULL, TRUE, FALSE, NULL);
    proc->hGpuIoWake = CreateEvent(NULL, FALSE, FALSE, NULL);
    proc->pGpuCursorPacket = (BYTE*)malloc(sizeof(VDD_WIRE_CURSOR_HEADER) + 256 * 256 * 4 * 2);
    InterlockedExchange(&proc->gpuIoFailed, 0);
    if (!proc->hGpuIoStop || !proc->hGpuIoWake || !proc->pGpuCursorPacket) {
        VddStopGpuIo(proc, TRUE);
        return FALSE;
    }
    proc->hGpuIoConn = proc->hClientConn;
    proc->hClientConn = NULL;
    proc->hGpuIoThread = CreateThread(NULL, 0, VddGpuIoThread, proc, 0, NULL);
    if (!proc->hGpuIoThread) {
        VddStopGpuIo(proc, TRUE);
        return FALSE;
    }
    return TRUE;
}

static BOOL VddSendGpuPacket(VDD_SWAP_PROC* proc, UINT type, HRESULT status, UINT slot = 0)
{
    if (!proc->hClientConn) return FALSE;
    DisplayGpuPacket packet = {};
    packet.magic = DISPLAY_GPU_MAGIC;
    packet.type = type;
    packet.session = proc->gpuSession;
    packet.process_id = GetCurrentProcessId();
    packet.width = proc->width;
    packet.height = proc->height;
    packet.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    packet.status = (UINT32)status;
    packet.version = DISPLAY_GPU_VERSION;
    packet.slot = slot;
    if (type == DISPLAY_GPU_CAPS) {
        packet.reserved = proc->pOwner->hDisplayWork ? DISPLAY_GPU_CAP_RESIZE : 0;
        packet.slot = WdfObjectGet_MonitorContextWrapper(proc->hMonitor)->matchWindow
            ? DISPLAY_SIZE_WINDOW : DISPLAY_SIZE_GUEST;
        if (asb_transport_is_ivshmem()) {
            packet.reserved |= DISPLAY_GPU_CAP_IVSHMEM;
        } else {
            IDXGIDevice* device = NULL;
            IDXGIAdapter* adapter = NULL;
            DXGI_ADAPTER_DESC desc = {};
            HRESULT hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&device));
            if (hr == S_OK) hr = device->GetAdapter(&adapter);
            if (hr == S_OK) hr = adapter->GetDesc(&desc);
            if (hr == S_OK) {
                packet.vendor_id = desc.VendorId;
                packet.device_id = desc.DeviceId;
                if (desc.VendorId == 0x1414 && desc.DeviceId == 0x008c)
                    packet.status = (UINT32)DXGI_ERROR_UNSUPPORTED;
            } else packet.status = (UINT32)hr;
            if (adapter) adapter->Release();
            if (device) device->Release();
        }
    }
    if (VddSendAll(proc->hClientConn, (const char*)&packet, sizeof(packet)) == 0)
        return TRUE;
    VddCloseConn(&proc->hClientConn);
    return FALSE;
}

static BOOL VddReleaseGpuTexture(VDD_SWAP_PROC* proc, BOOL preserve)
{
    if (proc->gpuModeNotified) VddNotifyGpuMode(proc, FALSE);
    proc->gpuModeNotified = FALSE;
    BOOL captured = FALSE;
    VDD_GPU_SLOT* last = &proc->gpuSlots[proc->gpuLastSlot];
    if (preserve && proc->bGpuPendingFrame && proc->pGpuPendingTex && proc->pStagingTex) {
        proc->pDeviceContext->CopyResource(proc->pStagingTex, proc->pGpuPendingTex);
        proc->pDeviceContext->Flush();
        captured = proc->pDevice->GetDeviceRemovedReason() == S_OK;
    }
    if (preserve && !captured && proc->bGpuHasFrame && last->pMutex && proc->pStagingTex) {
        ULONGLONG deadline = GetTickCount64() + 100;
        do {
            if (proc->bStopNetwork) break;
            UINT64 key = 0;
            HRESULT hr = last->pMutex->AcquireSync(key, 0);
            if (hr == (HRESULT)WAIT_TIMEOUT) {
                key = 1;
                hr = last->pMutex->AcquireSync(key, 0);
            }
            if (hr == S_OK) {
                proc->pDeviceContext->CopyResource(proc->pStagingTex, last->pTexture);
                proc->pDeviceContext->Flush();
                last->pMutex->ReleaseSync(key);
                captured = TRUE;
                break;
            }
            if (hr != (HRESULT)WAIT_TIMEOUT) break;
            if (WaitForSingleObject(proc->hTerminateEvent, 10) == WAIT_OBJECT_0) break;
        } while (GetTickCount64() < deadline);
        if (!captured) VddLog("GPU: could not preserve disconnected viewer's last frame");
    }
    for (UINT i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
        VDD_GPU_SLOT* slot = &proc->gpuSlots[i];
        if (slot->pMutex) slot->pMutex->Release();
        if (slot->pTexture) slot->pTexture->Release();
        if (slot->hTexture) CloseHandle(slot->hTexture);
        slot->pMutex = NULL;
        slot->pTexture = NULL;
        slot->hTexture = NULL;
    }
    proc->bGpuPoolReady = FALSE;
    proc->gpuNextSlot = 0;
    proc->gpuLastSlot = 0;
    proc->bGpuHasFrame = FALSE;
    if (proc->pGpuPendingTex) proc->pGpuPendingTex->Release();
    proc->pGpuPendingTex = NULL;
    proc->bGpuPendingFrame = FALSE;
    proc->gpuBlockedSince = 0;
    return captured;
}

static void VddPollGpuControl(VDD_SWAP_PROC* proc)
{
    if (!proc->hClientConn) return;
    BOOL memory = asb_transport_is_ivshmem();
    for (UINT count = 0; count < 8 && proc->hClientConn; ++count) {
        int ready = asb_poll(proc->hClientConn, 0);
        if (ready == 0) break;
        if (ready < 0) { VddCloseConn(&proc->hClientConn); break; }
        int received = asb_recv(proc->hClientConn,
            (char*)&proc->gpuControl + proc->gpuControlBytes,
            (int)(sizeof(proc->gpuControl) - proc->gpuControlBytes));
        if (received <= 0) { VddCloseConn(&proc->hClientConn); break; }
        proc->gpuControlBytes += (UINT)received;
        if (proc->gpuControlBytes < sizeof(proc->gpuControl)) continue;
        DisplayGpuPacket packet = proc->gpuControl;
        proc->gpuControlBytes = 0;
        if (packet.magic != DISPLAY_GPU_MAGIC) {
            VddCloseConn(&proc->hClientConn);
            break;
        }
        if (packet.version != DISPLAY_GPU_VERSION) continue;
        if (packet.type == DISPLAY_GPU_QUERY && packet.session && !proc->gpuSession) {
            proc->gpuSession = packet.session;
            proc->gpuResizeSupported = (packet.reserved & DISPLAY_GPU_CAP_RESIZE) != 0 &&
                (!memory || (packet.reserved & DISPLAY_GPU_CAP_IVSHMEM) != 0);
        } else if (memory && packet.type == DISPLAY_GPU_RESIZE && proc->bGpuCapsSent) {
            if (!VddHandleResize(proc, proc->hClientConn, packet)) {
                VddCloseConn(&proc->hClientConn);
                break;
            }
        } else if (!memory && packet.type == DISPLAY_GPU_OFFER && proc->bGpuCapsSent &&
                   packet.session == proc->gpuSession && packet.slot < DISPLAY_GPU_SLOTS &&
                   !proc->gpuSlots[packet.slot].bOfferSeen) {
            VDD_GPU_SLOT* slot = &proc->gpuSlots[packet.slot];
            slot->bOfferSeen = TRUE;
            HRESULT hr = E_INVALIDARG;
            ID3D11Device1* device = NULL;
            HANDLE handle = (HANDLE)(ULONG_PTR)packet.handle;
            BOOL validTarget = handle && handle != INVALID_HANDLE_VALUE &&
                packet.process_id == GetCurrentProcessId();
            for (UINT i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
                if (i != packet.slot && proc->gpuSlots[i].hTexture == handle)
                    validTarget = FALSE;
            }
            if (validTarget) slot->hTexture = handle;
            if (validTarget && packet.status == S_OK && !proc->bGpuPoolFailed &&
                packet.width == proc->width && packet.height == proc->height &&
                packet.format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                proc->bGpuSourceCompatible) {
                hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&device));
                if (hr == S_OK)
                    hr = device->OpenSharedResource1(handle, IID_PPV_ARGS(&slot->pTexture));
            }
            if (device) device->Release();
            if (hr == S_OK && !VddGpuTextureCompatible(proc, slot->pTexture)) hr = E_INVALIDARG;
            if (hr == S_OK) hr = slot->pTexture->QueryInterface(IID_PPV_ARGS(&slot->pMutex));
            if (hr != S_OK) {
                proc->bGpuPoolFailed = TRUE;
                VddReleaseGpuTexture(proc, FALSE);
            } else {
                proc->bGpuPoolReady = TRUE;
                for (UINT i = 0; i < DISPLAY_GPU_SLOTS; ++i) {
                    if (!proc->gpuSlots[i].pTexture || !proc->gpuSlots[i].pMutex)
                        proc->bGpuPoolReady = FALSE;
                }
            }
            if (!VddSendGpuPacket(proc, DISPLAY_GPU_READY, hr, packet.slot)) {
                VddReleaseGpuTexture(proc, FALSE);
                break;
            }
            if (proc->bGpuPoolReady) {
                if (!VddStartGpuIo(proc)) {
                    VddReleaseGpuTexture(proc, FALSE);
                    proc->bGpuPoolFailed = TRUE;
                    VddSendGpuPacket(proc, DISPLAY_GPU_READY, E_OUTOFMEMORY, packet.slot);
                }
                break;
            }
        }
    }
    if (proc->hClientConn && proc->gpuSession && !proc->bGpuCapsSent && proc->bGpuSourceKnown) {
        proc->bGpuCapsSent = TRUE;
        VddSendGpuPacket(proc, DISPLAY_GPU_CAPS,
            proc->bGpuSourceCompatible ? S_OK : DXGI_ERROR_UNSUPPORTED);
    }
}

static BOOL VddPublishGpuFrame(VDD_SWAP_PROC* proc, ID3D11Texture2D* source)
{
    if (!proc->bGpuPoolReady) return FALSE;
    if (proc->bStopNetwork || WaitForSingleObject(proc->hTerminateEvent, 0) == WAIT_OBJECT_0)
        return TRUE;
    HRESULT hr = E_INVALIDARG;
    UINT failedSlot = proc->gpuNextSlot;
    if (VddGpuTextureCompatible(proc, source)) {
        do {
            UINT index = proc->gpuNextSlot;
            VDD_GPU_SLOT* slot = &proc->gpuSlots[index];
            hr = slot->pMutex->AcquireSync(0, 0);
            failedSlot = index;
            if (hr != S_OK) break;
            proc->pDeviceContext->CopyResource(slot->pTexture, source);
            proc->pDeviceContext->Flush();
            hr = slot->pMutex->ReleaseSync(1);
            if (hr != S_OK) break;
            proc->gpuLastSlot = index;
            proc->gpuNextSlot = (index + 1) % DISPLAY_GPU_SLOTS;
            proc->gpuBlockedSince = 0;
            ++proc->frameSeq;
            proc->bGpuHasFrame = TRUE;
            proc->bGpuPendingFrame = FALSE;
            if (!proc->gpuModeNotified) {
                proc->gpuModeNotified = TRUE;
                VddNotifyGpuMode(proc, TRUE);
            }
            return TRUE;
        } while (FALSE);
        if (hr == (HRESULT)WAIT_TIMEOUT) {
            ULONGLONG now = GetTickCount64();
            if (!proc->gpuBlockedSince) proc->gpuBlockedSince = now;
            if (now - proc->gpuBlockedSince >= 2000) goto failed;
            if (source == proc->pGpuPendingTex) return TRUE;
            hr = S_OK;
            if (!proc->pGpuPendingTex) {
                D3D11_TEXTURE2D_DESC desc = {};
                source->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                hr = proc->pDevice->CreateTexture2D(&desc, NULL, &proc->pGpuPendingTex);
            }
            if (hr == S_OK) {
                proc->pDeviceContext->CopyResource(proc->pGpuPendingTex, source);
                proc->pDeviceContext->Flush();
                hr = proc->pDevice->GetDeviceRemovedReason();
                if (hr == S_OK) {
                    proc->bGpuPendingFrame = TRUE;
                    return TRUE;
                }
            }
        }
    }
failed:
    VddLog("GPU: handoff failed slot=%u status=0x%08X; resuming full CPU frame", failedSlot, hr);
    proc->bGpuPoolFailed = TRUE;
    VddStopGpuIo(proc, TRUE);
    VddReleaseGpuTexture(proc, FALSE);
    VddSendGpuPacket(proc, DISPLAY_GPU_READY,
        hr == (HRESULT)WAIT_ABANDONED || hr == (HRESULT)WAIT_TIMEOUT ? E_FAIL : hr, failedSlot);
    return FALSE;
}

/* ========================================================================= */
/*  Swap chain processor thread                                              */
/* ========================================================================= */

static void VddSwapChainRunCore(VDD_SWAP_PROC* proc);

static DWORD WINAPI VddSwapChainThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    DWORD taskIndex = 0;
    HANDLE hAvrt;
    IDXGIDevice* pDxgiDevice = NULL;
    HRESULT hr;

    hAvrt = AvSetMmThreadCharacteristicsW(L"Distribution", &taskIndex);

    VddLog("SwapChain thread started");

    /* Raise GPU priority (IddCx 1.9+) */
#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority) && proc->pDevice) {
        hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&pDxgiDevice));
        if (SUCCEEDED(hr)) {
            IDARG_IN_SETREALTIMEGPUPRIORITY priorityArgs = { 0 };
            priorityArgs.pDevice = pDxgiDevice;
            IddCxSetRealtimeGPUPriority(proc->hSwapChain, &priorityArgs);
            pDxgiDevice->Release();
            pDxgiDevice = NULL;
        }
    }

    VddSwapChainRunCore(proc);

    /* Always delete the swap chain when the processing loop terminates, to
       kick the system to provide a new swap chain if necessary.  The D3D
       device is NOT released here — it's cached in VDD_DEVICE_CONTEXT to
       survive IddCx teardown. */
    if (proc->hSwapChain) {
        VddLog("SwapChain thread: deleting swap chain (WdfObjectDelete)");
        WdfObjectDelete((WDFOBJECT)proc->hSwapChain);
        proc->hSwapChain = NULL;
    }

    if (hAvrt)
        AvRevertMmThreadCharacteristics(hAvrt);

    VddLog("SwapChain thread exiting");
    return 0;
}

/* Query hardware cursor and send update to host if connected */
static void VddSendCursorUpdate(VDD_SWAP_PROC* proc)
{
    IDARG_IN_QUERY_HWCURSOR inArgs = {};
    IDARG_OUT_QUERY_HWCURSOR3 outArgs = {};
    VDD_WIRE_CURSOR_HEADER chdr;
    AsbConn* s;

    if (!proc->hCursorEvent || !proc->pCursorShapeBuffer || !proc->hMonitor)
        return;

    s = proc->hClientConn;
    if (s == NULL && !proc->hGpuIoThread)
        return;

    inArgs.LastShapeId = proc->lastShapeId;
    inArgs.ShapeBufferSizeInBytes = proc->cursorBufSize;
    inArgs.pShapeBuffer = proc->pCursorShapeBuffer;

    HRESULT hr = IddCxMonitorQueryHardwareCursor3(proc->hMonitor, &inArgs, &outArgs);
    if (FAILED(hr)) {
        VddLog("Cursor: QueryHardwareCursor3 FAILED 0x%08X", hr);
        return;
    }

    chdr.magic          = VDD_CURSOR_MAGIC;
    chdr.x              = outArgs.X;
    chdr.y              = outArgs.Y;
    chdr.visible        = outArgs.IsCursorVisible ? 1 : 0;
    chdr.shape_updated  = outArgs.IsCursorShapeUpdated ? 1 : 0;
    chdr.shape_id       = outArgs.CursorShapeInfo.ShapeId;
    chdr.width          = outArgs.CursorShapeInfo.Width;
    chdr.height         = outArgs.CursorShapeInfo.Height;
    chdr.pitch          = outArgs.CursorShapeInfo.Pitch;
    chdr.xhot           = outArgs.CursorShapeInfo.XHot;
    chdr.yhot           = outArgs.CursorShapeInfo.YHot;
    chdr.cursor_type    = (UINT32)outArgs.CursorShapeInfo.CursorType;

    if (outArgs.IsCursorShapeUpdated) {
        UINT32 data_size = outArgs.CursorShapeInfo.Pitch * outArgs.CursorShapeInfo.Height;
        if (data_size > proc->cursorBufSize) {
            VddLog("Cursor: shape data %u exceeds buffer %u, skipping",
                   data_size, proc->cursorBufSize);
            chdr.shape_updated = 0;
            chdr.shape_data_size = 0;
        } else {
            chdr.shape_data_size = data_size;
            proc->lastShapeId = outArgs.CursorShapeInfo.ShapeId;
        }
    } else {
        chdr.shape_data_size = 0;
    }

    if (proc->hGpuIoThread) {
        AcquireSRWLockExclusive(&proc->gpuCursorLock);
        VDD_WIRE_CURSOR_HEADER* queued = (VDD_WIRE_CURSOR_HEADER*)proc->pGpuCursorPacket;
        if (proc->bGpuCursorPending && queued->shape_updated && !chdr.shape_updated) {
            queued->x = chdr.x;
            queued->y = chdr.y;
            queued->visible = chdr.visible;
        } else {
            memcpy(queued, &chdr, sizeof(chdr));
            if (chdr.shape_data_size)
                memcpy(queued + 1, proc->pCursorShapeBuffer, chdr.shape_data_size);
            proc->gpuCursorBytes = sizeof(chdr) + chdr.shape_data_size;
        }
        proc->bGpuCursorPending = TRUE;
        ReleaseSRWLockExclusive(&proc->gpuCursorLock);
        SetEvent(proc->hGpuIoWake);
        return;
    }

    /* Send cursor header */
    if (VddSendAll(s, (const char*)&chdr, sizeof(chdr)) != 0) {
        VddLog("Cursor: send header failed, disconnecting");
        VddCloseConn(&proc->hClientConn);
        return;
    }

    /* Send shape bitmap if updated */
    if (chdr.shape_data_size > 0) {
        if (VddSendAll(s, (const char*)proc->pCursorShapeBuffer, (int)chdr.shape_data_size) != 0) {
            VddLog("Cursor: send shape data failed, disconnecting");
            VddCloseConn(&proc->hClientConn);
            return;
        }
    }
}

static void VddSwapChainRunCore(VDD_SWAP_PROC* proc)
{
    IDXGIDevice* pDxgiDevice = NULL;
    IDARG_IN_SWAPCHAINSETDEVICE setDevice;
    HRESULT hr;
    HANDLE waitHandles[3];  /* swap chain, terminate, cursor */
    DWORD waitCount;

    /* Retry limits — on persistent failure, exit RunCore so the caller can
       WdfObjectDelete the swap chain, which tells IddCx to reassign a fresh one. */
    const int MAX_RETRIES = 5;
    const DWORD RETRY_DELAY_INIT = 1;
    const DWORD RETRY_DELAY_MAX = 100;
    DWORD retryDelay = RETRY_DELAY_INIT;
    int retryCount = 0;

    VddLog("SwapChainRunCore: entered");

    hr = proc->pDevice->QueryInterface(IID_PPV_ARGS(&pDxgiDevice));
    if (FAILED(hr)) {
        VddLog("SwapChainRunCore: QueryInterface for IDXGIDevice FAILED hr=0x%08X", hr);
        return;
    }

    memset(&setDevice, 0, sizeof(setDevice));
    setDevice.pDevice = pDxgiDevice;

    hr = IddCxSwapChainSetDevice(proc->hSwapChain, &setDevice);
    pDxgiDevice->Release();
    if (FAILED(hr)) {
        VddLog("SwapChainRunCore: IddCxSwapChainSetDevice FAILED hr=0x%08X, exiting for reassignment", hr);
        return;
    }
    VddLog("SwapChainRunCore: swap chain device set, entering frame loop");

    waitHandles[0] = proc->hAvailableEvent;
    waitHandles[1] = proc->hTerminateEvent;
    waitCount = 2;
    if (proc->hCursorEvent) {
        waitHandles[2] = proc->hCursorEvent;
        waitCount = 3;
    }

    BOOL bHasFrame = FALSE;
    BOOL bSentFullFrame = FALSE;   /* TRUE after first full frame sent to current client */


    /* Main frame acquisition loop */
    for (;;)
    {
        IDXGIResource* pSurface = NULL;
        if (proc->bStopNetwork || WaitForSingleObject(proc->hTerminateEvent, 0) == WAIT_OBJECT_0)
            break;
        if (FAILED(proc->pDevice->GetDeviceRemovedReason())) break;

        if (proc->hGpuIoThread && InterlockedCompareExchange(&proc->gpuIoFailed, 0, 0)) {
            VddStopGpuIo(proc, FALSE);
            bHasFrame = VddReleaseGpuTexture(proc, TRUE);
            bSentFullFrame = FALSE;
        }
        if (!proc->hClientConn && proc->gpuSession && !proc->bGpuPoolReady) {
            if (proc->bGpuHasFrame || proc->bGpuPendingFrame) bHasFrame = VddReleaseGpuTexture(proc, TRUE);
            else VddReleaseGpuTexture(proc, FALSE);
            proc->gpuSession = 0;
            bSentFullFrame = FALSE;
        }

        /* Check for pending new connection from network thread */
        {
            AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
                (PVOID volatile*)&proc->hPendingConn, NULL);
            if (pending) {
                VddStopGpuIo(proc, FALSE);
                /* Drop the previous client. On PC the old socket is distinct -> close it. On ivshmem
                   the pending connection is the SAME single slot the host just re-armed, so closing
                   the old wrapper would mark the shared slot CLOSING and kill the new one — abandon
                   the stale wrapper instead. */
                if (proc->hClientConn) {
                    if (asb_conn_socket_u64(proc->hClientConn) != ~0ull)
                        VddCloseConn(&proc->hClientConn);          /* PC: real close */
                    else { asb_abandon(proc->hClientConn); proc->hClientConn = NULL; }  /* ivshmem */
                    VddLog("Frame: replaced previous client connection");
                }
                if (proc->bGpuHasFrame || proc->bGpuPendingFrame) bHasFrame = VddReleaseGpuTexture(proc, TRUE);
                else VddReleaseGpuTexture(proc, FALSE);
                asb_stream_reset(pending);   /* ivshmem: align the fresh full frame at ring offset 0 (no-op on PC) */
                proc->hClientConn = pending;
                asb_set_timeout(pending, 1000, 1000);
                proc->frameSeq = 0;
                proc->gpuSession = 0;
                proc->gpuResizeSupported = FALSE;
                proc->bGpuCapsSent = FALSE;
                proc->bGpuPoolFailed = FALSE;
                ZeroMemory(proc->gpuSlots, sizeof(proc->gpuSlots));
                proc->gpuControlBytes = 0;
                bSentFullFrame = FALSE;   /* new connection -> first frame is full (the reconnect trigger) */
                VddLog("Frame: new client connection active");
            }
        }

        if (!proc->bGpuPoolReady) VddPollGpuControl(proc);
        if (proc->bGpuPoolReady && proc->bGpuPendingFrame) {
            ID3D11Texture2D* pending = proc->pGpuPendingTex;
            pending->AddRef();
            if (!VddPublishGpuFrame(proc, pending)) {
                proc->pDeviceContext->CopyResource(proc->pStagingTex, pending);
                proc->pDeviceContext->Flush();
                bHasFrame = proc->pDevice->GetDeviceRemovedReason() == S_OK;
                VddReleaseGpuTexture(proc, FALSE);
                bSentFullFrame = FALSE;
            }
            pending->Release();
        }
        if (proc->bGpuPoolReady && !proc->bGpuHasFrame && bHasFrame) {
            if (!VddPublishGpuFrame(proc, proc->pStagingTex)) {
                VddReleaseGpuTexture(proc, FALSE);
                bSentFullFrame = FALSE;
            }
        }

        /* Use v2 acquire when available */
#pragma warning(suppress: 4127)
        if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
            IDARG_IN_RELEASEANDACQUIREBUFFER2 bufferInArgs = { 0 };
            IDARG_OUT_RELEASEANDACQUIREBUFFER2 buffer = { 0 };
            bufferInArgs.Size = sizeof(bufferInArgs);
            hr = IddCxSwapChainReleaseAndAcquireBuffer2(proc->hSwapChain, &bufferInArgs, &buffer);
            pSurface = buffer.MetaData.pSurface;
        } else {
            IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = { 0 };
            hr = IddCxSwapChainReleaseAndAcquireBuffer(proc->hSwapChain, &buffer);
            pSurface = buffer.MetaData.pSurface;
        }

        if (hr == E_PENDING) {
            DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE,
                proc->bGpuPendingFrame ? 1 : 100);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                break;  /* Terminate */
            }
            if (waitResult == WAIT_OBJECT_0 + 2) {
                /* Cursor event — query and send cursor update over the transport */
                VddSendCursorUpdate(proc);
                continue;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
                VddLog("SwapChainRunCore: unexpected wait result 0x%lX, exiting for reassignment", waitResult);
                break;
            }

            /* On timeout with a connected client and a cached frame, resend
               the staging texture contents to keep the connection alive. */
            if (waitResult == WAIT_TIMEOUT && !proc->bGpuPoolReady && bHasFrame &&
                proc->hClientConn != NULL && proc->pStagingTex)
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    VDD_WIRE_FRAME_HEADER whdr;
                    UINT32 data_size = proc->stride * proc->height;
                    BOOL send_ok = TRUE;
                    AsbConn* s = proc->hClientConn;

                    proc->frameSeq++;
                    whdr.magic            = VDD_FRAME_MAGIC;
                    whdr.width            = proc->width;
                    whdr.height           = proc->height;
                    whdr.stride           = proc->stride;
                    whdr.frame_seq        = proc->frameSeq;
                    whdr.dirty_rect_count = 0;

                    if (VddSendAll(s, (const char*)&whdr, sizeof(whdr)) != 0 ||
                        VddSendAll(s, (const char*)&data_size, sizeof(data_size)) != 0) {
                        send_ok = FALSE;
                    }
                    if (send_ok) {
                        for (UINT row = 0; row < proc->height; row++) {
                            if (VddSendAll(s, (const char*)((const BYTE*)mapped.pData + row * mapped.RowPitch),
                                           proc->stride) != 0) {
                                send_ok = FALSE;
                                break;
                            }
                        }
                    }

                    proc->pDeviceContext->Unmap(proc->pStagingTex, 0);

                    if (!send_ok) {
                        VddLog("Frame: resend failed, disconnecting (seq=%llu)", proc->frameSeq);
                        VddCloseConn(&proc->hClientConn);
                        bSentFullFrame = FALSE;
                    }
                }
            }

            continue;
        } else if (SUCCEEDED(hr)) {
            /* Reset retry state on successful acquisition */
            retryDelay = RETRY_DELAY_INIT;
            retryCount = 0;

            /* New frame from IddCx — capture and send to host if connected */
            if (pSurface)
            {
                ID3D11Texture2D* pTexture = nullptr;
                hr = pSurface->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture);
                if (SUCCEEDED(hr) && pTexture)
                {
                    D3D11_TEXTURE2D_DESC desc = {};
                    pTexture->GetDesc(&desc);
                    if (!desc.Width || !desc.Height || desc.Width > DISPLAY_MAX_WIDTH ||
                        desc.Height > DISPLAY_MAX_HEIGHT || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
                        (proc->width && (desc.Width != proc->width || desc.Height != proc->height))) {
                        pTexture->Release();
                        pSurface->Release();
                        break;
                    }
                    if (!proc->pStagingTex) {
                        proc->width = desc.Width;
                        proc->height = desc.Height;
                        proc->stride = desc.Width * 4;
                        hr = VddCreateStagingTexture(proc, desc.Width, desc.Height);
                        if (FAILED(hr)) {
                            pTexture->Release();
                            pSurface->Release();
                            break;
                        }
                    }
                    proc->bGpuSourceKnown = TRUE;
                    proc->bGpuSourceCompatible = VddGpuTextureCompatible(proc, pTexture);
                    BOOL hadGpuTexture = proc->bGpuPoolReady;
                    if (VddPublishGpuFrame(proc, pTexture)) {
                        bHasFrame = FALSE;
                        pTexture->Release();
                        pTexture = nullptr;
                    } else if (hadGpuTexture) {
                        VddReleaseGpuTexture(proc, FALSE);
                        bSentFullFrame = FALSE;
                    }
                }
                if (pTexture)
                {
                    if (!proc->pStagingTex) {
                        pTexture->Release();
                        pSurface->Release();
                        break;
                    }
                    proc->pDeviceContext->CopyResource(proc->pStagingTex, pTexture);

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                    if (SUCCEEDED(hr))
                    {
                        bHasFrame = TRUE;

                        if (proc->hClientConn != NULL)
                        {
                            RECT dirtyRects[VDD_MAX_DIRTY_RECTS];
                            UINT32 rectCount = 0;
                            BOOL sendFull = !bSentFullFrame;  /* first frame must be full */

                            /* Query dirty rects from IddCx.  If GetDirtyRects returns
                               0 rects OR fails, fall back to sending a full frame.
                               0 rects does NOT reliably mean "identical" — during early
                               boot the presenter may not supply dirty rect metadata,
                               and skipping would stall updates until the E_PENDING
                               timeout resend path fires ~100ms later. */
                            if (!sendFull) {
                                IDARG_IN_GETDIRTYRECTS drIn = {};
                                IDARG_OUT_GETDIRTYRECTS drOut = {};
                                drIn.DirtyRectInCount = VDD_MAX_DIRTY_RECTS;
                                drIn.pDirtyRects = dirtyRects;
                                HRESULT drHr = IddCxSwapChainGetDirtyRects(proc->hSwapChain, &drIn, &drOut);
                                if (SUCCEEDED(drHr) && drOut.DirtyRectOutCount > 0) {
                                    rectCount = drOut.DirtyRectOutCount;
                                } else {
                                    sendFull = TRUE;
                                }
                            }

                            VDD_WIRE_FRAME_HEADER whdr;
                            BOOL send_ok = TRUE;
                            AsbConn* s = proc->hClientConn;

                            proc->frameSeq++;
                            whdr.magic            = VDD_FRAME_MAGIC;
                            whdr.width            = proc->width;
                            whdr.height           = proc->height;
                            whdr.stride           = proc->stride;
                            whdr.frame_seq        = proc->frameSeq;

                            if (sendFull) {
                                /* Full frame */
                                UINT32 data_size = proc->stride * proc->height;
                                whdr.dirty_rect_count = 0;

                                if (VddSendAll(s, (const char*)&whdr, sizeof(whdr)) != 0 ||
                                    VddSendAll(s, (const char*)&data_size, sizeof(data_size)) != 0)
                                    send_ok = FALSE;

                                if (send_ok) {
                                    for (UINT row = 0; row < proc->height; row++) {
                                        if (VddSendAll(s, (const char*)((const BYTE*)mapped.pData + row * mapped.RowPitch),
                                                       proc->stride) != 0) {
                                            send_ok = FALSE;
                                            break;
                                        }
                                    }
                                }
                                if (send_ok) bSentFullFrame = TRUE;
                            } else {
                                /* Dirty rects only */
                                UINT32 data_size = 0;
                                UINT32 i;

                                /* Clamp rects to frame bounds and calculate total data size */
                                for (i = 0; i < rectCount; i++) {
                                    if (dirtyRects[i].left < 0) dirtyRects[i].left = 0;
                                    if (dirtyRects[i].top  < 0) dirtyRects[i].top  = 0;
                                    if (dirtyRects[i].right  > (LONG)proc->width)  dirtyRects[i].right  = proc->width;
                                    if (dirtyRects[i].bottom > (LONG)proc->height) dirtyRects[i].bottom = proc->height;
                                    if (dirtyRects[i].left < dirtyRects[i].right &&
                                        dirtyRects[i].top < dirtyRects[i].bottom) {
                                        UINT rw = (UINT)(dirtyRects[i].right - dirtyRects[i].left);
                                        UINT rh = (UINT)(dirtyRects[i].bottom - dirtyRects[i].top);
                                        data_size += rw * 4 * rh;
                                    }
                                }

                                /* All rects zero area after clamping — skip send */
                                if (data_size == 0) goto skip_send;

                                whdr.dirty_rect_count = rectCount;

                                if (VddSendAll(s, (const char*)&whdr, sizeof(whdr)) != 0 ||
                                    VddSendAll(s, (const char*)dirtyRects, rectCount * sizeof(RECT)) != 0 ||
                                    VddSendAll(s, (const char*)&data_size, sizeof(data_size)) != 0)
                                    send_ok = FALSE;

                                /* Send per-rect pixel rows */
                                if (send_ok) {
                                    for (i = 0; i < rectCount && send_ok; i++) {
                                        LONG left = dirtyRects[i].left;
                                        LONG top  = dirtyRects[i].top;
                                        UINT rw   = (UINT)(dirtyRects[i].right - left);
                                        UINT rh   = (UINT)(dirtyRects[i].bottom - top);
                                        if (rw == 0 || rh == 0) continue;
                                        for (UINT row = 0; row < rh; row++) {
                                            const BYTE *src = (const BYTE*)mapped.pData
                                                              + ((UINT)top + row) * mapped.RowPitch
                                                              + (UINT)left * 4;
                                            if (VddSendAll(s, (const char*)src, rw * 4) != 0) {
                                                send_ok = FALSE;
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            if (!send_ok) {
                                VddLog("Frame: send failed, disconnecting (seq=%llu)", proc->frameSeq);
                                VddCloseConn(&proc->hClientConn);
                                bSentFullFrame = FALSE;
                            } else if (proc->frameSeq == 1) {
                                VddLog("Frame: first frame sent (%ux%u, %u bytes)",
                                       proc->width, proc->height, proc->stride * proc->height);
                            }
                        skip_send:;
                        }

                        proc->pDeviceContext->Unmap(proc->pStagingTex, 0);
                    }

                    pTexture->Release();
                }
            }

            if (pSurface) {
                pSurface->Release();
            }

            /* Check for cursor update alongside frame */
            if (proc->hCursorEvent &&
                WaitForSingleObject(proc->hCursorEvent, 0) == WAIT_OBJECT_0) {
                VddSendCursorUpdate(proc);
            }


            hr = IddCxSwapChainFinishedProcessingFrame(proc->hSwapChain);
            if (FAILED(hr)) {
                VddLog("SwapChainRunCore: FinishedProcessingFrame FAILED hr=0x%08X, exiting for reassignment", hr);
                break;
            }
            if (proc->bGpuPoolReady) proc->pDeviceContext->Flush();
        } else {
            /* ACCESS_LOST can be transient (display mode change) — retry with
               exponential backoff up to MAX_RETRIES.  All other errors: exit
               immediately so WdfObjectDelete triggers IddCx reassignment. */
            if (hr == (HRESULT)0x887A0026 /* DXGI_ERROR_ACCESS_LOST */ && retryCount < MAX_RETRIES) {
                retryCount++;
                VddLog("SwapChainRunCore: DXGI_ERROR_ACCESS_LOST, retry %d/%d after %lums",
                       retryCount, MAX_RETRIES, retryDelay);
                if (WaitForSingleObject(proc->hTerminateEvent, retryDelay) == WAIT_OBJECT_0)
                    break;
                retryDelay = min(retryDelay * 2, RETRY_DELAY_MAX);
                continue;
            }
            VddLog("SwapChainRunCore: AcquireBuffer FAILED hr=0x%08X (retries=%d), exiting for reassignment",
                   hr, retryCount);
            break;
        }
    }
    VddStopGpuIo(proc, FALSE);
    VddReleaseGpuTexture(proc, FALSE);
}

/* ========================================================================= */
/*  Swap chain cleanup helper                                                */
/* ========================================================================= */

static void VddDestroySwapProc(VDD_SWAP_PROC* proc)
{
    if (!proc) return;

    /* Stop network first */
    proc->bStopNetwork = TRUE;

    VddLog("DestroySwapProc: signaling terminate event");
    if (proc->hTerminateEvent)
        SetEvent(proc->hTerminateEvent);

    if (proc->hThread)
    {
        VddLog("DestroySwapProc: waiting for swap chain thread...");
        WaitForSingleObject(proc->hThread, INFINITE);
        VddLog("DestroySwapProc: swap chain thread exited");
        CloseHandle(proc->hThread);
    }

    /* Wait for network thread to exit */
    if (proc->hNetworkThread)
    {
        VddLog("DestroySwapProc: waiting for network thread...");
        WaitForSingleObject(proc->hNetworkThread, INFINITE);
        VddLog("DestroySwapProc: network thread exited");
        CloseHandle(proc->hNetworkThread);
    }

    /* Close any remaining client connection */
    VddLog("DestroySwapProc: closing client connection (%p)", (void*)proc->hClientConn);
    VddCloseConn(&proc->hClientConn);
    VddLog("DestroySwapProc: closing pending connection");
    {
        AsbConn* pending = (AsbConn*)InterlockedExchangePointer(
            (PVOID volatile*)&proc->hPendingConn, NULL);
        if (pending) VddCloseConn(&pending);
    }

    VddLog("DestroySwapProc: closing terminate event");
    if (proc->hTerminateEvent)
        CloseHandle(proc->hTerminateEvent);

    VddLog("DestroySwapProc: closing cursor resources");
    if (proc->hCursorEvent)
        CloseHandle(proc->hCursorEvent);
    if (proc->pCursorShapeBuffer)
        free(proc->pCursorShapeBuffer);

    /* Only release the per-swap-chain staging texture.  The D3D device and
       context are cached in VDD_DEVICE_CONTEXT and intentionally NOT released
       here — this is the fix for the pDevice->Release() crash after session
       logoff.  Destroying the D3D device after WdfObjectDelete triggers IddCx
       teardown corrupts DXGI internal state. */
    VddLog("DestroySwapProc: releasing staging texture (staging=%p, device=%p cached)",
           (void*)proc->pStagingTex, (void*)proc->pDevice);
    if (proc->pStagingTex)
        proc->pStagingTex->Release();
    VddLog("DestroySwapProc: staging texture released");

    free(proc);
    VddLog("DestroySwapProc: COMPLETE");
}

/* ========================================================================= */
/*  Device context init/cleanup                                              */
/* ========================================================================= */

static void VddContextInit(VDD_DEVICE_CONTEXT* ctx, WDFDEVICE device)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->wdfDevice = device;

    InitializeSRWLock(&ctx->swapLock);
    InitializeSRWLock(&ctx->displayLock);
    InitializeSRWLock(&ctx->displayWorkLock);
    CoCreateGuid(&ctx->monitorContainerId);
}

static void VddContextCleanup(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("Context cleanup starting");
    InterlockedExchange(&ctx->stopping, 1);
    if (ctx->hDisplayWork) WdfWorkItemFlush(ctx->hDisplayWork);

    /* Stop recovery timer (wait for any in-progress callback to finish) */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, TRUE);
    }

    if (ctx->pSwapProc)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }
    if (ctx->hDisplayWork) WdfWorkItemFlush(ctx->hDisplayWork);

    /* Release cached D3D device (safe here — no IddCx teardown in progress) */
    if (ctx->pCachedCtx) {
        ctx->pCachedCtx->Release();
        ctx->pCachedCtx = nullptr;
    }
    if (ctx->pCachedDevice) {
        VddLog("Context cleanup: releasing cached D3D device");
        ctx->pCachedDevice->Release();
        ctx->pCachedDevice = nullptr;
    }

    VddLog("Context cleanup done");
}

/* ========================================================================= */
/*  InitAdapter (called from D0Entry)                                        */
/* ========================================================================= */

static void VddInitAdapter(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("InitAdapter: entered (ctx=%p, wdfDevice=%p)", (void*)ctx, (void*)ctx->wdfDevice);

    IDDCX_ADAPTER_CAPS adapterCaps = {};
    adapterCaps.Size = sizeof(adapterCaps);
    VddLog("InitAdapter: IDDCX_ADAPTER_CAPS.Size=%u", (unsigned)adapterCaps.Size);

    adapterCaps.MaxMonitorsSupported = 1;
    adapterCaps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;

    adapterCaps.EndPointDiagnostics.Size = sizeof(adapterCaps.EndPointDiagnostics);
    adapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    adapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    adapterCaps.EndPointDiagnostics.pEndPointFriendlyName      = L"AppSandbox Virtual Display";
    adapterCaps.EndPointDiagnostics.pEndPointManufacturerName   = L"AppSandbox";
    adapterCaps.EndPointDiagnostics.pEndPointModelName          = L"VDD";

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    adapterCaps.EndPointDiagnostics.pFirmwareVersion = &version;
    adapterCaps.EndPointDiagnostics.pHardwareVersion = &version;

    VddLog("InitAdapter: adapter caps configured, calling IddCxAdapterInitAsync...");

    /* Put IndirectDeviceContextWrapper on the adapter object so we can
       retrieve our context in AdapterInitFinished */
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = ctx->wdfDevice;
    adapterInit.pCaps     = &adapterCaps;
    adapterInit.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT adapterInitOut = {};
    NTSTATUS st = IddCxAdapterInitAsync(&adapterInit, &adapterInitOut);

    if (NT_SUCCESS(st))
    {
        ctx->hAdapter = adapterInitOut.AdapterObject;

        /* Link adapter wrapper back to our context */
        IndirectDeviceContextWrapper* pWrapper =
            WdfObjectGet_IndirectDeviceContextWrapper(adapterInitOut.AdapterObject);
        pWrapper->pContext = ctx;

        VddLog("InitAdapter: IddCxAdapterInitAsync succeeded (adapter=%p)", (void*)adapterInitOut.AdapterObject);
    }
    else
    {
        VddLog("InitAdapter: IddCxAdapterInitAsync FAILED st=0x%08X", st);
    }
}

/* ========================================================================= */
/*  FinishInit (called from AdapterInitFinished)                             */
/* ========================================================================= */

static void VddFinishInit(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("FinishInit: adapter ready (ctx=%p)", (void*)ctx);
    UNREFERENCED_PARAMETER(ctx);
}

/* ========================================================================= */
/*  CreateMonitor (called after adapter init finished)                       */
/* ========================================================================= */

static NTSTATUS VddCreateMonitor(VDD_DEVICE_CONTEXT* ctx,
    UINT width = VDD_WIDTH, UINT height = VDD_HEIGHT, BOOL matchWindow = FALSE)
{
    VddLog("CreateMonitor: entered (ctx=%p, adapter=%p)", (void*)ctx, (void*)ctx->hAdapter);

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = 0;
    ResolutionEntry modes[VDD_MAX_MODES] = {};
    UINT modeCount = 1;
    modes[0] = { width, height };
    if (!matchWindow) {
        for (UINT i = 0; i < g_NumResolutions; ++i) {
            if (g_SupportedResolutions[i].width != width || g_SupportedResolutions[i].height != height)
                modes[modeCount++] = g_SupportedResolutions[i];
        }
    }
    BYTE edid[VDD_EDID_BYTES];

    monitorInfo.MonitorDescription.Size     = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = VddBuildEdid(edid, modes, modeCount);
    monitorInfo.MonitorDescription.pData    = edid;

    VddLog("CreateMonitor: EDID checksum byte=0x%02X, IDDCX_MONITOR_INFO.Size=%u",
           ((const BYTE*)monitorInfo.MonitorDescription.pData)[127],
           (unsigned)monitorInfo.Size);

    monitorInfo.MonitorContainerId = ctx->monitorContainerId;

    WDF_OBJECT_ATTRIBUTES monAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monAttrs, MonitorContextWrapper);

    IDARG_IN_MONITORCREATE createIn = {};
    createIn.ObjectAttributes = &monAttrs;
    createIn.pMonitorInfo     = &monitorInfo;

    IDARG_OUT_MONITORCREATE createOut = {};

    VddLog("CreateMonitor: calling IddCxMonitorCreate...");
    NTSTATUS st = IddCxMonitorCreate(ctx->hAdapter, &createIn, &createOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorCreate FAILED st=0x%08X", st);
        return st;
    }
    VddLog("CreateMonitor: IddCxMonitorCreate succeeded (monitor=%p)", (void*)createOut.MonitorObject);

    /* Store context on the monitor object */
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject);
    pMonCtx->pContext = ctx;
    pMonCtx->modeCount = modeCount;
    pMonCtx->matchWindow = matchWindow;
    memcpy(pMonCtx->modes, modes, modeCount * sizeof(*modes));
    AcquireSRWLockExclusive(&ctx->swapLock);
    ctx->hMonitor = createOut.MonitorObject;
    ReleaseSRWLockExclusive(&ctx->swapLock);

    VddLog("CreateMonitor: calling IddCxMonitorArrival...");
    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    st = IddCxMonitorArrival(ctx->hMonitor, &arrivalOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorArrival FAILED st=0x%08X", st);
        AcquireSRWLockExclusive(&ctx->swapLock);
        ctx->hMonitor = NULL;
        ReleaseSRWLockExclusive(&ctx->swapLock);
        WdfObjectDelete(createOut.MonitorObject);
        return st;
    }

    VddLog("CreateMonitor: monitor created and arrived successfully");

    return STATUS_SUCCESS;
}

static void VddUpdateDisplay(VDD_DEVICE_CONTEXT* ctx)
{
    if (InterlockedCompareExchange(&ctx->stopping, 0, 0)) return;
    DisplayGpuPacket request = {};
    UINT64 session;
    AcquireSRWLockExclusive(&ctx->swapLock);
    IDDCX_MONITOR monitor = ctx->hMonitor;
    VDD_SWAP_PROC* proc = ctx->pSwapProc;
    if (ctx->swapChanging) {
        ReleaseSRWLockExclusive(&ctx->swapLock);
        return;
    }
    AcquireSRWLockExclusive(&ctx->displayLock);
    session = ctx->gpuModeSession;
    BOOL memory = asb_transport_is_ivshmem();
    BOOL gpu = session && proc && proc->gpuSession == session &&
        proc->bGpuPoolReady && proc->gpuResizeSupported &&
        !InterlockedCompareExchange(&proc->gpuIoFailed, 0, 0) &&
        proc->pDevice->GetDeviceRemovedReason() == S_OK;
    BOOL available = gpu || (memory && proc && proc->gpuSession && proc->gpuResizeSupported &&
        proc->hClientConn && proc->pDevice->GetDeviceRemovedReason() == S_OK);
    if (!available && monitor && ctx->gpuModes && ctx->reconfiguring &&
        (!proc || proc->pDevice->GetDeviceRemovedReason() == S_OK)) {
        ReleaseSRWLockExclusive(&ctx->displayLock);
        ReleaseSRWLockExclusive(&ctx->swapLock);
        return;
    }
    request = ctx->displayRequest;
    ctx->displayRequest.type = 0;
    ReleaseSRWLockExclusive(&ctx->displayLock);
    if (request.type && (!proc || request.session != proc->gpuSession)) request.type = 0;
    InterlockedExchange(&ctx->gpuModes, gpu);
    BOOL fullModes = gpu || memory;
    BOOL modesChanged = InterlockedExchange(&ctx->fullModes, fullModes) != fullModes;
    if (!monitor) {
        ReleaseSRWLockExclusive(&ctx->swapLock);
        NTSTATUS status = VddCreateMonitor(ctx);
        if (!NT_SUCCESS(status) && ctx->hRecoveryTimer)
            WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(2));
        return;
    }
    MonitorContextWrapper* mon = WdfObjectGet_MonitorContextWrapper(monitor);
    UINT width = mon->modes[0].width, height = mon->modes[0].height;
    BOOL matchWindow = mon->matchWindow;
    BOOL recreate = !proc;
    if (request.type && available && request.session == proc->gpuSession) {
        matchWindow = request.slot == DISPLAY_SIZE_WINDOW;
        width = request.width;
        height = request.height;
        recreate = matchWindow != mon->matchWindow ||
            (matchWindow && (width != mon->modes[0].width || height != mon->modes[0].height));
    } else if ((!memory && !available && mon->matchWindow) || (!fullModes &&
        ((proc && (proc->width > VDD_WIDTH || proc->height > VDD_HEIGHT)) ||
         (!proc && (width > VDD_WIDTH || height > VDD_HEIGHT))))) {
        matchWindow = FALSE;
        width = VDD_WIDTH;
        height = VDD_HEIGHT;
        recreate = TRUE;
    }
    ResolutionEntry previous = mon->modes[0];
    BOOL previousMatch = mon->matchWindow;
    if (recreate) {
        InterlockedExchange(&ctx->reconfiguring, 1);
        ctx->hMonitor = NULL;
        ctx->pSwapProc = NULL;
    }
    ReleaseSRWLockExclusive(&ctx->swapLock);
    if (recreate) {
        VddDestroySwapProc(proc);
        VddLog("Display: recreating monitor %ux%u matchWindow=%d gpu=%d", width, height, matchWindow, available);
        NTSTATUS status = IddCxMonitorDeparture(monitor);
        if (!NT_SUCCESS(status)) {
            AcquireSRWLockExclusive(&ctx->swapLock);
            ctx->hMonitor = monitor;
            InterlockedExchange(&ctx->reconfiguring, 0);
            ReleaseSRWLockExclusive(&ctx->swapLock);
            VddLog("Display: monitor departure failed 0x%08X", status);
            if (ctx->hRecoveryTimer) WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(2));
            return;
        }
        status = VddCreateMonitor(ctx, width, height, matchWindow);
        if (!NT_SUCCESS(status)) {
            VddLog("Display: resize failed 0x%08X; restoring monitor", status);
            status = VddCreateMonitor(ctx, available ? previous.width : VDD_WIDTH,
                available ? previous.height : VDD_HEIGHT, available && previousMatch);
        }
        if (ctx->hRecoveryTimer)
            WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(NT_SUCCESS(status) ? 5 : 2));
    } else if (modesChanged) {
        InterlockedExchange(&ctx->reconfiguring, 1);
        ResolutionEntry resolutions[VDD_MAX_MODES];
        UINT count = VddTargetResolutions(monitor, resolutions);
        NTSTATUS status;
#pragma warning(suppress: 4127)
        if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2)) {
            IDDCX_TARGET_MODE2 modes[VDD_MAX_MODES] = {};
            for (UINT i = 0; i < count; ++i) {
                modes[i].Size = sizeof(modes[i]);
                modes[i].BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
                VddCreateMonitorMode(&modes[i].TargetVideoSignalInfo.targetVideoSignalInfo, 1,
                    resolutions[i].width, resolutions[i].height);
            }
            IDARG_IN_UPDATEMODES2 args = {};
            args.Reason = IDDCX_UPDATE_REASON_CONFIGURATION_CONSTRAINTS;
            args.TargetModeCount = count;
            args.pTargetModes = modes;
            status = IddCxMonitorUpdateModes2(monitor, &args);
        } else {
            IDDCX_TARGET_MODE modes[VDD_MAX_MODES] = {};
            for (UINT i = 0; i < count; ++i) {
                modes[i].Size = sizeof(modes[i]);
                VddCreateMonitorMode(&modes[i].TargetVideoSignalInfo.targetVideoSignalInfo, 1,
                    resolutions[i].width, resolutions[i].height);
            }
            IDARG_IN_UPDATEMODES args = {};
            args.Reason = IDDCX_UPDATE_REASON_CONFIGURATION_CONSTRAINTS;
            args.TargetModeCount = count;
            args.pTargetModes = modes;
            status = IddCxMonitorUpdateModes(monitor, &args);
        }
        VddLog("Display: full modes=%d count=%u status=0x%08X", fullModes, count, status);
        if (!NT_SUCCESS(status)) InterlockedExchange(&ctx->reconfiguring, 0);
        else if (ctx->hRecoveryTimer)
            WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
    }
}

static VOID VddDisplayWork(WDFWORKITEM work)
{
    VDD_DEVICE_CONTEXT* ctx = WdfObjectGet_IndirectDeviceContextWrapper(
        WdfWorkItemGetParentObject(work))->pContext;
    if (!ctx) return;
    AcquireSRWLockExclusive(&ctx->displayWorkLock);
    VddUpdateDisplay(ctx);
    ReleaseSRWLockExclusive(&ctx->displayWorkLock);
}

/* ========================================================================= */
/*  Monitor recovery: depart + re-arrive if no AssignSwapChain after Unassign*/
/*  Uses WDF timer so callback runs on a proper WDF thread — IddCx APIs     */
/*  crash when called from raw CreateThread threads in UMDF.                 */
/* ========================================================================= */

static VOID VddRecoveryTimerCallback(WDFTIMER Timer)
{
    RecoveryTimerContext* timerCtx = WdfObjectGet_RecoveryTimerContext(Timer);
    VDD_DEVICE_CONTEXT* ctx = timerCtx->pContext;

    if (InterlockedCompareExchange(&ctx->stopping, 0, 0)) return;
    if (!TryAcquireSRWLockExclusive(&ctx->displayWorkLock)) return;
    AcquireSRWLockExclusive(&ctx->swapLock);
    if (ctx->swapChanging) {
        ReleaseSRWLockExclusive(&ctx->swapLock);
        ReleaseSRWLockExclusive(&ctx->displayWorkLock);
        return;
    }
    VDD_SWAP_PROC* proc = ctx->pSwapProc;
    BOOL ready = proc && proc->bGpuPoolReady && proc->bGpuHasFrame &&
        !InterlockedCompareExchange(&proc->gpuIoFailed, 0, 0) &&
        proc->pDevice->GetDeviceRemovedReason() == S_OK;
    InterlockedExchange(&ctx->reconfiguring, 0);
    BOOL update = !ctx->hMonitor || !proc || (!ready && ctx->gpuModes);
    AcquireSRWLockExclusive(&ctx->displayLock);
    if (update) ctx->gpuModeSession = 0;
    update = update || ctx->displayRequest.type != 0;
    ReleaseSRWLockExclusive(&ctx->displayLock);
    ReleaseSRWLockExclusive(&ctx->swapLock);
    if (update && ctx->hDisplayWork) WdfWorkItemEnqueue(ctx->hDisplayWork);
    ReleaseSRWLockExclusive(&ctx->displayWorkLock);
}

/* ========================================================================= */
/*  IddCx: Assign / Unassign Swap Chain                                     */
/* ========================================================================= */

static NTSTATUS VddAssignSwapChain(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    VddLog("AssignSwapChain: entered (MonitorObject=%p)", (void*)MonitorObject);

    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    /* Cancel any pending recovery timer — a new swap chain is being assigned */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, FALSE);
        VddLog("AssignSwapChain: recovery timer cancelled");
    }

    VddLog("AssignSwapChain: allocating new swap proc (%u bytes)", (unsigned)sizeof(VDD_SWAP_PROC));
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)malloc(sizeof(VDD_SWAP_PROC));
    if (!proc)
    {
        VddLog("AssignSwapChain: malloc FAILED");
        return STATUS_NO_MEMORY;
    }
    memset(proc, 0, sizeof(*proc));
    proc->pOwner = ctx;
    VddLog("AssignSwapChain: swap proc allocated at %p", (void*)proc);

    proc->hSwapChain      = pInArgs->hSwapChain;
    proc->hAvailableEvent = pInArgs->hNextSurfaceAvailable;
    proc->hTerminateEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    VddLog("AssignSwapChain: hSwapChain=%p hAvailableEvent=%p hTerminateEvent=%p",
           (void*)proc->hSwapChain, (void*)proc->hAvailableEvent, (void*)proc->hTerminateEvent);
    if (!proc->hTerminateEvent) {
        free(proc);
        return STATUS_NO_MEMORY;
    }

    VddLog("AssignSwapChain: getting D3D11 device (LUID=%08X:%08X)...",
           pInArgs->RenderAdapterLuid.HighPart, pInArgs->RenderAdapterLuid.LowPart);

    /* Get or reuse cached D3D11 device (cached in ctx to survive swap chain
       transitions) */
    HRESULT hr = VddGetOrCreateD3DDevice(ctx, pInArgs->RenderAdapterLuid,
                                          &proc->pDevice, &proc->pDeviceContext);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: D3D11 device creation FAILED hr=0x%08X", hr);
        VddPreferSoftwareRenderAdapter(ctx->hAdapter);
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: D3D11 device ready (cached=%p)", (void*)proc->pDevice);

    /* Initialize network state */
    VddLog("AssignSwapChain: initializing network state");
    proc->hClientConn    = NULL;
    proc->hPendingConn   = NULL;
    proc->bStopNetwork   = FALSE;
    proc->frameSeq       = 0;

    /* Setup hardware cursor BEFORE starting swap chain thread — the thread
       reads proc->hCursorEvent at startup to build its wait handle array */
    VddLog("AssignSwapChain: setting up hardware cursor...");
    proc->hMonitor = MonitorObject;
    proc->hCursorEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    proc->cursorBufSize = 256 * 256 * 4 * 2;  /* max 256x256, 2x for MASKED_COLOR double-height */
    proc->pCursorShapeBuffer = (PBYTE)malloc(proc->cursorBufSize);
    proc->lastShapeId = 0;

    if (proc->hCursorEvent && proc->pCursorShapeBuffer) {
        IDDCX_CURSOR_CAPS cursorCaps = {};
        IDARG_IN_SETUP_HWCURSOR hwCursorArgs = {};

        cursorCaps.Size = sizeof(cursorCaps);
        cursorCaps.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
        cursorCaps.AlphaCursorSupport = TRUE;
        cursorCaps.MaxX = 256;
        cursorCaps.MaxY = 256;

        hwCursorArgs.CursorInfo = cursorCaps;
        hwCursorArgs.hNewCursorDataAvailable = proc->hCursorEvent;

        VddLog("AssignSwapChain: calling IddCxMonitorSetupHardwareCursor...");
        NTSTATUS status = IddCxMonitorSetupHardwareCursor(MonitorObject, &hwCursorArgs);
        if (NT_SUCCESS(status)) {
            VddLog("AssignSwapChain: hardware cursor enabled (max 256x256)");
        } else {
            VddLog("AssignSwapChain: hardware cursor setup FAILED 0x%08X", status);
            CloseHandle(proc->hCursorEvent);
            proc->hCursorEvent = NULL;
        }
    } else {
        VddLog("AssignSwapChain: failed to allocate cursor resources (event=%p buf=%p)",
               (void*)proc->hCursorEvent, (void*)proc->pCursorShapeBuffer);
    }

    /* Start swap chain processing thread (cursor must be set up first) */
    VddLog("AssignSwapChain: creating swap chain thread...");
    proc->hThread = CreateThread(NULL, 0, VddSwapChainThread, proc, 0, NULL);
    if (!proc->hThread)
    {
        VddLog("AssignSwapChain: CreateThread FAILED err=%u", GetLastError());
        if (proc->pStagingTex) proc->pStagingTex->Release();
        /* Don't release pDevice/pDeviceContext — they're cached in ctx */
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        if (proc->hCursorEvent)
            CloseHandle(proc->hCursorEvent);
        free(proc->pCursorShapeBuffer);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: swap chain thread created (handle=%p)", (void*)proc->hThread);

    /* Start network listener thread for direct host connection */
    VddLog("AssignSwapChain: creating network thread...");
    proc->hNetworkThread = CreateThread(NULL, 0, VddNetworkThread, proc, 0, NULL);
    if (!proc->hNetworkThread)
    {
        VddLog("AssignSwapChain: network thread CreateThread FAILED err=%u", GetLastError());
        /* Non-fatal: swap chain still works, just no remote display */
    }
    else
    {
        VddLog("AssignSwapChain: network thread created (handle=%p)", (void*)proc->hNetworkThread);
    }

    ctx->pSwapProc = proc;

    VddLog("AssignSwapChain: COMPLETE — swap chain assigned, all threads started");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS VddMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    VDD_DEVICE_CONTEXT* ctx = WdfObjectGet_MonitorContextWrapper(MonitorObject)->pContext;
    AcquireSRWLockExclusive(&ctx->swapLock);
    if (ctx->hMonitor != MonitorObject || ctx->swapChanging ||
        InterlockedCompareExchange(&ctx->stopping, 0, 0)) {
        ReleaseSRWLockExclusive(&ctx->swapLock);
        return (NTSTATUS)0xC01E0012L;
    }
    ctx->swapChanging = TRUE;
    VDD_SWAP_PROC* previous = ctx->pSwapProc;
    ctx->pSwapProc = NULL;
    ReleaseSRWLockExclusive(&ctx->swapLock);
    VddDestroySwapProc(previous);
    AcquireSRWLockExclusive(&ctx->swapLock);
    NTSTATUS status = VddAssignSwapChain(MonitorObject, pInArgs);
    ctx->swapChanging = FALSE;
    ReleaseSRWLockExclusive(&ctx->swapLock);
    if (ctx->hRecoveryTimer) WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
    return status;
}

_Use_decl_annotations_
NTSTATUS VddMonitorUnassignSwapChain(
    IDDCX_MONITOR MonitorObject)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    AcquireSRWLockExclusive(&ctx->swapLock);
    VDD_SWAP_PROC* proc = ctx->pSwapProc;
    if (!proc || proc->hMonitor != MonitorObject) {
        ReleaseSRWLockExclusive(&ctx->swapLock);
        return STATUS_SUCCESS;
    }
    ctx->pSwapProc = NULL;
    ctx->swapChanging = TRUE;
    ReleaseSRWLockExclusive(&ctx->swapLock);
    VddDestroySwapProc(proc);
    AcquireSRWLockExclusive(&ctx->swapLock);
    ctx->swapChanging = FALSE;
    ReleaseSRWLockExclusive(&ctx->swapLock);

    VddLog("SwapChain unassigned — starting recovery timer (5s)");
    if (ctx->pCachedDevice && FAILED(ctx->pCachedDevice->GetDeviceRemovedReason()))
        VddPreferSoftwareRenderAdapter(ctx->hAdapter);

    /* Start recovery timer: if IddCx doesn't call AssignSwapChain within
       5 seconds, the timer callback departs and re-arrives the monitor. */
    if (ctx->hRecoveryTimer) {
        WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
    } else {
        VddLog("Recovery: no timer available (not created during DeviceAdd)");
    }

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Parse Monitor Description (EDID)                                 */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    ResolutionEntry modes[VDD_MAX_MODES];
    UINT count = VddReadModes(&pInArgs->MonitorDescription, modes);
    if (!count) return STATUS_INVALID_PARAMETER;
    pOutArgs->MonitorModeBufferOutputCount = count;
    pOutArgs->PreferredMonitorModeIdx = 0;
    if (!pInArgs->MonitorModeBufferInputCount) return STATUS_SUCCESS;
    if (pInArgs->MonitorModeBufferInputCount < count) return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < count; ++i) {
        IDDCX_MONITOR_MODE* mode = &pInArgs->pMonitorModes[i];
        ZeroMemory(mode, sizeof(*mode));
        mode->Size = sizeof(*mode);
        mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        VddCreateMonitorMode(&mode->MonitorVideoSignalInfo, 0, modes[i].width, modes[i].height);
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Get Default Modes                                                */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorGetDefaultModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    MonitorContextWrapper* mon = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    pOutArgs->DefaultMonitorModeBufferOutputCount = mon->modeCount;
    pOutArgs->PreferredMonitorModeIdx = 0;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->DefaultMonitorModeBufferInputCount < mon->modeCount)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < mon->modeCount; ++i) {
        IDDCX_MONITOR_MODE* mode = &pInArgs->pDefaultMonitorModes[i];
        ZeroMemory(mode, sizeof(*mode));
        mode->Size = sizeof(*mode);
        mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
        VddCreateMonitorMode(&mode->MonitorVideoSignalInfo, 0, mon->modes[i].width, mon->modes[i].height);
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Query Target Modes (WAS MISSING - required by IddCx)             */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorQueryTargetModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    ResolutionEntry modes[VDD_MAX_MODES];
    UINT count = VddTargetResolutions(MonitorObject, modes);
    pOutArgs->TargetModeBufferOutputCount = count;

    if (pInArgs->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->TargetModeBufferInputCount < count)
        return STATUS_BUFFER_TOO_SMALL;

    for (UINT i = 0; i < count; ++i) {
        IDDCX_TARGET_MODE* mode = &pInArgs->pTargetModes[i];
        ZeroMemory(mode, sizeof(*mode));
        mode->Size = sizeof(*mode);
        VddCreateMonitorMode(&mode->TargetVideoSignalInfo.targetVideoSignalInfo, 1, modes[i].width, modes[i].height);
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Commit Modes (v1 fallback)                                       */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterCommitModes(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_COMMITMODES* pInArgs)
{
    VDD_DEVICE_CONTEXT* ctx = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject)->pContext;
    for (UINT i = 0; i < pInArgs->PathCount; ++i) {
        if (ctx->gpuModes && (pInArgs->pPaths[i].Flags & IDDCX_PATH_FLAGS_CHANGED)) {
            InterlockedExchange(&ctx->reconfiguring, 1);
            if (ctx->hRecoveryTimer) WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
        }
    }
    VddLog("CommitModes (v1): called");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Commit Modes 2                                                */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterCommitModes2(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_COMMITMODES2* pInArgs)
{
    VDD_DEVICE_CONTEXT* ctx = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject)->pContext;

    VddLog("CommitModes2 (v2): called, PathCount=%u", pInArgs->PathCount);
    for (UINT i = 0; i < pInArgs->PathCount; i++) {
        UINT flags = (UINT)pInArgs->pPaths[i].Flags;
        BOOL active = (flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        BOOL changed = (flags & IDDCX_PATH_FLAGS_CHANGED) != 0;
        if (ctx->gpuModes && changed) {
            InterlockedExchange(&ctx->reconfiguring, 1);
            if (ctx->hRecoveryTimer) WdfTimerStart(ctx->hRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(5));
        }
        VddLog("CommitModes2: path[%u] monitor=%p flags=0x%X (active=%d changed=%d)",
               i, (void*)pInArgs->pPaths[i].MonitorObject, flags, active, changed);
    }

    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Parse Monitor Description 2                                   */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    ResolutionEntry modes[VDD_MAX_MODES];
    UINT count = VddReadModes(&pInArgs->MonitorDescription, modes);
    if (!count) return STATUS_INVALID_PARAMETER;
    pOutArgs->MonitorModeBufferOutputCount = count;
    pOutArgs->PreferredMonitorModeIdx = 0;
    if (!pInArgs->MonitorModeBufferInputCount) return STATUS_SUCCESS;
    if (pInArgs->MonitorModeBufferInputCount < count) return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < count; ++i) {
        IDDCX_MONITOR_MODE2* mode = &pInArgs->pMonitorModes[i];
        ZeroMemory(mode, sizeof(*mode));
        mode->Size = sizeof(*mode);
        mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
        mode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
        VddCreateMonitorMode(&mode->MonitorVideoSignalInfo, 0, modes[i].width, modes[i].height);
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Query Target Modes 2                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorQueryTargetModes2(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES2* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    ResolutionEntry modes[VDD_MAX_MODES];
    UINT count = VddTargetResolutions(MonitorObject, modes);
    pOutArgs->TargetModeBufferOutputCount = count;
    if (!pInArgs->TargetModeBufferInputCount) return STATUS_SUCCESS;
    if (pInArgs->TargetModeBufferInputCount < count) return STATUS_BUFFER_TOO_SMALL;
    for (UINT i = 0; i < count; ++i) {
        IDDCX_TARGET_MODE2* mode = &pInArgs->pTargetModes[i];
        ZeroMemory(mode, sizeof(*mode));
        mode->Size = sizeof(*mode);
        mode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
        VddCreateMonitorMode(&mode->TargetVideoSignalInfo.targetVideoSignalInfo, 1, modes[i].width, modes[i].height);
    }
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Adapter Query Target Info                                     */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddAdapterQueryTargetInfo(
    IDDCX_ADAPTER AdapterObject,
    IDARG_IN_QUERYTARGET_INFO* pInArgs,
    IDARG_OUT_QUERYTARGET_INFO* pOutArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    VddLog("QueryTargetInfo: called (ConnectorIndex=%u)", pInArgs->ConnectorIndex);

    pOutArgs->TargetCaps = (IDDCX_TARGET_CAPS)0;
    pOutArgs->DitheringSupport.Rgb = IDDCX_BITS_PER_COMPONENT_8;

    VddLog("QueryTargetInfo: returning caps=0, dithering=8bpc");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Set Default HDR Metadata (no-op, SDR only)                    */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorSetDefaultHdrMetadata(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("SetDefaultHdrMetadata: called (no-op, SDR only)");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx v2: Set Gamma Ramp (no-op)                                        */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorSetGammaRamp(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_SET_GAMMARAMP* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);
    VddLog("SetGammaRamp: called (no-op)");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  IddCx: Adapter Init Finished                                            */
/* ========================================================================= */

static void VddPreferHardwareRenderAdapter(IDDCX_ADAPTER adapterObject)
{
#pragma warning(suppress: 4127)
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) return;
    IDXGIFactory1* factory = NULL;
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK) return;
    for (UINT index = 0; ; ++index) {
        IDXGIAdapter1* adapter = NULL;
        if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc = {};
        HRESULT hr = adapter->GetDesc1(&desc);
        adapter->Release();
        if (hr != S_OK || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
        args.PreferredRenderAdapter = desc.AdapterLuid;
        IddCxAdapterSetRenderAdapter(adapterObject, &args);
        break;
    }
    factory->Release();
}

_Use_decl_annotations_
NTSTATUS VddAdapterInitFinished(
    IDDCX_ADAPTER AdapterObject,
    const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);

    VddLog("AdapterInitFinished: AdapterInitStatus=0x%08X", pInArgs->AdapterInitStatus);

    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        VddLog("AdapterInitFinished: adapter init succeeded, creating monitor...");
        VddFinishInit(pWrapper->pContext);
        VddPreferHardwareRenderAdapter(AdapterObject);
        NTSTATUS st = VddCreateMonitor(pWrapper->pContext);
        VddLog("AdapterInitFinished: VddCreateMonitor returned 0x%08X", st);
        return st;
    }

    VddLog("AdapterInitFinished: adapter init FAILED, not creating monitor");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: D0 Entry (device power-up)                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddDeviceD0Entry(
    WDFDEVICE Device,
    WDF_POWER_DEVICE_STATE PreviousState)
{
    VddLog("D0Entry: entered (Device=%p, PreviousState=%d)", (void*)Device, (int)PreviousState);

    /* Set GPU scheduling priority to realtime for this process (WUDFHost.exe) */
    {
        NTSTATUS st = D3DKMTSetProcessSchedulingPriorityClass(
            GetCurrentProcess(), D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME);
        VddLog("D0Entry: D3DKMTSetProcessSchedulingPriorityClass(REALTIME) = 0x%08X", st);
    }

    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(Device);

    if (pWrapper && pWrapper->pContext)
    {
        VddLog("D0Entry: have context, calling VddInitAdapter");
        VddInitAdapter(pWrapper->pContext);
    }
    else
    {
        VddLog("D0Entry: WARNING - no context (pWrapper=%p)", (void*)pWrapper);
    }

    VddLog("D0Entry: returning STATUS_SUCCESS");
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: EvtCleanupCallback                                                  */
/* ========================================================================= */

static void EvtCleanupCallback(WDFOBJECT Object)
{
    VddLog("EvtCleanupCallback: entered (Object=%p)", (void*)Object);
    IndirectDeviceContextWrapper* pWrapper =
        WdfObjectGet_IndirectDeviceContextWrapper(Object);
    if (pWrapper && pWrapper->pContext)
    {
        VddLog("EvtCleanupCallback: cleaning up context %p", (void*)pWrapper->pContext);
        VddContextCleanup(pWrapper->pContext);
        free(pWrapper->pContext);
        pWrapper->pContext = NULL;
    }
    else
    {
        VddLog("EvtCleanupCallback: no context to clean up");
    }
}

/* ========================================================================= */
/*  WDF: Device Add                                                          */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT pDeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    IDD_CX_CLIENT_CONFIG iddConfig;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE hDevice = NULL;
    IndirectDeviceContextWrapper* pWrapper;
    VDD_DEVICE_CONTEXT* ctx;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);

    VddLog("DeviceAdd: entered");

    /* PnP power callbacks FIRST */
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = VddDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpCallbacks);
    VddLog("DeviceAdd: PnP callbacks set");

    /* Configure IddCx callbacks */
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);

    iddConfig.EvtIddCxAdapterInitFinished               = VddAdapterInitFinished;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes  = VddMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain             = VddMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain           = VddMonitorUnassignSwapChain;

    /* Use v2 callbacks when compiled with IddCx 1.10+ */
    if (IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo)) {
        VddLog("DeviceAdd: registering v2 callbacks (IddCx 1.10+)");
        iddConfig.EvtIddCxAdapterQueryTargetInfo            = VddAdapterQueryTargetInfo;
        iddConfig.EvtIddCxMonitorSetDefaultHdrMetaData      = VddMonitorSetDefaultHdrMetadata;
        iddConfig.EvtIddCxParseMonitorDescription2          = VddParseMonitorDescription2;
        iddConfig.EvtIddCxMonitorQueryTargetModes2          = VddMonitorQueryTargetModes2;
        iddConfig.EvtIddCxAdapterCommitModes2               = VddAdapterCommitModes2;
        iddConfig.EvtIddCxMonitorSetGammaRamp               = VddMonitorSetGammaRamp;
    } else {
        VddLog("DeviceAdd: registering v1 callbacks");
        iddConfig.EvtIddCxParseMonitorDescription           = VddParseMonitorDescription;
        iddConfig.EvtIddCxMonitorQueryTargetModes            = VddMonitorQueryTargetModes;
        iddConfig.EvtIddCxAdapterCommitModes                = VddAdapterCommitModes;
    }

    st = IddCxDeviceInitConfig(pDeviceInit, &iddConfig);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: IddCxDeviceInitConfig FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: IddCxDeviceInitConfig succeeded");

    /* Create WDFDEVICE with IndirectDeviceContextWrapper + EvtCleanupCallback */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);
    attr.EvtCleanupCallback = EvtCleanupCallback;

    st = WdfDeviceCreate(&pDeviceInit, &attr, &hDevice);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: WdfDeviceCreate FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: WdfDeviceCreate succeeded (hDevice=%p)", (void*)hDevice);

    st = IddCxDeviceInitialize(hDevice);
    if (!NT_SUCCESS(st))
    {
        VddLog("DeviceAdd: IddCxDeviceInitialize FAILED st=0x%08X", st);
        return st;
    }
    VddLog("DeviceAdd: IddCxDeviceInitialize succeeded");

    /* Heap-allocate device context */
    ctx = (VDD_DEVICE_CONTEXT*)malloc(sizeof(VDD_DEVICE_CONTEXT));
    if (!ctx)
    {
        VddLog("DeviceAdd: malloc for VDD_DEVICE_CONTEXT FAILED");
        return STATUS_NO_MEMORY;
    }

    VddContextInit(ctx, hDevice);

    pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(hDevice);
    pWrapper->pContext = ctx;

    WDF_WORKITEM_CONFIG workConfig;
    WDF_WORKITEM_CONFIG_INIT(&workConfig, VddDisplayWork);
    workConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES workAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&workAttrs);
    workAttrs.ParentObject = hDevice;
    st = WdfWorkItemCreate(&workConfig, &workAttrs, &ctx->hDisplayWork);
    if (!NT_SUCCESS(st)) VddLog("Display: work item creation failed 0x%08X", st);

    /* Create WDF timer for monitor recovery — fires on a proper WDF thread
       so IddCx APIs (MonitorDeparture, MonitorCreate, MonitorArrival) are safe. */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;
        WDFTIMER hTimer = NULL;

        WDF_TIMER_CONFIG_INIT(&timerConfig, VddRecoveryTimerCallback);
        timerConfig.AutomaticSerialization = FALSE;

        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&timerAttrs, RecoveryTimerContext);
        timerAttrs.ParentObject = hDevice;

        st = WdfTimerCreate(&timerConfig, &timerAttrs, &hTimer);
        if (NT_SUCCESS(st)) {
            RecoveryTimerContext* timerCtx = WdfObjectGet_RecoveryTimerContext(hTimer);
            timerCtx->pContext = ctx;
            ctx->hRecoveryTimer = hTimer;
            VddLog("DeviceAdd: recovery timer created");
        } else {
            VddLog("DeviceAdd: WdfTimerCreate FAILED st=0x%08X (recovery disabled)", st);
        }
    }

    VddLog("DeviceAdd: complete, context=%p", (void*)ctx);
    return STATUS_SUCCESS;
}

/* ========================================================================= */
/*  WDF: Driver Unload                                                       */
/* ========================================================================= */

_Use_decl_annotations_
VOID VddDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    VddLog("DriverUnload: called");
}

/* ========================================================================= */
/*  DllMain                                                                  */
/* ========================================================================= */

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    if (dwReason == DLL_PROCESS_ATTACH) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        VddLog("DllMain: DLL_PROCESS_ATTACH (WSAStartup done)");
    } else if (dwReason == DLL_PROCESS_DETACH) {
        VddLog("DllMain: DLL_PROCESS_DETACH");
        WSACleanup();
    }
    return TRUE;
}

/* ========================================================================= */
/*  DriverEntry                                                              */
/* ========================================================================= */

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT pDriverObject,
    PUNICODE_STRING pRegistryPath)
{
    VddLog("DriverEntry: entered (pDriverObject=%p)", pDriverObject);

    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, VddDeviceAdd);
    config.EvtDriverUnload = VddDriverUnload;

    NTSTATUS st = WdfDriverCreate(pDriverObject, pRegistryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(st))
    {
        VddLog("DriverEntry: WdfDriverCreate FAILED st=0x%08X", st);
    }
    else
    {
        VddLog("DriverEntry: WdfDriverCreate succeeded");
    }

    return st;
}
