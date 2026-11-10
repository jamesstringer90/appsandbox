#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include "WebView2.h"
#include "agent_overlay.h"
#include "headless_vm.h"
#include "dcomp_c.h"
#include "webview2_bridge.h"
#include "ui.h"
#include "../../tools/transport/asb_automation.h"

#pragma comment(lib,"dcomp.lib")

struct AgentOverlay {
    AsbVmId vm_id;
    LONG refs;
    HWND render,notify;
    UINT take_message;
    BOOL closed,ready,hovered,waiting,finished;
    int state,x,y;
    UINT frame_width,frame_height;
    double dpi;
    RECT bounds,menu_bounds;
    wchar_t context_json[4096];
    ULONGLONG context_check;
    ULONGLONG pointer_next;
    POINT pointer;
    BOOL pointer_known;
    UINT32 work_id;
    UINT mouse_buttons;
    EventRegistrationToken message_token;
    BOOL has_message_token;
    AsbDCompDevice *device;
    AsbDCompTarget *target;
    IUnknown *visual;
    ICoreWebView2CompositionController *composition;
    ICoreWebView2Controller *controller;
    ICoreWebView2 *webview;
};

typedef struct OverlayCallback {
    const void *lpVtbl;
    LONG refs;
    const IID *iid;
    AgentOverlay *state;
} OverlayCallback;

static HMODULE g_loader;
static SRWLOCK g_loader_lock = SRWLOCK_INIT;

static BOOL active(const AgentOverlay *s)
{
    return !s->closed && s->ready && !s->finished && s->state==ASB_CONTROL_AGENT;
}

static BOOL menu_visible(const AgentOverlay *s)
{
    return !s->closed && s->ready && !s->finished && (active(s) || s->waiting);
}

static void overlay_error(AgentOverlay *s,const wchar_t *step,HRESULT hr)
{
    (void)s;
    ui_log(L"Agent overlay: %s failed (0x%08X)",step,(unsigned)hr);
}

static void state_addref(AgentOverlay *s)
{
    InterlockedIncrement(&s->refs);
}

static void state_release(AgentOverlay *s)
{
    if (!InterlockedDecrement(&s->refs)) free(s);
}

static OverlayCallback *callback_new(const void *vtable,const IID *iid,AgentOverlay *state)
{
    OverlayCallback *cb=(OverlayCallback*)calloc(1,sizeof(*cb));
    if (!cb) return NULL;
    cb->lpVtbl=vtable;
    cb->iid=iid;
    cb->refs=1;
    cb->state=state;
    state_addref(state);
    return cb;
}

