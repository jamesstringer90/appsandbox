/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-004 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-008 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-012 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-013 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-016 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-017 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-019 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-020 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-021 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-009::REQ-021 */
/*
 * AppSandboxVGD_XUSB — First-party UMDF2 Virtual Xbox 360 XUSB Function Driver.
 *
 * Runs in user mode (WUDFHost.exe) under Class = System with the "xinputhid"
 * UpperFilter registry tripwire and publishes GUID_DEVINTERFACE_XUSB so both
 * xinput1_4.dll and Windows.Gaming.Input discover each plugged Controller Slot
 * (0..3) and route dual-motor rumble back via shared memory.
 *
 * Uses dedicated Global\AppSandboxGamepadXusbIn<N> doorbell and per-file-object
 * cursors over the 16-entry VgdShmBlock ring so short button taps are never
 * dropped or stolen across concurrent clients.
 */

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wdf.h>
#include <string.h>

#include "../vgd_shm.h"
#include "../vgd_xusb_codec.h"

static const GUID GUID_DEVINTERFACE_XUSB = {
    0xEC87F1E3u, 0xC13Bu, 0x4100u,
    { 0xB5u, 0xF7u, 0x8Bu, 0x84u, 0xD5u, 0x42u, 0x60u, 0xCBu }
};

typedef struct _XUSB_HANDLE_CTX {
    uint32_t Cursor;
    uint16_t LastButtons;
    BOOLEAN  Initialized;
} XUSB_HANDLE_CTX, *PXUSB_HANDLE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_HANDLE_CTX, GetXusbHandleCtx)

typedef struct _XUSB_DEV_CTX {
    uint32_t         Slot;
    WCHAR            ShmName[64];
    WCHAR            InEvtName[64];
    WCHAR            OutEvtName[64];
    HANDLE           ShmHandle;
    VgdShmBlock     *Shm;
    HANDLE           InEvt;
    HANDLE           OutEvt;
    HANDLE           StopEvt;
    HANDLE           DoorbellThread;
    WDFQUEUE         WaitQueue;
    CRITICAL_SECTION Lock;
    BOOLEAN          LockInit;
    GamepadState     LastState;
    uint32_t         LastGen;
    uint32_t         FallbackCursor;
    uint16_t         FallbackButtons;
    BOOLEAN          FallbackInit;
} XUSB_DEV_CTX, *PXUSB_DEV_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(XUSB_DEV_CTX, GetXusbCtx)

DRIVER_INITIALIZE                     DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD             XusbEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL    XusbEvtIoDeviceControl;
EVT_WDF_DEVICE_SELF_MANAGED_IO_FLUSH  XusbEvtSelfManagedIoFlush;
EVT_WDF_OBJECT_CONTEXT_CLEANUP        XusbEvtDeviceCleanup;

static void XusbCloseShmLocked(PXUSB_DEV_CTX ctx)
{
    if (ctx->Shm != NULL) {
        UnmapViewOfFile(ctx->Shm);
        ctx->Shm = NULL;
    }
    if (ctx->ShmHandle != NULL) {
        CloseHandle(ctx->ShmHandle);
        ctx->ShmHandle = NULL;
    }
    if (ctx->InEvt != NULL) {
        CloseHandle(ctx->InEvt);
        ctx->InEvt = NULL;
    }
    if (ctx->OutEvt != NULL) {
        CloseHandle(ctx->OutEvt);
        ctx->OutEvt = NULL;
    }
}

static void XusbEnsureShmLocked(PXUSB_DEV_CTX ctx)
{
    if (ctx->Shm == NULL) {
        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, ctx->ShmName);
        if (hMap != NULL) {
            VgdShmBlock *view = (VgdShmBlock *)MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(VgdShmBlock));
            if (view != NULL && vgd_shm_is_valid(view)) {
                ctx->ShmHandle = hMap;
                ctx->Shm = view;
            } else {
                if (view != NULL) UnmapViewOfFile(view);
                CloseHandle(hMap);
            }
        }
    }
    if (ctx->InEvt == NULL) {
        ctx->InEvt = OpenEventW(SYNCHRONIZE, FALSE, ctx->InEvtName);
    }
    if (ctx->OutEvt == NULL) {
        ctx->OutEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, ctx->OutEvtName);
    }
}

