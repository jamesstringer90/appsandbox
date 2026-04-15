/*
 * webview2_bridge.c — WebView2 hosting in pure C via COM vtables.
 *
 * Loads WebView2Loader.dll dynamically. Implements the three required
 * COM callback interfaces as plain C structs with vtables.
 */

#include "webview2_bridge.h"
#include "ui.h"
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "shlwapi.lib")

/* We need the WebView2 COM interface definitions.
   The header uses MIDL_INTERFACE which expands to plain vtable structs in C. */
#include "WebView2.h"

/* ---- Dynamic loading of WebView2Loader.dll ---- */

typedef HRESULT (STDAPICALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *options,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

typedef HRESULT (STDAPICALLTYPE *PFN_GetAvailableCoreWebView2BrowserVersionString)(
    PCWSTR browserExecutableFolder,
    LPWSTR *versionInfo);

static PFN_CreateCoreWebView2EnvironmentWithOptions pfnCreateEnv;
static PFN_GetAvailableCoreWebView2BrowserVersionString pfnGetVersion;
static HMODULE g_wv2_module;

/* ---- Global state ---- */

static ICoreWebView2 *g_webview;
static ICoreWebView2Controller *g_controller;
static HWND g_parent_hwnd;
static HINSTANCE g_hInstance;
static BOOL g_ready;

/* Message queue for messages sent before WebView2 is ready */
#define MAX_QUEUED 64
static wchar_t *g_queue[MAX_QUEUED];
static int g_queue_count;

/* ---- Forward declarations ---- */

static WebView2MessageCallback g_message_callback;

void webview2_set_message_callback(WebView2MessageCallback cb) {
    g_message_callback = cb;
}

/* ---- COM callback: Environment Created ---- */

typedef struct {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
    LONG ref;
} EnvHandler;

static HRESULT STDMETHODCALLTYPE EnvH_QI(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE EnvH_AddRef(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    return InterlockedIncrement(&((EnvHandler *)This)->ref);
}
static ULONG STDMETHODCALLTYPE EnvH_Release(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This) {
    LONG r = InterlockedDecrement(&((EnvHandler *)This)->ref);
    if (r == 0) free(This);
    return r;
}

/* Forward: controller handler creation */
static HRESULT STDMETHODCALLTYPE EnvH_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *env);

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl = {
    EnvH_QI, EnvH_AddRef, EnvH_Release, EnvH_Invoke
};

/* ---- COM callback: Controller Created ---- */

typedef struct {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
    LONG ref;
} CtrlHandler;

static HRESULT STDMETHODCALLTYPE CtrlH_QI(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE CtrlH_AddRef(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This) {
    return InterlockedIncrement(&((CtrlHandler *)This)->ref);
}
static ULONG STDMETHODCALLTYPE CtrlH_Release(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This) {
    LONG r = InterlockedDecrement(&((CtrlHandler *)This)->ref);
    if (r == 0) free(This);
    return r;
}

static HRESULT STDMETHODCALLTYPE CtrlH_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *controller);

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl = {
    CtrlH_QI, CtrlH_AddRef, CtrlH_Release, CtrlH_Invoke
};

/* ---- COM callback: WebMessage Received ---- */

typedef struct {
    ICoreWebView2WebMessageReceivedEventHandlerVtbl *lpVtbl;
    LONG ref;
} MsgHandler;

static HRESULT STDMETHODCALLTYPE MsgH_QI(ICoreWebView2WebMessageReceivedEventHandler *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_ICoreWebView2WebMessageReceivedEventHandler)) {
        *ppv = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE MsgH_AddRef(ICoreWebView2WebMessageReceivedEventHandler *This) {
    return InterlockedIncrement(&((MsgHandler *)This)->ref);
}
static ULONG STDMETHODCALLTYPE MsgH_Release(ICoreWebView2WebMessageReceivedEventHandler *This) {
    LONG r = InterlockedDecrement(&((MsgHandler *)This)->ref);
    if (r == 0) free(This);
    return r;
}

static HRESULT STDMETHODCALLTYPE MsgH_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args)
{
    LPWSTR json = NULL;
    (void)This; (void)sender;

    if (SUCCEEDED(args->lpVtbl->get_WebMessageAsJson(args, &json)) && json) {
        if (g_message_callback)
            g_message_callback(json);
        CoTaskMemFree(json);
    }
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_msgVtbl = {
    MsgH_QI, MsgH_AddRef, MsgH_Release, MsgH_Invoke
};

/* ---- Callback implementations ---- */

static HRESULT STDMETHODCALLTYPE EnvH_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Environment *env)
{
    CtrlHandler *ch;
    (void)This;

    if (FAILED(errorCode) || !env) {
        ui_log(L"WebView2: Environment creation failed (0x%08X)", errorCode);
        return S_OK;
    }

    ch = (CtrlHandler *)calloc(1, sizeof(CtrlHandler));
    if (!ch) return E_OUTOFMEMORY;
    ch->lpVtbl = &g_ctrlVtbl;
    ch->ref = 1;

    env->lpVtbl->CreateCoreWebView2Controller(env, g_parent_hwnd,
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)ch);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE CtrlH_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT errorCode, ICoreWebView2Controller *controller)
{
    ICoreWebView2 *webview = NULL;
    ICoreWebView2Settings *settings = NULL;
    MsgHandler *mh;
    EventRegistrationToken token;
    wchar_t html_path[MAX_PATH];
    wchar_t url[MAX_PATH + 16];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;
    (void)This;

    if (FAILED(errorCode) || !controller) {
        ui_log(L"WebView2: Controller creation failed (0x%08X)", errorCode);
        return S_OK;
    }

    g_controller = controller;
    controller->lpVtbl->AddRef(controller);

    controller->lpVtbl->get_CoreWebView2(controller, &webview);
    if (!webview) {
        ui_log(L"WebView2: Failed to get CoreWebView2");
        return S_OK;
    }

    g_webview = webview;

    /* Configure settings */
    if (SUCCEEDED(webview->lpVtbl->get_Settings(webview, &settings)) && settings) {
        settings->lpVtbl->put_AreDevToolsEnabled(settings, FALSE);
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, TRUE);
        settings->lpVtbl->Release(settings);
    }

    /* Register message handler */
    mh = (MsgHandler *)calloc(1, sizeof(MsgHandler));
    if (mh) {
        mh->lpVtbl = &g_msgVtbl;
        mh->ref = 1;
        webview->lpVtbl->add_WebMessageReceived(webview,
            (ICoreWebView2WebMessageReceivedEventHandler *)mh, &token);
    }

    /* Resize to fill parent */
    webview2_resize(g_parent_hwnd);

    /* Navigate to local HTML */
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';
    swprintf_s(html_path, MAX_PATH, L"%s\\web\\index.html", exe_dir);

    /* Check if file exists, fall back to source tree path for dev */
    if (GetFileAttributesW(html_path) == INVALID_FILE_ATTRIBUTES) {
        /* Try project directory (for development) */
        wchar_t proj_path[MAX_PATH];
        swprintf_s(proj_path, MAX_PATH, L"%s\\..\\web\\index.html", exe_dir);
        if (GetFileAttributesW(proj_path) != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(html_path, MAX_PATH, proj_path);
        }
    }

    swprintf_s(url, MAX_PATH + 16, L"file:///%s", html_path);
    /* Convert backslashes to forward slashes for file URL */
    {
        wchar_t *p;
        for (p = url + 8; *p; p++) {
            if (*p == L'\\') *p = L'/';
        }
    }

    webview->lpVtbl->Navigate(webview, url);

    g_ready = TRUE;

    ui_log(L"WebView2: Ready");
    return S_OK;
}