static HRESULT callback_qi(OverlayCallback *cb,REFIID iid,void **out)
{
    if (!out) return E_POINTER;
    *out=NULL;
    if (IsEqualIID(iid,&IID_IUnknown) || IsEqualIID(iid,cb->iid)) {
        *out=cb;
        InterlockedIncrement(&cb->refs);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG callback_addref(OverlayCallback *cb)
{
    return InterlockedIncrement(&cb->refs);
}

static ULONG callback_release(OverlayCallback *cb)
{
    LONG refs=InterlockedDecrement(&cb->refs);
    if (!refs) {
        AgentOverlay *s=cb->state;
        free(cb);
        state_release(s);
    }
    return refs;
}

#define CALLBACK_COMMON(prefix,type) \
static HRESULT STDMETHODCALLTYPE prefix##_qi(type *self,REFIID iid,void **out) { return callback_qi((OverlayCallback*)self,iid,out); } \
static ULONG STDMETHODCALLTYPE prefix##_addref(type *self) { return callback_addref((OverlayCallback*)self); } \
static ULONG STDMETHODCALLTYPE prefix##_release(type *self) { return callback_release((OverlayCallback*)self); }

CALLBACK_COMMON(env,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
CALLBACK_COMMON(ctrl,ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler)
CALLBACK_COMMON(message,ICoreWebView2WebMessageReceivedEventHandler)

static void leave(AgentOverlay *s)
{
    if (s->hovered && s->webview) {
        s->webview->lpVtbl->PostWebMessageAsJson(s->webview,L"{\"type\":\"input\",\"kind\":\"leave\"}");
    }
    s->hovered=FALSE;
}

static void update(AgentOverlay *s)
{
    wchar_t json[4608];
    double px=0,py=0;
    BOOL show;
    if (s->closed || !s->ready || !s->webview || !s->controller) return;
    show=!s->finished && s->x>=0 && s->y>=0 && s->frame_width && s->frame_height &&
        (s->state==ASB_CONTROL_AGENT || s->state==ASB_CONTROL_USER);
    if (show) {
        double width=s->bounds.right,height=s->bounds.bottom;
        double scale=fmin(width/s->frame_width,height/s->frame_height);
        px=((width-s->frame_width*scale)/2+s->x/65535.0*s->frame_width*scale)/s->dpi;
        py=((height-s->frame_height*scale)/2+s->y/65535.0*s->frame_height*scale)/s->dpi;
    }
    swprintf_s(json,ARRAYSIZE(json),
        L"{\"active\":%s,\"menu\":%s,\"showCursor\":%s,\"x\":%.3f,\"y\":%.3f,\"context\":%s}",
        active(s)?L"true":L"false",menu_visible(s)?L"true":L"false",show?L"true":L"false",px,py,s->context_json);
    s->webview->lpVtbl->PostWebMessageAsJson(s->webview,json);
    if (!menu_visible(s)) leave(s);
    s->controller->lpVtbl->put_IsVisible(s->controller,menu_visible(s)||show);
}

static void resize(AgentOverlay *s)
{
    ICoreWebView2Controller3 *scale=NULL;
    if (s->closed || !s->render) return;
    GetClientRect(s->render,&s->bounds);
    s->dpi=fmax(1.0,GetDpiForWindow(s->render)/96.0);
    if (s->controller) {
        if (SUCCEEDED(s->controller->lpVtbl->QueryInterface(s->controller,&IID_ICoreWebView2Controller3,(void**)&scale))) {
            scale->lpVtbl->put_ShouldDetectMonitorScaleChanges(scale,FALSE);
            scale->lpVtbl->put_RasterizationScale(scale,s->dpi);
            scale->lpVtbl->put_BoundsMode(scale,COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
            scale->lpVtbl->Release(scale);
        }
        s->controller->lpVtbl->put_Bounds(s->controller,s->bounds);
        s->controller->lpVtbl->NotifyParentWindowPositionChanged(s->controller);
    }
    update(s);
}

static void close_overlay(AgentOverlay *s)
{
    if (s->closed) return;
    leave(s);
    s->closed=TRUE;
    s->ready=FALSE;
    s->render=s->notify=NULL;
    if (s->webview && s->has_message_token) s->webview->lpVtbl->remove_WebMessageReceived(s->webview,s->message_token);
    s->has_message_token=FALSE;
    if (s->composition) s->composition->lpVtbl->put_RootVisualTarget(s->composition,NULL);
    if (s->controller) s->controller->lpVtbl->Close(s->controller);
    if (s->target) s->target->lpVtbl->SetRoot(s->target,NULL);
    if (s->device) s->device->lpVtbl->Commit(s->device);
    if (s->webview) { s->webview->lpVtbl->Release(s->webview);s->webview=NULL; }
    if (s->controller) { s->controller->lpVtbl->Release(s->controller);s->controller=NULL; }
    if (s->composition) { s->composition->lpVtbl->Release(s->composition);s->composition=NULL; }
    if (s->visual) { s->visual->lpVtbl->Release(s->visual);s->visual=NULL; }
    if (s->target) { s->target->lpVtbl->Release(s->target);s->target=NULL; }
    if (s->device) { s->device->lpVtbl->Release(s->device);s->device=NULL; }
}

static BOOL refresh_context(AgentOverlay *s,BOOL force)
{
    ULONGLONG now=GetTickCount64();
    if (!force && now<s->context_check) return FALSE;
    s->context_check=now+100;
    AsbInputStatus context;
    asb_input_context(s->vm_id,&context);
    wchar_t json[4096];
    JsonBuilder builder;
    jb_init(&builder,json,ARRAYSIZE(json));
    jb_object_begin(&builder);
    jb_int(&builder,L"work_id",(int)context.work_id);
    jb_string(&builder,L"identity",context.identity);
    jb_string(&builder,L"vendor",context.vendor);
    jb_string(&builder,L"model",context.model);
    jb_string(&builder,L"purpose",context.purpose);
    jb_object_end(&builder);
    if (!wcscmp(json,s->context_json)) return FALSE;
    wcscpy_s(s->context_json,ARRAYSIZE(s->context_json),json);
    s->work_id=context.work_id;
    return TRUE;
}

static void layout(AgentOverlay *s,const wchar_t *json)
{
    int x,y,width,height;
    if (!json_get_int(json,L"x",&x) || !json_get_int(json,L"y",&y) ||
        !json_get_int(json,L"width",&width) || !json_get_int(json,L"height",&height) ||
        x<0 || y<0 || width<=0 || height<=0 ||
        (x+width)*s->dpi>s->bounds.right+2 || (y+height)*s->dpi>s->bounds.bottom+2) return;
    s->menu_bounds.left=(LONG)lround(x*s->dpi);
    s->menu_bounds.top=(LONG)lround(y*s->dpi);
    s->menu_bounds.right=(LONG)lround((x+width)*s->dpi);
    s->menu_bounds.bottom=(LONG)lround((y+height)*s->dpi);
}

static HRESULT STDMETHODCALLTYPE message_invoke(ICoreWebView2WebMessageReceivedEventHandler *self,
    ICoreWebView2 *sender,ICoreWebView2WebMessageReceivedEventArgs *args)
{
    AgentOverlay *s=((OverlayCallback*)self)->state;
    LPWSTR text=NULL;
    (void)sender;
    if (s->closed) return S_OK;
    if (FAILED(args->lpVtbl->TryGetWebMessageAsString(args,&text))) return S_OK;
    if (!wcscmp(text,L"ready")) { s->ready=TRUE;resize(s); }
    else if (!wcscmp(text,L"take_control") && active(s)) {
        s->state=ASB_CONTROL_USER;
        s->waiting=TRUE;
        update(s);
        PostMessageW(s->notify,s->take_message,ASB_INPUT_USER_REVOKE,s->work_id);
    } else if (!wcscmp(text,L"return_control") && menu_visible(s) && s->waiting) {
        PostMessageW(s->notify,s->take_message,ASB_INPUT_USER_GRANT,s->work_id);
    } else if (!wcscmp(text,L"stop_agent") && menu_visible(s)) {
        s->state=ASB_CONTROL_USER;
        s->finished=TRUE;
        s->waiting=FALSE;
        update(s);
        PostMessageW(s->notify,s->take_message,ASB_INPUT_USER_CANCEL,s->work_id);
    } else if (!wcsncmp(text,L"layout:",7) && menu_visible(s)) layout(s,text+7);
    CoTaskMemFree(text);
    return S_OK;
}

static const ICoreWebView2WebMessageReceivedEventHandlerVtbl message_vtable={
    message_qi,message_addref,message_release,message_invoke
};

static wchar_t *load_page(void)
{
    wchar_t path[MAX_PATH],*slash,*page=NULL;
    HANDLE file;
    DWORD size,count=0;
    char *bytes;
    int chars;
    GetModuleFileNameW(NULL,path,ARRAYSIZE(path));
    slash=wcsrchr(path,L'\\');
    if (!slash) return NULL;
    wcscpy_s(slash+1,ARRAYSIZE(path)-(slash+1-path),L"web\\agent-overlay.html");
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return NULL;
    size=GetFileSize(file,NULL);
    if (!size || size>262144) { CloseHandle(file);return NULL; }
    bytes=(char*)malloc(size);
    if (bytes && ReadFile(file,bytes,size,&count,NULL) && count==size) {
        chars=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,count,NULL,0);
        if (chars>0) {
            page=(wchar_t*)calloc((size_t)chars+1,sizeof(wchar_t));
            if (page) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,count,page,chars);
        }
    }
    free(bytes);
    CloseHandle(file);
    return page;
}

static HRESULT STDMETHODCALLTYPE ctrl_invoke(ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler *self,
    HRESULT result,ICoreWebView2CompositionController *composition)
{
    AgentOverlay *s=((OverlayCallback*)self)->state;
    ICoreWebView2Controller2 *transparent=NULL;
    ICoreWebView2Controller4 *drops=NULL;
    ICoreWebView2Settings *settings=NULL;
    ICoreWebView2Settings3 *accelerators=NULL;
    OverlayCallback *messages;
    COREWEBVIEW2_COLOR color={0};
    wchar_t *page;
    HRESULT hr;
    if (s->closed) {
        ICoreWebView2Controller *controller=NULL;
        if (composition && SUCCEEDED(composition->lpVtbl->QueryInterface(composition,&IID_ICoreWebView2Controller,(void**)&controller))) {
            controller->lpVtbl->Close(controller);
            controller->lpVtbl->Release(controller);
        }
        return S_OK;
    }
    if (FAILED(result) || !composition) { overlay_error(s,L"composition controller",result);return S_OK; }
    s->composition=composition;
    composition->lpVtbl->AddRef(composition);
    hr=composition->lpVtbl->QueryInterface(composition,&IID_ICoreWebView2Controller,(void**)&s->controller);
    if (FAILED(hr)) { overlay_error(s,L"base controller",hr);return S_OK; }
    s->controller->lpVtbl->put_IsVisible(s->controller,FALSE);
    hr=s->controller->lpVtbl->QueryInterface(s->controller,&IID_ICoreWebView2Controller2,(void**)&transparent);
    if (SUCCEEDED(hr)) {
        hr=transparent->lpVtbl->put_DefaultBackgroundColor(transparent,color);
        transparent->lpVtbl->Release(transparent);
    }
    if (SUCCEEDED(hr)) hr=s->controller->lpVtbl->get_CoreWebView2(s->controller,&s->webview);
    if (FAILED(hr)) { overlay_error(s,L"transparent controller",hr);close_overlay(s);return S_OK; }
    if (SUCCEEDED(s->webview->lpVtbl->get_Settings(s->webview,&settings))) {
        settings->lpVtbl->put_IsStatusBarEnabled(settings,FALSE);
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings,FALSE);
        settings->lpVtbl->put_AreDevToolsEnabled(settings,FALSE);
        settings->lpVtbl->put_IsZoomControlEnabled(settings,FALSE);
        settings->lpVtbl->put_IsBuiltInErrorPageEnabled(settings,FALSE);
        settings->lpVtbl->put_AreDefaultScriptDialogsEnabled(settings,FALSE);
        if (SUCCEEDED(settings->lpVtbl->QueryInterface(settings,&IID_ICoreWebView2Settings3,(void**)&accelerators))) {
            accelerators->lpVtbl->put_AreBrowserAcceleratorKeysEnabled(accelerators,FALSE);
            accelerators->lpVtbl->Release(accelerators);
        }
        settings->lpVtbl->Release(settings);
    }
    if (SUCCEEDED(s->controller->lpVtbl->QueryInterface(s->controller,&IID_ICoreWebView2Controller4,(void**)&drops))) {
        drops->lpVtbl->put_AllowExternalDrop(drops,FALSE);
        drops->lpVtbl->Release(drops);
    }
    messages=callback_new(&message_vtable,&IID_ICoreWebView2WebMessageReceivedEventHandler,s);
    if (!messages) { close_overlay(s);return E_OUTOFMEMORY; }
    hr=s->webview->lpVtbl->add_WebMessageReceived(s->webview,(ICoreWebView2WebMessageReceivedEventHandler*)messages,&s->message_token);
    callback_release(messages);
    s->has_message_token=SUCCEEDED(hr);
    if (SUCCEEDED(hr)) hr=composition->lpVtbl->put_RootVisualTarget(composition,s->visual);
    if (SUCCEEDED(hr)) hr=s->device->lpVtbl->Commit(s->device);
    resize(s);
    page=load_page();
    if (!page) hr=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (SUCCEEDED(hr)) hr=s->webview->lpVtbl->NavigateToString(s->webview,page);
    free(page);
    if (FAILED(hr)) { overlay_error(s,L"content initialization",hr);close_overlay(s); }
    return S_OK;
}

static const ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandlerVtbl ctrl_vtable={
    ctrl_qi,ctrl_addref,ctrl_release,ctrl_invoke
};

static HRESULT STDMETHODCALLTYPE env_invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *self,
    HRESULT result,ICoreWebView2Environment *environment)
{
    AgentOverlay *s=((OverlayCallback*)self)->state;
    ICoreWebView2Environment3 *env3=NULL;
    OverlayCallback *cb;
    HRESULT hr;
    if (s->closed) return S_OK;
    if (FAILED(result) || !environment) { overlay_error(s,L"environment",result);return S_OK; }
    hr=environment->lpVtbl->QueryInterface(environment,&IID_ICoreWebView2Environment3,(void**)&env3);
    if (FAILED(hr)) { overlay_error(s,L"composition environment",hr);return S_OK; }
    cb=callback_new(&ctrl_vtable,&IID_ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler,s);
    if (!cb) { env3->lpVtbl->Release(env3);return E_OUTOFMEMORY; }
    hr=env3->lpVtbl->CreateCoreWebView2CompositionController(env3,s->render,
        (ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler*)cb);
    callback_release(cb);
    env3->lpVtbl->Release(env3);
    if (FAILED(hr)) overlay_error(s,L"controller creation",hr);
    return S_OK;
}