static uint32_t XusbQuerySlot(WDFDEVICE device)
{
    WDFMEMORY mem = NULL;
    NTSTATUS st = WdfDeviceAllocAndQueryProperty(
        device, DevicePropertyHardwareID, NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, &mem);
    if (!NT_SUCCESS(st) || mem == NULL) {
        return 0u;
    }
    {
        size_t bytes = 0;
        const WCHAR *wbuf = (const WCHAR *)WdfMemoryGetBuffer(mem, &bytes);
        size_t wchars = bytes / sizeof(WCHAR);
        size_t pos = 0;
        while (pos < wchars && wbuf[pos] != L'\0') {
            char ascii[128];
            size_t len = 0;
            while (pos < wchars && wbuf[pos] != L'\0') {
                if (len + 1u < sizeof(ascii)) {
                    WCHAR ch = wbuf[pos];
                    ascii[len++] = (ch < 128) ? (char)ch : '?';
                }
                pos++;
            }
            ascii[len] = '\0';
            if (pos < wchars) pos++;
            {
                int parsed = vgd_parse_slot_from_hwid(ascii);
                if (parsed >= 0) {
                    WdfObjectDelete(mem);
                    return (uint32_t)parsed;
                }
            }
        }
    }
    WdfObjectDelete(mem);
    return 0u;
}

static void XusbReadCurrentStateLocked(PXUSB_DEV_CTX ctx, GamepadState *out_state, uint32_t *out_gen)
{
    GamepadState snap;
    uint32_t gen = 0;
    XusbEnsureShmLocked(ctx);
    if (ctx->Shm != NULL && vgd_shm_read_state(ctx->Shm, &snap, &gen) == 0) {
        ctx->LastState = snap;
        ctx->LastGen = gen;
        *out_state = snap;
        *out_gen = gen;
    } else {
        *out_state = ctx->LastState;
        *out_gen = ctx->LastGen;
    }
}

/* Pop the next unread state for the request's file handle from the 16-entry ring.
 * Returns 1 if an unread state was popped, 0 if handle is caught up. Under ctx->Lock. */
static int XusbTryPopForRequestLocked(PXUSB_DEV_CTX ctx, WDFREQUEST req,
                                      GamepadState *out_state, uint32_t *out_gen)
{
    WDFFILEOBJECT fo = WdfRequestGetFileObject(req);
    uint32_t *pCur;
    uint16_t *pBtns;
    BOOLEAN  *pInit;
    int rc;

    XusbEnsureShmLocked(ctx);
    if (ctx->Shm == NULL) return 0;

    {
        PXUSB_HANDLE_CTX h = (fo != NULL) ? GetXusbHandleCtx(fo) : NULL;
        if (h != NULL) {
            pCur  = &h->Cursor;
            pBtns = &h->LastButtons;
            pInit = &h->Initialized;
        } else {
            pCur  = &ctx->FallbackCursor;
            pBtns = &ctx->FallbackButtons;
            pInit = &ctx->FallbackInit;
        }
    }

    if (!(*pInit)) {
        uint32_t widx = ctx->Shm->ring_write_idx;
        *pCur  = (widx > 0u) ? (widx - 1u) : 0u;
        *pBtns = 0u;
        *pInit = TRUE;
        if (widx == 0u) {
            XusbReadCurrentStateLocked(ctx, out_state, out_gen);
            return 1;
        }
    }

    rc = vgd_shm_read_next_state(ctx->Shm, pCur, pBtns, out_state, out_gen);
    if (rc == 1) {
        ctx->LastState = *out_state;
        ctx->LastGen   = *out_gen;
        return 1;
    }
    return 0;
}

