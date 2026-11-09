#define COBJMACROS
#include "screenshot.h"
#include <winternl.h>
#include <ntstatus.h>
#include "../transport/asb_transport.h"
#include "../transport/asb_capture_service.h"
#include <stdlib.h>
#include <string.h>

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxguid.lib")

typedef struct {
    AsbVddScreenshot *screenshot;
    ULONGLONG deadline;
} ScreenshotClient;

static int receive_bytes(void *context,void *connection,void *buffer,size_t size)
{
    ScreenshotClient *client=context;
    while (WaitForSingleObject(client->screenshot->hTerminateEvent,0)==WAIT_TIMEOUT) {
        ULONGLONG now=GetTickCount64();
        if (now>=client->deadline) break;
        int ready=asb_poll(connection,(int)min(client->deadline-now,50));
        if (WaitForSingleObject(client->screenshot->hTerminateEvent,0)!=WAIT_TIMEOUT) break;
        if (ready<0) break;
        if (ready>0) return asb_recv(connection,buffer,(int)size);
    }
    return -1;
}

static int send_bytes(void *context,void *connection,const void *buffer,size_t size)
{
    ScreenshotClient *client=context;
    while (WaitForSingleObject(client->screenshot->hTerminateEvent,0)==WAIT_TIMEOUT &&
        GetTickCount64()<client->deadline) {
        int count=asb_send(connection,buffer,(int)min(size,16384));
        if (count>=0) return count;
        if (!asb_transport_is_ivshmem() || asb_poll(connection,0)<0) break;
    }
    return -1;
}

static void release_images(AsbVddScreenshot *s)
{
    if (s->readerMutex) IDXGIKeyedMutex_Release(s->readerMutex);
    if (s->writerMutex) IDXGIKeyedMutex_Release(s->writerMutex);
    if (s->opened) ID3D11Texture2D_Release(s->opened);
    if (s->readback) ID3D11Texture2D_Release(s->readback);
    if (s->mirror) ID3D11Texture2D_Release(s->mirror);
    if (s->sharedHandle) CloseHandle(s->sharedHandle);
    s->readerMutex=s->writerMutex=NULL;
    s->opened=s->readback=s->mirror=NULL;
    s->sharedHandle=NULL;s->mirrorWidth=s->mirrorHeight=0;
}

static HRESULT prepare(AsbVddScreenshot *s,UINT width,UINT height)
{
    HRESULT hr=S_OK;
    if (!s->readerDevice) {
        IDXGIDevice *dxgi=NULL;
        IDXGIAdapter *adapter=NULL;
        hr=ID3D11Device_QueryInterface(s->pDevice,&IID_IDXGIDevice,(void **)&dxgi);
        if (SUCCEEDED(hr)) hr=IDXGIDevice_GetAdapter(dxgi,&adapter);
        if (SUCCEEDED(hr)) hr=D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,NULL,0,D3D11_SDK_VERSION,
            &s->readerDevice,NULL,&s->readerContext);
        if (adapter) IDXGIAdapter_Release(adapter);
        if (dxgi) IDXGIDevice_Release(dxgi);
        if (FAILED(hr)) return hr;
        if (SUCCEEDED(ID3D11Device_QueryInterface(s->readerDevice,&IID_IDXGIDevice,(void **)&dxgi))) {
            IDXGIDevice_SetGPUThreadPriority(dxgi,-7);
            IDXGIDevice_Release(dxgi);
        }
    }
    if (s->mirror && s->mirrorWidth==width && s->mirrorHeight==height) return S_OK;
    AcquireSRWLockExclusive(&s->screenshotLock);
    release_images(s);
    D3D11_TEXTURE2D_DESC desc={0};
    desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    IDXGIResource1 *resource=NULL;
    ID3D11Device1 *device=NULL;
    hr=ID3D11Device_CreateTexture2D(s->pDevice,&desc,NULL,&s->mirror);
    if (SUCCEEDED(hr)) hr=ID3D11Texture2D_QueryInterface(s->mirror,&IID_IDXGIKeyedMutex,(void **)&s->writerMutex);
    if (SUCCEEDED(hr)) hr=ID3D11Texture2D_QueryInterface(s->mirror,&IID_IDXGIResource1,(void **)&resource);
    if (SUCCEEDED(hr)) hr=IDXGIResource1_CreateSharedHandle(resource,NULL,
        DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,NULL,&s->sharedHandle);
    if (SUCCEEDED(hr)) hr=ID3D11Device_QueryInterface(s->readerDevice,&IID_ID3D11Device1,(void **)&device);
    if (SUCCEEDED(hr)) hr=ID3D11Device1_OpenSharedResource1(device,s->sharedHandle,
        &IID_ID3D11Texture2D,(void **)&s->opened);
    if (SUCCEEDED(hr)) hr=ID3D11Texture2D_QueryInterface(s->opened,&IID_IDXGIKeyedMutex,(void **)&s->readerMutex);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr=ID3D11Device_CreateTexture2D(s->readerDevice,&desc,NULL,&s->readback);
    if (device) ID3D11Device1_Release(device);
    if (resource) IDXGIResource1_Release(resource);
    if (SUCCEEDED(hr)) { s->mirrorWidth=width;s->mirrorHeight=height; }
    else release_images(s);
    InterlockedExchange(&s->offered,0);
    ReleaseSRWLockExclusive(&s->screenshotLock);
    return hr;
}

