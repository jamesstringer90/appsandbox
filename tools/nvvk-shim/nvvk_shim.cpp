/*
 * nvvk_shim.cpp: Vulkan ICD wrapper that lets NVIDIA's native Vulkan driver
 * (nvoglv64.dll) find the GPU inside a GPU-PV guest.
 *
 * Why this exists
 * ---------------
 * The guest already has everything for native Vulkan: the loader finds
 * nv-vk64.json by itself (VulkanDriverName comes back from the host through
 * the paravirtualized adapter), nvoglv64.dll loads, creates an instance, and
 * the driver's escapes, allocations and submits all work through the VRD. It
 * just reports zero physical devices, because it discovers its GPUs in
 * DllMain by walking the GDI display devices (EnumDisplayDevicesA), keeping
 * the ones whose DeviceID is NVIDIA and opening the adapter behind each
 * display (D3DKMTOpenAdapterFromHdc). In a GPU-PV guest no display belongs to
 * the NVIDIA adapter (they are the IDD's / Hyper-V Video's), so the walk
 * finds nothing and games fall back to Mesa's Vulkan-on-D3D12 (Dozen).
 *
 * What it does
 * ------------
 * The agent registers this DLL as the ICD in the guest copy of nv-vk64.json
 * (library_path), next to nvoglv64.dll. On the loader's first call it
 * patches two import-table entries (no code patching, no thread suspension)
 * and then loads the real nvoglv64.dll so its DllMain runs with them in
 * place:
 *
 *   user32!NtUserEnumDisplayDevices    index 0 becomes a fake NVIDIA display
 *                                      carrying the real primary display name
 *   gdi32!NtGdiDdDDIOpenAdapterFromHdc an adapter that isn't the NVIDIA
 *                                      paravirtualized one is swapped for it
 *
 * Both act only while nvoglv64.dll is on the call stack; the rest of the
 * process sees the real functions. The ICD entry points are forwarded to
 * the real DLL. vk_icdEnumerateAdapterPhysicalDevices maps every NVIDIA
 * paravirtualized LUID (a guest exposes several ghosts of the same GPU) to
 * the one the driver was pointed at, so the loader ranks the native device
 * ahead of Dozen's for the same adapter, so apps that take physical device 0
 * get the real driver.
 *
 * Without an NVIDIA paravirtualized adapter nothing is patched: the DLL is
 * a plain forwarder.
 *
 * Set ASB_NVVK_TRACE=1 in the app's environment to log to %TEMP%\asb_nvvk.log,
 * ASB_NVVK_DISABLE=1 to get the stock driver behaviour (plain forwarding).
 *
 * Known limits: 64-bit only (nv-vk32.json is left alone); one NVIDIA GPU per
 * guest (with several, the one with the most VidPN sources is exposed).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* Just enough of the Vulkan ICD interface; no Vulkan headers needed. */
typedef int VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void (*PFN_vkVoidFunction)(void);
#define VK_ERROR_INCOMPATIBLE_DRIVER (-9)
typedef VkResult (*PFN_Negotiate)(unsigned int *);
typedef PFN_vkVoidFunction (*PFN_GetProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_EnumAdapterPD)(VkInstance, LUID, unsigned int *, VkPhysicalDevice *);

static HMODULE g_self, g_real;
static ULONG_PTR g_real_base, g_real_end;      /* nvoglv64.dll image range, for the stack check */
static PFN_Negotiate   g_real_negotiate;       /* the real ICD's entry points, resolved once */
static PFN_GetProcAddr g_real_gipa, g_real_gpdpa;
static PFN_EnumAdapterPD g_real_enum_adapter;
static volatile LONG g_init;
static LUID g_nv_luid;                        /* the NVIDIA paravirtualized adapter the driver is pointed at */
static LUID g_nv_luids[16];                   /* every NVIDIA paravirtualized adapter */
static int  g_nv_luid_count;
static wchar_t g_primary_name[32];            /* \\.\DISPLAYn of the real primary display */
static wchar_t g_nv_devid[128];               /* PnP DeviceID of the chosen adapter */
static FILE *g_log;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

typedef NTSTATUS (APIENTRY *PFN_KMT)(void *);
typedef NTSTATUS (NTAPI *PFN_NtUserEnumDisplayDevices)(PUNICODE_STRING, DWORD, PDISPLAY_DEVICEW, DWORD);
static PFN_KMT real_OpenAdapterFromHdc, real_OpenAdapterFromLuid, real_CloseAdapter,
               real_EnumAdapters2, real_QueryAdapterInfo;
static PFN_NtUserEnumDisplayDevices real_EnumDisplayDevices;

