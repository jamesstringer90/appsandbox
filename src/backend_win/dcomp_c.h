#pragma once
#include <windows.h>
#include <unknwn.h>

typedef struct AsbDCompDevice AsbDCompDevice;
typedef struct AsbDCompTarget AsbDCompTarget;

typedef struct AsbDCompDeviceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AsbDCompDevice*,REFIID,void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(AsbDCompDevice*);
    ULONG (STDMETHODCALLTYPE *Release)(AsbDCompDevice*);
    HRESULT (STDMETHODCALLTYPE *Commit)(AsbDCompDevice*);
    HRESULT (STDMETHODCALLTYPE *WaitForCommitCompletion)(AsbDCompDevice*);
    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(AsbDCompDevice*,void*);
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(AsbDCompDevice*,HWND,BOOL,AsbDCompTarget**);
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(AsbDCompDevice*,IUnknown**);
} AsbDCompDeviceVtbl;
struct AsbDCompDevice { const AsbDCompDeviceVtbl *lpVtbl; };

typedef struct AsbDCompTargetVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(AsbDCompTarget*,REFIID,void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(AsbDCompTarget*);
    ULONG (STDMETHODCALLTYPE *Release)(AsbDCompTarget*);
    HRESULT (STDMETHODCALLTYPE *SetRoot)(AsbDCompTarget*,IUnknown*);
} AsbDCompTargetVtbl;
struct AsbDCompTarget { const AsbDCompTargetVtbl *lpVtbl; };

static const GUID ASB_IID_DCOMP_DEVICE =
    {0xc37ea93a,0xe7aa,0x450d,{0xb1,0x6f,0x97,0x46,0xcb,0x04,0x07,0xf3}};

HRESULT WINAPI DCompositionCreateDevice(IUnknown *dxgi_device,REFIID iid,void **device);