void asb_vdd_screenshot_frame(AsbVddScreenshot *s,ID3D11Texture2D *source,
                             UINT64 sequence,BOOL shared)
{
    BOOL wanted=InterlockedCompareExchange(&s->screenshotRequested,0,0);
    if (wanted && shared && !s->current) {
        ID3D11Texture2D_AddRef(source);
        s->current=source;
    }
    D3D11_TEXTURE2D_DESC desc={0};
    if (!s->width || wanted) {
        ID3D11Texture2D_GetDesc(source,&desc);
        InterlockedExchange(&s->width,(LONG)desc.Width);
        InterlockedExchange(&s->height,(LONG)desc.Height);
    }
    if (!wanted || InterlockedCompareExchange(&s->offered,0,0) ||
        !TryAcquireSRWLockExclusive(&s->screenshotLock)) return;
    if (s->screenshotRequested && !s->offered && s->mirror &&
        s->mirrorWidth==desc.Width && s->mirrorHeight==desc.Height &&
        IDXGIKeyedMutex_AcquireSync(s->writerMutex,0,0)==S_OK) {
        ID3D11DeviceContext_CopyResource(s->pDeviceContext,
            (ID3D11Resource *)s->mirror,(ID3D11Resource *)source);
        HRESULT hr=IDXGIKeyedMutex_ReleaseSync(s->writerMutex,1);
        if (SUCCEEDED(hr)) {
            s->screenshotReply.width=desc.Width;s->screenshotReply.height=desc.Height;
            s->screenshotReply.stride=desc.Width*4;
            s->screenshotReply.data_size=desc.Width*desc.Height*4;
            s->screenshotReply.frame_seq=sequence;
            s->screenshotReply.capture_time_ms=GetTickCount64();
            s->screenshotReply.flags=shared?ASB_SCREENSHOT_GPU_SHARED:0;
            s->screenshotCopyGeneration=s->screenshotGeneration;
            InterlockedExchange(&s->offered,1);
        }
    }
    ReleaseSRWLockExclusive(&s->screenshotLock);
}

void asb_vdd_screenshot_poll(AsbVddScreenshot *s,ID3D11Texture2D *cached,
    ID3D11Texture2D *pending,IDXGIKeyedMutex *mutex,ID3D11Texture2D *latest,
    UINT64 sequence,BOOL shared)
{
    if (!sequence || !cached || !InterlockedCompareExchange(&s->screenshotRequested,0,0) ||
        InterlockedCompareExchange(&s->offered,0,0)) return;
    if (shared && s->current) asb_vdd_screenshot_frame(s,s->current,sequence,TRUE);
    else if (!shared) asb_vdd_screenshot_frame(s,cached,sequence,FALSE);
    else if (pending) asb_vdd_screenshot_frame(s,pending,sequence,TRUE);
    else if (mutex && latest && IDXGIKeyedMutex_AcquireSync(mutex,0,0)==S_OK) {
        asb_vdd_screenshot_frame(s,latest,sequence,TRUE);
        IDXGIKeyedMutex_ReleaseSync(mutex,0);
    }
}

void asb_vdd_screenshot_release_frame(AsbVddScreenshot *s)
{
    if (s->current) ID3D11Texture2D_Release(s->current);
    s->current=NULL;
}

