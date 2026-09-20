/* hcn_network.c - base HCN networking.
 *
 * HCN entry-point loading (hcn_init), the fixed-ID NAT/Internal
 * networks, endpoint and network create/delete. The External-mode
 * feature lives in hcn_external.c, hcn_wmi.c, hcn_inventory.c, and
 * hcn_parse.c; this file keeps the base product's networking surface
 * and owns the loaded HCN function pointers. */

#include "hcn_network.h"
#include "hcn_private.h"
#include "ui.h"
#include <objbase.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")

/* ---- Loaded function pointers ---- */

HMODULE g_hcn_dll = NULL;
static SRWLOCK g_network_lock = SRWLOCK_INIT;

PFN_HcnCreateNetwork    pfnCreateNet;
PFN_HcnCreateEndpoint   pfnCreateEp;
PFN_HcnDeleteNetwork    pfnDeleteNet;
PFN_HcnDeleteEndpoint   pfnDeleteEp;
PFN_HcnCloseNetwork     pfnCloseNet;
PFN_HcnCloseEndpoint    pfnCloseEp;
PFN_HcnOpenNetwork      pfnOpenNet;
PFN_HcnEnumerateNetworks pfnEnumNet;
PFN_HcnQueryNetworkProperties pfnQueryNetProps;

/* ---- Shared network-create lock ---- */

/* Thin accessors for the static g_network_lock (hcn_private.h). Owned
   External and Internal create regions hold it across re-check+create;
   the shared owned-delete core holds it across the actual-ID check and
   HcnDeleteNetwork call. */
void hcn_network_lock_acquire(void)
{
    AcquireSRWLockExclusive(&g_network_lock);
}

void hcn_network_lock_release(void)
{
    ReleaseSRWLockExclusive(&g_network_lock);
}

/* ---- Public API ---- */

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
    pfnQueryNetProps = (PFN_HcnQueryNetworkProperties)GetProcAddress(g_hcn_dll, "HcnQueryNetworkProperties");

    /* Only the functions required by NAT/Internal creation are mandatory
       for global HCN availability. Query/Enumerate are additional External
       acquisition capabilities: when they are missing, External
       acquisition fails with an explicit capability error while
       NAT/Internal keep working. */
    if (!pfnCreateNet || !pfnCreateEp || !pfnCloseNet || !pfnCloseEp) {
        FreeLibrary(g_hcn_dll);
        g_hcn_dll = NULL;
        return FALSE;
    }

    return TRUE;
}

/* Fixed GUIDs for AppSandbox networks so we can clean up across runs.
   The two the pure classify TU also compares against compile from the
   single-source initializers in hcn_private.h. */
const GUID APPSANDBOX_NAT_GUID = APPSANDBOX_NAT_GUID_INIT;

const GUID APPSANDBOX_INTERNAL_GUID = APPSANDBOX_INTERNAL_GUID_INIT;

const GUID APPSANDBOX_EXTERNAL_GUID = {
    0xA5B01234, 0x5678, 0x9ABC,
    { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x88 }
};

/* Check if a network already exists by GUID. Returns TRUE if it does. */
static BOOL hcn_network_exists(const GUID *id)
{
    void *network = NULL;
    PWSTR er = NULL;
    HRESULT hr;

    if (!pfnOpenNet) return FALSE;

    hr = pfnOpenNet(id, &network, &er);
    if (er) hcn_free_string(er);
    if (SUCCEEDED(hr) && network) {
        if (pfnCloseNet) pfnCloseNet(network);
        return TRUE;
    }
    return FALSE;
}

void hcn_cleanup(void)
{
    if (g_hcn_dll) {
        FreeLibrary(g_hcn_dll);
        g_hcn_dll = NULL;
    }
}

/* -------------------------------------------------------------------------
 *  NAT subnet selection
 *
 *  We use a small fixed list of /24 candidates and pick the first one that
 *  doesn't overlap any IPv4 subnet currently bound to a host adapter.
 *  Default Switch (172.20.48.0/20) is built-in on every Windows 11 host;
 *  192.168.42.0/24 sidesteps it. The fallback 192.168.142.0/24 covers the
 *  unlikely case where the user's home network or VPN claims .42.
 *
 *  Decided once on first call, stable for process lifetime. No saved-state
 *  coordination, no per-VM adoption -- the same probe runs every start.
 *  HCN's startup network-delete (hcn_cleanup_stale_networks) wipes the
 *  previous run's network so the freshly-picked subnet is always honored.
 * ------------------------------------------------------------------------- */

static char g_nat_base[16];   /* "192.168.42" or "192.168.142", no trailing dot */

typedef struct { DWORD network; int prefix_len; } SubnetInfo;

