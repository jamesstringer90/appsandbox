/*
 * nvgl_wrapper.cpp: opengl32.dll wrapper that lets NVIDIA's OpenGL ICD work
 * inside a GPU-PV guest.
 *
 * Why this exists
 * ---------------
 * Microsoft's opengl32.dll already finds NVIDIA's ICD in a GPU-PV guest: the
 * paravirtualized adapter answers KMTQAITYPE_UMOPENGLINFO with the path of
 * nvoglv64.dll translated into HostDriverStore, and the ICD itself (the same
 * DLL that serves Vulkan) works through the paravirtualized adapter. It just
 * reports zero pixel formats, because its DllMain discovers GPUs by walking
 * the GDI display devices, keeping the ones whose DeviceID is NVIDIA's, and
 * in a VM no display belongs to the NVIDIA adapter. So opengl32 falls back to
 * GDI's software renderer, which is why App Sandbox ships Mesa's OpenGL-on-
 * D3D12 instead.
 *
 * What it does
 * ------------
 * The agent installs this DLL as System32\opengl32.dll and keeps Microsoft's
 * beside it as asb_gl_ms.dll. Every export is forwarded to asb_gl_ms.dll
 * (forwarders.inc, generated from Microsoft's export table, same ordinals),
 * except the wgl* entry points: those are real stubs that, on the first call,
 * patch two import-table entries (the same two as the Vulkan shim asb_nvvk)
 * and then hand over to Microsoft's implementation:
 *
 *   user32!NtUserEnumDisplayDevices    index 0 becomes a fake NVIDIA display
 *                                      carrying the real primary display name
 *   gdi32!NtGdiDdDDIOpenAdapterFromHdc an adapter that isn't the NVIDIA
 *                                      paravirtualized one is swapped for it
 *
 * Both act only while nvoglv64.dll is on the call stack. Microsoft's code
 * loads the ICD from inside those wgl* calls, so the patches are always in
 * place before the ICD's DllMain runs.
 *
 * Nothing happens in DllMain on purpose: opengl32.dll is a static import of
 * many programs (Chromium's GPU process among them), so DllMain runs during
 * process initialization, where win32k calls are fatal.
 *
 * Without an NVIDIA paravirtualized adapter nothing is patched: the DLL is a
 * plain forwarder. Set ASB_NVGL_TRACE=1 in the app's environment to log to
 * %TEMP%\asb_nvgl.log, ASB_NVGL_DISABLE=1 to skip the patches.
 */
#define WIN32_LEAN_AND_MEAN
#define _GDI32_              /* we define the wgl* entry points ourselves: no dllimport on them */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "forwarders.inc"    /* Microsoft's opengl32 exports minus wgl*, forwarded to asb_gl_ms.dll */
#include "wgl_ordinals.inc"  /* the wgl* stubs below, exported with Microsoft's ordinals */

typedef NTSTATUS (APIENTRY *PFN_KMT)(void *);
typedef NTSTATUS (NTAPI *PFN_NtUserEnumDisplayDevices)(PUNICODE_STRING, DWORD, PDISPLAY_DEVICEW, DWORD);
static PFN_KMT real_OpenAdapterFromHdc, real_OpenAdapterFromLuid, real_CloseAdapter, real_EnumAdapters2, real_QueryAdapterInfo;
static PFN_NtUserEnumDisplayDevices real_EnumDisplayDevices;
static HMODULE g_self, g_ms;
static volatile LONG g_init;
static LUID g_nv_luid;
static wchar_t g_primary_name[32], g_nv_devid[128];
static ULONG_PTR g_nv_base, g_nv_end;
static FILE *g_log;
static struct { ULONG_PTR *slot; ULONG_PTR orig; } g_patches[2];
static int g_patch_count;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

/* TRUE if nvoglv64.dll is within the first frames of the current call stack.
   Once the ICD is loaded this is an address-range check; the module lookup
   is only needed for the calls made from the ICD's own DllMain. */
