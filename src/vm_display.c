/*
 * vm_display.c — VM display via RDP ActiveX control over named pipe.
 *
 * Uses RdpBase.dll (undocumented) for named-pipe transport and
 * mstscax.dll (standard RDP ActiveX) hosted via ATL (atl.dll).
 * Uses undocumented RDP COM interfaces for named-pipe transport.
 */

#define COBJMACROS
#include <initguid.h>
#include <cguid.h>
#include "vm_display.h"
#include "ui.h"
#include <ole2.h>
#include <oleauto.h>
#include <ocidl.h>   /* IConnectionPointContainer */
#include <stdio.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* ---- GUIDs ---- */

DEFINE_GUID(CLSID_MsRdpClient10NotSafeForScripting,
    0xA0C63C30, 0xF08D, 0x4AB4, 0x90, 0x7C, 0x34, 0x90, 0x5D, 0x77, 0x0C, 0x7D);

DEFINE_GUID(CLSID_RDPRuntimeSTAContext,
    0xFB332AE7, 0x0055, 0x4208, 0x92, 0xB7, 0x20, 0x41, 0x0C, 0xA8, 0x38, 0x2B);

DEFINE_GUID(IID_IRDPENCPlatformContext,
    0xFB332AE7, 0x000E, 0x4208, 0x92, 0xB7, 0x20, 0x41, 0x0C, 0xA8, 0x38, 0x2B);

DEFINE_GUID(CLSID_RDPENCNamedPipeDirectConnector,
    0xFB332AE7, 0x0088, 0x4208, 0x92, 0xB7, 0x20, 0x41, 0x0C, 0xA8, 0x38, 0x2B);

DEFINE_GUID(IID_IRDPENCNamedPipeDirectConnector,
    0x4ACF942D, 0xEADC, 0x45BF, 0x8E, 0xA8, 0x79, 0x3F, 0xE3, 0xCE, 0x31, 0xE8);

DEFINE_GUID(IID_IRDPENCNamedPipeDirectConnectorCallbacks,
    0xD923EFE9, 0x0A6D, 0x4344, 0x92, 0xB3, 0x16, 0x42, 0x29, 0xDB, 0x8D, 0x2D);

/* ==================================================================
 * RdpBase.dll C vtable definitions (undocumented COM interfaces)
 * ================================================================== */

typedef struct IRDPENCCallbacksC IRDPENCCallbacksC;
typedef struct IRDPENCConnectorC IRDPENCConnectorC;
typedef struct IRDPENCPlatformC  IRDPENCPlatformC;

/* -- Named pipe connector callbacks -- */

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IRDPENCCallbacksC *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(IRDPENCCallbacksC *This);
    ULONG (STDMETHODCALLTYPE *Release)(IRDPENCCallbacksC *This);
    void (STDMETHODCALLTYPE *OnConnectionCompleted)(
        IRDPENCCallbacksC *This, IUnknown *pNetStream);
    void (STDMETHODCALLTYPE *OnConnectorError)(
        IRDPENCCallbacksC *This, HRESULT hrError);
} IRDPENCCallbacksCVtbl;

struct IRDPENCCallbacksC {
    const IRDPENCCallbacksCVtbl *lpVtbl;
};

/* -- Named pipe connector -- */

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IRDPENCConnectorC *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(IRDPENCConnectorC *This);
    ULONG (STDMETHODCALLTYPE *Release)(IRDPENCConnectorC *This);
    HRESULT (STDMETHODCALLTYPE *InitializeInstance)(
        IRDPENCConnectorC *This, IRDPENCCallbacksC *pCallback);
    HRESULT (STDMETHODCALLTYPE *TerminateInstance)(
        IRDPENCConnectorC *This);
    HRESULT (STDMETHODCALLTYPE *StartConnect)(
        IRDPENCConnectorC *This, LPCWSTR szPipeName);
} IRDPENCConnectorCVtbl;

struct IRDPENCConnectorC {
    const IRDPENCConnectorCVtbl *lpVtbl;
};

/* -- Platform context -- */

typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
        IRDPENCPlatformC *This, REFIID riid, void **ppv);
    ULONG (STDMETHODCALLTYPE *AddRef)(IRDPENCPlatformC *This);
    ULONG (STDMETHODCALLTYPE *Release)(IRDPENCPlatformC *This);
    HRESULT (STDMETHODCALLTYPE *GetProperty)(
        IRDPENCPlatformC *This, LPCWSTR name, VARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *GetPropertyBool)(
        IRDPENCPlatformC *This, LPCWSTR name, BOOL *pf);
    HRESULT (STDMETHODCALLTYPE *GetPropertyUnsignedLong)(
        IRDPENCPlatformC *This, LPCWSTR name, UINT *pul);
    HRESULT (STDMETHODCALLTYPE *GetPropertyInterface)(
        IRDPENCPlatformC *This, LPCWSTR name, REFIID riid, void **pp);
    HRESULT (STDMETHODCALLTYPE *SetProperty)(
        IRDPENCPlatformC *This, LPCWSTR name, VARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *SetPropertyBool)(
        IRDPENCPlatformC *This, LPCWSTR name, BOOL f);
    HRESULT (STDMETHODCALLTYPE *SetPropertyUnsignedLong)(
        IRDPENCPlatformC *This, LPCWSTR name, UINT ul);
    HRESULT (STDMETHODCALLTYPE *SetPropertyInterface)(
        IRDPENCPlatformC *This, LPCWSTR name, IUnknown *p);
    HRESULT (STDMETHODCALLTYPE *SetPropertyIfNotPresent)(
        IRDPENCPlatformC *This, LPCWSTR name, VARIANT *pValue);
    HRESULT (STDMETHODCALLTYPE *InitializeInstance)(IRDPENCPlatformC *This);
    HRESULT (STDMETHODCALLTYPE *TerminateInstance)(IRDPENCPlatformC *This);
} IRDPENCPlatformCVtbl;