/* ---- Public API ---- */

static BOOL load_webview2_loader(void)
{
    wchar_t dll_path[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    /* Try next to exe first */
    swprintf_s(dll_path, MAX_PATH, L"%s\\WebView2Loader.dll", exe_dir);
    g_wv2_module = LoadLibraryW(dll_path);

    /* Try vendor directory (platform-specific) */
    if (!g_wv2_module) {
#ifdef _M_ARM64
        swprintf_s(dll_path, MAX_PATH, L"%s\\..\\vendor\\webview2\\arm64\\WebView2Loader.dll", exe_dir);
#else
        swprintf_s(dll_path, MAX_PATH, L"%s\\..\\vendor\\webview2\\x64\\WebView2Loader.dll", exe_dir);
#endif
        g_wv2_module = LoadLibraryW(dll_path);
    }

    if (!g_wv2_module) {
        ui_log(L"WebView2: Failed to load WebView2Loader.dll");
        return FALSE;
    }

    pfnCreateEnv = (PFN_CreateCoreWebView2EnvironmentWithOptions)
        GetProcAddress(g_wv2_module, "CreateCoreWebView2EnvironmentWithOptions");
    pfnGetVersion = (PFN_GetAvailableCoreWebView2BrowserVersionString)
        GetProcAddress(g_wv2_module, "GetAvailableCoreWebView2BrowserVersionString");

    if (!pfnCreateEnv) {
        ui_log(L"WebView2: Missing CreateCoreWebView2EnvironmentWithOptions");
        FreeLibrary(g_wv2_module);
        g_wv2_module = NULL;
        return FALSE;
    }

    return TRUE;
}

BOOL webview2_init(HWND parent, HINSTANCE hInstance)
{
    EnvHandler *eh;
    wchar_t udf[MAX_PATH];
    wchar_t base[MAX_PATH];
    HRESULT hr;
    LPWSTR version = NULL;

    g_parent_hwnd = parent;
    g_hInstance = hInstance;
    g_ready = FALSE;
    g_queue_count = 0;

    if (!load_webview2_loader())
        return FALSE;

    /* Check runtime availability */
    if (pfnGetVersion) {
        hr = pfnGetVersion(NULL, &version);
        if (SUCCEEDED(hr) && version) {
            ui_log(L"WebView2 Runtime: %s", version);
            CoTaskMemFree(version);
        } else {
            ui_log(L"WebView2 Runtime not found. Please install Microsoft Edge WebView2 Runtime.");
            return FALSE;
        }
    }

    /* User data folder */
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(udf, MAX_PATH, L"%s\\AppSandbox\\WebView2Data", base);

    /* Create environment */
    eh = (EnvHandler *)calloc(1, sizeof(EnvHandler));
    if (!eh) return FALSE;
    eh->lpVtbl = &g_envVtbl;
    eh->ref = 1;

    hr = pfnCreateEnv(NULL, udf, NULL,
        (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)eh);
    if (FAILED(hr)) {
        ui_log(L"WebView2: CreateEnvironment failed (0x%08X)", hr);
        free(eh);
        return FALSE;
    }

    return TRUE;
}

void webview2_post(const wchar_t *json)
{
    if (!json) return;

    if (g_ready && g_webview) {
        g_webview->lpVtbl->PostWebMessageAsJson(g_webview, json);
    } else {
        /* Queue for later */
        if (g_queue_count < MAX_QUEUED) {
            size_t len = wcslen(json) + 1;
            wchar_t *copy = (wchar_t *)malloc(len * sizeof(wchar_t));
            if (copy) {
                wcscpy_s(copy, len, json);
                g_queue[g_queue_count++] = copy;
            }
        }
    }
}

void webview2_flush_queue(void)
{
    int i;
    if (!g_ready || !g_webview) return;
    for (i = 0; i < g_queue_count; i++) {
        g_webview->lpVtbl->PostWebMessageAsJson(g_webview, g_queue[i]);
        free(g_queue[i]);
    }
    g_queue_count = 0;
}

void webview2_resize(HWND parent)
{
    if (g_controller) {
        RECT rc;
        GetClientRect(parent, &rc);
        g_controller->lpVtbl->put_Bounds(g_controller, rc);
    }
}

void webview2_cleanup(void)
{
    int i;
    if (g_controller) {
        g_controller->lpVtbl->Close(g_controller);
        g_controller->lpVtbl->Release(g_controller);
        g_controller = NULL;
    }
    if (g_webview) {
        g_webview->lpVtbl->Release(g_webview);
        g_webview = NULL;
    }
    for (i = 0; i < g_queue_count; i++)
        free(g_queue[i]);
    g_queue_count = 0;
    g_ready = FALSE;

    if (g_wv2_module) {
        FreeLibrary(g_wv2_module);
        g_wv2_module = NULL;
    }
}

BOOL webview2_is_ready(void)
{
    return g_ready;
}

/* ---- JSON builder ---- */

void jb_init(JsonBuilder *jb, wchar_t *buf, size_t cap)
{
    jb->buf = buf;
    jb->cap = cap;
    jb->len = 0;
    jb->count = 0;
    if (cap > 0) buf[0] = L'\0';
}

void jb_append(JsonBuilder *jb, const wchar_t *s)
{
    size_t slen = wcslen(s);
    if (jb->len + slen < jb->cap) {
        wcscpy_s(jb->buf + jb->len, jb->cap - jb->len, s);
        jb->len += slen;
    }
}

void jb_append_escaped(JsonBuilder *jb, const wchar_t *s)
{
    const wchar_t *p;
    for (p = s; *p; p++) {
        if (jb->len + 6 >= jb->cap) break;
        switch (*p) {
        case L'\\': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'\\'; break;
        case L'"':  jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'"'; break;
        case L'\n': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'n'; break;
        case L'\r': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L'r'; break;
        case L'\t': jb->buf[jb->len++] = L'\\'; jb->buf[jb->len++] = L't'; break;
        default:    jb->buf[jb->len++] = *p; break;
        }
    }
    jb->buf[jb->len] = L'\0';
}

void jb_object_begin(JsonBuilder *jb)
{
    jb_append(jb, L"{");
    jb->count = 0;
}

void jb_object_end(JsonBuilder *jb)
{
    jb_append(jb, L"}");
}

void jb_array_begin(JsonBuilder *jb, const wchar_t *key)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, L"\":[");
    jb->count = 0;
}