static BOOL ranges_overlap(DWORD a_net, int a_prefix, DWORD b_net, int b_prefix)
{
    DWORD a_mask = (a_prefix == 0) ? 0 : (~0u << (32 - a_prefix));
    DWORD b_mask = (b_prefix == 0) ? 0 : (~0u << (32 - b_prefix));
    DWORD a_start = a_net & a_mask;
    DWORD a_end   = a_start | ~a_mask;
    DWORD b_start = b_net & b_mask;
    DWORD b_end   = b_start | ~b_mask;
    return (a_start <= b_end) && (b_start <= a_end);
}

static int collect_inuse_subnets(SubnetInfo *out, int cap)
{
    ULONG size = 0;
    int n = 0;
    PIP_ADAPTER_ADDRESSES buf = NULL;
    PIP_ADAPTER_ADDRESSES a;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                          NULL, NULL, &size);
    if (size == 0) return 0;
    buf = (PIP_ADAPTER_ADDRESSES)malloc(size);
    if (!buf) return 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                              NULL, buf, &size) != NO_ERROR) {
        free(buf); return 0;
    }
    for (a = buf; a && n < cap; a = a->Next) {
        PIP_ADAPTER_UNICAST_ADDRESS u;
        for (u = a->FirstUnicastAddress; u && n < cap; u = u->Next) {
            SOCKADDR_IN *sin;
            if (!u->Address.lpSockaddr) continue;
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            if (u->OnLinkPrefixLength > 32) continue;
            sin = (SOCKADDR_IN *)u->Address.lpSockaddr;
            out[n].network    = ntohl(sin->sin_addr.s_addr);
            out[n].prefix_len = u->OnLinkPrefixLength;
            n++;
        }
    }
    free(buf);
    return n;
}

static void pick_nat_base_once(void)
{
    static const char *CANDIDATES[] = { "192.168.42", "192.168.142", NULL };
    SubnetInfo used[64];
    int n_used, i, j;

    if (g_nat_base[0]) return;

    n_used = collect_inuse_subnets(used, 64);
    for (i = 0; CANDIDATES[i]; i++) {
        int a, b, c;
        DWORD candidate;
        BOOL conflict = FALSE;
        if (sscanf_s(CANDIDATES[i], "%d.%d.%d", &a, &b, &c) != 3) continue;
        candidate = ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)c << 8);
        for (j = 0; j < n_used; j++) {
            if (ranges_overlap(candidate, 24, used[j].network, used[j].prefix_len)) {
                conflict = TRUE; break;
            }
        }
        if (!conflict) {
            strcpy_s(g_nat_base, sizeof(g_nat_base), CANDIDATES[i]);
            ui_log(L"NAT subnet: %S.0/24 (gateway %S.1)", g_nat_base, g_nat_base);
            return;
        }
    }
    strcpy_s(g_nat_base, sizeof(g_nat_base), CANDIDATES[0]);
    ui_log(L"WARNING: every candidate NAT subnet (192.168.42.0/24, 192.168.142.0/24) overlaps a host adapter. "
           L"Using %S.0/24 anyway; HCN create may fail. Edit hcn_network.c CANDIDATES to add another.",
           g_nat_base);
}