struct IRDPENCPlatformC {
    const IRDPENCPlatformCVtbl *lpVtbl;
};

/* ==================================================================
 * DLL dynamic loading
 * ================================================================== */

typedef HRESULT (WINAPI *PFN_RDPBASE_CreateInstance)(
    IUnknown *pCtx, REFCLSID rclsid, REFIID riid, void **ppv);
typedef BOOL (WINAPI *PFN_AtlAxWinInit)(void);
typedef HRESULT (WINAPI *PFN_AtlAxAttachControl)(
    IUnknown *pControl, HWND hWnd, IUnknown **ppContainer);

static HMODULE g_rdpbase;
static HMODULE g_atl;
static PFN_RDPBASE_CreateInstance pfnRdpBaseCreate;
static PFN_AtlAxWinInit          pfnAtlAxWinInit;
static PFN_AtlAxAttachControl    pfnAtlAxAttachControl;
static BOOL g_dlls_loaded;

static BOOL load_dlls(void)
{
    if (g_dlls_loaded) return TRUE;

    g_rdpbase = LoadLibraryW(L"RdpBase.dll");
    if (!g_rdpbase) {
        ui_log(L"Error: Failed to load RdpBase.dll (0x%08X)", GetLastError());
        return FALSE;
    }
    pfnRdpBaseCreate = (PFN_RDPBASE_CreateInstance)
        GetProcAddress(g_rdpbase, "RDPBASE_CreateInstance");
    if (!pfnRdpBaseCreate) {
        ui_log(L"Error: RDPBASE_CreateInstance export not found");
        return FALSE;
    }

    g_atl = LoadLibraryW(L"atl.dll");
    if (!g_atl) {
        ui_log(L"Error: Failed to load atl.dll (0x%08X)", GetLastError());
        return FALSE;
    }
    pfnAtlAxWinInit = (PFN_AtlAxWinInit)
        GetProcAddress(g_atl, "AtlAxWinInit");
    pfnAtlAxAttachControl = (PFN_AtlAxAttachControl)
        GetProcAddress(g_atl, "AtlAxAttachControl");
    if (!pfnAtlAxWinInit || !pfnAtlAxAttachControl) {
        ui_log(L"Error: ATL exports not found in atl.dll");
        return FALSE;
    }

    pfnAtlAxWinInit();
    g_dlls_loaded = TRUE;
    return TRUE;
}

/* ==================================================================
 * IDispatch helpers for late-bound calls to the RDP ActiveX control
 * ================================================================== */