static bool nv_on_stack(void)
{
    void *frames[16];
    USHORT n = RtlCaptureStackBackTrace(1, 16, frames, NULL);
    if (!g_nv_base) {
        HMODULE nv = GetModuleHandleW(L"nvoglv64.dll");
        if (nv) {
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)nv + ((IMAGE_DOS_HEADER *)nv)->e_lfanew);
            g_nv_base = (ULONG_PTR)nv; g_nv_end = g_nv_base + nt->OptionalHeader.SizeOfImage;
        }
    }
    for (USHORT i = 0; i < n; i++) {
        if (g_nv_base) {
            if ((ULONG_PTR)frames[i] >= g_nv_base && (ULONG_PTR)frames[i] < g_nv_end) return true;
            continue;
        }
        HMODULE m = NULL; wchar_t path[MAX_PATH]; const wchar_t *base;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)frames[i], &m) || !m) continue;
        GetModuleFileNameW(m, path, MAX_PATH);
        base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        if (_wcsicmp(base, L"nvoglv64.dll") == 0) return true;
    }
    return false;
}

static void iat_write(ULONG_PTR *slot, ULONG_PTR value)
{
    DWORD old;
    if (VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) { *slot = value; VirtualProtect(slot, sizeof *slot, old, &old); }
}

/* Replace `fn`, imported by `mod` from `dll`, in mod's import address table. */
static bool iat_patch(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY dir;
    if (!mod || g_patch_count >= 2) return false;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char *)(base + imp->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn)) continue;
            if (iat->u1.Function == (ULONG_PTR)hook) return false;   /* never chain to ourselves */
            *orig = (void *)iat->u1.Function;
            iat_write(&iat->u1.Function, (ULONG_PTR)hook);
            if (iat->u1.Function != (ULONG_PTR)hook) return false;
            g_patches[g_patch_count].slot = &iat->u1.Function; g_patches[g_patch_count].orig = (ULONG_PTR)*orig; g_patch_count++;
            return true;
        }
    }
    return false;
}

/* Pick the NVIDIA paravirtualized adapter (vendor 0x10de + Paravirtualized),
   preferring the one with the most VidPN sources. */
static bool find_nv_paravirt_adapter(void)
{
    D3DKMT_ENUMADAPTERS2 e = {};
    if (real_EnumAdapters2(&e) != 0 || !e.NumAdapters) return false;
    D3DKMT_ADAPTERINFO *list = (D3DKMT_ADAPTERINFO *)calloc(e.NumAdapters, sizeof *list);
    if (!list) return false;
    e.pAdapters = list;
    if (real_EnumAdapters2(&e) != 0) { free(list); return false; }
    bool found = false; UINT best = 0;
    for (UINT i = 0; i < e.NumAdapters; i++) {
        D3DKMT_ADAPTERTYPE type = {}; D3DKMT_QUERY_DEVICE_IDS ids = {}; D3DKMT_QUERYADAPTERINFO q = {};
        q.hAdapter = list[i].hAdapter;
        q.Type = KMTQAITYPE_ADAPTERTYPE_RENDER; q.pPrivateDriverData = &type; q.PrivateDriverDataSize = sizeof type;
        bool pv = real_QueryAdapterInfo(&q) == 0 && type.Paravirtualized;
        q.Type = KMTQAITYPE_PHYSICALADAPTERDEVICEIDS; q.pPrivateDriverData = &ids; q.PrivateDriverDataSize = sizeof ids;
        bool nv = real_QueryAdapterInfo(&q) == 0 && ids.DeviceIds.VendorID == 0x10de;
        if (nv && pv && (!found || list[i].NumOfSources > best)) {
            found = true; best = list[i].NumOfSources; g_nv_luid = list[i].AdapterLuid;
            swprintf_s(g_nv_devid, L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X", ids.DeviceIds.VendorID, ids.DeviceIds.DeviceID,
                       ids.DeviceIds.SubSystemID, ids.DeviceIds.SubVendorID, ids.DeviceIds.RevisionID);
        }
        D3DKMT_CLOSEADAPTER c = { list[i].hAdapter }; real_CloseAdapter(&c);
    }
    free(list);
    return found;
}

