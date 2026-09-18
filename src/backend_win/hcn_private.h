#ifndef HCN_PRIVATE_H
#define HCN_PRIVATE_H

/* hcn_private.h - contracts shared between the hcn_* translation units.
 *
 * NOT public API: nothing here is ASB_API or declared in hcn_network.h.
 * Consumers: hcn_parse.c, hcn_inventory.c, hcn_wmi.c, hcn_external.c,
 * hcn_network.c (the self-check tool projects compile the same sources
 * with ASB_API neutralized).
 *
 * Includes wbemidl.h: WmiSession is held by value by its callers, so its
 * full definition (IWbemServices*) must be visible here. */

#include "hcn_network.h"

#include <objbase.h>
#include <wbemidl.h>

/* ---- HCN entry points (loaded by hcn_init in hcn_network.c) ---- */

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
typedef HRESULT (WINAPI *PFN_HcnEnumerateNetworks)(
    PCWSTR query, PWSTR *networks, PWSTR *errorRecord);
typedef HRESULT (WINAPI *PFN_HcnQueryNetworkProperties)(
    void *network, PCWSTR query, PWSTR *properties, PWSTR *errorRecord);

/* ---- Loaded function pointers and DLL handle (hcn_network.c) ---- */

extern HMODULE g_hcn_dll;
extern PFN_HcnCreateNetwork    pfnCreateNet;
extern PFN_HcnCreateEndpoint   pfnCreateEp;
extern PFN_HcnDeleteNetwork    pfnDeleteNet;
extern PFN_HcnDeleteEndpoint   pfnDeleteEp;
extern PFN_HcnCloseNetwork     pfnCloseNet;
extern PFN_HcnCloseEndpoint    pfnCloseEp;
extern PFN_HcnOpenNetwork      pfnOpenNet;
extern PFN_HcnEnumerateNetworks pfnEnumNet;
extern PFN_HcnQueryNetworkProperties pfnQueryNetProps;

/* ---- Fixed historical network IDs (hcn_network.c) ---- */

extern const GUID APPSANDBOX_NAT_GUID;
extern const GUID APPSANDBOX_INTERNAL_GUID;
extern const GUID APPSANDBOX_EXTERNAL_GUID;

/* ---- Shared network-create lock (hcn_network.c) ---- */

/* Thin accessors for the static network-create lock: the owned create's
   re-check-then-create region and the owned-delete shield's
   identity-check+delete hold it, so concurrently started VMs on one
   adapter reuse one network instead of racing its create/delete. The
   lock itself is not exported. */
void hcn_network_lock_acquire(void);
void hcn_network_lock_release(void);

/* ---- Pure helpers (hcn_parse.c) ---- */

/* GUID to canonical lowercase string (log/JSON fields; never identity). */
void guid_to_string(const GUID *g, wchar_t *out, size_t out_len);

/* Parse a GUID string value (36-char no-brace or 38-char braced;
   case-insensitive) via CLSIDFromString. Identity comparisons remain
   binary; textual differences are never identity evidence. */
BOOL parse_guid_value(const wchar_t *s, size_t len, GUID *out);

/* Find an inventory entry by binary InterfaceGuid. */
HcnAdapterEntry *inventory_entry_by_guid(HcnAdapterEntry *entries,
                                         size_t count, const GUID *guid);

/* Parse the HcnEnumerateNetworks result (a JSON array of GUID strings)
   into an allocated GUID array (free() it even when the count is 0). */
HRESULT parse_hcn_network_id_array(const wchar_t *json,
                                   GUID **out_ids, size_t *out_count);

/* ---- Owned-network identity and delete (hcn_external.c) ---- */

/* Delete a network when (and only when) its ID is the derived owned ID
   for the adapter (pure binary identity; no Name parsing, no HCN
   topology query). The identity check and the delete run under the
   shared network lock so a last-user delete cannot interleave with a
   concurrent acquire's re-check-then-create. S_FALSE = refused (null
   ID/T, a non-owned ID, or exact delete-path not-found); otherwise the
   HcnDeleteNetwork HRESULT. */
HRESULT hcn_delete_network_if_owned(const GUID *network_id,
                                    const GUID *adapter_interface_guid);

/* ---- HCN read layer (hcn_inventory.c) ---- */

typedef enum {
    HCN_LOOKUP_FOUND,       /* object opened successfully */
    HCN_LOOKUP_NOT_FOUND,   /* HCN authoritatively reported absence (exact code) */
    HCN_LOOKUP_ERROR        /* open failed for any other reason */
} HcnLookupKind;

/* Free an HCN-returned string (properties JSON, enumeration result,
   error record), per the HCN API memory contract. */
void hcn_free_string(PWSTR s);