static HRESULT disp_get(IDispatch *obj, const wchar_t *name, VARIANT *result)
{
    DISPID id;
    OLECHAR *n = (OLECHAR *)name;
    DISPPARAMS dp;
    HRESULT hr;

    hr = IDispatch_GetIDsOfNames(obj, &IID_NULL, &n, 1,
                                  LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    ZeroMemory(&dp, sizeof(dp));
    return IDispatch_Invoke(obj, id, &IID_NULL, LOCALE_USER_DEFAULT,
                            DISPATCH_PROPERTYGET, &dp, result, NULL, NULL);
}

static HRESULT disp_put(IDispatch *obj, const wchar_t *name, VARIANT *val)
{
    DISPID id, named = DISPID_PROPERTYPUT;
    OLECHAR *n = (OLECHAR *)name;
    DISPPARAMS dp;
    HRESULT hr;

    hr = IDispatch_GetIDsOfNames(obj, &IID_NULL, &n, 1,
                                  LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    dp.rgvarg            = val;
    dp.rgdispidNamedArgs = &named;
    dp.cArgs             = 1;
    dp.cNamedArgs        = 1;
    return IDispatch_Invoke(obj, id, &IID_NULL, LOCALE_USER_DEFAULT,
                            DISPATCH_PROPERTYPUT, &dp, NULL, NULL, NULL);
}

static HRESULT disp_put_long(IDispatch *obj, const wchar_t *name, LONG v)
{
    VARIANT var;
    VariantInit(&var);
    var.vt   = VT_I4;
    var.lVal = v;
    return disp_put(obj, name, &var);
}

static HRESULT disp_call(IDispatch *obj, const wchar_t *name)
{
    DISPID id;
    OLECHAR *n = (OLECHAR *)name;
    DISPPARAMS dp;
    HRESULT hr;

    hr = IDispatch_GetIDsOfNames(obj, &IID_NULL, &n, 1,
                                  LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    ZeroMemory(&dp, sizeof(dp));
    return IDispatch_Invoke(obj, id, &IID_NULL, LOCALE_USER_DEFAULT,
                            DISPATCH_METHOD, &dp, NULL, NULL, NULL);
}

/* ==================================================================
 * Our COM callback object for the named-pipe connector
 * ================================================================== */

typedef struct PipeCallbacks {
    const IRDPENCCallbacksCVtbl *lpVtbl;
    LONG              refCount;
    struct VmDisplay  *display;
} PipeCallbacks;

/* Forward declarations of callback methods */
static HRESULT STDMETHODCALLTYPE CB_QI(
    IRDPENCCallbacksC *This, REFIID riid, void **ppv);
static ULONG   STDMETHODCALLTYPE CB_AddRef(IRDPENCCallbacksC *This);
static ULONG   STDMETHODCALLTYPE CB_Release(IRDPENCCallbacksC *This);
static void    STDMETHODCALLTYPE CB_OnConnectionCompleted(
    IRDPENCCallbacksC *This, IUnknown *pNetStream);
static void    STDMETHODCALLTYPE CB_OnConnectorError(
    IRDPENCCallbacksC *This, HRESULT hrError);

static const IRDPENCCallbacksCVtbl g_cb_vtbl = {
    CB_QI, CB_AddRef, CB_Release,
    CB_OnConnectionCompleted, CB_OnConnectorError
};

/* ==================================================================
 * RDP ActiveX event sink (IMsTscAxEvents via IDispatch)
 * ================================================================== */

/* DIID_IMsTscAxEvents — event dispinterface for MsRdpClient */
DEFINE_GUID(DIID_IMsTscAxEvents,
    0x336D5562, 0xEFA8, 0x482E, 0x8C, 0xB3, 0xC5, 0xC0, 0xFC, 0x7A, 0x7D, 0xB6);

/* Event DISPIDs from mstscax.idl */
#define CYCDD_OnConnecting      4
#define CYCDD_OnConnected        1
#define CYCDD_OnDisconnected     2
#define CYCDD_OnLoginComplete   14
#define CYCDD_OnLogonError      19
#define CYCDD_OnFatalError      22

typedef struct RdpEventSink RdpEventSink;
struct VmDisplay;  /* forward */

struct RdpEventSink {
    IDispatch          iface;  /* must be first */
    LONG               refCount;
    struct VmDisplay  *display;
};

static HRESULT STDMETHODCALLTYPE Sink_QI(IDispatch *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDispatch) ||
        IsEqualIID(riid, &DIID_IMsTscAxEvents)) {
        *ppv = This;
        IDispatch_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Sink_AddRef(IDispatch *This)
{
    return InterlockedIncrement(&((RdpEventSink *)This)->refCount);
}

static ULONG STDMETHODCALLTYPE Sink_Release(IDispatch *This)
{
    RdpEventSink *s = (RdpEventSink *)This;
    LONG ref = InterlockedDecrement(&s->refCount);
    if (ref == 0)
        HeapFree(GetProcessHeap(), 0, s);
    return (ULONG)ref;
}

static HRESULT STDMETHODCALLTYPE Sink_GetTypeInfoCount(IDispatch *This, UINT *p)
{
    (void)This; *p = 0; return S_OK;
}

static HRESULT STDMETHODCALLTYPE Sink_GetTypeInfo(IDispatch *This, UINT i, LCID l, ITypeInfo **pp)
{
    (void)This; (void)i; (void)l; *pp = NULL; return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE Sink_GetIDsOfNames(IDispatch *This, REFIID r,
    LPOLESTR *names, UINT cnt, LCID l, DISPID *ids)
{
    (void)This; (void)r; (void)names; (void)cnt; (void)l; (void)ids;
    return E_NOTIMPL;
}

/* Sink_Invoke — forward declaration (body after struct VmDisplay) */
static HRESULT STDMETHODCALLTYPE Sink_Invoke(IDispatch *This, DISPID id,
    REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pParams,
    VARIANT *pResult, EXCEPINFO *pExcep, UINT *puArgErr);

static const IDispatchVtbl g_sink_vtbl = {
    Sink_QI, Sink_AddRef, Sink_Release,
    Sink_GetTypeInfoCount, Sink_GetTypeInfo, Sink_GetIDsOfNames,
    Sink_Invoke
};

/* ==================================================================
 * Display context
 * ================================================================== */

#define IDC_BTN_SESSION_TOGGLE 5001
#define TOOLBAR_HEIGHT 30
/* Private messages for display thread */
#define WM_DISPLAY_ENHANCE    (WM_USER + 1)
#define WM_DISPLAY_DISCONNECT (WM_USER + 2)

#define IDT_RDP_POLL      1001
#define RDP_POLL_MS       1000
struct VmDisplay {
    HWND                hwnd;          /* main display window           */
    HWND                ax_hwnd;       /* child hosting ActiveX control */
    HWND                btn_toggle;    /* session mode toggle button    */
    HWND                main_hwnd;     /* parent app window for notifications */
    volatile BOOL       running;
    BOOL                enhanced_mode; /* TRUE = EnhancedSession pipe   */
    LONG                last_connected; /* Last polled Connected state   */
    BOOL                pipe_callback_fired; /* TRUE after OnConnected or OnError */
    ULONGLONG           connect_start_tick;  /* GetTickCount64() when StartConnect called */
    BOOL                pipe_timeout_logged;  /* Only log timeout once */
    VmInstance         *vm;
    HINSTANCE           hInstance;
    BOOL                ole_inited;

    /* Display thread */
    HANDLE              thread;        /* dedicated thread handle       */

    /* COM objects (owned by display thread) */
    IUnknown           *rdp_control;   /* MsRdpClient10 IUnknown  */
    IDispatch          *rdp_dispatch;  /* MsRdpClient10 IDispatch */
    IUnknown           *ax_container;  /* ATL host container       */
    IRDPENCPlatformC   *platform_ctx;
    IRDPENCConnectorC  *connector;
    PipeCallbacks      *callbacks;
    RdpEventSink       *event_sink;    /* IDispatch event sink     */
    DWORD               event_cookie;  /* IConnectionPoint cookie  */
};

/* ==================================================================
 * RDP event sink — Invoke + advise/unadvise helpers
 * (placed after struct VmDisplay so the full type is visible)
 * ================================================================== */

static HRESULT STDMETHODCALLTYPE Sink_Invoke(IDispatch *This, DISPID id,
    REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pParams,
    VARIANT *pResult, EXCEPINFO *pExcep, UINT *puArgErr)
{
    RdpEventSink *s = (RdpEventSink *)This;
    VmDisplay *d = s->display;
    (void)riid; (void)lcid; (void)wFlags; (void)pResult; (void)pExcep; (void)puArgErr;

    switch (id) {
    case CYCDD_OnConnecting:
        if (d && d->vm)
            ui_log(L"RDP event: Connecting \"%s\" (%s)",
                d->vm->name, d->enhanced_mode ? L"Enhanced" : L"Basic");
        break;
    case CYCDD_OnConnected:
        if (d && d->vm) {
            ui_log(L"RDP event: Connected \"%s\" (%s)",
                d->vm->name, d->enhanced_mode ? L"Enhanced" : L"Basic");
        }
        break;
    case CYCDD_OnDisconnected:
        if (d && d->vm) {
            LONG reason = 0;
            if (pParams && pParams->cArgs >= 1 && pParams->rgvarg[0].vt == VT_I4)
                reason = pParams->rgvarg[0].lVal;
            ui_log(L"RDP event: Disconnected \"%s\" (%s), reason=%ld",
                d->vm->name, d->enhanced_mode ? L"Enhanced" : L"Basic", reason);

            if (d->rdp_dispatch) {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(disp_get(d->rdp_dispatch, L"ExtendedDisconnectReason", &v))) {
                    LONG ext = (v.vt == VT_I4) ? v.lVal : (v.vt == VT_I2) ? v.iVal : -1;
                    ui_log(L"  ExtendedDisconnectReason: %ld", ext);
                }
                VariantClear(&v);
            }

        }
        break;
    case CYCDD_OnLoginComplete:
        if (d && d->vm)
            ui_log(L"RDP event: LoginComplete \"%s\"", d->vm->name);
        break;
    case CYCDD_OnLogonError:
        if (d && d->vm) {
            LONG err = 0;
            if (pParams && pParams->cArgs >= 1 && pParams->rgvarg[0].vt == VT_I4)
                err = pParams->rgvarg[0].lVal;
            ui_log(L"RDP event: LogonError \"%s\", code=%ld", d->vm->name, err);
        }
        break;
    case CYCDD_OnFatalError:
        if (d && d->vm) {
            LONG err = 0;
            if (pParams && pParams->cArgs >= 1 && pParams->rgvarg[0].vt == VT_I4)
                err = pParams->rgvarg[0].lVal;
            (void)err; /* FatalError is expected during RDP negotiation */
        }
        break;
    }
    return S_OK;
}

/* Create and advise an event sink on the RDP control.
   Returns the IConnectionPoint cookie (0 on failure). */
static DWORD rdp_advise_events(IUnknown *rdp_control, VmDisplay *display,
                                RdpEventSink **out_sink)
{
    IConnectionPointContainer *cpc = NULL;
    IConnectionPoint *cp = NULL;
    RdpEventSink *sink = NULL;
    DWORD cookie = 0;
    HRESULT hr;

    hr = IUnknown_QueryInterface(rdp_control, &IID_IConnectionPointContainer, (void **)&cpc);
    if (FAILED(hr)) {
        ui_log(L"RDP events: QI IConnectionPointContainer failed (0x%08X)", hr);
        return 0;
    }

    hr = IConnectionPointContainer_FindConnectionPoint(cpc, &DIID_IMsTscAxEvents, &cp);
    IConnectionPointContainer_Release(cpc);
    if (FAILED(hr)) {
        ui_log(L"RDP events: FindConnectionPoint failed (0x%08X)", hr);
        return 0;
    }

    sink = (RdpEventSink *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RdpEventSink));
    if (!sink) { IConnectionPoint_Release(cp); return 0; }
    sink->iface.lpVtbl = (IDispatchVtbl *)&g_sink_vtbl;
    sink->refCount = 1;
    sink->display = display;

    hr = IConnectionPoint_Advise(cp, (IUnknown *)&sink->iface, &cookie);
    IConnectionPoint_Release(cp);
    if (FAILED(hr)) {
        ui_log(L"RDP events: Advise failed (0x%08X)", hr);
        HeapFree(GetProcessHeap(), 0, sink);
        return 0;
    }

    *out_sink = sink;
    ui_log(L"RDP events: sink advised (cookie=%lu)", cookie);
    return cookie;
}

/* Unadvise and release the event sink. */
static void rdp_unadvise_events(IUnknown *rdp_control, DWORD cookie, RdpEventSink *sink)
{
    if (rdp_control && cookie) {
        IConnectionPointContainer *cpc = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(rdp_control, &IID_IConnectionPointContainer, (void **)&cpc))) {
            IConnectionPoint *cp = NULL;
            if (SUCCEEDED(IConnectionPointContainer_FindConnectionPoint(cpc, &DIID_IMsTscAxEvents, &cp))) {
                IConnectionPoint_Unadvise(cp, cookie);
                IConnectionPoint_Release(cp);
            }
            IConnectionPointContainer_Release(cpc);
        }
    }
    if (sink)
        IDispatch_Release(&sink->iface);
}

