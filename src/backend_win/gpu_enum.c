#include "gpu_enum.h"
#include "ui.h"
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <stdio.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* {064092b3-625e-43bf-9eb5-dc845897dd59} — GPU Partition Adapter interface */
static const GUID GUID_GPU_PARTITION_ADAPTER = {
    0x064092b3, 0x625e, 0x43bf,
    { 0x9e, 0xb5, 0xdc, 0x84, 0x58, 0x97, 0xdd, 0x59 }
};

/* ---- WMI helpers ---- */

/* Connect to WMI namespace. Caller must Release the returned service. */
static IWbemServices *wmi_connect(const wchar_t *ns)
{
    IWbemLocator *locator = NULL;
    IWbemServices *svc = NULL;
    BSTR bstr_ns = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&locator);
    if (FAILED(hr)) return NULL;

    bstr_ns = SysAllocString(ns);
    hr = locator->lpVtbl->ConnectServer(locator, bstr_ns, NULL, NULL, NULL,
                                         0, NULL, NULL, &svc);
    SysFreeString(bstr_ns);
    locator->lpVtbl->Release(locator);
    if (FAILED(hr)) return NULL;

    CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                      NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      NULL, EOAC_NONE);
    return svc;
}

/* Execute a WQL query. Caller must Release the enumerator. */
static IEnumWbemClassObject *wmi_query(IWbemServices *svc, const wchar_t *wql)
{
    IEnumWbemClassObject *enumerator = NULL;
    BSTR bstr_wql = SysAllocString(L"WQL");
    BSTR bstr_query = SysAllocString(wql);

    svc->lpVtbl->ExecQuery(svc, bstr_wql, bstr_query,
                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                            NULL, &enumerator);
    SysFreeString(bstr_wql);
    SysFreeString(bstr_query);
    return enumerator;
}

/* Get a string property from a WMI object. Returns allocated wide string, caller must free. */
static wchar_t *wmi_get_string(IWbemClassObject *obj, const wchar_t *prop)
{
    VARIANT v;
    wchar_t *result = NULL;

    VariantInit(&v);
    if (SUCCEEDED(obj->lpVtbl->Get(obj, prop, 0, &v, NULL, NULL)) &&
        v.vt == VT_BSTR && v.bstrVal) {
        size_t len = wcslen(v.bstrVal) + 1;
        result = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
        if (result) wcscpy_s(result, len, v.bstrVal);
    }
    VariantClear(&v);
    return result;
}

static void wmi_free_string(wchar_t *s)
{
    if (s) HeapFree(GetProcessHeap(), 0, s);
}

/* Resolve DriverStore path by scanning actual loaded driver files via WMI.
   Mirrors the PowerShell approach:
     1. Win32_PNPSignedDriver WHERE DeviceName → DeviceID
     2. Full scan Win32_PNPSignedDriverCIMDataFile, match Antecedent client-side
     3. Extract DriverStore folder from matched file paths
   device_name:  GPU friendly name (e.g. "NVIDIA GeForce RTX 5080 Laptop GPU").
   svc_cimv2:    already-connected WMI service (ROOT\CIMV2).
   out_path:     receives e.g. "C:\Windows\System32\DriverStore\FileRepository\nvltsi.inf_amd64_xxx"
   Returns TRUE on success. */
