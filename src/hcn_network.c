#include <winsock2.h>
#include "hcn_network.h"
#include "ui.h"
#include <stdio.h>
#include <objbase.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

#pragma comment(lib, "ole32.lib")

/* ---- HCN function pointer types ---- */

typedef HRESULT (WINAPI *PFN_HcnCreateNetwork)(
    REFGUID id, PCWSTR settings, void **network, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnCreateEndpoint)(
    void *network, REFGUID id, PCWSTR settings, void **endpoint, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnDeleteNetwork)(
    REFGUID id, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnDeleteEndpoint)(
    REFGUID id, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnCloseNetwork)(void *network);
typedef HRESULT (WINAPI *PFN_HcnCloseEndpoint)(void *endpoint);
typedef HRESULT (WINAPI *PFN_HcnOpenNetwork)(
    REFGUID id, void **network, PWSTR *errorRecord);

/* ---- Loaded function pointers ---- */

static HMODULE g_hcn_dll = NULL;

static PFN_HcnCreateNetwork    pfnCreateNet;
static PFN_HcnCreateEndpoint   pfnCreateEp;
static PFN_HcnDeleteNetwork    pfnDeleteNet;
static PFN_HcnDeleteEndpoint   pfnDeleteEp;
static PFN_HcnCloseNetwork     pfnCloseNet;
static PFN_HcnCloseEndpoint    pfnCloseEp;
static PFN_HcnOpenNetwork      pfnOpenNet;

/* ---- Helpers ---- */

static void guid_to_string(const GUID *g, wchar_t *out, size_t out_len)
{
    swprintf_s(out, out_len,
        L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* ---- Public API ---- */

typedef HRESULT (WINAPI *PFN_HcnEnumerateNetworks)(
    PCWSTR query, PWSTR *networks, PWSTR *errorRecord);

static PFN_HcnEnumerateNetworks pfnEnumNet;

BOOL hcn_init(void)
{
    g_hcn_dll = LoadLibraryW(L"computenetwork.dll");
    if (!g_hcn_dll)
        return FALSE;

    pfnCreateNet  = (PFN_HcnCreateNetwork)GetProcAddress(g_hcn_dll, "HcnCreateNetwork");
    pfnCreateEp   = (PFN_HcnCreateEndpoint)GetProcAddress(g_hcn_dll, "HcnCreateEndpoint");
    pfnDeleteNet  = (PFN_HcnDeleteNetwork)GetProcAddress(g_hcn_dll, "HcnDeleteNetwork");
    pfnDeleteEp   = (PFN_HcnDeleteEndpoint)GetProcAddress(g_hcn_dll, "HcnDeleteEndpoint");
    pfnCloseNet   = (PFN_HcnCloseNetwork)GetProcAddress(g_hcn_dll, "HcnCloseNetwork");
    pfnCloseEp    = (PFN_HcnCloseEndpoint)GetProcAddress(g_hcn_dll, "HcnCloseEndpoint");
    pfnOpenNet    = (PFN_HcnOpenNetwork)GetProcAddress(g_hcn_dll, "HcnOpenNetwork");
    pfnEnumNet    = (PFN_HcnEnumerateNetworks)GetProcAddress(g_hcn_dll, "HcnEnumerateNetworks");

    if (!pfnCreateNet || !pfnCreateEp || !pfnCloseNet || !pfnCloseEp) {
        FreeLibrary(g_hcn_dll);
        g_hcn_dll = NULL;
        return FALSE;
    }

    return TRUE;
}

/* Fixed GUIDs for AppSandbox networks so we can clean up across runs */
static const GUID APPSANDBOX_NAT_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }
};

static const GUID APPSANDBOX_INTERNAL_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x77 }
};

static const GUID APPSANDBOX_EXTERNAL_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x88 }
};

/* Find the adapter that carries the default route (0.0.0.0/0). */
/* Find a physical adapter that is UP, has a default gateway, and isn't virtual.
   Prefers Ethernet (IF_TYPE_ETHERNET_CSMACD) over Wi-Fi (IF_TYPE_IEEE80211). */