static NTSTATUS APIENTRY hook_OpenAdapterFromHdc(void *a)
{
    D3DKMT_OPENADAPTERFROMHDC *p = (D3DKMT_OPENADAPTERFROMHDC *)a;
    NTSTATUS s = real_OpenAdapterFromHdc(a);
    if (s == 0 && (p->AdapterLuid.LowPart != g_nv_luid.LowPart || p->AdapterLuid.HighPart != g_nv_luid.HighPart) && nv_on_stack()) {
        D3DKMT_CLOSEADAPTER c = { p->hAdapter }; real_CloseAdapter(&c);
        D3DKMT_OPENADAPTERFROMLUID o = {}; o.AdapterLuid = g_nv_luid;
        NTSTATUS s2 = real_OpenAdapterFromLuid(&o);
        logf_("OpenAdapterFromHdc: LUID %08x -> NVIDIA %08x (0x%08lx)", p->AdapterLuid.LowPart, g_nv_luid.LowPart, s2);
        if (s2 == 0) { p->hAdapter = o.hAdapter; p->AdapterLuid = g_nv_luid; p->VidPnSourceId = 0; } else s = s2;
    }
    return s;
}

static void find_primary_display(void)
{
    DISPLAY_DEVICEW t;
    for (DWORD pass = 0; pass < 2 && !g_primary_name[0]; pass++) {
        DWORD want = pass == 0 ? DISPLAY_DEVICE_PRIMARY_DEVICE : DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
        for (DWORD k = 0; ; k++) {
            t.cb = sizeof t;
            if (real_EnumDisplayDevices(NULL, k, &t, 0) != 0) break;
            if (t.StateFlags & want) { wcscpy_s(g_primary_name, t.DeviceName); break; }
        }
    }
    if (!g_primary_name[0]) wcscpy_s(g_primary_name, L"\\\\.\\DISPLAY1");   /* no desktop: give the driver a real name anyway */
}

static NTSTATUS NTAPI hook_EnumDisplayDevices(PUNICODE_STRING dev, DWORD i, PDISPLAY_DEVICEW d, DWORD f)
{
    bool adapters = !dev || !dev->Buffer || dev->Length == 0;   /* NULL device = walk the adapters */
    if (adapters && d && d->cb >= sizeof(DISPLAY_DEVICEW) && nv_on_stack()) {
        if (i == 0) {
            DWORD cb = d->cb;
            if (!g_primary_name[0]) find_primary_display();
            memset(d, 0, sizeof(DISPLAY_DEVICEW)); d->cb = cb;
            wcscpy_s(d->DeviceName, g_primary_name);
            wcscpy_s(d->DeviceString, L"NVIDIA GPU-PV (App Sandbox)");
            wcscpy_s(d->DeviceID, g_nv_devid);
            d->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
            return 0;
        }
        i--;
    }
    return real_EnumDisplayDevices(dev, i, d, f);
}

static void open_log(void)
{
    char path[MAX_PATH], exe[MAX_PATH];
    if (!GetEnvironmentVariableA("ASB_NVGL_TRACE", NULL, 0) || !GetEnvironmentVariableA("TEMP", path, MAX_PATH)) return;
    strcat_s(path, "\\asb_nvgl.log");
    if (fopen_s(&g_log, path, "a") != 0) { g_log = NULL; return; }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    logf_("--- asb_opengl32 pid %lu %s", GetCurrentProcessId(), exe);
}