/* TRUE if nvoglv64.dll is within the first frames of the current call stack.
   The hooks stay in place for the life of the process, so this runs on every
   EnumDisplayDevices / OpenAdapterFromHdc of the process: once the driver is
   loaded it is a plain address-range check, no loader lock. */
static bool nv_on_stack(void)
{
    void *frames[12];
    USHORT n = RtlCaptureStackBackTrace(1, 12, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL;
        wchar_t path[MAX_PATH];
        const wchar_t *base;
        if (g_real_base) {
            if ((ULONG_PTR)frames[i] >= g_real_base && (ULONG_PTR)frames[i] < g_real_end) return true;
            continue;
        }
        /* Still inside the driver's DllMain: the module is not ours yet. */
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)frames[i], &m) || !m)
            continue;
        GetModuleFileNameW(m, path, MAX_PATH);
        base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        if (_wcsicmp(base, L"nvoglv64.dll") == 0) return true;
    }
    return false;
}

/* The IAT slots we changed, so they can be put back when we are unloaded. */
static struct { ULONG_PTR *slot; ULONG_PTR orig; } g_patches[2];
static int g_patch_count;

static void iat_write(ULONG_PTR *slot, ULONG_PTR value)
{
    DWORD old;
    if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        *slot = value;
        VirtualProtect(slot, sizeof(*slot), old, &old);
    }
}

/* Replace `fn`, imported by `mod` from `dll`, in mod's import address table. */
static bool iat_patch(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;

    if (!mod || g_patch_count >= 2) return false;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *iat;
        if (_stricmp((const char *)(base + imp->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        iat   = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            if (strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, fn))
                continue;
            if (iat->u1.Function == (ULONG_PTR)hook) return false;   /* never chain to ourselves */
            *orig = (void *)iat->u1.Function;
            iat_write(&iat->u1.Function, (ULONG_PTR)hook);
            if (iat->u1.Function != (ULONG_PTR)hook) return false;
            g_patches[g_patch_count].slot = &iat->u1.Function;
            g_patches[g_patch_count].orig = (ULONG_PTR)*orig;
            g_patch_count++;
            return true;
        }
    }
    return false;
}

/* Pick the NVIDIA paravirtualized adapter (vendor 0x10de + Paravirtualized),
   preferring the one with the most VidPN sources; remember all of them. */
static bool find_nv_paravirt_adapter(void)
{
    D3DKMT_ENUMADAPTERS2 e = {};
    D3DKMT_ADAPTERINFO *list;
    bool found = false;
    UINT best = 0;

    if (real_EnumAdapters2(&e) != 0 || e.NumAdapters == 0) return false;
    list = (D3DKMT_ADAPTERINFO *)calloc(e.NumAdapters, sizeof(*list));
    if (!list) return false;
    e.pAdapters = list;
    if (real_EnumAdapters2(&e) != 0) { free(list); return false; }
    for (UINT i = 0; i < e.NumAdapters; i++) {
        D3DKMT_ADAPTERTYPE type = {};
        D3DKMT_QUERY_DEVICE_IDS ids = {};
        D3DKMT_QUERYADAPTERINFO q = {};
        D3DKMT_CLOSEADAPTER c = { list[i].hAdapter };
        bool paravirt, nv;

        q.hAdapter = list[i].hAdapter;
        q.Type = KMTQAITYPE_ADAPTERTYPE_RENDER; q.pPrivateDriverData = &type; q.PrivateDriverDataSize = sizeof type;
        paravirt = real_QueryAdapterInfo(&q) == 0 && type.Paravirtualized;
        q.Type = KMTQAITYPE_PHYSICALADAPTERDEVICEIDS; q.pPrivateDriverData = &ids; q.PrivateDriverDataSize = sizeof ids;
        nv = real_QueryAdapterInfo(&q) == 0 && ids.DeviceIds.VendorID == 0x10de;
        logf_("adapter %u LUID %08x:%08x sources=%u vendor=0x%04x paravirt=%d", i,
              list[i].AdapterLuid.HighPart, list[i].AdapterLuid.LowPart, list[i].NumOfSources,
              ids.DeviceIds.VendorID, paravirt);
        if (nv && paravirt) {
            if (g_nv_luid_count < 16) g_nv_luids[g_nv_luid_count++] = list[i].AdapterLuid;
            if (!found || list[i].NumOfSources > best) {
                found = true; best = list[i].NumOfSources; g_nv_luid = list[i].AdapterLuid;
                swprintf_s(g_nv_devid, L"PCI\\VEN_%04X&DEV_%04X&SUBSYS_%04X%04X&REV_%02X",
                           ids.DeviceIds.VendorID, ids.DeviceIds.DeviceID, ids.DeviceIds.SubSystemID,
                           ids.DeviceIds.SubVendorID, ids.DeviceIds.RevisionID);
            }
        }
        real_CloseAdapter(&c);
    }
    free(list);
    return found;
}