/* ---- Window class ---- */

static const wchar_t *DISPLAY_CLASS = L"AppSandbox_VmDisplay";
static BOOL g_class_registered;

static LRESULT CALLBACK display_wnd_proc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void ensure_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    if (g_class_registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = display_wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = DISPLAY_CLASS;
    RegisterClassExW(&wc);
    g_class_registered = TRUE;
}

/* ==================================================================
 * Callback implementations
 * ================================================================== */

static HRESULT STDMETHODCALLTYPE CB_QI(
    IRDPENCCallbacksC *This, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IRDPENCNamedPipeDirectConnectorCallbacks)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE CB_AddRef(IRDPENCCallbacksC *This)
{
    return InterlockedIncrement(&((PipeCallbacks *)This)->refCount);
}

static ULONG STDMETHODCALLTYPE CB_Release(IRDPENCCallbacksC *This)
{
    PipeCallbacks *cb = (PipeCallbacks *)This;
    LONG ref = InterlockedDecrement(&cb->refCount);
    if (ref == 0)
        HeapFree(GetProcessHeap(), 0, cb);
    return (ULONG)ref;
}

static void STDMETHODCALLTYPE CB_OnConnectionCompleted(
    IRDPENCCallbacksC *This, IUnknown *pNetStream)
{
    PipeCallbacks *cb = (PipeCallbacks *)This;
    VmDisplay *d      = cb->display;
    VARIANT var_adv, var_ep;
    IDispatch *adv = NULL;
    HRESULT hr;

    d->pipe_callback_fired = TRUE;
    ui_log(L"RDP pipe connected for \"%s\" (pNetStream=%p)", d->vm->name, (void *)pNetStream);

    if (!d->rdp_dispatch) {
        ui_log(L"RDP pipe connected but rdp_dispatch is NULL — cannot proceed");
        return;
    }

    /* 1. Get AdvancedSettings from the RDP control */
    VariantInit(&var_adv);
    hr = disp_get(d->rdp_dispatch, L"AdvancedSettings9", &var_adv);
    if (FAILED(hr))
        hr = disp_get(d->rdp_dispatch, L"AdvancedSettings8", &var_adv);
    if (FAILED(hr)) {
        ui_log(L"Error: get AdvancedSettings failed (0x%08X)", hr);
        return;
    }

    /* Extract IDispatch from the returned VARIANT */
    if (var_adv.vt == VT_DISPATCH && var_adv.pdispVal) {
        adv = var_adv.pdispVal;
    } else if (var_adv.vt == VT_UNKNOWN && var_adv.punkVal) {
        hr = IUnknown_QueryInterface(var_adv.punkVal, &IID_IDispatch,
                                      (void **)&adv);
        IUnknown_Release(var_adv.punkVal);
        if (FAILED(hr)) {
            ui_log(L"Error: AdvancedSettings QI IDispatch failed (0x%08X)", hr);
            return;
        }
    } else {
        ui_log(L"Error: AdvancedSettings unexpected vt=0x%04X", var_adv.vt);
        VariantClear(&var_adv);
        return;
    }

    /* 2. Set ConnectWithEndpoint = pNetStream */
    VariantInit(&var_ep);
    var_ep.vt      = VT_UNKNOWN;
    var_ep.punkVal = pNetStream;

    hr = disp_put(adv, L"ConnectWithEndpoint", &var_ep);
    if (FAILED(hr)) {
        ui_log(L"Error: put ConnectWithEndpoint failed (0x%08X)", hr);
        IDispatch_Release(adv);
        return;
    }
    IDispatch_Release(adv);

    /* 3. Call Connect() on the RDP control */
    hr = disp_call(d->rdp_dispatch, L"Connect");
    if (FAILED(hr)) {
        ui_log(L"Error: RDP Connect() failed (0x%08X)", hr);
    } else {
        VARIANT v;

        ui_log(L"RDP session connecting for \"%s\" (%s)...",
            d->vm->name,
            d->enhanced_mode ? L"Enhanced" : L"Basic");



        /* Log connected status text */
        VariantInit(&v);
        if (SUCCEEDED(disp_get(d->rdp_dispatch, L"ConnectedStatusText", &v))
            && v.vt == VT_BSTR && v.bstrVal && v.bstrVal[0])
            ui_log(L"  StatusText: %s", v.bstrVal);
        VariantClear(&v);
    }
}