static const ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl env_vtable={
    env_qi,env_addref,env_release,env_invoke
};

AgentOverlay *agent_overlay_create(HWND render,HWND notify,UINT take_message,UINT64 vm_id)
{
    typedef HRESULT (STDAPICALLTYPE *CreateEnvironment)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    AgentOverlay *s=(AgentOverlay*)calloc(1,sizeof(*s));
    OverlayCallback *cb;
    CreateEnvironment create;
    wchar_t path[MAX_PATH],base[MAX_PATH],profile[MAX_PATH],*slash;
    HRESULT hr;
    if (!s) return NULL;
    s->refs=1;
    s->vm_id=vm_id;
    s->render=render;s->notify=notify;s->take_message=take_message;
    s->state=s->x=s->y=-1;s->finished=TRUE;s->dpi=1;
    wcscpy_s(s->context_json,ARRAYSIZE(s->context_json),L"{}");
    hr=DCompositionCreateDevice(NULL,&ASB_IID_DCOMP_DEVICE,(void**)&s->device);
    if (SUCCEEDED(hr)) hr=s->device->lpVtbl->CreateTargetForHwnd(s->device,render,TRUE,&s->target);
    if (SUCCEEDED(hr)) hr=s->device->lpVtbl->CreateVisual(s->device,&s->visual);
    if (SUCCEEDED(hr)) hr=s->target->lpVtbl->SetRoot(s->target,s->visual);
    if (SUCCEEDED(hr)) hr=s->device->lpVtbl->Commit(s->device);
    if (FAILED(hr)) { overlay_error(s,L"DirectComposition initialization",hr);agent_overlay_destroy(s);return NULL; }
    GetModuleFileNameW(NULL,path,ARRAYSIZE(path));
    slash=wcsrchr(path,L'\\');
    if (!slash) { agent_overlay_destroy(s);return NULL; }
    wcscpy_s(slash+1,ARRAYSIZE(path)-(slash+1-path),L"WebView2Loader.dll");
    AcquireSRWLockExclusive(&g_loader_lock);
    if (!g_loader) g_loader=LoadLibraryW(path);
    create=g_loader?(CreateEnvironment)GetProcAddress(g_loader,"CreateCoreWebView2EnvironmentWithOptions"):NULL;
    ReleaseSRWLockExclusive(&g_loader_lock);
    if (!create) { agent_overlay_destroy(s);return NULL; }
    if (!GetEnvironmentVariableW(L"ProgramData",base,ARRAYSIZE(base))) wcscpy_s(base,ARRAYSIZE(base),L"C:\\ProgramData");
    swprintf_s(profile,ARRAYSIZE(profile),L"%s\\AppSandbox\\AgentOverlayWebView2Data",base);
    cb=callback_new(&env_vtable,&IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,s);
    if (!cb) { agent_overlay_destroy(s);return NULL; }
    hr=create(NULL,profile,NULL,(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*)cb);
    callback_release(cb);
    if (FAILED(hr)) { overlay_error(s,L"environment creation",hr);agent_overlay_destroy(s);return NULL; }
    return s;
}

