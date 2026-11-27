/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-005 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-012 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-013 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-016 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-019 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-020 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-021 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
/*
 * AppSandboxVGD_HID — First-party UMDF2 Virtual Xbox 360 HID Minidriver.
 *
 * Layered as a UMDF2 lower filter under inbox mshidumdf.sys in Class = HIDClass.
 * Serves standard Xbox 360 HID descriptors, attributes, strings, and pended
 * IOCTL_HID_READ_REPORT requests from per-slot shared memory so joy.cpl,
 * DirectInput, and RawInput see every active Controller Slot (0..3).
 */

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <wdf.h>
#include <winioctl.h>
#include <hidclass.h>
#include <string.h>

#ifndef IOCTL_HID_GET_DEVICE_DESCRIPTOR
#define IOCTL_HID_GET_DEVICE_DESCRIPTOR     HID_CTL_CODE(0)
#define IOCTL_HID_GET_REPORT_DESCRIPTOR     HID_CTL_CODE(1)
#define IOCTL_HID_READ_REPORT               HID_CTL_CODE(2)
#define IOCTL_HID_WRITE_REPORT              HID_CTL_CODE(3)
#define IOCTL_HID_GET_STRING                HID_CTL_CODE(4)
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES     HID_CTL_CODE(9)
#define HID_HID_DESCRIPTOR_TYPE             0x21
#define HID_REPORT_DESCRIPTOR_TYPE          0x22
#define HID_STRING_ID_IMANUFACTURER         14
#define HID_STRING_ID_IPRODUCT              15
#define HID_STRING_ID_ISERIALNUMBER         16

typedef struct _HID_DEVICE_ATTRIBUTES {
    ULONG  Size;
    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;
    USHORT Reserved[11];
} HID_DEVICE_ATTRIBUTES, *PHID_DEVICE_ATTRIBUTES;

#include <pshpack1.h>
typedef struct _HID_DESCRIPTOR {
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    USHORT bcdHID;
    UCHAR  bCountry;
    UCHAR  bNumDescriptors;
    struct _HID_DESCRIPTOR_DESC_LIST {
        UCHAR  bReportType;
        USHORT wReportLength;
    } DescriptorList[1];
} HID_DESCRIPTOR, *PHID_DESCRIPTOR;
#include <poppack.h>
#endif

#include "../vgd_shm.h"
#include "../vgd_xusb_codec.h"
#include "../vgd_hid_codec.h"

static const WCHAR k_mfr_str[]  = L"Microsoft Corporation";
static const WCHAR k_prod_str[] = L"Controller (XBOX 360 For Windows)";

typedef struct _HID_DEV_CTX {
    uint32_t              Slot;
    WCHAR                 ShmName[64];
    WCHAR                 InEvtName[64];
    WCHAR                 SerialStr[32];
    size_t                SerialStrBytes;
    HID_DESCRIPTOR        HidDesc;
    HID_DEVICE_ATTRIBUTES HidAttrs;
    HANDLE                ShmHandle;
    VgdShmBlock          *Shm;
    HANDLE                InEvt;
    HANDLE                StopEvt;
    HANDLE                DoorbellThread;
    WDFQUEUE              ReadQueue;
    CRITICAL_SECTION      Lock;
    BOOLEAN               LockInit;
    GamepadState          LastState;
    uint32_t              RingCursor;
    uint16_t              LastButtons;
    BOOLEAN               RingInit;
} HID_DEV_CTX, *PHID_DEV_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HID_DEV_CTX, GetHidCtx)

DRIVER_INITIALIZE                     DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD             HidEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL    HidEvtIoDeviceControl;
EVT_WDF_DEVICE_SELF_MANAGED_IO_FLUSH  HidEvtSelfManagedIoFlush;
EVT_WDF_OBJECT_CONTEXT_CLEANUP        HidEvtDeviceCleanup;