static void XusbCompleteStateRequest(WDFREQUEST req, const GamepadState *st, uint32_t gen)
{
    PVOID outBuf = NULL;
    size_t outLen = 0;
    size_t written = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(req, VGD_XUSB_STATE_SIZE, &outBuf, &outLen);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    if (vgd_xusb_build_get_state(st, gen, (uint8_t *)outBuf, outLen, &written) != VGD_CODEC_OK) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, written);
}

typedef struct _XUSB_COMPLETION_ITEM {
    WDFREQUEST   Req;
    GamepadState State;
    uint32_t     Gen;
} XUSB_COMPLETION_ITEM;

static void XusbDrainWaitQueue(PXUSB_DEV_CTX ctx)
{
    XUSB_COMPLETION_ITEM done[16];
    WDFREQUEST requeue[16];
    ULONG doneCount = 0;
    ULONG requeueCount = 0;
    ULONG i;

    EnterCriticalSection(&ctx->Lock);
    XusbEnsureShmLocked(ctx);
    if (ctx->Shm != NULL) {
        while (doneCount < 16u && requeueCount < 16u) {
            WDFREQUEST req = NULL;
            GamepadState st;
            uint32_t gen = 0;
            if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->WaitQueue, &req))) {
                break;
            }
            if (XusbTryPopForRequestLocked(ctx, req, &st, &gen)) {
                done[doneCount].Req   = req;
                done[doneCount].State = st;
                done[doneCount].Gen   = gen;
                doneCount++;
            } else {
                requeue[requeueCount++] = req;
            }
        }
        for (i = 0; i < requeueCount; i++) {
            NTSTATUS fwd = WdfRequestForwardToIoQueue(requeue[i], ctx->WaitQueue);
            if (!NT_SUCCESS(fwd)) {
                WdfRequestComplete(requeue[i], fwd);
            }
        }
    }
    LeaveCriticalSection(&ctx->Lock);

    for (i = 0; i < doneCount; i++) {
        XusbCompleteStateRequest(done[i].Req, &done[i].State, done[i].Gen);
    }
}

static DWORD WINAPI XusbDoorbellThreadProc(LPVOID param)
{
    PXUSB_DEV_CTX ctx = (PXUSB_DEV_CTX)param;
    for (;;) {
        HANDLE inEvt = NULL;
        EnterCriticalSection(&ctx->Lock);
        XusbEnsureShmLocked(ctx);
        inEvt = ctx->InEvt;
        LeaveCriticalSection(&ctx->Lock);

        if (inEvt == NULL) {
            if (WaitForSingleObject(ctx->StopEvt, 25) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        {
            HANDLE handles[2];
            DWORD wr;
            handles[0] = ctx->StopEvt;
            handles[1] = inEvt;
            wr = WaitForMultipleObjects(2, handles, FALSE, 25);
            if (wr == WAIT_OBJECT_0) {
                break;
            }
            if (wr == WAIT_OBJECT_0 + 1 || wr == WAIT_TIMEOUT) {
                XusbDrainWaitQueue(ctx);
            }
        }
    }
    return 0;
}

static void XusbCompleteCodecResult(WDFREQUEST req, int rc, size_t written)
{
    if (rc == VGD_CODEC_OK) {
        WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, written);
    } else if (rc == VGD_CODEC_ERR_SMALL_BUF) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
    } else {
        WdfRequestComplete(req, STATUS_INVALID_PARAMETER);
    }
}