static BOOL resolve_driver_store_wmi(const wchar_t *device_name,
                                      IWbemServices *svc_cimv2,
                                      wchar_t *out_path, int out_max)
{
    IEnumWbemClassObject *enumerator = NULL;
    IWbemClassObject *obj = NULL;
    ULONG returned;
    wchar_t query[1024];
    wchar_t *device_id = NULL;
    wchar_t escaped_name[512];
    wchar_t escaped_id[1024];
    wchar_t hostname[64];
    wchar_t antecedent[2048];
    const wchar_t *src;
    wchar_t *dst;
    BOOL ok = FALSE;
    DWORD host_sz = 64;

    out_path[0] = L'\0';
    if (!device_name[0] || !svc_cimv2) return FALSE;

    /* Step 1: Get DeviceID from Win32_PNPSignedDriver */
    src = device_name;
    dst = escaped_name;
    while (*src && dst < escaped_name + 510) {
        if (*src == L'\'') { *dst++ = L'\''; }
        *dst++ = *src++;
    }
    *dst = L'\0';

    swprintf_s(query, 1024,
        L"SELECT DeviceID FROM Win32_PNPSignedDriver WHERE DeviceName='%s'",
        escaped_name);

    enumerator = wmi_query(svc_cimv2, query);
    if (!enumerator) return FALSE;

    returned = 0;
    if (SUCCEEDED(enumerator->lpVtbl->Next(enumerator, 5000, 1, &obj, &returned))
        && returned > 0) {
        device_id = wmi_get_string(obj, L"DeviceID");
        obj->lpVtbl->Release(obj);
        obj = NULL;
    }
    enumerator->lpVtbl->Release(enumerator);
    enumerator = NULL;

    if (!device_id) return FALSE;

    /* Step 2: Build Antecedent match string with doubled backslashes.
       Format: \\HOSTNAME\ROOT\cimv2:Win32_PNPSignedDriver.DeviceID="PCI\\VEN..." */
    src = device_id;
    dst = escaped_id;
    while (*src && dst < escaped_id + 1020) {
        if (*src == L'\\') { *dst++ = L'\\'; }
        *dst++ = *src++;
    }
    *dst = L'\0';

    GetComputerNameW(hostname, &host_sz);
    swprintf_s(antecedent, 2048,
        L"\\\\%s\\ROOT\\cimv2:Win32_PNPSignedDriver.DeviceID=\"%s\"",
        hostname, escaped_id);

    ui_log(L"Scanning driver files for %s (this may take a moment)...", device_name);

    /* Step 3: Full scan of Win32_PNPSignedDriverCIMDataFile, match Antecedent */
    enumerator = wmi_query(svc_cimv2,
        L"SELECT Antecedent,Dependent FROM Win32_PNPSignedDriverCIMDataFile");
    if (!enumerator) {
        wmi_free_string(device_id);
        return FALSE;
    }

    while (!ok && SUCCEEDED(enumerator->lpVtbl->Next(enumerator, WBEM_INFINITE, 1, &obj, &returned))
           && returned > 0) {
        wchar_t *ante_val = wmi_get_string(obj, L"Antecedent");
        wchar_t *dep_val = wmi_get_string(obj, L"Dependent");
        obj->lpVtbl->Release(obj);
        obj = NULL;

        if (ante_val && dep_val && wcsstr(ante_val, escaped_id)) {
            /* Extract file path from Dependent.
               Format: \\HOST\root\cimv2:CIM_DataFile.Name="c:\\windows\\..." */
            wchar_t *name_start = wcsstr(dep_val, L"Name=\"");
            if (name_start) {
                wchar_t file_path[MAX_PATH];
                wchar_t *fp = file_path;
                name_start += 6; /* skip Name=" */
                while (*name_start && *name_start != L'"' && fp < file_path + MAX_PATH - 1) {
                    if (*name_start == L'\\' && *(name_start + 1) == L'\\') {
                        *fp++ = L'\\';
                        name_start += 2;
                    } else {
                        *fp++ = *name_start++;
                    }
                }
                *fp = L'\0';

                if (_wcsnicmp(file_path, L"c:\\windows\\system32\\driverstore\\", 31) == 0) {
                    /* Extract folder: first 6 path segments */
                    wchar_t tmp[MAX_PATH];
                    int seg = 0;
                    wchar_t *p;
                    wcscpy_s(tmp, MAX_PATH, file_path);
                    for (p = tmp; *p; p++) {
                        if (*p == L'\\') {
                            seg++;
                            if (seg == 6) { *p = L'\0'; break; }
                        }
                    }
                    wcscpy_s(out_path, out_max, tmp);
                    ok = TRUE;
                }
            }
        }

        wmi_free_string(ante_val);
        wmi_free_string(dep_val);
    }

    enumerator->lpVtbl->Release(enumerator);
    wmi_free_string(device_id);
    return ok;
}

