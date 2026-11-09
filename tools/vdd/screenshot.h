#pragma once
#include <winsock2.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable:4201)
#include <d3d11_1.h>
#pragma warning(pop)
#include "../transport/asb_automation.h"

typedef struct {
    HANDLE hTerminateEvent,hScreenshotThread,sharedHandle;
    SRWLOCK screenshotLock;
    volatile LONG screenshotRequested,offered,width,height;
    UINT mirrorWidth,mirrorHeight;
    UINT64 screenshotGeneration,screenshotCopyGeneration;
    ID3D11Device *pDevice;
    ID3D11DeviceContext *pDeviceContext;
    ID3D11Device *readerDevice;
    ID3D11DeviceContext *readerContext;
    ID3D11Texture2D *mirror,*opened,*readback,*current;
    IDXGIKeyedMutex *writerMutex,*readerMutex;
    AsbScreenshotReply screenshotReply;
    void (*log)(const char *,...);
} AsbVddScreenshot;

#ifdef __cplusplus
extern "C" {
#endif
void asb_vdd_screenshot_init(AsbVddScreenshot *,ID3D11Device *,ID3D11DeviceContext *,HANDLE,void (*)(const char *,...));
void asb_vdd_screenshot_start(AsbVddScreenshot *);
void asb_vdd_screenshot_destroy(AsbVddScreenshot *);
void asb_vdd_screenshot_frame(AsbVddScreenshot *,ID3D11Texture2D *,UINT64,BOOL);
void asb_vdd_screenshot_release_frame(AsbVddScreenshot *);
void asb_vdd_screenshot_poll(AsbVddScreenshot *,ID3D11Texture2D *,ID3D11Texture2D *,IDXGIKeyedMutex *,ID3D11Texture2D *,UINT64,BOOL);
#ifdef __cplusplus
}
#endif
