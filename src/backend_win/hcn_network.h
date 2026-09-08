#ifndef HCN_NETWORK_H
#define HCN_NETWORK_H

#include <windows.h>

/* DLL export/import */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

/* Opaque HCN handles */
typedef void *HCN_NETWORK;
typedef void *HCN_ENDPOINT;

/* Initialize HCN - loads computenetwork.dll dynamically.
   Returns TRUE if HCN is available. */
BOOL hcn_init(void);

/* Clean up HCN module. */
void hcn_cleanup(void);

/* Delete any stale AppSandbox networks left over from a previous run.
   Call once at startup, before any VMs are started. */
void hcn_cleanup_stale_networks(void);

/* Create a NAT network. Returns S_OK on success.
   network_id: output GUID for the created network.

   The NAT subnet is chosen on first use from a small static candidate
   list (192.168.42.0/24, then 192.168.142.0/24) - the first that does
   not overlap a host adapter wins. Decided once per process, stable. */
HRESULT hcn_create_nat_network(GUID *network_id);

/* Returns the first 3 octets of the chosen NAT subnet ("192.168.42" or
   "192.168.142"). Triggers the pick on first call. Stable for process
   lifetime. Callers build full IPs via printf-style formatting. */
const char *hcn_nat_subnet_base(void);

/* Create an internal (host-only) network. Returns S_OK on success. */
HRESULT hcn_create_internal_network(GUID *network_id);

/* Create a network bridged to an external adapter.
   adapter_name: friendly name (e.g. L"Ethernet"). If NULL/empty, auto-detects.
   Returns S_OK on success. */
HRESULT hcn_create_external_network(GUID *network_id, const wchar_t *adapter_name);

/* Create an endpoint on a network.
   network_id: the network to attach to.
   endpoint_id: output GUID for the created endpoint.
   endpoint_guid_str: output string representation of endpoint GUID (for HCS JSON).
   nat_ip: for NAT networks, the static IP to assign (e.g. "172.20.0.2"); if
           NULL/empty, NAT endpoints default to "172.20.0.2". Ignored for
           Internal and External networks (which always use DHCP). */
HRESULT hcn_create_endpoint(const GUID *network_id, GUID *endpoint_id,
                            wchar_t *endpoint_guid_str, size_t str_len,
                            const char *nat_ip);

/* Delete a network by GUID. */
ASB_API HRESULT hcn_delete_network(const GUID *network_id);

/* Delete an endpoint by GUID. */
ASB_API HRESULT hcn_delete_endpoint(const GUID *endpoint_id);

/* Enumerate network adapters suitable for External networking.
   Calls the callback for each adapter found. Returns count. */
typedef void (*HcnAdapterCallback)(const wchar_t *friendly_name, int if_type, void *ctx);
ASB_API int hcn_enum_adapters(HcnAdapterCallback cb, void *ctx);

#endif /* HCN_NETWORK_H */