void agent_overlay_destroy(AgentOverlay *s)
{
    if (!s) return;
    close_overlay(s);
    state_release(s);
}

void agent_overlay_update(AgentOverlay *s,int state,int x,int y,UINT width,UINT height,BOOL waiting,BOOL finished)
{
    BOOL changed;
    if (!s || s->closed) return;
    changed=refresh_context(s,s->state!=state || s->finished!=!!finished);
    if (s->state==state && s->x==x && s->y==y && s->frame_width==width && s->frame_height==height &&
        s->waiting==!!waiting && s->finished==!!finished && !changed) return;
    s->state=state;s->x=x;s->y=y;s->frame_width=width;s->frame_height=height;
    s->waiting=!!waiting;s->finished=!!finished;
    update(s);
}

void agent_overlay_resize(AgentOverlay *s)
{
    if (s) resize(s);
}

BOOL agent_overlay_hit_test(AgentOverlay *s,POINT point)
{
    return s && menu_visible(s) && PtInRect(&s->menu_bounds,point);
}

void agent_overlay_pointer_activity(AgentOverlay *s)
{
    POINT point;
    if (!s || !menu_visible(s) || !GetCursorPos(&point)) return;
    if (s->pointer_known && point.x==s->pointer.x && point.y==s->pointer.y) return;
    s->pointer=point;s->pointer_known=TRUE;
    ULONGLONG now=GetTickCount64();
    if (now<s->pointer_next) return;
    s->pointer_next=now+50;
    s->webview->lpVtbl->PostWebMessageAsJson(s->webview,L"{\"type\":\"pointer_activity\"}");
}