BOOL gpu_enumerate(GpuList *list)
{
    HDEVINFO iface_set;
    SP_DEVICE_INTERFACE_DATA iface_data;
    SP_DEVINFO_DATA dev_data;
    DWORD idx;

    if (!list) return FALSE;
    list->count = 0;
    list->shares.count = 0;

    /* Enumerate GPU-PV partition adapter interfaces only */
    iface_set = SetupDiGetClassDevsW(&GUID_GPU_PARTITION_ADAPTER, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (iface_set == INVALID_HANDLE_VALUE) return FALSE;

    iface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (idx = 0; SetupDiEnumDeviceInterfaces(iface_set, NULL,
            &GUID_GPU_PARTITION_ADAPTER, idx, &iface_data); idx++) {
        BYTE detail_buf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 512 * sizeof(wchar_t)];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;

        if (list->count >= MAX_GPUS) break;

        dev_data.cbSize = sizeof(SP_DEVINFO_DATA);
        detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)detail_buf;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(iface_set, &iface_data,
                detail, sizeof(detail_buf), NULL, &dev_data))
            continue;

        list->gpus[list->count].driver_store_path[0] = L'\0';
        list->gpus[list->count].service[0] = L'\0';

        /* Interface path */
        wcscpy_s(list->gpus[list->count].interface_path, 512, detail->DevicePath);

        /* Device name */
        if (!SetupDiGetDeviceRegistryPropertyW(iface_set, &dev_data,
                SPDRP_DEVICEDESC, NULL,
                (BYTE *)list->gpus[list->count].name,
                sizeof(list->gpus[list->count].name), NULL))
            wcscpy_s(list->gpus[list->count].name, 256, L"Unknown GPU");

        /* Instance path */
        if (CM_Get_Device_IDW(dev_data.DevInst,
                list->gpus[list->count].instance_path, 512, 0) != CR_SUCCESS)
            list->gpus[list->count].instance_path[0] = L'\0';

        /* Service name */
        SetupDiGetDeviceRegistryPropertyW(iface_set, &dev_data,
            SPDRP_SERVICE, NULL,
            (BYTE *)list->gpus[list->count].service,
            sizeof(list->gpus[list->count].service), NULL);

        /* DriverStore path via SetupAPI: read INF name from driver key,
           resolve to DriverStore\FileRepository\<folder> */
        {
            HKEY drv_key = SetupDiOpenDevRegKey(iface_set, &dev_data,
                DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
            if (drv_key != INVALID_HANDLE_VALUE) {
                wchar_t inf_name[MAX_PATH] = {0};
                DWORD inf_size = sizeof(inf_name);
                if (RegQueryValueExW(drv_key, L"InfPath", NULL, NULL,
                                     (BYTE *)inf_name, &inf_size) == ERROR_SUCCESS
                    && inf_name[0]) {
                    wchar_t store_path[MAX_PATH];
                    DWORD store_size = MAX_PATH;
                    if (SetupGetInfDriverStoreLocationW(inf_name, NULL, NULL,
                                                         store_path, store_size, &store_size)) {
                        wchar_t *slash = wcsrchr(store_path, L'\\');
                        if (slash) *slash = L'\0';
                        wcscpy_s(list->gpus[list->count].driver_store_path, MAX_PATH, store_path);
                    }
                }
                RegCloseKey(drv_key);
            }
        }

        list->count++;
    }

    SetupDiDestroyDeviceInfoList(iface_set);

    /* Build Plan9 shares from the DriverStore paths found above */
    {
        int i;
        for (i = 0; i < list->count; i++) {
            if (list->gpus[i].driver_store_path[0] &&
                list->shares.count < MAX_GPU_SHARES) {
                GpuDriverShare *s = &list->shares.shares[list->shares.count];
                const wchar_t *ds_pos;
                wchar_t guest[MAX_PATH];

                /* Guest path: DriverStore → HostDriverStore */
                ds_pos = wcsstr(list->gpus[i].driver_store_path, L"DriverStore");
                if (ds_pos) {
                    size_t prefix_len = (size_t)(ds_pos - list->gpus[i].driver_store_path);
                    wcsncpy_s(guest, MAX_PATH, list->gpus[i].driver_store_path, prefix_len);
                    wcscat_s(guest, MAX_PATH, L"HostDriverStore");
                    wcscat_s(guest, MAX_PATH, ds_pos + 11);
                } else {
                    wcscpy_s(guest, MAX_PATH, list->gpus[i].driver_store_path);
                }

                /* Check for duplicate (multiple GPUs may share the same folder) */
                {
                    int j, dup = 0;
                    for (j = 0; j < list->shares.count; j++) {
                        if (_wcsicmp(list->shares.shares[j].host_path,
                                     list->gpus[i].driver_store_path) == 0) {
                            dup = 1; break;
                        }
                    }
                    if (dup) continue;
                }

                swprintf_s(s->share_name, 128, L"AppSandbox.Drv.%d", list->shares.count);
                wcscpy_s(s->host_path, MAX_PATH, list->gpus[i].driver_store_path);
                wcscpy_s(s->guest_path, MAX_PATH, guest);
                s->file_filter[0] = L'\0';
                list->shares.count++;
            }
        }
    }

    return TRUE;
}