static void STDMETHODCALLTYPE CB_OnConnectorError(
    IRDPENCCallbacksC *This, HRESULT hrError)
{
    PipeCallbacks *cb = (PipeCallbacks *)This;
    VmDisplay *d = cb->display;
    d->pipe_callback_fired = TRUE;
    ui_log(L"RDP pipe error for \"%s\" (%s): 0x%08X",
        d->vm->name,
        d->enhanced_mode ? L"Enhanced" : L"Basic",
        hrError);

    /* Log RDP connection state and disconnect reason */
    if (d->rdp_dispatch) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(disp_get(d->rdp_dispatch, L"Connected", &v))) {
            LONG state = (v.vt == VT_I4) ? v.lVal : (v.vt == VT_I2) ? v.iVal : -1;
            ui_log(L"  Connected state: %ld", state);
        }
        VariantClear(&v);

        VariantInit(&v);
        if (SUCCEEDED(disp_get(d->rdp_dispatch, L"ExtendedDisconnectReason", &v))) {
            LONG reason = (v.vt == VT_I4) ? v.lVal : (v.vt == VT_I2) ? v.iVal : -1;
            ui_log(L"  ExtendedDisconnectReason: %ld", reason);
        }
        VariantClear(&v);
    }

    if (d->enhanced_mode)
        ui_log(L"VM \"%s\" disconnected from enhanced session.", d->vm->name);
}

/* ==================================================================
 * Session mode switching
 * ================================================================== */

/* Fully tear down and rebuild the RDP ActiveX control + pipe connector.
   The RDP control can't be reused after disconnect. */