static NTSTATUS APIENTRY hook_OpenAdapterFromHdc(void *a)
{
    D3DKMT_OPENADAPTERFROMHDC *p = (D3DKMT_OPENADAPTERFROMHDC *)a;
    NTSTATUS s = real_OpenAdapterFromHdc(a);
    if (s == 0 &&
        (p->AdapterLuid.LowPart != g_nv_luid.LowPart || p->AdapterLuid.HighPart != g_nv_luid.HighPart) &&
        nv_on_stack()) {
        D3DKMT_CLOSEADAPTER c = { p->hAdapter };
        D3DKMT_OPENADAPTERFROMLUID o = {};
        NTSTATUS s2;
        real_CloseAdapter(&c);
        o.AdapterLuid = g_nv_luid;
        s2 = real_OpenAdapterFromLuid(&o);
        logf_("OpenAdapterFromHdc: LUID %08x -> NVIDIA %08x (0x%08lx)", p->AdapterLuid.LowPart, g_nv_luid.LowPart, s2);
        if (s2 == 0) { p->hAdapter = o.hAdapter; p->AdapterLuid = g_nv_luid; p->VidPnSourceId = 0; }
        else s = s2;
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
    if (!g_primary_name[0])                      /* no desktop (session 0): give the driver a real name anyway */
        wcscpy_s(g_primary_name, L"\\\\.\\DISPLAY1");
}

static NTSTATUS NTAPI hook_EnumDisplayDevices(PUNICODE_STRING dev, DWORD i, PDISPLAY_DEVICEW d, DWORD f)
{
    bool adapters = !dev || !dev->Buffer || dev->Length == 0;   /* NULL device = walk the adapters */
    if (adapters && d && nv_on_stack()) {
        if (i == 0) {
            DWORD cb = d->cb;
            if (!g_primary_name[0]) find_primary_display();
            memset(d, 0, cb); d->cb = cb;
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
    if (!GetEnvironmentVariableA("ASB_NVVK_TRACE", NULL, 0)) return;
    if (!GetEnvironmentVariableA("TEMP", path, MAX_PATH)) return;
    strcat_s(path, "\\asb_nvvk.log");
    if (fopen_s(&g_log, path, "a") != 0) { g_log = NULL; return; }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    logf_("--- asb_nvvk pid %lu %s", GetCurrentProcessId(), exe);
}

static void init_once(void)
{
    HMODULE w32u, u32, g32;
    wchar_t path[MAX_PATH], *slash;

    if (InterlockedCompareExchange(&g_init, 1, 0) != 0) { while (g_init != 2) Sleep(1); return; }
    open_log();
    w32u = LoadLibraryW(L"win32u.dll"); u32 = LoadLibraryW(L"user32.dll"); g32 = LoadLibraryW(L"gdi32.dll");
    real_EnumAdapters2       = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDIEnumAdapters2");
    real_QueryAdapterInfo    = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDIQueryAdapterInfo");
    real_OpenAdapterFromLuid = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDIOpenAdapterFromLuid");
    real_CloseAdapter        = (PFN_KMT)GetProcAddress(w32u, "NtGdiDdDDICloseAdapter");
    real_EnumDisplayDevices  = (PFN_NtUserEnumDisplayDevices)GetProcAddress(w32u, "NtUserEnumDisplayDevices");
    if (GetEnvironmentVariableA("ASB_NVVK_DISABLE", NULL, 0)) {
        logf_("ASB_NVVK_DISABLE set: passthrough");       /* stock driver behaviour, for troubleshooting */
    } else if (real_EnumAdapters2 && real_QueryAdapterInfo && real_OpenAdapterFromLuid && real_CloseAdapter &&
               real_EnumDisplayDevices && find_nv_paravirt_adapter()) {
        void *o1 = NULL, *o2 = NULL;
        bool ok1 = iat_patch(u32, "win32u.dll", "NtUserEnumDisplayDevices", (void *)hook_EnumDisplayDevices, &o1);
        bool ok2 = iat_patch(g32, "win32u.dll", "NtGdiDdDDIOpenAdapterFromHdc", (void *)hook_OpenAdapterFromHdc, &o2);
        if (ok1) real_EnumDisplayDevices = (PFN_NtUserEnumDisplayDevices)o1;
        if (ok2) real_OpenAdapterFromHdc = (PFN_KMT)o2;
        logf_("NVIDIA paravirt LUID %08x:%08x, IAT patches: EnumDisplayDevices=%d OpenAdapterFromHdc=%d",
              g_nv_luid.HighPart, g_nv_luid.LowPart, ok1, ok2);
        /* The loader unloads ICD libraries when the last VkInstance goes away and
           reloads them on the next one (Unity does that during its "Vulkan
           detection"). Stay resident: the patches point into us and nvoglv64.dll
           has done its discovery under them; a fresh copy at the same address
           would find the IAT already pointing at itself. */
        HMODULE pin;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)&init_once, &pin);
    } else {
        g_nv_luid_count = 0;
        logf_("no NVIDIA paravirtualized adapter: passthrough");
    }

    /* The real ICD lives next to this DLL (the agent puts us in the driver dir).
       It must not be loaded yet: its GPU discovery runs once, in DllMain. */
    if (GetModuleHandleW(L"nvoglv64.dll"))
        logf_("nvoglv64.dll already loaded before the shim: its discovery ran unpatched");
    GetModuleFileNameW(g_self, path, MAX_PATH);
    slash = wcsrchr(path, L'\\'); if (slash) slash[1] = 0;
    wcscat_s(path, L"nvoglv64.dll");
    g_real = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    logf_("nvoglv64.dll -> %p (%ls)", (void *)g_real, path);
    if (g_real) {
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)g_real + ((IMAGE_DOS_HEADER *)g_real)->e_lfanew);
        g_real_negotiate = (PFN_Negotiate)GetProcAddress(g_real, "vk_icdNegotiateLoaderICDInterfaceVersion");
        g_real_gipa      = (PFN_GetProcAddr)GetProcAddress(g_real, "vk_icdGetInstanceProcAddr");
        g_real_gpdpa     = (PFN_GetProcAddr)GetProcAddress(g_real, "vk_icdGetPhysicalDeviceProcAddr");
        /* Interface >= 7: the loader asks vk_icdGetInstanceProcAddr for this, so do the same. */
        g_real_enum_adapter = g_real_gipa ? (PFN_EnumAdapterPD)g_real_gipa(NULL, "vk_icdEnumerateAdapterPhysicalDevices") : NULL;
        if (!g_real_enum_adapter)
            g_real_enum_adapter = (PFN_EnumAdapterPD)GetProcAddress(g_real, "vk_icdEnumerateAdapterPhysicalDevices");
        g_real_base = (ULONG_PTR)g_real;
        g_real_end  = g_real_base + nt->OptionalHeader.SizeOfImage;
    }
    InterlockedExchange(&g_init, 2);
}