/* ---- Share helpers ---- */

/* Add a share entry if the host_path is not already in the list.
   If filename is non-NULL, appends it to the file_filter (semicolon-separated).
   filename==NULL means copy all (DriverStore shares). */
static BOOL add_share_unique(GpuDriverShareList *list,
                              const wchar_t *host_path,
                              const wchar_t *guest_path,
                              const wchar_t *filename)
{
    int i;
    for (i = 0; i < list->count; i++) {
        if (_wcsicmp(list->shares[i].host_path, host_path) == 0) {
            /* Already present — append filename to filter if given */
            if (filename && filename[0]) {
                size_t cur_len = wcslen(list->shares[i].file_filter);
                if (cur_len > 0 && cur_len < 4090)
                    wcscat_s(list->shares[i].file_filter, 4096, L";");
                if (cur_len < 4090)
                    wcscat_s(list->shares[i].file_filter, 4096, filename);
            }
            return TRUE;
        }
    }
    if (list->count >= MAX_GPU_SHARES)
        return FALSE;

    swprintf_s(list->shares[list->count].share_name, 128,
               L"AppSandbox.Drv.%d", list->count);
    wcscpy_s(list->shares[list->count].host_path, MAX_PATH, host_path);
    wcscpy_s(list->shares[list->count].guest_path, MAX_PATH, guest_path);
    list->shares[list->count].file_filter[0] = L'\0';
    if (filename && filename[0])
        wcscpy_s(list->shares[list->count].file_filter, 4096, filename);
    list->count++;
    return TRUE;
}

/* Extract directory from a file path.
   DriverStore paths: returns the FileRepository\<folder> directory.
   Non-DriverStore paths: returns the parent directory. */
static void get_driver_dir(const wchar_t *file_path, wchar_t *dir_out, int dir_max,
                            BOOL *is_driverstore)
{
    wchar_t tmp[MAX_PATH];
    wchar_t *p;

    wcscpy_s(tmp, MAX_PATH, file_path);
    *is_driverstore = FALSE;

    if (_wcsnicmp(tmp, L"c:\\windows\\system32\\driverstore\\", 31) == 0) {
        int seg = 0;
        *is_driverstore = TRUE;
        for (p = tmp; *p; p++) {
            if (*p == L'\\') {
                seg++;
                if (seg == 6) { *p = L'\0'; break; }
            }
        }
        wcscpy_s(dir_out, dir_max, tmp);
    } else {
        wchar_t *last_slash = wcsrchr(tmp, L'\\');
        if (last_slash) *last_slash = L'\0';
        wcscpy_s(dir_out, dir_max, tmp);
    }
}

/* Build guest destination path from host path.
   DriverStore: replace "DriverStore" with "HostDriverStore".
   Non-DriverStore: same path on guest. */