VOID XusbEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PXUSB_DEV_CTX ctx = GetXusbCtx(device);
    PVOID outBuf = NULL;
    size_t outLen = 0;
    size_t written = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode) {
    case IOCTL_XUSB_GET_INFORMATION: {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 1, &outBuf, &outLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        XusbCompleteCodecResult(Request,
            vgd_xusb_build_get_information((uint8_t *)outBuf, outLen, &written), written);
        return;
    }

    case IOCTL_XUSB_GET_CAPABILITIES: {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 1, &outBuf, &outLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        XusbCompleteCodecResult(Request,
            vgd_xusb_build_get_capabilities((uint8_t)ctx->Slot, (uint8_t *)outBuf, outLen, &written), written);
        return;
    }

    case IOCTL_XUSB_GET_LED_STATE: {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 1, &outBuf, &outLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        XusbCompleteCodecResult(Request,
            vgd_xusb_build_get_led_state((uint8_t)ctx->Slot, (uint8_t *)outBuf, outLen, &written), written);
        return;
    }

    case IOCTL_XUSB_GET_BATTERY_INFO: {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 1, &outBuf, &outLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        XusbCompleteCodecResult(Request,
            vgd_xusb_build_get_battery_info((uint8_t *)outBuf, outLen, &written), written);
        return;
    }

    case IOCTL_XUSB_GET_INFORMATION_EX: {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 1, &outBuf, &outLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        XusbCompleteCodecResult(Request,
            vgd_xusb_build_get_information_ex((uint8_t *)outBuf, outLen, &written), written);
        return;
    }

    case IOCTL_XUSB_GET_STATE: {
        GamepadState st;
        uint32_t gen = 0;
        EnterCriticalSection(&ctx->Lock);
        if (!XusbTryPopForRequestLocked(ctx, Request, &st, &gen)) {
            XusbReadCurrentStateLocked(ctx, &st, &gen);
        }
        LeaveCriticalSection(&ctx->Lock);
        XusbCompleteStateRequest(Request, &st, gen);
        return;
    }

    case IOCTL_XUSB_SET_STATE: {
        PVOID inBuf = NULL;
        size_t inLen = 0;
        uint8_t lm = 0, sm = 0;
        int rc;
        if (InputBufferLength < 4 ||
            !NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, 4, &inBuf, &inLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        rc = vgd_xusb_parse_set_state((const uint8_t *)inBuf, inLen, &lm, &sm);
        if (rc == 1) {
            EnterCriticalSection(&ctx->Lock);
            XusbEnsureShmLocked(ctx);
            if (ctx->Shm != NULL) {
                vgd_shm_write_rumble(ctx->Shm, lm, sm);
            }
            if (ctx->OutEvt != NULL) {
                SetEvent(ctx->OutEvt);
            }
            LeaveCriticalSection(&ctx->Lock);
        } else if (rc < 0) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;
    }

    case IOCTL_XUSB_WAIT_FOR_INPUT: {
        PVOID inBuf = NULL;
        size_t inLen = 0;
        const uint8_t *inBytes = NULL;
        uint16_t ver;
        if (InputBufferLength < 4 || OutputBufferLength < VGD_XUSB_STATE_SIZE ||
            !NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, 4, &inBuf, &inLen))) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        inBytes = (const uint8_t *)inBuf;
        ver = (uint16_t)inBytes[0] | ((uint16_t)inBytes[1] << 8);
        if (ver != 0x0102u || inBytes[2] != 0u) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            return;
        }
        if (inBytes[3] == 2u) {
            /* WGI focus-resume probe: complete immediately with state[2] = 0x03 */
            if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, VGD_XUSB_STATE_SIZE, &outBuf, &outLen))) {
                WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
                return;
            }
            XusbCompleteCodecResult(Request,
                vgd_xusb_build_wait_resumed((uint8_t *)outBuf, outLen, &written), written);
            return;
        }
        {
            GamepadState st;
            uint32_t gen = 0;
            int popped;
            EnterCriticalSection(&ctx->Lock);
            popped = XusbTryPopForRequestLocked(ctx, Request, &st, &gen);
            LeaveCriticalSection(&ctx->Lock);

            if (popped) {
                XusbCompleteStateRequest(Request, &st, gen);
                return;
            }
        }
        {
            NTSTATUS fwd = WdfRequestForwardToIoQueue(Request, ctx->WaitQueue);
            if (!NT_SUCCESS(fwd)) {
                WdfRequestComplete(Request, fwd);
                return;
            }
            /* Park-then-recheck: immediately drain if a state arrived during the park window (REQ-021). */
            XusbDrainWaitQueue(ctx);
        }
        return;
    }

    case IOCTL_XUSB_POWER_INFO: {
        if (!NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, 4, &outBuf, &outLen))) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        RtlZeroMemory(outBuf, 4);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 4);
        return;
    }

    case IOCTL_XUSB_WAIT_GUIDE:
    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}