static DWORD capture(AsbVddScreenshot *s,DWORD timeout,AsbScreenshotReply *reply,BYTE **pixels)
{
    ULONGLONG deadline=GetTickCount64()+timeout;
    DWORD error=ERROR_TIMEOUT;
    InterlockedExchange(&s->screenshotRequested,1);
    while (GetTickCount64()<deadline && WaitForSingleObject(s->hTerminateEvent,0)!=WAIT_OBJECT_0) {
        UINT width=(UINT)InterlockedCompareExchange(&s->width,0,0);
        UINT height=(UINT)InterlockedCompareExchange(&s->height,0,0);
        if (!width || !height) { WaitForSingleObject(s->hTerminateEvent,2);continue; }
        if (width>7680 || height>4320) { error=ERROR_INVALID_DATA;break; }
        HRESULT hr=prepare(s,width,height);
        if (FAILED(hr)) { error=(DWORD)hr;break; }
        hr=IDXGIKeyedMutex_AcquireSync(s->readerMutex,1,0);
        if (hr==(HRESULT)WAIT_TIMEOUT) { WaitForSingleObject(s->hTerminateEvent,2);continue; }
        if (hr!=S_OK) { error=(DWORD)hr;break; }
        AcquireSRWLockShared(&s->screenshotLock);
        BOOL current=s->screenshotCopyGeneration==s->screenshotGeneration;
        AsbScreenshotReply frame=s->screenshotReply;
        ReleaseSRWLockShared(&s->screenshotLock);
        if (!current) {
            IDXGIKeyedMutex_ReleaseSync(s->readerMutex,0);
            InterlockedExchange(&s->offered,0);
            continue;
        }
        InterlockedExchange(&s->screenshotRequested,0);
        ID3D11DeviceContext_CopyResource(s->readerContext,
            (ID3D11Resource *)s->readback,(ID3D11Resource *)s->opened);
        ID3D11DeviceContext_Flush(s->readerContext);
        IDXGIKeyedMutex_ReleaseSync(s->readerMutex,0);
        InterlockedExchange(&s->offered,0);
        D3D11_MAPPED_SUBRESOURCE mapped={0};
        do {
            hr=ID3D11DeviceContext_Map(s->readerContext,(ID3D11Resource *)s->readback,
                0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if (hr!=DXGI_ERROR_WAS_STILL_DRAWING) break;
        } while (GetTickCount64()<deadline && WaitForSingleObject(s->hTerminateEvent,2)!=WAIT_OBJECT_0);
        if (FAILED(hr)) { error=hr==DXGI_ERROR_WAS_STILL_DRAWING?ERROR_TIMEOUT:(DWORD)hr;break; }
        if (!mapped.pData || mapped.RowPitch<frame.stride) {
            ID3D11DeviceContext_Unmap(s->readerContext,(ID3D11Resource *)s->readback,0);
            error=ERROR_INVALID_DATA;
            break;
        }
        error=asb_capture_pack(mapped.pData,mapped.RowPitch,&frame,pixels);
        ID3D11DeviceContext_Unmap(s->readerContext,(ID3D11Resource *)s->readback,0);
        if (!error) *reply=frame;
        break;
    }
    InterlockedExchange(&s->screenshotRequested,0);
    return error;
}

static uint32_t capture_request(void *context,const AsbScreenshotRequest *request,
                               AsbScreenshotReply *reply,unsigned char **pixels)
{
    ScreenshotClient *client=context;
    AsbVddScreenshot *s=client->screenshot;
    AcquireSRWLockExclusive(&s->screenshotLock);
    ++s->screenshotGeneration;
    ReleaseSRWLockExclusive(&s->screenshotLock);
    uint32_t error=capture(s,request->timeout_ms,reply,pixels);
    client->deadline=GetTickCount64()+2000;
    return error;
}

static DWORD WINAPI worker(void *parameter)
{
    AsbVddScreenshot *s=parameter;
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    AsbListener *listener=asb_listen(ASB_CH_SCREENSHOT);
    if (!listener) { s->log("Screenshot: listen failed");return 1; }
    ScreenshotClient client={s,0};
    AsbCaptureService service={&client,receive_bytes,send_bytes,capture_request};
    while (WaitForSingleObject(s->hTerminateEvent,0)!=WAIT_OBJECT_0) {
        AsbConn *connection=asb_accept(listener,100);
        if (!connection) continue;
        asb_set_timeout(connection,50,50);
        client.deadline=GetTickCount64()+2000;
        asb_capture_service_request(&service,connection);
        asb_close(connection);
    }
    asb_close_listener(listener);
    return 0;
}

void asb_vdd_screenshot_init(AsbVddScreenshot *s,ID3D11Device *device,
    ID3D11DeviceContext *context,HANDLE terminate,void (*log)(const char *,...))
{
    ZeroMemory(s,sizeof(*s));
    s->pDevice=device;s->pDeviceContext=context;s->hTerminateEvent=terminate;s->log=log;
    InitializeSRWLock(&s->screenshotLock);
}

void asb_vdd_screenshot_start(AsbVddScreenshot *s)
{
    s->hScreenshotThread=CreateThread(NULL,0,worker,s,0,NULL);
}

void asb_vdd_screenshot_destroy(AsbVddScreenshot *s)
{
    if (s->hScreenshotThread) {
        SetEvent(s->hTerminateEvent);
        WaitForSingleObject(s->hScreenshotThread,INFINITE);
        CloseHandle(s->hScreenshotThread);
    }
    asb_vdd_screenshot_release_frame(s);
    release_images(s);
    if (s->readerContext) ID3D11DeviceContext_Release(s->readerContext);
    if (s->readerDevice) ID3D11Device_Release(s->readerDevice);
}