static void display_reconnect(VmDisplay *d)
{
    wchar_t pipe_path[512];
    HRESULT hr;
    RECT rc;

    /* Reset state tracking for fresh control */
    d->last_connected = 0;

    /* 1. Unadvise event sink before tearing down control */
    if (d->rdp_control && d->event_cookie) {
        rdp_unadvise_events(d->rdp_control, d->event_cookie, d->event_sink);
        d->event_sink = NULL;
        d->event_cookie = 0;
    }

    /* 2. Disconnect the active RDP session */
    if (d->rdp_dispatch)
        disp_call(d->rdp_dispatch, L"Disconnect");

    /* 3. Tear down pipe connector */
    if (d->connector) {
        d->connector->lpVtbl->TerminateInstance(d->connector);
        d->connector->lpVtbl->Release(d->connector);
        d->connector = NULL;
    }
    if (d->callbacks) {
        IRDPENCCallbacksC *cb = (IRDPENCCallbacksC *)d->callbacks;
        cb->lpVtbl->Release(cb);
        d->callbacks = NULL;
    }

    /* 3. Tear down RDP ActiveX control */
    if (d->rdp_dispatch) { IDispatch_Release(d->rdp_dispatch); d->rdp_dispatch = NULL; }
    if (d->ax_container) { IUnknown_Release(d->ax_container); d->ax_container = NULL; }
    if (d->rdp_control)  { IUnknown_Release(d->rdp_control); d->rdp_control = NULL; }

    /* 4. Tear down platform context */
    if (d->platform_ctx) {
        d->platform_ctx->lpVtbl->TerminateInstance(d->platform_ctx);
        d->platform_ctx->lpVtbl->Release(d->platform_ctx);
        d->platform_ctx = NULL;
    }

    /* 5. Destroy and recreate the ActiveX host window (clean slate) */
    if (d->ax_hwnd) DestroyWindow(d->ax_hwnd);
    GetClientRect(d->hwnd, &rc);
    d->ax_hwnd = CreateWindowExW(
        0, L"Static", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, TOOLBAR_HEIGHT, rc.right, rc.bottom - TOOLBAR_HEIGHT,
        d->hwnd, NULL, d->hInstance, NULL);

    /* 6. Create fresh MsRdpClient10 */
    hr = CoCreateInstance(&CLSID_MsRdpClient10NotSafeForScripting, NULL,
        CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&d->rdp_control);
    if (FAILED(hr)) {
        ui_log(L"Error: CoCreateInstance MsRdpClient10 failed (0x%08X)", hr);
        return;
    }

    hr = pfnAtlAxAttachControl(d->rdp_control, d->ax_hwnd, &d->ax_container);
    if (FAILED(hr)) {
        ui_log(L"Error: AtlAxAttachControl failed (0x%08X)", hr);
        return;
    }

    hr = IUnknown_QueryInterface(d->rdp_control, &IID_IDispatch,
                                  (void **)&d->rdp_dispatch);
    if (FAILED(hr)) {
        ui_log(L"Error: RDP control QI IDispatch failed (0x%08X)", hr);
        return;
    }

    disp_put_long(d->rdp_dispatch, L"AuthenticationLevel", 0);

    /* Advise event sink for disconnect notifications */
    d->event_cookie = rdp_advise_events(d->rdp_control, d, &d->event_sink);

    /* 7. Create fresh platform context */
    hr = pfnRdpBaseCreate(NULL, &CLSID_RDPRuntimeSTAContext,
        &IID_IRDPENCPlatformContext, (void **)&d->platform_ctx);
    if (FAILED(hr)) {
        ui_log(L"Error: RDP platform context failed (0x%08X)", hr);
        return;
    }
    hr = d->platform_ctx->lpVtbl->InitializeInstance(d->platform_ctx);
    if (FAILED(hr)) {
        ui_log(L"Error: Platform ctx init failed (0x%08X)", hr);
        return;
    }

    /* 8. Create fresh pipe connector */
    hr = pfnRdpBaseCreate((IUnknown *)d->platform_ctx,
        &CLSID_RDPENCNamedPipeDirectConnector,
        &IID_IRDPENCNamedPipeDirectConnector, (void **)&d->connector);
    if (FAILED(hr)) {
        ui_log(L"Error: Pipe connector creation failed (0x%08X)", hr);
        return;
    }

    d->callbacks = (PipeCallbacks *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PipeCallbacks));
    if (!d->callbacks) return;

    d->callbacks->lpVtbl   = &g_cb_vtbl;
    d->callbacks->refCount = 1;
    d->callbacks->display  = d;

    hr = d->connector->lpVtbl->InitializeInstance(
        d->connector, (IRDPENCCallbacksC *)d->callbacks);
    if (FAILED(hr)) {
        ui_log(L"Error: Connector init failed (0x%08X)", hr);
        return;
    }

    /* 7. Connect to the selected pipe */
    swprintf_s(pipe_path, 512, L"\\\\.\\pipe\\%s.%s",
        d->vm->name,
        d->enhanced_mode ? L"EnhancedSession" : L"BasicSession");

    hr = d->connector->lpVtbl->StartConnect(d->connector, pipe_path);
    if (FAILED(hr))
        ui_log(L"Error: StartConnect failed (0x%08X) for %s", hr, pipe_path);
    else
        ui_log(L"RDP reconnecting via %s...", pipe_path);

    /* Update button text */
    SetWindowTextW(d->btn_toggle,
        d->enhanced_mode ? L"Basic Session" : L"Enhanced Session");
}

/* ==================================================================
 * Display thread — owns the window, ActiveX control, and all COM objects.
 * The main UI thread communicates via PostMessage.
 * ================================================================== */