static BOOL get_default_adapter_name(wchar_t *out, size_t out_len)
{
    ULONG buf_len;
    PIP_ADAPTER_ADDRESSES addrs;
    PIP_ADAPTER_ADDRESSES cur;
    PIP_ADAPTER_ADDRESSES best_eth = NULL;
    PIP_ADAPTER_ADDRESSES best_wifi = NULL;
    PIP_ADAPTER_ADDRESSES pick;
    DWORD ret;

    buf_len = 15000;
    addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
    if (!addrs) return FALSE;

    ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        NULL, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
        if (!addrs) return FALSE;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, addrs, &buf_len);
    }
    if (ret != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, addrs);
        return FALSE;
    }

    for (cur = addrs; cur != NULL; cur = cur->Next) {
        BOOL has_gateway = FALSE;

        /* Must be UP */
        if (cur->OperStatus != IfOperStatusUp)
            continue;

        /* Skip virtual adapters (Hyper-V vSwitch, VPN, loopback) */
        if (wcsstr(cur->FriendlyName, L"vEthernet") != NULL)
            continue;
        if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (cur->IfType == IF_TYPE_TUNNEL)
            continue;

        /* Must have a gateway (i.e. connected to a network with internet) */
        if (cur->FirstGatewayAddress != NULL)
            has_gateway = TRUE;
        if (!has_gateway)
            continue;

        /* Prefer Ethernet over Wi-Fi */
        if (cur->IfType == IF_TYPE_ETHERNET_CSMACD && !best_eth)
            best_eth = cur;
        else if (cur->IfType == IF_TYPE_IEEE80211 && !best_wifi)
            best_wifi = cur;
    }

    pick = best_eth ? best_eth : best_wifi;
    if (pick) {
        wcscpy_s(out, out_len, pick->FriendlyName);
        ui_log(L"Selected adapter: %s (type %lu, index %lu)",
               out, pick->IfType, pick->IfIndex);
        HeapFree(GetProcessHeap(), 0, addrs);
        return TRUE;
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return FALSE;
}

/* Check if a network already exists by GUID. Returns TRUE if it does. */
static BOOL hcn_network_exists(const GUID *id)
{
    void *network = NULL;
    PWSTR er = NULL;
    HRESULT hr;

    if (!pfnOpenNet) return FALSE;

    hr = pfnOpenNet(id, &network, &er);
    if (er) LocalFree(er);
    if (SUCCEEDED(hr) && network) {
        if (pfnCloseNet) pfnCloseNet(network);
        return TRUE;
    }
    return FALSE;
}

/* Delete all AppSandbox networks by their fixed GUIDs. Fast — no enumeration. */
static void hcn_cleanup_our_networks(void)
{
    PWSTR er = NULL;
    if (!pfnDeleteNet) return;
    pfnDeleteNet(&APPSANDBOX_NAT_GUID, &er);
    if (er) { LocalFree(er); er = NULL; }
    pfnDeleteNet(&APPSANDBOX_INTERNAL_GUID, &er);
    if (er) { LocalFree(er); er = NULL; }
    pfnDeleteNet(&APPSANDBOX_EXTERNAL_GUID, &er);
    if (er) { LocalFree(er); er = NULL; }
}

void hcn_cleanup(void)
{
    if (g_hcn_dll) {
        FreeLibrary(g_hcn_dll);
        g_hcn_dll = NULL;
    }
}

HRESULT hcn_create_nat_network(GUID *network_id)
{
    wchar_t settings[1024];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    *network_id = APPSANDBOX_NAT_GUID;

    /* Clean up stale network from previous run */
    hcn_cleanup_our_networks();

    swprintf_s(settings, 1024,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxNAT\","
        L"\"Type\":\"NAT\","
        L"\"Ipams\":[{"
            L"\"Type\":\"Static\","
            L"\"Subnets\":[{"
                L"\"IpAddressPrefix\":\"172.20.0.0/16\","
                L"\"Routes\":[{\"NextHop\":\"172.20.0.1\",\"DestinationPrefix\":\"0.0.0.0/0\"}]"
            L"}]"
        L"}]"
        L"}");

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN NAT error: %s", error_record);
        LocalFree(error_record);
    }
    if (network && pfnCloseNet)
        pfnCloseNet(network);

    return hr;
}

HRESULT hcn_create_internal_network(GUID *network_id)
{
    wchar_t settings[1024];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    *network_id = APPSANDBOX_INTERNAL_GUID;

    hcn_cleanup_our_networks();

    swprintf_s(settings, 1024,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxInternal\","
        L"\"Type\":\"ICS\""
        L"}");

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN Internal error: %s", error_record);
        LocalFree(error_record);
    }
    if (network && pfnCloseNet) pfnCloseNet(network);

    return hr;
}

HRESULT hcn_create_external_network(GUID *network_id, const wchar_t *adapter_name)
{
    wchar_t settings[2048];
    wchar_t adapter[256];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    *network_id = APPSANDBOX_EXTERNAL_GUID;

    hcn_cleanup_our_networks();

    /* Use specified adapter, or auto-detect */
    if (adapter_name && adapter_name[0] != L'\0') {
        wcscpy_s(adapter, 256, adapter_name);
        ui_log(L"Using adapter: %s", adapter);
    } else {
        if (!get_default_adapter_name(adapter, 256)) {
            ui_log(L"Error: No connected network adapter found for External network.");
            return E_FAIL;
        }
    }

    swprintf_s(settings, 2048,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxExternal\","
        L"\"Type\":\"Transparent\","
        L"\"Policies\":[{\"Type\":\"NetAdapterName\",\"Settings\":{\"NetworkAdapterName\":\"%s\"}}]"
        L"}", adapter);

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN External error: %s", error_record);
        LocalFree(error_record);
    }
    if (network && pfnCloseNet) pfnCloseNet(network);

    return hr;
}