static void make_guest_path(const wchar_t *host_dir, BOOL is_driverstore,
                             wchar_t *guest_out, int guest_max)
{
    if (is_driverstore) {
        const wchar_t *scan = host_dir;
        while (*scan) {
            if (_wcsnicmp(scan, L"DriverStore", 11) == 0) {
                size_t prefix_len = (size_t)(scan - host_dir);
                wchar_t prefix[MAX_PATH];
                wcsncpy_s(prefix, MAX_PATH, host_dir, prefix_len);
                swprintf_s(guest_out, guest_max, L"%sHostDriverStore%s",
                           prefix, scan + 11);
                return;
            }
            scan++;
        }
    }
    wcscpy_s(guest_out, guest_max, host_dir);
}

/* Extract file path from a CIM_DataFile Dependent reference string.
   Format: \\HOST\root\cimv2:CIM_DataFile.Name="c:\\windows\\..."
   Un-doubles backslashes. Returns TRUE on success. */
static BOOL extract_file_path(const wchar_t *dep, wchar_t *out, int out_max)
{
    wchar_t *ns = wcsstr(dep, L"Name=\"");
    wchar_t *w;
    if (!ns) return FALSE;
    ns += 6;
    w = out;
    while (*ns && *ns != L'"' && w < out + out_max - 1) {
        if (*ns == L'\\' && *(ns + 1) == L'\\') {
            *w++ = L'\\';
            ns += 2;
        } else {
            *w++ = *ns++;
        }
    }
    *w = L'\0';
    return out[0] != L'\0';
}


BOOL gpu_get_driver_shares(GpuList *gpu_list, GpuDriverShareList *out)
{
    if (!gpu_list || !out) return FALSE;

    if (gpu_list->shares.count > 0) {
        memcpy(out, &gpu_list->shares, sizeof(GpuDriverShareList));
        return TRUE;
    }

    out->count = 0;
    return FALSE;
}

BOOL gpu_get_default_driver_path(GpuList *list,
    wchar_t *out_path, int path_max,
    wchar_t *out_folder, int folder_max)
{
    HDEVINFO iface_set;
    SP_DEVICE_INTERFACE_DATA iface_data;
    SP_DEVINFO_DATA dev_data;
    wchar_t drv_path[MAX_PATH];
    wchar_t dev_name[256];
    const wchar_t *slash;
    DWORD idx;
    IWbemServices *svc = NULL;
    HRESULT hr;
    BOOL found = FALSE;

    (void)list;

    out_path[0] = L'\0';
    out_folder[0] = L'\0';

    iface_set = SetupDiGetClassDevsW(&GUID_GPU_PARTITION_ADAPTER, NULL, NULL,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (iface_set == INVALID_HANDLE_VALUE) return FALSE;

    /* Connect to WMI for DriverStore resolution */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
            SetupDiDestroyDeviceInfoList(iface_set);
            return FALSE;
        }
    }
    CoInitializeSecurity(NULL, -1, NULL, NULL,
                         RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE,
                         NULL, EOAC_NONE, NULL);
    svc = wmi_connect(L"ROOT\\CIMV2");

    iface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (idx = 0; SetupDiEnumDeviceInterfaces(iface_set, NULL,
            &GUID_GPU_PARTITION_ADAPTER, idx, &iface_data); idx++) {

        dev_data.cbSize = sizeof(SP_DEVINFO_DATA);
        drv_path[0] = L'\0';
        dev_name[0] = L'\0';

        if (!SetupDiGetDeviceInterfaceDetailW(iface_set, &iface_data,
                NULL, 0, NULL, &dev_data) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            continue;

        /* Get device name for WMI lookup */
        SetupDiGetDeviceRegistryPropertyW(iface_set, &dev_data,
            SPDRP_DEVICEDESC, NULL, (BYTE *)dev_name, sizeof(dev_name), NULL);

        if (svc && dev_name[0])
            resolve_driver_store_wmi(dev_name, svc, drv_path, MAX_PATH);

        if (drv_path[0]) {
            wcscpy_s(out_path, path_max, drv_path);
            slash = wcsrchr(drv_path, L'\\');
            if (slash)
                wcscpy_s(out_folder, folder_max, slash + 1);
            else
                wcscpy_s(out_folder, folder_max, drv_path);
            found = TRUE;
            break;
        }
    }

    if (svc) svc->lpVtbl->Release(svc);
    CoUninitialize();
    SetupDiDestroyDeviceInfoList(iface_set);
    return found;
}