UINT32 agent_overlay_work_id(AgentOverlay *s)
{
    if (!s) return 0;
    refresh_context(s,TRUE);
    return s->work_id;
}

BOOL agent_overlay_mouse(AgentOverlay *s,UINT message,WPARAM keys,LPARAM position)
{
    POINT point={(SHORT)LOWORD(position),(SHORT)HIWORD(position)};
    const wchar_t *kind;
    int button=-1,delta=0;
    BOOL down=FALSE,up=FALSE,hit;
    UINT bit=0;
    wchar_t json[256];
    TRACKMOUSEEVENT track;
    if (!s) return FALSE;
    switch (message) {
    case WM_MOUSEMOVE:case WM_MOUSELEAVE:case WM_LBUTTONDOWN:case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_MBUTTONDOWN:case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:break;
    default:return FALSE;
    }
    if (message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) ScreenToClient(s->render,&point);
    switch (message) {
    case WM_MOUSEMOVE:kind=L"move";break;
    case WM_LBUTTONDOWN:kind=L"down";button=0;down=TRUE;break;
    case WM_LBUTTONUP:kind=L"up";button=0;up=TRUE;break;
    case WM_RBUTTONDOWN:kind=L"down";button=2;down=TRUE;break;
    case WM_RBUTTONUP:kind=L"up";button=2;up=TRUE;break;
    case WM_MBUTTONDOWN:kind=L"down";button=1;down=TRUE;break;
    case WM_MBUTTONUP:kind=L"up";button=1;up=TRUE;break;
    case WM_MOUSEWHEEL:kind=L"wheel";delta=(SHORT)HIWORD(keys);break;
    case WM_MOUSEHWHEEL:kind=L"hwheel";delta=(SHORT)HIWORD(keys);break;
    case WM_MOUSELEAVE:leave(s);return FALSE;
    default:return FALSE;
    }
    if (button>=0) bit=1u<<button;
    hit=agent_overlay_hit_test(s,point);
    if (up && (s->mouse_buttons&bit)) {
        s->mouse_buttons&=~bit;
        if (!s->mouse_buttons && GetCapture()==s->render) ReleaseCapture();
        if (!hit) { leave(s);return TRUE; }
    } else if (!hit) { leave(s);return FALSE; }
    if (down) {
        s->mouse_buttons|=bit;
        SetCapture(s->render);
    }
    swprintf_s(json,ARRAYSIZE(json),L"{\"type\":\"input\",\"kind\":\"%s\",\"x\":%.3f,\"y\":%.3f,\"button\":%d,\"delta\":%d}",
        kind,point.x/s->dpi,point.y/s->dpi,button,delta);
    s->webview->lpVtbl->PostWebMessageAsJson(s->webview,json);
    s->hovered=TRUE;
    track.cbSize=sizeof(track);track.dwFlags=TME_LEAVE;track.hwndTrack=s->render;track.dwHoverTime=0;
    TrackMouseEvent(&track);
    return TRUE;
}

void agent_overlay_cancel_input(AgentOverlay *s)
{
    if (!s) return;
    leave(s);
    s->mouse_buttons=0;
    if (GetCapture()==s->render) ReleaseCapture();
    if (s->webview) s->webview->lpVtbl->PostWebMessageAsJson(s->webview,L"{\"type\":\"input\",\"kind\":\"cancel\"}");
}