/* Open a network by exact GUID and classify the outcome. On
   HCN_LOOKUP_FOUND, *out_handle owns an open network handle the caller
   must close with pfnCloseNet. */
HcnLookupKind hcn_open_network_exact(const GUID *id, void **out_handle,
                                     HRESULT *out_hr);

/* Query a network's properties document (caller owns *out_json). */
HRESULT hcn_query_network_properties(void *network, PWSTR *out_json);

/* Build the full L0 adapter inventory (caller frees with
   adapter_inventory_free). */
HRESULT build_adapter_inventory(HcnAdapterInventory *inv);
void adapter_inventory_free(HcnAdapterInventory *inv);

/* Overlay IPv4 gateway presence onto the inventory (GAA, read-only). */
HRESULT inventory_overlay_ipv4_gateways(HcnAdapterEntry *entries, size_t count);

/* ---- WMI topology module (hcn_wmi.c) ---- */

#define WMI_DEFAULT_DEADLINE_MS 60000

typedef enum {
    TOPOLOGY_FOUND,
    TOPOLOGY_NOT_FOUND,
    TOPOLOGY_PROVIDER_ABSENT,
    TOPOLOGY_CONFLICT,     /* object-level: multiple owners of T - stops the walk, not host-wide ERROR */
    TOPOLOGY_ERROR,
    TOPOLOGY_CAP_OVERFLOW, /* a topology query exceeded the object cap: WMI_FAILED, cap line, no service hint */
    TOPOLOGY_DATA_MALFORMED /* malformed topology data/chain: WMI_FAILED, no service hint */
} TopologyResultKind;

typedef enum {
    WMI_PROVIDER_INSTALLED,  /* the class exists: the WMI work may run */
    WMI_PROVIDER_ABSENT,     /* infrastructure/namespace/class genuinely absent */
    WMI_PROVIDER_ERROR       /* anything else: an error, never provider absence */
} WmiProviderStatus;

typedef struct {
    IWbemServices *svc;
    BOOL com_ref_owned;  /* TRUE when this helper owns a CoInitializeEx ref */
} WmiSession;

typedef struct {
    GUID id;                 /* switch Name GUID (authoritative) */
    BOOL has_id;
    int bound_external_ports; /* count of bound external ports chained to this switch */
} WmiSwitchEntry;

typedef struct {
    GUID adapter_guid;       /* DeviceID GUID (InterfaceGuid identity) */
    BOOL has_guid;
    BOOL is_bound;           /* Ethernet: Msvm_ExternalEthernetPort.IsBound; wireless: the endpoint walk reached a switch */
    wchar_t mac[32];         /* normalized PermanentAddress/MACAddress (cross-evidence) */
    BOOL has_switch;         /* chain completed */
    GUID switch_id;          /* valid when has_switch */
    BOOL is_wifi;            /* uplink modeled as an Msvm_WiFiEndpoint (wireless binding) */
} WmiPortEntry;

typedef struct {
    WmiSwitchEntry *switches;
    size_t switch_count;
    WmiPortEntry *external_ports;   /* all external ports; unbound ones are
                                       diagnostics-only, never owners */
    size_t external_port_count;
    BOOL host_system_visible;       /* Msvm_ComputerSystem was visible. The
                                       authoritative-NOT_FOUND condition
                                       additionally requires the unfiltered
                                       token gate - visibility alone does not
                                       prove an unfiltered view (measured). */
} WmiTopology;

/* Classify a WMI session-establishment failure: infrastructure or
   namespace absence is PROVIDER_ABSENT; everything else is ERROR. */
TopologyResultKind wmi_classify_session_failure(HRESULT hr);

/* Open/close a WMI session with the three-branch COM rule (see the
   source for the apartment-ownership contract). */
HRESULT wmi_session_open(WmiSession *s);
void wmi_session_close(WmiSession *s);

void wmi_topology_free(WmiTopology *t);

/* Build the one-shot read-only WMI topology inventory. */
HRESULT wmi_build_topology(WmiSession *s, ULONGLONG deadline,
                           WmiTopology *out, TopologyResultKind *out_kind);

/* Per-target association: the switch owning the unique BOUND external
   port for the given physical adapter GUID. */
TopologyResultKind wmi_topology_switch_for_external_adapter(
    const WmiTopology *t, const GUID *adapter_guid, const WmiSwitchEntry **out);

/* One cheap class-existence probe per acquire (not a topology build). */
WmiProviderStatus wmi_probe_provider_installed(void);

/* TRUE when the effective token can read Hyper-V WMI classes without
   UAC filtering (elevated token or Hyper-V Administrators membership). */
BOOL hcn_coverage_token_unfiltered(void);

#endif /* HCN_PRIVATE_H */