void jb_array_end(JsonBuilder *jb)
{
    jb_append(jb, L"]");
    jb->count = 1; /* so next field gets a comma */
}

void jb_string(JsonBuilder *jb, const wchar_t *key, const wchar_t *val)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, L"\":\"");
    jb_append_escaped(jb, val ? val : L"");
    jb_append(jb, L"\"");
    jb->count++;
}

void jb_int(JsonBuilder *jb, const wchar_t *key, int val)
{
    wchar_t tmp[32];
    if (jb->count > 0) jb_append(jb, L",");
    swprintf_s(tmp, 32, L"\"%s\":%d", key, val);
    jb_append(jb, tmp);
    jb->count++;
}

void jb_bool(JsonBuilder *jb, const wchar_t *key, BOOL val)
{
    if (jb->count > 0) jb_append(jb, L",");
    jb_append(jb, L"\"");
    jb_append(jb, key);
    jb_append(jb, val ? L"\":true" : L"\":false");
    jb->count++;
}

/* ---- Simple JSON parser ---- */

BOOL json_get_string(const wchar_t *json, const wchar_t *key,
                     wchar_t *out, size_t out_len)
{
    wchar_t pattern[300];
    const wchar_t *p, *start, *end;

    swprintf_s(pattern, 300, L"\"%s\"", key);
    p = wcsstr(json, pattern);
    if (!p) return FALSE;

    p += wcslen(pattern);
    while (*p == L' ' || *p == L':') p++;
    if (*p != L'"') return FALSE;
    p++;
    start = p;

    /* Find closing quote, handling escapes */
    while (*p && *p != L'"') {
        if (*p == L'\\' && *(p + 1)) p++;
        p++;
    }
    end = p;

    if ((size_t)(end - start) >= out_len) return FALSE;
    wcsncpy_s(out, out_len, start, end - start);
    return TRUE;
}

BOOL json_get_int(const wchar_t *json, const wchar_t *key, int *out)
{
    wchar_t pattern[300];
    const wchar_t *p;

    swprintf_s(pattern, 300, L"\"%s\"", key);
    p = wcsstr(json, pattern);
    if (!p) return FALSE;

    p += wcslen(pattern);
    while (*p == L' ' || *p == L':') p++;

    /* Handle quoted numbers from JS */
    if (*p == L'"') p++;
    *out = _wtoi(p);
    return TRUE;
}

BOOL json_get_bool(const wchar_t *json, const wchar_t *key, BOOL *out)
{
    wchar_t pattern[300];
    const wchar_t *p;

    swprintf_s(pattern, 300, L"\"%s\"", key);
    p = wcsstr(json, pattern);
    if (!p) return FALSE;

    p += wcslen(pattern);
    while (*p == L' ' || *p == L':') p++;
    *out = (*p == L't' || *p == L'T') ? TRUE : FALSE;
    return TRUE;
}