extern "C" {

__declspec(dllexport) VkResult vk_icdNegotiateLoaderICDInterfaceVersion(unsigned int *pVersion)
{
    init_once();
    return g_real_negotiate ? g_real_negotiate(pVersion) : VK_ERROR_INCOMPATIBLE_DRIVER;
}

__declspec(dllexport) VkResult vk_icdEnumerateAdapterPhysicalDevices(VkInstance instance, LUID luid,
                                                                     unsigned int *pCount, VkPhysicalDevice *pDevices)
{
    init_once();
    if (!g_real_enum_adapter) return VK_ERROR_INCOMPATIBLE_DRIVER;
    for (int i = 0; i < g_nv_luid_count; i++)
        if (luid.LowPart == g_nv_luids[i].LowPart && luid.HighPart == g_nv_luids[i].HighPart) {
            luid = g_nv_luid;
            break;
        }
    return g_real_enum_adapter(instance, luid, pCount, pDevices);
}

__declspec(dllexport) PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    init_once();
    if (pName && g_nv_luid_count && strcmp(pName, "vk_icdEnumerateAdapterPhysicalDevices") == 0)
        return (PFN_vkVoidFunction)vk_icdEnumerateAdapterPhysicalDevices;
    return g_real_gipa ? g_real_gipa(instance, pName) : NULL;
}

__declspec(dllexport) PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    init_once();
    return g_real_gpdpa ? g_real_gpdpa(instance, pName) : NULL;
}

/* Marker the agent looks for to recognise its own DLL. */
__declspec(dllexport) unsigned int appsandbox_nvvk_shim(void) { return 0x41534202; }

} /* extern "C" */

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); }
    /* Unloaded while the process lives on (should not happen once pinned, but
       never leave an import pointing into a gone DLL). */
    if (reason == DLL_PROCESS_DETACH && !reserved)
        for (int i = 0; i < g_patch_count; i++)
            iat_write(g_patches[i].slot, g_patches[i].orig);
    return TRUE;
}