static void XusbStopWorker(PXUSB_DEV_CTX ctx)
{
    if (ctx->StopEvt != NULL) {
        SetEvent(ctx->StopEvt);
    }
    if (ctx->DoorbellThread != NULL) {
        WaitForSingleObject(ctx->DoorbellThread, 2000);
        CloseHandle(ctx->DoorbellThread);
        ctx->DoorbellThread = NULL;
    }
    if (ctx->WaitQueue != NULL) {
        WdfIoQueuePurgeSynchronously(ctx->WaitQueue);
    }
}

VOID XusbEvtSelfManagedIoFlush(_In_ WDFDEVICE Device)
{
    PXUSB_DEV_CTX ctx = GetXusbCtx(Device);
    XusbStopWorker(ctx);
}

VOID XusbEvtDeviceCleanup(_In_ WDFOBJECT DeviceObject)
{
    PXUSB_DEV_CTX ctx = GetXusbCtx((WDFDEVICE)DeviceObject);
    XusbStopWorker(ctx);
    if (ctx->StopEvt != NULL) {
        CloseHandle(ctx->StopEvt);
        ctx->StopEvt = NULL;
    }
    if (ctx->LockInit) {
        EnterCriticalSection(&ctx->Lock);
        XusbCloseShmLocked(ctx);
        LeaveCriticalSection(&ctx->Lock);
        DeleteCriticalSection(&ctx->Lock);
        ctx->LockInit = FALSE;
    }
}

NTSTATUS XusbEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES devAttrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDFDEVICE device;
    PXUSB_DEV_CTX ctx;
    WDF_IO_QUEUE_CONFIG qCfg;
    WDF_IO_QUEUE_CONFIG waitCfg;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceSelfManagedIoFlush = XusbEvtSelfManagedIoFlush;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    {
        WDF_FILEOBJECT_CONFIG fileCfg;
        WDF_OBJECT_ATTRIBUTES fileAttrs;
        WDF_FILEOBJECT_CONFIG_INIT(&fileCfg, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
        fileCfg.FileObjectClass = (WDF_FILEOBJECT_CLASS)(WdfFileObjectWdfCannotUseFsContexts | WdfFileObjectCanBeOptional);
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttrs, XUSB_HANDLE_CTX);
        WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileCfg, &fileAttrs);
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttrs, XUSB_DEV_CTX);
    devAttrs.EvtCleanupCallback = XusbEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &devAttrs, &device);
    if (!NT_SUCCESS(status)) return status;

    ctx = GetXusbCtx(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Slot = XusbQuerySlot(device);
    vgd_shm_format_names_w(ctx->Slot, (uint16_t *)ctx->ShmName, 64, NULL, 0, (uint16_t *)ctx->OutEvtName, 64);
    vgd_shm_format_doorbells_w(ctx->Slot, (uint16_t *)ctx->InEvtName, 64, NULL, 0);

    InitializeCriticalSection(&ctx->Lock);
    ctx->LockInit = TRUE;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoDeviceControl = XusbEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT(&waitCfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &waitCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->WaitQueue);
    if (!NT_SUCCESS(status)) return status;

    ctx->StopEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ctx->StopEvt != NULL) {
        ctx->DoorbellThread = CreateThread(NULL, 0, XusbDoorbellThreadProc, ctx, 0, NULL);
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_XUSB, NULL);
    return status;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, XusbEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