static void HidCloseShmLocked(PHID_DEV_CTX ctx)
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
}

static void HidEnsureShmLocked(PHID_DEV_CTX ctx)
{
    if (ctx->Shm == NULL) {
        HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, ctx->ShmName);
        if (hMap != NULL) {
            VgdShmBlock *view = (VgdShmBlock *)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(VgdShmBlock));
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
}

static uint32_t HidQuerySlot(WDFDEVICE device)
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

static int HidTryPopStateLocked(PHID_DEV_CTX ctx, GamepadState *out_state)
{
    uint32_t gen = 0;
    HidEnsureShmLocked(ctx);
    if (ctx->Shm == NULL) return 0;

    if (!ctx->RingInit) {
        uint32_t widx = ctx->Shm->ring_write_idx;
        ctx->RingCursor = (widx > 0u) ? (widx - 1u) : 0u;
        ctx->LastButtons = 0u;
        ctx->RingInit = TRUE;
        if (widx == 0u) {
            if (vgd_shm_read_state(ctx->Shm, out_state, &gen) == 0) {
                ctx->LastState = *out_state;
            } else {
                *out_state = ctx->LastState;
            }
            return 1;
        }
    }

    if (vgd_shm_read_next_state(ctx->Shm, &ctx->RingCursor, &ctx->LastButtons, out_state, &gen) == 1) {
        ctx->LastState = *out_state;
        return 1;
    }
    return 0;
}

static NTSTATUS HidCopyOutput(WDFREQUEST req, const void *src, size_t len)
{
    PVOID outBuf = NULL;
    size_t outLen = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(req, len, &outBuf, &outLen);
    if (!NT_SUCCESS(status)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(outBuf, src, len);
    WdfRequestSetInformation(req, len);
    return STATUS_SUCCESS;
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
static NTSTATUS HidCopyProductString(PHID_DEV_CTX ctx, WDFREQUEST req)
{
    WCHAR dyn_name[VGD_PRODUCT_NAME_MAX];
    size_t len = 0;

    EnterCriticalSection(&ctx->Lock);
    HidEnsureShmLocked(ctx);
    if (ctx->Shm != NULL) {
        len = vgd_shm_get_product_name_w(ctx->Shm, (uint16_t *)dyn_name, ARRAYSIZE(dyn_name));
    }
    LeaveCriticalSection(&ctx->Lock);

    if (len > 0u) {
        return HidCopyOutput(req, dyn_name, (len + 1u) * sizeof(WCHAR));
    }
    return HidCopyOutput(req, k_prod_str, sizeof(k_prod_str));
}

static void HidCompleteReadRequest(WDFREQUEST req, const GamepadState *st)
{
    PVOID outBuf = NULL;
    size_t outLen = 0;
    size_t written = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(req, VGD_HID_INPUT_REPORT_SIZE, &outBuf, &outLen);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    if (vgd_hid_build_input_report(st, (uint8_t *)outBuf, outLen, &written) != VGD_CODEC_OK) {
        WdfRequestComplete(req, STATUS_BUFFER_TOO_SMALL);
        return;
    }
    WdfRequestCompleteWithInformation(req, STATUS_SUCCESS, written);
}

static void HidDrainReadQueue(PHID_DEV_CTX ctx)
{
    for (;;) {
        GamepadState st;
        WDFREQUEST req = NULL;

        EnterCriticalSection(&ctx->Lock);
        HidEnsureShmLocked(ctx);
        if (ctx->Shm == NULL) {
            LeaveCriticalSection(&ctx->Lock);
            return;
        }
        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ctx->ReadQueue, &req))) {
            LeaveCriticalSection(&ctx->Lock);
            return;
        }
        if (!HidTryPopStateLocked(ctx, &st)) {
            NTSTATUS fwd = WdfRequestForwardToIoQueue(req, ctx->ReadQueue);
            LeaveCriticalSection(&ctx->Lock);
            if (!NT_SUCCESS(fwd)) {
                WdfRequestComplete(req, fwd);
            }
            return;
        }
        LeaveCriticalSection(&ctx->Lock);

        HidCompleteReadRequest(req, &st);
    }
}

