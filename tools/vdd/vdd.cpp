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

static const BYTE* VddGetEdid()
{
    static BYTE edid[128];
    static BOOL computed = FALSE;

    if (!computed)
    {
        memcpy(edid, VDD_EDID, 127);
        BYTE sum = 0;
        for (int i = 0; i < 127; i++)
            sum += edid[i];
        edid[127] = (BYTE)(256 - sum);
        computed = TRUE;
    }
    return edid;
}

/* ========================================================================= */
/*  Monitor mode helper                                                      */
/* ========================================================================= */

static void VddCreateMonitorMode(DISPLAYCONFIG_VIDEO_SIGNAL_INFO* sig, UINT vSyncDivider)
{
    /* totalSize == activeSize, pixelRate = refresh * w * h.
       IddCx validates these relationships; using actual blanking values causes
       STATUS_INVALID_PARAMETER from IddCxMonitorArrival. */
    sig->totalSize.cx                           = VDD_WIDTH;
    sig->totalSize.cy                           = VDD_HEIGHT;
    sig->activeSize.cx                          = VDD_WIDTH;
    sig->activeSize.cy                          = VDD_HEIGHT;
    sig->AdditionalSignalInfo.vSyncFreqDivider  = vSyncDivider;
    sig->AdditionalSignalInfo.videoStandard     = 255;
    sig->vSyncFreq.Numerator                    = 60;
    sig->vSyncFreq.Denominator                  = 1;
    sig->hSyncFreq.Numerator                    = 60 * VDD_HEIGHT;
    sig->hSyncFreq.Denominator                  = 1;
    sig->scanLineOrdering                       = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    sig->pixelRate                               = (UINT64)60 * VDD_WIDTH * VDD_HEIGHT;
}