static DWORD WINAPI display_thread_proc(LPVOID param)
{
    VmDisplay *d = (VmDisplay *)param;
    wchar_t title[300], pipe_path[512];
    HRESULT hr;
    RECT rc;
    MSG msg;

    /* Initialize COM/OLE for this thread (STA required for ActiveX) */
    hr = OleInitialize(NULL);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        ui_log(L"Error: OleInitialize failed on display thread (0x%08X)", hr);
        d->running = FALSE;
        return 1;
    }
    d->ole_inited = SUCCEEDED(hr);

    if (!load_dlls()) {
        d->running = FALSE;
        if (d->ole_inited) OleUninitialize();
        return 1;
    }
    ensure_class(d->hInstance);

    swprintf_s(title, 300, L"%s - Display", d->vm->name);

    /* ---- Create main display window ---- */

    d->hwnd = CreateWindowExW(
        0, DISPLAY_CLASS, title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
        NULL, NULL, d->hInstance, d);

    if (!d->hwnd) {
        ui_log(L"Error: CreateWindowEx failed for display");
        goto fail;
    }

    /* ---- Toolbar with session toggle button ---- */

    GetClientRect(d->hwnd, &rc);
    d->btn_toggle = CreateWindowExW(0, L"BUTTON", L"Enhanced Session",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        4, 2, 150, TOOLBAR_HEIGHT - 4,
        d->hwnd, (HMENU)(INT_PTR)IDC_BTN_SESSION_TOGGLE, d->hInstance, NULL);

    /* ---- Child window to host the ActiveX RDP control ---- */

    d->ax_hwnd = CreateWindowExW(
        0, L"Static", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, TOOLBAR_HEIGHT, rc.right, rc.bottom - TOOLBAR_HEIGHT,
        d->hwnd, NULL, d->hInstance, NULL);

    if (!d->ax_hwnd) {
        ui_log(L"Error: CreateWindowEx failed for ActiveX host");
        goto fail;
    }

    /* ---- Create MsRdpClient10 ActiveX control ---- */

    hr = CoCreateInstance(&CLSID_MsRdpClient10NotSafeForScripting, NULL,
        CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&d->rdp_control);
    if (FAILED(hr)) {
        ui_log(L"Error: CoCreateInstance MsRdpClient10 failed (0x%08X)", hr);
        goto fail;
    }

    hr = pfnAtlAxAttachControl(d->rdp_control, d->ax_hwnd, &d->ax_container);
    if (FAILED(hr)) {
        ui_log(L"Error: AtlAxAttachControl failed (0x%08X)", hr);
        goto fail;
    }

    hr = IUnknown_QueryInterface(d->rdp_control, &IID_IDispatch,
                                  (void **)&d->rdp_dispatch);
    if (FAILED(hr)) {
        ui_log(L"Error: RDP control QI IDispatch failed (0x%08X)", hr);
        goto fail;
    }

    disp_put_long(d->rdp_dispatch, L"AuthenticationLevel", 0);

    /* Advise event sink for disconnect notifications */
    d->event_cookie = rdp_advise_events(d->rdp_control, d, &d->event_sink);

    /* ---- RdpBase: platform context ---- */

    hr = pfnRdpBaseCreate(NULL, &CLSID_RDPRuntimeSTAContext,
        &IID_IRDPENCPlatformContext, (void **)&d->platform_ctx);
    if (FAILED(hr)) {
        ui_log(L"Error: RDP platform context failed (0x%08X)", hr);
        goto show_window;
    }

    hr = d->platform_ctx->lpVtbl->InitializeInstance(d->platform_ctx);
    if (FAILED(hr)) {
        ui_log(L"Error: Platform ctx init failed (0x%08X)", hr);
        goto show_window;
    }

    /* ---- RdpBase: named pipe connector ---- */

    hr = pfnRdpBaseCreate((IUnknown *)d->platform_ctx,
        &CLSID_RDPENCNamedPipeDirectConnector,
        &IID_IRDPENCNamedPipeDirectConnector, (void **)&d->connector);
    if (FAILED(hr)) {
        ui_log(L"Error: Pipe connector creation failed (0x%08X)", hr);
        goto show_window;
    }

    d->callbacks = (PipeCallbacks *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PipeCallbacks));
    if (!d->callbacks) goto show_window;

    d->callbacks->lpVtbl    = &g_cb_vtbl;
    d->callbacks->refCount  = 1;
    d->callbacks->display   = d;

    hr = d->connector->lpVtbl->InitializeInstance(
        d->connector, (IRDPENCCallbacksC *)d->callbacks);
    if (FAILED(hr)) {
        ui_log(L"Error: Connector init failed (0x%08X)", hr);
        goto show_window;
    }

    /* Start connecting to the VM's RDP pipe */
    swprintf_s(pipe_path, 512, L"\\\\.\\pipe\\%s.%s", d->vm->name,
        d->enhanced_mode ? L"EnhancedSession" : L"BasicSession");

    /* Probe pipe existence without consuming the connection.
       WaitNamedPipeW(path, 0) returns immediately: TRUE = pipe exists, FALSE = not found. */
    if (WaitNamedPipeW(pipe_path, 0))
        ui_log(L"RDP pipe exists: %s", pipe_path);
    else
        ui_log(L"RDP pipe NOT found: %s (error %lu)", pipe_path, GetLastError());

    d->pipe_callback_fired = FALSE;
    d->pipe_timeout_logged = FALSE;
    d->connect_start_tick = GetTickCount64();

    hr = d->connector->lpVtbl->StartConnect(d->connector, pipe_path);
    if (FAILED(hr))
        ui_log(L"Error: StartConnect failed (0x%08X) for %s", hr, pipe_path);
    else
        ui_log(L"RDP StartConnect OK for %s (waiting for callback...)", pipe_path);

show_window:
    /* Start polling RDP Connected state */
    SetTimer(d->hwnd, IDT_RDP_POLL, RDP_POLL_MS, NULL);

    /* ---- Message pump (runs until WM_QUIT) ---- */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (d->ole_inited) OleUninitialize();
    return 0;

fail:
    if (d->rdp_control && d->event_cookie)
        rdp_unadvise_events(d->rdp_control, d->event_cookie, d->event_sink);
    if (d->rdp_dispatch)  IDispatch_Release(d->rdp_dispatch);
    if (d->rdp_control)   IUnknown_Release(d->rdp_control);
    if (d->ax_container)  IUnknown_Release(d->ax_container);
    if (d->ax_hwnd)       DestroyWindow(d->ax_hwnd);
    if (d->hwnd)          DestroyWindow(d->hwnd);
    d->running = FALSE;
    if (d->ole_inited) OleUninitialize();
    return 1;
}

/* ==================================================================
 * Public API
 * ================================================================== */