static DWORD WINAPI HidDoorbellThreadProc(LPVOID param)
{
    PHID_DEV_CTX ctx = (PHID_DEV_CTX)param;
    for (;;) {
        HANDLE inEvt = NULL;
        EnterCriticalSection(&ctx->Lock);
        HidEnsureShmLocked(ctx);
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
                HidDrainReadQueue(ctx);
            }
        }
    }
    return 0;
}

VOID HidEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PHID_DEV_CTX ctx = GetHidCtx(device);
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = HidCopyOutput(Request, &ctx->HidDesc, ctx->HidDesc.bLength);
        WdfRequestComplete(Request, status);
        return;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        size_t descLen = 0;
        const uint8_t *desc = vgd_hid_get_report_descriptor(&descLen);
        status = HidCopyOutput(Request, desc, descLen);
        WdfRequestComplete(Request, status);
        return;
    }

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = HidCopyOutput(Request, &ctx->HidAttrs, sizeof(ctx->HidAttrs));
        WdfRequestComplete(Request, status);
        return;

    case IOCTL_HID_GET_STRING: {
        PVOID inBuf = NULL;
        size_t inLen = 0;
        ULONG strId = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG), &inBuf, &inLen))) {
            strId = (*(const ULONG *)inBuf) & 0xFFFFu;
        }
        if (strId == 14u || strId == HID_STRING_ID_IMANUFACTURER) {
            status = HidCopyOutput(Request, k_mfr_str, sizeof(k_mfr_str));
        } else if (strId == 16u || strId == HID_STRING_ID_ISERIALNUMBER) {
            status = HidCopyOutput(Request, ctx->SerialStr, ctx->SerialStrBytes);
        } else {
            status = HidCopyProductString(ctx, Request);
        }
        WdfRequestComplete(Request, status);
        return;
    }

    case IOCTL_HID_GET_INDEXED_STRING:
        status = HidCopyProductString(ctx, Request);
        WdfRequestComplete(Request, status);
        return;

    case IOCTL_HID_READ_REPORT: {
        GamepadState st;
        int popped;

        EnterCriticalSection(&ctx->Lock);
        popped = HidTryPopStateLocked(ctx, &st);
        LeaveCriticalSection(&ctx->Lock);

        if (popped) {
            HidCompleteReadRequest(Request, &st);
            return;
        }
        status = WdfRequestForwardToIoQueue(Request, ctx->ReadQueue);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        /* Park-then-recheck: drain immediately if an input arrived during park window (REQ-021). */
        HidDrainReadQueue(ctx);
        return;
    }

    case IOCTL_HID_WRITE_REPORT:
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        return;

    default:
        WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
        return;
    }
}

static void HidStopWorker(PHID_DEV_CTX ctx)
{
    if (ctx->StopEvt != NULL) {
        SetEvent(ctx->StopEvt);
    }
    if (ctx->DoorbellThread != NULL) {
        WaitForSingleObject(ctx->DoorbellThread, 2000);
        CloseHandle(ctx->DoorbellThread);
        ctx->DoorbellThread = NULL;
    }
    if (ctx->ReadQueue != NULL) {
        WdfIoQueuePurgeSynchronously(ctx->ReadQueue);
    }
}

VOID HidEvtSelfManagedIoFlush(_In_ WDFDEVICE Device)
{
    PHID_DEV_CTX ctx = GetHidCtx(Device);
    HidStopWorker(ctx);
}

