#pragma once
#include <winsock2.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#include <d3d11_4.h>
#pragma warning(pop)
#include "../transport/asb_automation.h"
#include "../../src/core/protocol.h"

enum {
    ASB_SCREENSHOT_HANDOFF_IDLE,
    ASB_SCREENSHOT_HANDOFF_QUEUED,
    ASB_SCREENSHOT_HANDOFF_WAITING,
    ASB_SCREENSHOT_HANDOFF_READY
};

typedef struct {
    HANDLE hTerminateEvent,hScreenshotThread,sharedHandle;
    SRWLOCK screenshotLock;
    volatile LONG screenshotRequested,offered,width,height,format,screenshotError;
    volatile LONG handoffState;
    DisplayGpuPacket handoff;
    ULONGLONG handoffDeadline;
    UINT mirrorWidth,mirrorHeight;
    DXGI_FORMAT mirrorFormat;
    UINT64 screenshotGeneration,screenshotCopyGeneration;
    ID3D11Device *pDevice;
    ID3D11DeviceContext *pDeviceContext;
    ID3D11Device *readerDevice;
    ID3D11DeviceContext *readerContext;
    ID3D11Texture2D *mirror,*opened,*readback;
    IDXGIKeyedMutex *writerMutex,*readerMutex;
    AsbScreenshotReply screenshotReply;
    void (*log)(const char *,...);
} AsbVddScreenshot;

typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext4 *context;
    ID3D11Fence *fence;
    HANDLE event;
    UINT64 value;
    BOOL fenceUnavailable;
} AsbGpuWaiter;

#define ASB_GPU_HANDOFF_WAIT_MS 50

#ifdef __cplusplus
extern "C" {
#endif
void asb_vdd_screenshot_init(AsbVddScreenshot *,ID3D11Device *,ID3D11DeviceContext *,HANDLE,void (*)(const char *,...));
void asb_vdd_screenshot_start(AsbVddScreenshot *);
void asb_vdd_screenshot_destroy(AsbVddScreenshot *);
void asb_vdd_screenshot_frame(AsbVddScreenshot *,ID3D11Texture2D *,UINT64,BOOL,UINT64);
void asb_vdd_screenshot_poll(AsbVddScreenshot *,ID3D11Texture2D *,ID3D11Texture2D *,IDXGIKeyedMutex *,ID3D11Texture2D *,UINT64,BOOL,UINT64,UINT);
BOOL asb_vdd_screenshot_take_handoff(AsbVddScreenshot *,DisplayGpuPacket *);
BOOL asb_vdd_screenshot_reply(AsbVddScreenshot *,const DisplayGpuPacket *);
void asb_vdd_screenshot_handoff(AsbVddScreenshot *,IDXGIKeyedMutex *,ID3D11Texture2D *,UINT64,AsbGpuWaiter *);
HRESULT asb_gpu_waiter_wait(AsbGpuWaiter *,ID3D11Device *,ID3D11DeviceContext *,HANDLE stop,DWORD timeout);
void asb_gpu_waiter_destroy(AsbGpuWaiter *);
#ifdef __cplusplus
}
#endif