static void init(void)
{
    open_log();
    HMODULE w32u = GetModuleHandleW(L"win32u.dll"), u32 = GetModuleHandleW(L"user32.dll"), g32 = GetModuleHandleW(L"gdi32.dll");
    if (!w32u || !u32 || !g32) { logf_("win32u/user32/gdi32 not loaded: plain forwarding"); return; }
    real_EnumAdapters2       = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDIEnumAdapters2");
    real_QueryAdapterInfo    = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDIQueryAdapterInfo");
    real_OpenAdapterFromLuid = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDIOpenAdapterFromLuid");
    real_CloseAdapter        = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDICloseAdapter");
    real_EnumDisplayDevices  = (PFN_NtUserEnumDisplayDevices)GetProcAddress(w32u, "NtUserEnumDisplayDevices");
    if (GetEnvironmentVariableA("ASB_NVGL_DISABLE", NULL, 0)) { logf_("ASB_NVGL_DISABLE set: plain forwarding"); return; }
    if (!real_EnumAdapters2 || !real_QueryAdapterInfo || !real_OpenAdapterFromLuid || !real_CloseAdapter || !real_EnumDisplayDevices ||
        !find_nv_paravirt_adapter()) { logf_("no NVIDIA paravirtualized adapter: plain forwarding"); return; }
    if (GetModuleHandleW(L"nvoglv64.dll"))
        logf_("nvoglv64.dll already loaded before the first wgl call: its discovery ran unpatched");
    void *o1 = NULL, *o2 = NULL;
    bool ok1 = iat_patch(u32, "win32u.dll", "NtUserEnumDisplayDevices", (void *)hook_EnumDisplayDevices, &o1);
    bool ok2 = iat_patch(g32, "win32u.dll", "NtGdiDdDDIOpenAdapterFromHdc", (void *)hook_OpenAdapterFromHdc, &o2);
    if (ok1) real_EnumDisplayDevices = (PFN_NtUserEnumDisplayDevices)o1;
    if (ok2) real_OpenAdapterFromHdc = (PFN_KMT)o2;
    logf_("NVIDIA paravirt LUID %08x:%08x, IAT patches: EnumDisplayDevices=%d OpenAdapterFromHdc=%d", g_nv_luid.HighPart, g_nv_luid.LowPart, ok1, ok2);
    /* The patches point into us: stay resident even if the app unloads opengl32. */
    HMODULE pin;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&init, &pin);
}

/* Whatever goes wrong while setting up, the host process must keep working:
   undo any patch made so far and fall back to plain forwarding. */
static void init_guarded(void)
{
    __try {
        init();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        for (int i = 0; i < g_patch_count; i++) iat_write(g_patches[i].slot, g_patches[i].orig);
        g_patch_count = 0;
        logf_("init failed with exception 0x%08lx: plain forwarding", GetExceptionCode());
    }
}

/* First wgl* call in the process: set up the patches (once), then resolve
   Microsoft's implementation. Never from DllMain (see the header comment). */
static FARPROC ms_proc(const char *name)
{
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0) {
        wchar_t path[MAX_PATH], *slash;
        init_guarded();
        GetModuleFileNameW(g_self, path, MAX_PATH);
        slash = wcsrchr(path, L'\\'); if (slash) slash[1] = 0;
        wcscat_s(path, L"asb_gl_ms.dll");
        g_ms = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_ms) g_ms = LoadLibraryW(L"asb_gl_ms.dll");
        logf_("asb_gl_ms.dll -> %p", (void *)g_ms);
        InterlockedExchange(&g_init, 2);
    }
    while (g_init != 2) Sleep(1);
    return g_ms ? GetProcAddress(g_ms, name) : NULL;
}