VOID HidEvtDeviceCleanup(_In_ WDFOBJECT DeviceObject)
{
    PHID_DEV_CTX ctx = GetHidCtx((WDFDEVICE)DeviceObject);
    HidStopWorker(ctx);
    if (ctx->StopEvt != NULL) {
        CloseHandle(ctx->StopEvt);
        ctx->StopEvt = NULL;
    }
    if (ctx->LockInit) {
        EnterCriticalSection(&ctx->Lock);
        HidCloseShmLocked(ctx);
        LeaveCriticalSection(&ctx->Lock);
        DeleteCriticalSection(&ctx->Lock);
        ctx->LockInit = FALSE;
    }
}

NTSTATUS HidEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES devAttrs;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDFDEVICE device;
    PHID_DEV_CTX ctx;
    WDF_IO_QUEUE_CONFIG qCfg;
    WDF_IO_QUEUE_CONFIG readCfg;
    size_t descLen = 0;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceSelfManagedIoFlush = HidEvtSelfManagedIoFlush;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    {
        WDF_FILEOBJECT_CONFIG fileCfg;
        WDF_FILEOBJECT_CONFIG_INIT(&fileCfg, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
        fileCfg.FileObjectClass = (WDF_FILEOBJECT_CLASS)(WdfFileObjectWdfCannotUseFsContexts | WdfFileObjectCanBeOptional);
        WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileCfg, WDF_NO_OBJECT_ATTRIBUTES);
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttrs, HID_DEV_CTX);
    devAttrs.EvtCleanupCallback = HidEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &devAttrs, &device);
    if (!NT_SUCCESS(status)) return status;

    ctx = GetHidCtx(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Slot = HidQuerySlot(device);
    vgd_shm_format_names_w(ctx->Slot, (uint16_t *)ctx->ShmName, 64, NULL, 0, NULL, 0);
    vgd_shm_format_doorbells_w(ctx->Slot, NULL, 0, (uint16_t *)ctx->InEvtName, 64);

    {
        static const WCHAR k_serial_prefix[] = L"ASB-X360-SLOT-";
        size_t n = (sizeof(k_serial_prefix) / sizeof(WCHAR)) - 1u;
        RtlCopyMemory(ctx->SerialStr, k_serial_prefix, n * sizeof(WCHAR));
        ctx->SerialStr[n] = (WCHAR)(L'0' + (WCHAR)(ctx->Slot & 3u));
        ctx->SerialStr[n + 1u] = L'\0';
        ctx->SerialStrBytes = (n + 2u) * sizeof(WCHAR);
    }

    (void)vgd_hid_get_report_descriptor(&descLen);
    ctx->HidDesc.bLength = sizeof(HID_DESCRIPTOR);
    ctx->HidDesc.bDescriptorType = 0x21u; /* HID_HID_DESCRIPTOR_TYPE */
    ctx->HidDesc.bcdHID = 0x0111u;
    ctx->HidDesc.bCountry = 0u;
    ctx->HidDesc.bNumDescriptors = 1u;
    ctx->HidDesc.DescriptorList[0].bReportType = 0x22u; /* HID_REPORT_DESCRIPTOR_TYPE */
    ctx->HidDesc.DescriptorList[0].wReportLength = (USHORT)descLen;

    ctx->HidAttrs.Size = sizeof(HID_DEVICE_ATTRIBUTES);
    ctx->HidAttrs.VendorID = VGD_XBOX360_VID;
    ctx->HidAttrs.ProductID = VGD_XBOX360_PID;
    ctx->HidAttrs.VersionNumber = 0x0114u;

    InitializeCriticalSection(&ctx->Lock);
    ctx->LockInit = TRUE;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qCfg, WdfIoQueueDispatchParallel);
    qCfg.EvtIoDeviceControl = HidEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &qCfg, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT(&readCfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &readCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->ReadQueue);
    if (!NT_SUCCESS(status)) return status;

    ctx->StopEvt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ctx->StopEvt != NULL) {
        ctx->DoorbellThread = CreateThread(NULL, 0, HidDoorbellThreadProc, ctx, 0, NULL);
    }

    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, HidEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