/* ========================================================================= */
/*  HvSocket / networking definitions                                        */
/* ========================================================================= */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* HV_GUID_WILDCARD — bind to accept from any partition */
static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Frame channel service GUID: {A5B0CAFE-0002-4000-8000-000000000001} */
static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Send exactly len bytes. Returns 0 on success, -1 on failure. */
static int VddSendAll(SOCKET s, const char* data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ========================================================================= */
/*  Network listener thread (accepts host connections over HvSocket)         */
/* ========================================================================= */

static DWORD WINAPI VddNetworkThread(LPVOID lpParameter)
{
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)lpParameter;
    SOCKET listen_s = INVALID_SOCKET;
    SOCKADDR_HV addr;

    VddLog("Network: starting");

    while (!proc->bStopNetwork) {
        /* (Re)create listen socket */
        listen_s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        if (listen_s == INVALID_SOCKET) {
            VddLog("Network: socket failed (%d), retrying in 3s", WSAGetLastError());
            for (int w = 0; w < 3000 && !proc->bStopNetwork; w += 500)
                Sleep(500);
            continue;
        }

        memset(&addr, 0, sizeof(addr));
        addr.Family = AF_HYPERV;
        addr.VmId = HV_GUID_WILDCARD;
        addr.ServiceId = FRAME_SERVICE_GUID;

        if (bind(listen_s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            VddLog("Network: bind failed (%d), retrying in 3s", WSAGetLastError());
            closesocket(listen_s);
            listen_s = INVALID_SOCKET;
            for (int w = 0; w < 3000 && !proc->bStopNetwork; w += 500)
                Sleep(500);
            continue;
        }

        if (listen(listen_s, 1) != 0) {
            VddLog("Network: listen failed (%d), retrying in 3s", WSAGetLastError());
            closesocket(listen_s);
            listen_s = INVALID_SOCKET;
            for (int w = 0; w < 3000 && !proc->bStopNetwork; w += 500)
                Sleep(500);
            continue;
        }

        VddLog("Network: listening for host connections");

        /* Accept loop — breaks out on error to retry socket creation */
        while (!proc->bStopNetwork) {
        fd_set fds;
        struct timeval tv;
        SOCKET new_s;

        FD_ZERO(&fds);
        FD_SET(listen_s, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(0, &fds, NULL, NULL, &tv) <= 0)
            continue;

        new_s = accept(listen_s, NULL, NULL);
        if (new_s == INVALID_SOCKET)
            continue;

        VddLog("Network: host connected");

        /* If a previous pending socket wasn't picked up yet, close it */
        {
            SOCKET old = (SOCKET)InterlockedExchange64(
                (volatile LONG64*)&proc->hPendingSocket, (LONG64)new_s);
            if (old != INVALID_SOCKET) {
                shutdown(old, SD_BOTH);
                closesocket(old);
                VddLog("Network: replaced unclaimed pending connection");
            }
        }
        } /* end accept loop */

        /* Clean up listen socket before retrying */
        closesocket(listen_s);
        listen_s = INVALID_SOCKET;
    } /* end retry loop */

    /* Close any pending socket */
    {
        SOCKET pending = (SOCKET)InterlockedExchange64(
            (volatile LONG64*)&proc->hPendingSocket, (LONG64)INVALID_SOCKET);
        if (pending != INVALID_SOCKET) {
            shutdown(pending, SD_BOTH);
            closesocket(pending);
        }
    }

    if (listen_s != INVALID_SOCKET)
        closesocket(listen_s);

    VddLog("Network: stopped");
    return 0;
}

/* ========================================================================= */
/*  D3D11 device — cached in VDD_DEVICE_CONTEXT across swap chain transitions
 *  to avoid destroying the D3D device during IddCx's WdfObjectDelete
 *  teardown (which crashes WUDFHost.exe).                                    */
/* ========================================================================= */

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
    SOCKET s;

    if (!proc->hCursorEvent || !proc->pCursorShapeBuffer || !proc->hMonitor)
        return;

    s = proc->hClientSocket;
    if (s == INVALID_SOCKET)
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

    /* Send cursor header */
    if (VddSendAll(s, (const char*)&chdr, sizeof(chdr)) != 0) {
        VddLog("Cursor: send header failed, disconnecting");
        shutdown(s, SD_BOTH);
        closesocket(s);
        proc->hClientSocket = INVALID_SOCKET;
        return;
    }

    /* Send shape bitmap if updated */
    if (chdr.shape_data_size > 0) {
        if (VddSendAll(s, (const char*)proc->pCursorShapeBuffer, (int)chdr.shape_data_size) != 0) {
            VddLog("Cursor: send shape data failed, disconnecting");
            shutdown(s, SD_BOTH);
            closesocket(s);
            proc->hClientSocket = INVALID_SOCKET;
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

    /* Track staging texture row pitch (set on first capture) and whether
       we have a valid frame in the staging texture for resend on idle. */
    BOOL bHasFrame = FALSE;
    BOOL bSentFullFrame = FALSE;   /* TRUE after first full frame sent to current client */
    UINT cachedRowPitch = VDD_STRIDE;


    /* Main frame acquisition loop */
    for (;;)
    {
        IDXGIResource* pSurface = NULL;

        /* Check for pending new connection from network thread */
        {
            SOCKET pending = (SOCKET)InterlockedExchange64(
                (volatile LONG64*)&proc->hPendingSocket, (LONG64)INVALID_SOCKET);
            if (pending != INVALID_SOCKET) {
                /* Close old connection if any */
                if (proc->hClientSocket != INVALID_SOCKET) {
                    shutdown(proc->hClientSocket, SD_BOTH);
                    closesocket(proc->hClientSocket);
                    VddLog("Frame: closed previous client connection");
                }
                proc->hClientSocket = pending;
                proc->frameSeq = 0;
                bSentFullFrame = FALSE;
                VddLog("Frame: new client connection active");
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
            DWORD waitResult = WaitForMultipleObjects(waitCount, waitHandles, FALSE, 100);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                break;  /* Terminate */
            }
            if (waitResult == WAIT_OBJECT_0 + 2) {
                /* Cursor event — query and send cursor update */
                VddSendCursorUpdate(proc);
                continue;
            }
            if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_TIMEOUT) {
                VddLog("SwapChainRunCore: unexpected wait result 0x%lX, exiting for reassignment", waitResult);
                break;
            }

            /* On timeout with a connected client and a cached frame, resend
               the staging texture contents to keep the connection alive. */
            if (waitResult == WAIT_TIMEOUT && bHasFrame &&
                proc->hClientSocket != INVALID_SOCKET && proc->pStagingTex)
            {
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                if (SUCCEEDED(hr))
                {
                    VDD_WIRE_FRAME_HEADER whdr;
                    UINT32 data_size = VDD_STRIDE * VDD_HEIGHT;
                    BOOL send_ok = TRUE;
                    SOCKET s = proc->hClientSocket;

                    proc->frameSeq++;
                    whdr.magic            = VDD_FRAME_MAGIC;
                    whdr.width            = VDD_WIDTH;
                    whdr.height           = VDD_HEIGHT;
                    whdr.stride           = VDD_STRIDE;
                    whdr.frame_seq        = proc->frameSeq;
                    whdr.dirty_rect_count = 0;

                    if (VddSendAll(s, (const char*)&whdr, sizeof(whdr)) != 0 ||
                        VddSendAll(s, (const char*)&data_size, sizeof(data_size)) != 0) {
                        send_ok = FALSE;
                    }
                    if (send_ok) {
                        for (UINT row = 0; row < VDD_HEIGHT; row++) {
                            if (VddSendAll(s, (const char*)((const BYTE*)mapped.pData + row * mapped.RowPitch),
                                           VDD_STRIDE) != 0) {
                                send_ok = FALSE;
                                break;
                            }
                        }
                    }

                    proc->pDeviceContext->Unmap(proc->pStagingTex, 0);

                    if (!send_ok) {
                        VddLog("Frame: resend failed, disconnecting (seq=%llu)", proc->frameSeq);
                        shutdown(s, SD_BOTH);
                        closesocket(s);
                        proc->hClientSocket = INVALID_SOCKET;
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
            if (pSurface && proc->pStagingTex)
            {
                ID3D11Texture2D* pTexture = nullptr;
                hr = pSurface->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture);
                if (SUCCEEDED(hr) && pTexture)
                {
                    proc->pDeviceContext->CopyResource(proc->pStagingTex, pTexture);

                    D3D11_MAPPED_SUBRESOURCE mapped = {};
                    hr = proc->pDeviceContext->Map(proc->pStagingTex, 0, D3D11_MAP_READ, 0, &mapped);
                    if (SUCCEEDED(hr))
                    {
                        bHasFrame = TRUE;
                        cachedRowPitch = mapped.RowPitch;

                        if (proc->hClientSocket != INVALID_SOCKET)
                        {
                            RECT dirtyRects[VDD_MAX_DIRTY_RECTS];
                            UINT32 rectCount = 0;
                            BOOL sendFull = !bSentFullFrame;  /* first frame must be full */

                            /* Query dirty rects from IddCx */
                            if (!sendFull) {
                                IDARG_IN_GETDIRTYRECTS drIn = {};
                                IDARG_OUT_GETDIRTYRECTS drOut = {};
                                drIn.DirtyRectInCount = VDD_MAX_DIRTY_RECTS;
                                drIn.pDirtyRects = dirtyRects;
                                HRESULT drHr = IddCxSwapChainGetDirtyRects(proc->hSwapChain, &drIn, &drOut);
                                if (SUCCEEDED(drHr) && drOut.DirtyRectOutCount > 0) {
                                    rectCount = drOut.DirtyRectOutCount;
                                } else if (SUCCEEDED(drHr) && drOut.DirtyRectOutCount == 0) {
                                    /* No dirty rects — frame is identical, skip send */
                                    goto skip_send;
                                } else {
                                    /* Can't get dirty rects — send full frame */
                                    sendFull = TRUE;
                                }
                            }

                            VDD_WIRE_FRAME_HEADER whdr;
                            BOOL send_ok = TRUE;
                            SOCKET s = proc->hClientSocket;

                            proc->frameSeq++;
                            whdr.magic            = VDD_FRAME_MAGIC;
                            whdr.width            = VDD_WIDTH;
                            whdr.height           = VDD_HEIGHT;
                            whdr.stride           = VDD_STRIDE;
                            whdr.frame_seq        = proc->frameSeq;

                            if (sendFull) {
                                /* Full frame */
                                UINT32 data_size = VDD_STRIDE * VDD_HEIGHT;
                                whdr.dirty_rect_count = 0;

                                if (VddSendAll(s, (const char*)&whdr, sizeof(whdr)) != 0 ||
                                    VddSendAll(s, (const char*)&data_size, sizeof(data_size)) != 0)
                                    send_ok = FALSE;

                                if (send_ok) {
                                    for (UINT row = 0; row < VDD_HEIGHT; row++) {
                                        if (VddSendAll(s, (const char*)((const BYTE*)mapped.pData + row * mapped.RowPitch),
                                                       VDD_STRIDE) != 0) {
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
                                    if (dirtyRects[i].right  > (LONG)VDD_WIDTH)  dirtyRects[i].right  = VDD_WIDTH;
                                    if (dirtyRects[i].bottom > (LONG)VDD_HEIGHT) dirtyRects[i].bottom = VDD_HEIGHT;
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
                                shutdown(s, SD_BOTH);
                                closesocket(s);
                                proc->hClientSocket = INVALID_SOCKET;
                                bSentFullFrame = FALSE;
                            } else if (proc->frameSeq == 1) {
                                VddLog("Frame: first frame sent (%ux%u, %u bytes)",
                                       VDD_WIDTH, VDD_HEIGHT, VDD_STRIDE * VDD_HEIGHT);
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
        WaitForSingleObject(proc->hThread, 5000);
        VddLog("DestroySwapProc: swap chain thread exited");
        CloseHandle(proc->hThread);
    }

    /* Wait for network thread to exit */
    if (proc->hNetworkThread)
    {
        VddLog("DestroySwapProc: waiting for network thread...");
        WaitForSingleObject(proc->hNetworkThread, 5000);
        VddLog("DestroySwapProc: network thread exited");
        CloseHandle(proc->hNetworkThread);
    }

    /* Close any remaining client socket */
    VddLog("DestroySwapProc: closing client socket (sock=%lld)", (long long)proc->hClientSocket);
    if (proc->hClientSocket != INVALID_SOCKET) {
        shutdown(proc->hClientSocket, SD_BOTH);
        closesocket(proc->hClientSocket);
    }
    VddLog("DestroySwapProc: closing pending socket");
    {
        SOCKET pending = (SOCKET)InterlockedExchange64(
            (volatile LONG64*)&proc->hPendingSocket, (LONG64)INVALID_SOCKET);
        if (pending != INVALID_SOCKET) {
            shutdown(pending, SD_BOTH);
            closesocket(pending);
        }
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

    /* Pre-populate the single 1920x1080@60Hz mode */
    VddCreateMonitorMode(&ctx->modes[0], 0);  /* vSyncDivider=0 for monitor modes */
    ctx->modeCount = 1;
}

static void VddContextCleanup(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("Context cleanup starting");

    /* Stop recovery timer (wait for any in-progress callback to finish) */
    if (ctx->hRecoveryTimer) {
        WdfTimerStop(ctx->hRecoveryTimer, TRUE);
    }

    if (ctx->pSwapProc)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }

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

    /* Set FP16 processing flag when supported */
#pragma warning(suppress: 4127)
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
        adapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
        VddLog("InitAdapter: FP16 flag set");
    } else {
        VddLog("InitAdapter: FP16 not available");
    }

    adapterCaps.MaxMonitorsSupported = 1;

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

static NTSTATUS VddCreateMonitor(VDD_DEVICE_CONTEXT* ctx)
{
    VddLog("CreateMonitor: entered (ctx=%p, adapter=%p)", (void*)ctx, (void*)ctx->hAdapter);

    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    monitorInfo.ConnectorIndex = 0;

    monitorInfo.MonitorDescription.Size     = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = 128;
    monitorInfo.MonitorDescription.pData    = (void*)VddGetEdid();

    VddLog("CreateMonitor: EDID checksum byte=0x%02X, IDDCX_MONITOR_INFO.Size=%u",
           ((const BYTE*)monitorInfo.MonitorDescription.pData)[127],
           (unsigned)monitorInfo.Size);

    /* Generate a container GUID */
    HRESULT hrGuid = CoCreateGuid(&monitorInfo.MonitorContainerId);
    VddLog("CreateMonitor: CoCreateGuid hr=0x%08X", hrGuid);

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

    ctx->hMonitor = createOut.MonitorObject;

    /* Store context on the monitor object */
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject);
    pMonCtx->pContext = ctx;

    VddLog("CreateMonitor: calling IddCxMonitorArrival...");
    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    st = IddCxMonitorArrival(ctx->hMonitor, &arrivalOut);
    if (!NT_SUCCESS(st))
    {
        VddLog("CreateMonitor: IddCxMonitorArrival FAILED st=0x%08X", st);
        return st;
    }

    VddLog("CreateMonitor: monitor created and arrived successfully");

    return STATUS_SUCCESS;
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

    /* If a swap chain was assigned in the meantime, nothing to do */
    if (ctx->pSwapProc) {
        VddLog("Recovery: swap chain already reassigned, skipping");
        return;
    }

    /* No swap chain arrived yet.  This is normal during session transitions
       (logoff → login screen) — DWM may take longer than 5s to start
       compositing.  Do NOT call WdfDeviceSetFailed here; that permanently
       bricks the device and requires a full PnP restart.  The monitor stays
       arrived and IddCx will call AssignSwapChain when DWM is ready. */
    VddLog("Recovery: no AssignSwapChain after timeout — waiting (device remains available)");
}

/* ========================================================================= */
/*  IddCx: Assign / Unassign Swap Chain                                     */
/* ========================================================================= */

_Use_decl_annotations_
NTSTATUS VddMonitorAssignSwapChain(
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

    /* Destroy existing swap proc if any */
    if (ctx->pSwapProc)
    {
        VddLog("AssignSwapChain: destroying existing swap proc");
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
        VddLog("AssignSwapChain: existing swap proc destroyed");
    }

    VddLog("AssignSwapChain: allocating new swap proc (%u bytes)", (unsigned)sizeof(VDD_SWAP_PROC));
    VDD_SWAP_PROC* proc = (VDD_SWAP_PROC*)malloc(sizeof(VDD_SWAP_PROC));
    if (!proc)
    {
        VddLog("AssignSwapChain: malloc FAILED");
        return STATUS_NO_MEMORY;
    }
    memset(proc, 0, sizeof(*proc));
    VddLog("AssignSwapChain: swap proc allocated at %p", (void*)proc);

    proc->hSwapChain      = pInArgs->hSwapChain;
    proc->hAvailableEvent = pInArgs->hNextSurfaceAvailable;
    proc->hTerminateEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    VddLog("AssignSwapChain: hSwapChain=%p hAvailableEvent=%p hTerminateEvent=%p",
           (void*)proc->hSwapChain, (void*)proc->hAvailableEvent, (void*)proc->hTerminateEvent);

    VddLog("AssignSwapChain: getting D3D11 device (LUID=%08X:%08X)...",
           pInArgs->RenderAdapterLuid.HighPart, pInArgs->RenderAdapterLuid.LowPart);

    /* Get or reuse cached D3D11 device (cached in ctx to survive swap chain
       transitions) */
    HRESULT hr = VddGetOrCreateD3DDevice(ctx, pInArgs->RenderAdapterLuid,
                                          &proc->pDevice, &proc->pDeviceContext);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: D3D11 device creation FAILED hr=0x%08X", hr);
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: D3D11 device ready (cached=%p)", (void*)proc->pDevice);

    /* Initialize network state */
    VddLog("AssignSwapChain: initializing network state");
    proc->hClientSocket  = INVALID_SOCKET;
    proc->hPendingSocket = INVALID_SOCKET;
    proc->bStopNetwork   = FALSE;
    proc->frameSeq       = 0;

    /* Create staging texture for CPU readback */
    VddLog("AssignSwapChain: creating staging texture (%ux%u)...", VDD_WIDTH, VDD_HEIGHT);
    hr = VddCreateStagingTexture(proc, VDD_WIDTH, VDD_HEIGHT);
    if (FAILED(hr))
    {
        VddLog("AssignSwapChain: staging texture creation FAILED hr=0x%08X", hr);
        /* Don't release pDevice/pDeviceContext — they're cached in ctx */
        if (proc->hTerminateEvent)
            CloseHandle(proc->hTerminateEvent);
        free(proc);
        return (NTSTATUS)0xC01E0012L; /* STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN */
    }
    VddLog("AssignSwapChain: staging texture created");

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
        proc->pStagingTex->Release();
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
NTSTATUS VddMonitorUnassignSwapChain(
    IDDCX_MONITOR MonitorObject)
{
    MonitorContextWrapper* pMonCtx = WdfObjectGet_MonitorContextWrapper(MonitorObject);
    VDD_DEVICE_CONTEXT* ctx = pMonCtx->pContext;

    if (ctx->pSwapProc)
    {
        VddDestroySwapProc(ctx->pSwapProc);
        ctx->pSwapProc = nullptr;
    }

    VddLog("SwapChain unassigned — starting recovery timer (5s)");

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
    VddLog("ParseMonitorDescription: InputCount=%u", pInArgs->MonitorModeBufferInputCount);

    pOutArgs->MonitorModeBufferOutputCount = 1;

    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->MonitorModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_MONITOR_MODE* pMode = pInArgs->pMonitorModes;
    pMode->Size = sizeof(IDDCX_MONITOR_MODE);
    pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, 0); /* vSyncDivider=0 for monitor modes */

    pOutArgs->PreferredMonitorModeIdx = 0;
    VddLog("ParseMonitorDescription: returning 1 mode (1920x1080@60)");
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
    UNREFERENCED_PARAMETER(MonitorObject);

    VddLog("GetDefaultModes: InputCount=%u", pInArgs->DefaultMonitorModeBufferInputCount);

    pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
    pOutArgs->PreferredMonitorModeIdx = 0;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->DefaultMonitorModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_MONITOR_MODE* pMode = pInArgs->pDefaultMonitorModes;
    pMode->Size = sizeof(IDDCX_MONITOR_MODE);
    pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, 0); /* vSyncDivider=0 for monitor modes */

    VddLog("GetDefaultModes: returning 1 default mode");
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
    UNREFERENCED_PARAMETER(MonitorObject);

    VddLog("QueryTargetModes: InputCount=%u", pInArgs->TargetModeBufferInputCount);

    pOutArgs->TargetModeBufferOutputCount = 1;

    if (pInArgs->TargetModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->TargetModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_TARGET_MODE* pMode = pInArgs->pTargetModes;
    pMode->Size = sizeof(IDDCX_TARGET_MODE);
    VddCreateMonitorMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, 1); /* vSyncDivider=1 for TARGET modes */

    VddLog("QueryTargetModes: returning 1 target mode");
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
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);
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
    UNREFERENCED_PARAMETER(AdapterObject);

    VddLog("CommitModes2 (v2): called, PathCount=%u", pInArgs->PathCount);
    for (UINT i = 0; i < pInArgs->PathCount; i++) {
        UINT flags = (UINT)pInArgs->pPaths[i].Flags;
        BOOL active = (flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        BOOL changed = (flags & IDDCX_PATH_FLAGS_CHANGED) != 0;
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
    VddLog("ParseMonitorDescription2: InputCount=%u", pInArgs->MonitorModeBufferInputCount);

    pOutArgs->MonitorModeBufferOutputCount = 1;

    if (pInArgs->MonitorModeBufferInputCount == 0)
        return STATUS_SUCCESS;

    if (pInArgs->MonitorModeBufferInputCount < 1)
        return STATUS_BUFFER_TOO_SMALL;

    IDDCX_MONITOR_MODE2* pMode = pInArgs->pMonitorModes;
    pMode->Size = sizeof(IDDCX_MONITOR_MODE2);
    pMode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    VddCreateMonitorMode(&pMode->MonitorVideoSignalInfo, 0);
    pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;

    pOutArgs->PreferredMonitorModeIdx = 0;
    VddLog("ParseMonitorDescription2: returning 1 mode (8bpc)");
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
    UNREFERENCED_PARAMETER(MonitorObject);

    VddLog("QueryTargetModes2: InputCount=%u", pInArgs->TargetModeBufferInputCount);

    pOutArgs->TargetModeBufferOutputCount = 1;

    if (pInArgs->TargetModeBufferInputCount >= 1)
    {
        IDDCX_TARGET_MODE2* pMode = pInArgs->pTargetModes;
        pMode->Size = sizeof(IDDCX_TARGET_MODE2);
        pMode->BitsPerComponent.Rgb = IDDCX_BITS_PER_COMPONENT_8;
        VddCreateMonitorMode(&pMode->TargetVideoSignalInfo.targetVideoSignalInfo, 1);
    }

    VddLog("QueryTargetModes2: returning 1 target mode (8bpc)");
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