#define WGL_STUB(ret, name, params, args)                          \
    extern "C" ret WINAPI name params                              \
    {                                                              \
        typedef ret (WINAPI *fn_t) params;                         \
        static fn_t fn;                                            \
        if (!fn) fn = (fn_t)ms_proc(#name);                        \
        return fn ? fn args : (ret)0;                              \
    }

WGL_STUB(int,   wglChoosePixelFormat,   (HDC dc, const PIXELFORMATDESCRIPTOR *pfd), (dc, pfd))
WGL_STUB(BOOL,  wglCopyContext,         (HGLRC a, HGLRC b, UINT mask), (a, b, mask))
WGL_STUB(HGLRC, wglCreateContext,       (HDC dc), (dc))
WGL_STUB(HGLRC, wglCreateLayerContext,  (HDC dc, int plane), (dc, plane))
WGL_STUB(BOOL,  wglDeleteContext,       (HGLRC rc), (rc))
WGL_STUB(BOOL,  wglDescribeLayerPlane,  (HDC dc, int pf, int plane, UINT n, LPLAYERPLANEDESCRIPTOR d), (dc, pf, plane, n, d))
WGL_STUB(int,   wglDescribePixelFormat, (HDC dc, int pf, UINT n, LPPIXELFORMATDESCRIPTOR d), (dc, pf, n, d))
WGL_STUB(HGLRC, wglGetCurrentContext,   (void), ())
WGL_STUB(HDC,   wglGetCurrentDC,        (void), ())
WGL_STUB(PROC,  wglGetDefaultProcAddress, (LPCSTR name), (name))
WGL_STUB(int,   wglGetLayerPaletteEntries, (HDC dc, int plane, int start, int n, COLORREF *cr), (dc, plane, start, n, cr))
WGL_STUB(int,   wglGetPixelFormat,      (HDC dc), (dc))
WGL_STUB(PROC,  wglGetProcAddress,      (LPCSTR name), (name))
WGL_STUB(BOOL,  wglMakeCurrent,         (HDC dc, HGLRC rc), (dc, rc))
WGL_STUB(BOOL,  wglRealizeLayerPalette, (HDC dc, int plane, BOOL realize), (dc, plane, realize))
WGL_STUB(int,   wglSetLayerPaletteEntries, (HDC dc, int plane, int start, int n, const COLORREF *cr), (dc, plane, start, n, cr))
WGL_STUB(BOOL,  wglSetPixelFormat,      (HDC dc, int pf, const PIXELFORMATDESCRIPTOR *pfd), (dc, pf, pfd))
WGL_STUB(BOOL,  wglShareLists,          (HGLRC a, HGLRC b), (a, b))
WGL_STUB(BOOL,  wglSwapBuffers,         (HDC dc), (dc))
WGL_STUB(BOOL,  wglSwapLayerBuffers,    (HDC dc, UINT planes), (dc, planes))
WGL_STUB(DWORD, wglSwapMultipleBuffers, (UINT n, const WGLSWAP *swaps), (n, swaps))
WGL_STUB(BOOL,  wglUseFontBitmapsA,     (HDC dc, DWORD first, DWORD count, DWORD base), (dc, first, count, base))
WGL_STUB(BOOL,  wglUseFontBitmapsW,     (HDC dc, DWORD first, DWORD count, DWORD base), (dc, first, count, base))
WGL_STUB(BOOL,  wglUseFontOutlinesA,    (HDC dc, DWORD first, DWORD count, DWORD base, FLOAT dev, FLOAT ext, int fmt, LPGLYPHMETRICSFLOAT gm), (dc, first, count, base, dev, ext, fmt, gm))
WGL_STUB(BOOL,  wglUseFontOutlinesW,    (HDC dc, DWORD first, DWORD count, DWORD base, FLOAT dev, FLOAT ext, int fmt, LPGLYPHMETRICSFLOAT gm), (dc, first, count, base, dev, ext, fmt, gm))

/* Marker the agent looks for to recognise its own DLL. */
extern "C" __declspec(dllexport) unsigned int appsandbox_nvgl_wrapper(void) { return 0x41534203; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    /* Unloaded while the process lives on (should not happen once pinned, but
       never leave an import pointing into a gone DLL). */
    if (reason == DLL_PROCESS_DETACH && !reserved)
        for (int i = 0; i < g_patch_count; i++) iat_write(g_patches[i].slot, g_patches[i].orig);
    return TRUE;
}