const char *hcn_nat_subnet_base(void)
{
    AcquireSRWLockExclusive(&g_network_lock);
    pick_nat_base_once();
    ReleaseSRWLockExclusive(&g_network_lock);
    return g_nat_base;
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

    AcquireSRWLockExclusive(&g_network_lock);
    /* Reuse the existing AppSandbox NAT network if it's already up -
       multiple VMs share one network, each with its own endpoint. */
    if (hcn_network_exists(&APPSANDBOX_NAT_GUID)) {
        ReleaseSRWLockExclusive(&g_network_lock);
        return S_OK;
    }

    pick_nat_base_once();

    swprintf_s(settings, 1024,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxNAT\","
        L"\"Type\":\"NAT\","
        L"\"Ipams\":[{"
            L"\"Type\":\"Static\","
            L"\"Subnets\":[{"
                L"\"IpAddressPrefix\":\"%S.0/24\","
                L"\"Routes\":[{\"NextHop\":\"%S.1\",\"DestinationPrefix\":\"0.0.0.0/0\"}]"
            L"}]"
        L"}]"
        L"}",
        g_nat_base, g_nat_base);

    hr = pfnCreateNet(network_id, settings, &network, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN NAT error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (network && pfnCloseNet)
        pfnCloseNet(network);

    ReleaseSRWLockExclusive(&g_network_lock);
    return hr;
}

static BOOL valid_mac_address(const wchar_t *address)
{
    size_t i;
    if (wcslen(address) != 17) return FALSE;
    for (i = 0; i < 17; i++) {
        if (i % 3 == 2) {
            if (address[i] != L'-' && address[i] != L':') return FALSE;
        } else if (!wcschr(L"0123456789abcdefABCDEF", address[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

HRESULT hcn_create_endpoint(const GUID *network_id, GUID *endpoint_id,
                            wchar_t *endpoint_guid_str, size_t str_len,
                            const char *nat_ip, const wchar_t *mac_address,
                            BOOL mark_borrowed)
{
    wchar_t net_guid_str[64];
    wchar_t ep_guid_str[64];
    wchar_t settings[1024];
    wchar_t mac_settings[64] = L"";
    void *network = NULL;
    void *endpoint = NULL;
    PWSTR error_record = NULL;
    GUID new_endpoint_id;
    HRESULT hr;

    /* Validate parameters first, then establish the entry invariant: every
       failure return leaves the public outputs empty (a zero GUID and a
       NUL string), so no caller state can ever reference an endpoint that
       was not fully created. The GUID is zeroed before the string-length
       validation so even the E_POINTER/E_INVALIDARG returns leave it
       empty; the string itself is never written on a rejected length - a
       buffer the caller declared too small is not trusted for any
       write. */
    if (!network_id || !endpoint_id)
        return E_POINTER;
    ZeroMemory(endpoint_id, sizeof(*endpoint_id));
    if (endpoint_guid_str && str_len < 37) /* 36 GUID chars + NUL */
        return E_INVALIDARG;
    if (endpoint_guid_str)
        endpoint_guid_str[0] = L'\0';
    ZeroMemory(&new_endpoint_id, sizeof(new_endpoint_id));

    if (!g_hcn_dll || !pfnCreateEp || !pfnOpenNet)
        return E_NOT_VALID_STATE;
    if (!mac_address || !valid_mac_address(mac_address))
        return E_INVALIDARG;
    swprintf_s(mac_settings, ARRAYSIZE(mac_settings), L",\"MacAddress\":\"%s\"", mac_address);

    /* Open the network */
    hr = pfnOpenNet(network_id, &network, &error_record);
    if (error_record) { hcn_free_string(error_record); error_record = NULL; }
    if (FAILED(hr)) {
        /* A failed call that still returned a handle owns it: close
           defensively, never leak it. */
        if (network && pfnCloseNet) pfnCloseNet(network);
        return hr;
    }
    if (!network) {
        /* Success HRESULT without a handle: contract failure. */
        ui_log(L"External: HcnOpenNetwork returned success without a network handle.");
        return E_FAIL;
    }

    /* Endpoint GUID generation must succeed before any create or publish.
       On failure no HCN create is called and the outputs stay empty. The
       GUID lives in a local until the endpoint truly exists; the caller's
       outputs are published only after a successful create. */
    if (FAILED(CoCreateGuid(&new_endpoint_id))) {
        ui_log(L"External: endpoint GUID generation failed; no endpoint created.");
        if (pfnCloseNet) pfnCloseNet(network);
        return E_FAIL;
    }
    guid_to_string(network_id, net_guid_str, 64);
    guid_to_string(&new_endpoint_id, ep_guid_str, 64);

    /* Static IP for NAT; DHCP for Internal (ICS) and External (Transparent).
       Borrowed External endpoints additionally carry the human-identifiable
       Name marker (same GUID as the endpoint; log/diagnostic only - never
       an ownership or cleanup criterion). */
    if (IsEqualGUID(network_id, &APPSANDBOX_NAT_GUID) && nat_ip && nat_ip[0]) {
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\","
            L"\"IpConfigurations\":[{\"IpAddress\":\"%S\",\"PrefixLength\":24}]"
            L"%s}", net_guid_str, nat_ip, mac_settings);
    } else if (IsEqualGUID(network_id, &APPSANDBOX_NAT_GUID)) {
        hcn_nat_subnet_base();
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\","
            L"\"IpConfigurations\":[{\"IpAddress\":\"%S.2\",\"PrefixLength\":24}]"
            L"%s}", net_guid_str, g_nat_base, mac_settings);
    } else if (mark_borrowed &&
               !IsEqualGUID(network_id, &APPSANDBOX_INTERNAL_GUID)) {
        /* Borrowed External: Name uses the SAME random endpoint GUID; no
           PortName; HostComputeNetwork is the actual acquisition network. */
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"Name\":\"AppSandboxBorrowedEndpoint-%s\","
            L"\"HostComputeNetwork\":\"%s\""
            L"%s}", ep_guid_str, net_guid_str, mac_settings);
    } else {
        /* Internal (ICS DHCP) or owned External (LAN DHCP) - no static IP */
        swprintf_s(settings, 1024,
            L"{"
            L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
            L"\"HostComputeNetwork\":\"%s\""
            L"%s}", net_guid_str, mac_settings);
    }

    hr = pfnCreateEp(network, &new_endpoint_id, settings, &endpoint, &error_record);

    if (error_record) {
        if (FAILED(hr)) ui_log(L"HCN Endpoint error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (endpoint && pfnCloseEp) {
        /* The endpoint handle is not kept after creation; close it
           defensively whether the call reported failure or success. */
        pfnCloseEp(endpoint);
    }
    if (network && pfnCloseNet) pfnCloseNet(network);
    network = NULL;

    if (SUCCEEDED(hr) && !endpoint) {
        /* Success HRESULT without an endpoint handle: contract failure;
           outputs are not published. */
        ui_log(L"External: endpoint create returned success without an endpoint handle.");
        hr = E_FAIL;
    }
    if (FAILED(hr))
        return hr; /* public outputs remain empty */

    /* Success: publish the endpoint identity to the caller now, exactly
       once, as one group. */
    *endpoint_id = new_endpoint_id;
    if (endpoint_guid_str)
        wcscpy_s(endpoint_guid_str, str_len, ep_guid_str);

    if (mark_borrowed &&
        !IsEqualGUID(network_id, &APPSANDBOX_NAT_GUID) &&
        !IsEqualGUID(network_id, &APPSANDBOX_INTERNAL_GUID)) {
        ui_log(L"borrowed endpoint AppSandboxBorrowedEndpoint-%s created.",
               ep_guid_str);
    }

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
    if (error_record) hcn_free_string(error_record);
    return hr;
}

/* Shared owned-delete core. The wrappers decide what ID is owned for
   their mode; this layer only compares binary IDs and serializes that
   comparison with the HCN delete against concurrent create/re-check
   regions. The mode-specific wrappers keep their own logging and
   not-found policy. The output flag distinguishes a missing export from
   an HCN call that itself returns the same HRESULT. */
HRESULT hcn_delete_network_if_owned(const GUID *network_id,
                                    const GUID *expected_owned_id,
                                    BOOL *out_delete_attempted,
                                    PWSTR *out_error_record)
{
    PWSTR error_record = NULL;
    HRESULT hr;

    if (out_error_record)
        *out_error_record = NULL;
    if (out_delete_attempted)
        *out_delete_attempted = FALSE;

    if (!network_id || !expected_owned_id ||
        IsEqualGUID(network_id, &GUID_NULL) ||
        IsEqualGUID(expected_owned_id, &GUID_NULL))
        return S_FALSE;

    hcn_network_lock_acquire();
    if (!IsEqualGUID(network_id, expected_owned_id)) {
        hcn_network_lock_release();
        return S_FALSE;
    }

    if (!pfnDeleteNet) {
        hcn_network_lock_release();
        return E_NOT_VALID_STATE;
    }

    if (out_delete_attempted)
        *out_delete_attempted = TRUE;
    hr = pfnDeleteNet(network_id, &error_record);
    hcn_network_lock_release();

    if (out_error_record)
        *out_error_record = error_record;
    else if (error_record)
        hcn_free_string(error_record);

    return SUCCEEDED(hr) ? S_OK : hr;
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
    if (error_record) hcn_free_string(error_record);
    return hr;
}

/* ---- Internal vSwitch census support (the stale-ping notifier) ----
 * The acquire's failure exit fires the registered callback - the single
 * stale-ping emission site (the acquire itself lives in
 * hcn_internal.c; the pair lives here because it is the HCN layer's
 * own state, and asb_core.h is never included by hcn_*.c). The public
 * asb_set_census_stale_callback in asb_core.h delegates. Registration
 * is once, before any worker thread exists. */

static void (*g_census_stale_cb)(void *user_data) = NULL;
static void *g_census_stale_user_data = NULL;

void hcn_set_census_stale_callback(void (*cb)(void *user_data),
                                   void *user_data)
{
    g_census_stale_cb = cb;
    g_census_stale_user_data = user_data;
}

void hcn_notify_census_stale(void)
{
    if (g_census_stale_cb)
        g_census_stale_cb(g_census_stale_user_data);
}

/* Free a census (the classify TU's HeapAlloc'd entries). Safe on the
   UNAVAILABLE early-exit shape by construction. */
void hcn_internal_switch_census_free(HcnInternalSwitchCensus *c)
{
    if (!c)
        return;
    if (c->entries) {
        HeapFree(GetProcessHeap(), 0, c->entries);
        c->entries = NULL;
    }
    c->count = 0;
}