HRESULT hcn_create_endpoint(const GUID *network_id, GUID *endpoint_id,
                            wchar_t *endpoint_guid_str, size_t str_len)
{
    wchar_t net_guid_str[64];
    wchar_t ep_guid_str[64];
    wchar_t settings[1024];
    void *network = NULL;
    void *endpoint = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateEp || !pfnOpenNet)
        return E_NOT_VALID_STATE;

    /* Open the network */
    hr = pfnOpenNet(network_id, &network, &error_record);
    if (error_record) { LocalFree(error_record); error_record = NULL; }
    if (FAILED(hr)) return hr;

    CoCreateGuid(endpoint_id);
    guid_to_string(network_id, net_guid_str, 64);
    guid_to_string(endpoint_id, ep_guid_str, 64);

    /* Static IP for NAT; DHCP for Internal (ICS) and External (Transparent) */
    if (IsEqualGUID(network_id, &APPSANDBOX_NAT_GUID)) {
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\","
            L"\"IpConfigurations\":[{\"IpAddress\":\"172.20.0.2\",\"PrefixLength\":16}]"
            L"}", net_guid_str);
    } else {
        /* Internal (ICS DHCP) or External (LAN DHCP) — no static IP */
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\""
            L"}", net_guid_str);
    }

    hr = pfnCreateEp(network, endpoint_id, settings, &endpoint, &error_record);

    if (SUCCEEDED(hr) && endpoint_guid_str) {
        wcscpy_s(endpoint_guid_str, str_len, ep_guid_str);
    }

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN Endpoint error: %s", error_record);
        LocalFree(error_record);
    }
    if (endpoint && pfnCloseEp) pfnCloseEp(endpoint);
    if (network && pfnCloseNet) pfnCloseNet(network);

    return hr;
}

HRESULT hcn_delete_network(const GUID *network_id)
{
    PWSTR error_record = NULL;
    HRESULT hr;
    wchar_t guid_str[64];

    if (!g_hcn_dll || !pfnDeleteNet)
        return E_NOT_VALID_STATE;

    StringFromGUID2(network_id, guid_str, 64);
    ui_log(L"HCN: Deleting network %s...", guid_str);

    hr = pfnDeleteNet(network_id, &error_record);
    if (SUCCEEDED(hr)) {
        ui_log(L"HCN: Network deleted.");
    } else {
        ui_log(L"HCN: Delete network failed (0x%08X).", hr);
        if (error_record) {
            ui_log(L"HCN error: %s", error_record);
        }
    }
    if (error_record) LocalFree(error_record);
    return hr;
}

HRESULT hcn_delete_endpoint(const GUID *endpoint_id)
{
    PWSTR error_record = NULL;
    HRESULT hr;
    wchar_t guid_str[64];

    if (!g_hcn_dll || !pfnDeleteEp)
        return E_NOT_VALID_STATE;

    StringFromGUID2(endpoint_id, guid_str, 64);
    ui_log(L"HCN: Deleting endpoint %s...", guid_str);

    hr = pfnDeleteEp(endpoint_id, &error_record);
    if (SUCCEEDED(hr)) {
        ui_log(L"HCN: Endpoint deleted.");
    } else {
        ui_log(L"HCN: Delete endpoint failed (0x%08X).", hr);
        if (error_record) {
            ui_log(L"HCN error: %s", error_record);
        }
    }
    if (error_record) LocalFree(error_record);
    return hr;
}

int hcn_enum_adapters(HcnAdapterCallback cb, void *ctx)
{
    ULONG buf_len = 15000;
    PIP_ADAPTER_ADDRESSES addrs, cur;
    DWORD ret;
    int count = 0;

    addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
    if (!addrs) return 0;

    ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        NULL, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
        if (!addrs) return 0;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, addrs, &buf_len);
    }

    if (ret == ERROR_SUCCESS) {
        for (cur = addrs; cur != NULL; cur = cur->Next) {
            if (cur->OperStatus != IfOperStatusUp) continue;
            if (cur->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (cur->IfType == IF_TYPE_TUNNEL) continue;
            if (wcsstr(cur->FriendlyName, L"vEthernet") != NULL) continue;

            cb(cur->FriendlyName, (int)cur->IfType, ctx);
            count++;
        }
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return count;
}