VmDisplay *vm_display_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd)
{
    VmDisplay *d;

    if (!vm) return NULL;

    d = (VmDisplay *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                sizeof(VmDisplay));
    if (!d) return NULL;

    d->vm         = vm;
    d->hInstance  = hInstance;
    d->main_hwnd  = main_hwnd;
    d->running    = TRUE;

    d->thread = CreateThread(NULL, 0, display_thread_proc, d, 0, NULL);
    if (!d->thread) {
        ui_log(L"Error: Failed to create display thread");
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

void vm_display_destroy(VmDisplay *display)
{
    if (!display) return;

    /* Tell the display thread to close (d->running = FALSE signals
       programmatic close so WM_CLOSE won't re-notify main_hwnd). */
    display->running = FALSE;
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for the display thread, pumping messages to keep UI responsive */
    if (display->thread) {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(
                1, &display->thread, FALSE, 5000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
        CloseHandle(display->thread);
    }

    HeapFree(GetProcessHeap(), 0, display);
}

BOOL vm_display_is_open(VmDisplay *display)
{
    if (!display) return FALSE;
    return display->running && IsWindow(display->hwnd);
}

/* Post disconnect to display thread — returns immediately. */
void vm_display_disconnect(VmDisplay *display)
{
    if (!display) return;
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_DISPLAY_DISCONNECT, 0, 0);
}

/* Post enhanced-mode switch to display thread — returns immediately. */
void vm_display_set_enhanced(VmDisplay *display)
{
    if (!display || display->enhanced_mode) return;
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_DISPLAY_ENHANCE, 0, 0);
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

static LRESULT CALLBACK display_wnd_proc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplay *d;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    d = (VmDisplay *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CLOSE:
        if (d) {
            /* d->running == FALSE means vm_display_destroy initiated this close.
               d->running == TRUE means the user clicked X. */
            BOOL user_initiated = d->running;
            d->running = FALSE;

            /* Unadvise event sink */
            if (d->rdp_control && d->event_cookie) {
                rdp_unadvise_events(d->rdp_control, d->event_cookie, d->event_sink);
                d->event_sink = NULL;
                d->event_cookie = 0;
            }

            /* Tear down pipe connector — breaks the transport so the
               RDP control won't hang trying to talk to a dead VM. */
            if (d->connector) {
                d->connector->lpVtbl->TerminateInstance(d->connector);
                d->connector->lpVtbl->Release(d->connector);
                d->connector = NULL;
            }
            if (d->callbacks) {
                IRDPENCCallbacksC *cb = (IRDPENCCallbacksC *)d->callbacks;
                cb->lpVtbl->Release(cb);
                d->callbacks = NULL;
            }

            /* Destroy the ActiveX host window — tears down the RDP control
               in-process without calling Disconnect (which can hang). */
            if (d->ax_hwnd) { DestroyWindow(d->ax_hwnd); d->ax_hwnd = NULL; }
            if (d->rdp_dispatch) { IDispatch_Release(d->rdp_dispatch); d->rdp_dispatch = NULL; }
            if (d->ax_container) { IUnknown_Release(d->ax_container); d->ax_container = NULL; }
            if (d->rdp_control)  { IUnknown_Release(d->rdp_control); d->rdp_control = NULL; }
            if (d->platform_ctx) {
                d->platform_ctx->lpVtbl->TerminateInstance(d->platform_ctx);
                d->platform_ctx->lpVtbl->Release(d->platform_ctx);
                d->platform_ctx = NULL;
            }

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm)
                PostMessageW(d->main_hwnd, (WM_APP + 5) /*WM_VM_DISPLAY_CLOSED*/,
                             0, (LPARAM)d->vm);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (d) d->hwnd = NULL;
        PostQuitMessage(0);  /* Exit display thread message pump */
        return 0;

    case WM_SIZE:
        if (d && d->ax_hwnd) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(d->ax_hwnd, 0, TOOLBAR_HEIGHT,
                       rc.right, rc.bottom - TOOLBAR_HEIGHT, TRUE);
        }
        return 0;

    case WM_COMMAND:
        if (d && LOWORD(wp) == IDC_BTN_SESSION_TOGGLE) {
            d->enhanced_mode = !d->enhanced_mode;
            display_reconnect(d);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wp == IDT_RDP_POLL && d && d->rdp_dispatch) {
            VARIANT v;
            LONG state = -1;
            HRESULT hrPoll;

            /* Detect if pipe connector callback never fired */
            if (!d->pipe_callback_fired && !d->pipe_timeout_logged &&
                d->connect_start_tick > 0) {
                ULONGLONG elapsed = GetTickCount64() - d->connect_start_tick;
                if (elapsed > 10000) {
                    wchar_t pp[512];
                    swprintf_s(pp, 512, L"\\\\.\\pipe\\%s.%s", d->vm->name,
                        d->enhanced_mode ? L"EnhancedSession" : L"BasicSession");
                    ui_log(L"RDP DIAG: No pipe callback after %llus for \"%s\"",
                        elapsed / 1000, d->vm->name);
                    if (WaitNamedPipeW(pp, 0))
                        ui_log(L"RDP DIAG: Pipe now exists: %s", pp);
                    else
                        ui_log(L"RDP DIAG: Pipe still missing: %s (error %lu)",
                            pp, GetLastError());
                    d->pipe_timeout_logged = TRUE;
                }
            }

            VariantInit(&v);
            hrPoll = disp_get(d->rdp_dispatch, L"Connected", &v);
            if (SUCCEEDED(hrPoll))
                state = (v.vt == VT_I4) ? v.lVal : (v.vt == VT_I2) ? v.iVal : -1;
            VariantClear(&v);

            if (state != d->last_connected) {
                ui_log(L"RDP Connected state: %ld -> %ld for \"%s\"",
                    d->last_connected, state, d->vm->name);
                if (d->last_connected > 0 && state == 0) {
                    VARIANT v2;
                    VariantInit(&v2);
                    if (SUCCEEDED(disp_get(d->rdp_dispatch, L"ExtendedDisconnectReason", &v2))) {
                        LONG reason = (v2.vt == VT_I4) ? v2.lVal : (v2.vt == VT_I2) ? v2.iVal : -1;
                        ui_log(L"  ExtendedDisconnectReason: %ld", reason);
                    }
                    VariantClear(&v2);
                }
                d->last_connected = state;
            }
        }
        return 0;

    case WM_DISPLAY_ENHANCE:
        if (d && !d->enhanced_mode) {
            d->enhanced_mode = TRUE;
            display_reconnect(d);
        }
        return 0;

    case WM_DISPLAY_DISCONNECT:
        if (d) {
            ui_log(L"RDP disconnecting \"%s\" (%s)...",
                d->vm ? d->vm->name : L"?",
                d->enhanced_mode ? L"Enhanced" : L"Basic");
            if (d->connector) {
                d->connector->lpVtbl->TerminateInstance(d->connector);
                d->connector->lpVtbl->Release(d->connector);
                d->connector = NULL;
            }
            if (d->rdp_dispatch)
                disp_call(d->rdp_dispatch, L"Disconnect");
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
