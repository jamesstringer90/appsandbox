#ifndef HCN_NETWORK_H
#define HCN_NETWORK_H

/* winsock2/ws2tcpip before windows.h: ws2tcpip.h is what makes
   iphlpapi.h pull in netioapi.h (MIB_IF_ROW2, GetIfTable2, NET_LUID);
   without it the NTDDI-guarded include chain silently omits those
   types. Consumers that already include winsock2.h first are
   unaffected (include guards). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

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

/* ---- External owned-network identity ----
   Internal contracts, deliberately without ASB_API: they are not part of
   the DLL's public surface. The core uses them in-repository, and the
   tools/hcn-selfcheck tests compile the production hcn_parse.c (the
   pure contract unit) as their own translation unit to exercise the
   real code (no duplicate implementation anywhere). */

/* Derive the stable per-NIC owned External network ID:
   UUID v5(APPSANDBOX_EXTERNAL_NS, RFC 4122 bytes of nic_interface_guid).
   Deterministic per NIC; the namespace GUID is never an HCN network ID. */
HRESULT hcn_external_owned_id(const GUID *nic_interface_guid, GUID *out_id);

/* Owned network display Name: exactly
   "AppSandboxExternal-{<36-char lowercase NIC GUID>}" - one brace pair.
   out must hold at least 58 wchars. */
HRESULT hcn_external_owned_name(const GUID *nic_interface_guid,
                                wchar_t *out, size_t out_len);

/* Strict Name parse: the entire format, GUID hex case variants
   accepted, no suffix. Yields only a candidate NIC G - never an
   authorization (callers must require actual network-ID equality too). */
BOOL hcn_external_parse_owned_name(const wchar_t *name, GUID *out_nic_guid);

/* ---- L0 read-only adapter inventory ---- */

#define HCN_PHYS_NAME_MAX 256
#define HCN_PHYS_DESC_MAX 256
#define HCN_PHYS_COMPONENT_MAX 128

/* One NDIS interface row (GetIfTable2) merged with SetupAPI PnP identity
   and the IPv4 gateway overlay (GetAdaptersAddresses by InterfaceGuid).
   The inventory holds ALL interface rows: foreign binding-evidence
   translation uses the full table. is_hardware_eligible marks the
   physical-adapter subset used for selection, the sweep set, and UI
   enumeration (loopback, tunnel, vms_mp management, and
   HardwareInterface=false software-only entries are ineligible). Binary
   InterfaceGuid is the canonical identity; FriendlyName is only a
   selector/log key. Temporary data: lives inside one acquisition or
   enumeration call, never persisted. */
typedef struct {
    NET_LUID   luid;
    NET_IFINDEX ifindex;
    GUID       interface_guid;              /* canonical binary identity */
    wchar_t    friendly_name[HCN_PHYS_NAME_MAX]; /* connection name (or NDIS alias) */
    BOOL       name_is_alias;               /* TRUE when no registry connection name found */
    wchar_t    description[HCN_PHYS_DESC_MAX];
    ULONG      if_type;                     /* IF_TYPE_ETHERNET_CSMACD, IF_TYPE_IEEE80211, ... */
    ULONG      oper_status;                 /* NET_IF_OPER_STATUS_* */
    BOOL       not_media_connected;         /* MIB_IF_ROW2 NotMediaConnected flag */
    BOOL       hw_interface;                /* MIB_IF_ROW2 HardwareInterface bit */
    BOOL       has_ipv4_gateway;            /* GAA overlay: physical IPv4 gateway present */
    BOOL       is_hardware_eligible;        /* physical-adapter subset flag (see above) */
    UCHAR      mac[IF_MAX_PHYS_ADDRESS_LENGTH]; /* normalized physical address */
    ULONG      mac_len;
    BOOL       has_component_id;            /* SetupAPI driver ComponentId */
    wchar_t    component_id[HCN_PHYS_COMPONENT_MAX];
} HcnAdapterEntry;

typedef struct {
    HcnAdapterEntry *entries;
    size_t           count;
} HcnAdapterInventory;

/* ---- HCN network properties parsing and candidate scan ----
   Internal contracts (no ASB_API): consumed inside the core; the
   tools/hcn-selfcheck tests compile the production hcn_parse.c and
   exercise the parser/classifier with fixtures. */

#define HCN_MAX_NAME_CHARS     128
#define HCN_MAX_TYPE_CHARS     64
#define HCN_MAX_BINDING_VALUES 8
#define HCN_MAX_BINDING_CHARS  256

/* Which supported location a decoded binding value came from. The
   rules differ by kind: GUID-typed evidence compares directly with the
   target (an unequal GUID is off-T even when absent from L0),
   while a NetAdapterName value counts as GUID evidence only when it
   equals the target or a current interface - otherwise it translates
   as a name. */
typedef enum {
    BINDING_SRC_IC_POLICY,  /* Policies[] Type=InterfaceConstraint, payload InterfaceGuid (Settings or Data) */
    BINDING_SRC_NAA_POLICY, /* Policies[] Type=NetAdapterName, Settings.NetworkAdapterName */
    BINDING_SRC_TOP_IC      /* top-level string InterfaceConstraint (old-branch handler, retained) */
} BindingSource;

/* One binding value's resolution against the target. */
typedef enum {
    BINDING_MATCH,          /* definite: resolves to the target GUID */
    BINDING_MISMATCH,       /* definite: resolves to another GUID */
    BINDING_UNRESOLVED      /* cannot be located relative to the target */
} BindingMatch;

typedef struct {
    BindingMatch match;
    BOOL resolved;          /* TRUE when definitely resolved to a GUID */
    GUID resolved_guid;     /* valid when resolved */
} BindingResolution;

/* Aggregate classification of one network's binding evidence relative
   to a target (the directional classification). */
typedef enum {
    BINDING_ON_T,       /* a definite value resolves to T (whatever else failed to read) */
    BINDING_OFF_T,      /* definite elsewhere, nothing left unresolved */
    BINDING_UNPROVEN,   /* no definite value; definite elsewhere plus unresolved; or off-T values disagree */
    BINDING_CONFLICT    /* definite values disagree, any resolving to T */
} BindingClass;

/* Parsed HcnQueryNetworkProperties document. Binding values are retained
   with their source kind; unreadability markers are directional inputs
   to classify_binding_values (never first-wins, never silently
   ignored). */
typedef struct {
    BOOL has_id;                  /* top-level "ID" */
    GUID id;
    BOOL has_switch_guid;         /* optional "SwitchGuid" (correlation data, not binding evidence) */
    GUID switch_guid;

    BOOL has_name;                /* top-level "Name" */
    BOOL name_truncated;
    wchar_t name[HCN_MAX_NAME_CHARS];

    BOOL has_type;                /* top-level "Type" */
    BOOL type_truncated;
    wchar_t type[HCN_MAX_TYPE_CHARS];

    int binding_count;
    BindingSource binding_src[HCN_MAX_BINDING_VALUES];
    BOOL binding_truncated;       /* a value longer than the buffer */
    BOOL binding_overflow;        /* more values than the slot capacity: object-level incompleteness */
    BOOL binding_unreadable;      /* recognized binding policy without a readable value */
    BOOL binding_non_string;      /* top-level InterfaceConstraint present but not a string */
    BOOL unresolved_type_policy;  /* a policy whose Type is absent/unreadable: unresolved, never ignored */
    wchar_t bindings[HCN_MAX_BINDING_VALUES][HCN_MAX_BINDING_CHARS];
} HcnNetworkProps;

/* Parse one HcnQueryNetworkProperties document. S_OK when structurally
   valid (required fields may still be absent - callers validate);
   E_FAIL on malformed structure or conflicting duplicate fields. A
   parse failure degrades that network's scan record to unproven; it
   never fails the whole enumeration scan. */
HRESULT parse_hcn_network_properties(const wchar_t *json, HcnNetworkProps *props);

/* Aggregate one parsed document's binding evidence relative to
   target_guid, using the FULL interface table (all rows, not the
   hardware-filtered subset) for name translation. */
BindingClass classify_binding_values(const HcnNetworkProps *props,
                                     const HcnAdapterInventory *inv,
                                     const GUID *target_guid);

/* One enumerated network's scan record (deduplicated by ID). */
typedef enum {
    HCN_REC_COMPLETE,   /* opened, queried, parsed; ID matches; complete Type */
    HCN_REC_UNPROVEN,   /* open/query/parse/ID/Type failure (not exact not-found): possible occupant */
    HCN_REC_VANISHED    /* exact not-found on open: not an occupant; the list is untrustworthy */
} HcnRecordState;

typedef struct {
    GUID enumerated_id;
    HcnRecordState state;
    HcnNetworkProps props;      /* valid fields only when state == HCN_REC_COMPLETE */
    BOOL transparent;           /* complete Type == Transparent */
    BOOL has_derived_identity;  /* enumerated ID equals owned_id(G) (caller set), or owned-Name+actual-ID equality */
    GUID derived_nic_guid;      /* the G of the derived identity */
    wchar_t degrade_reason[80]; /* why an UNPROVEN record could not be located ("" when COMPLETE) */
} HcnScanRecord;

/* Caller-precomputed owned-ID set entry: the scan consumes
   this set, it never computes it. */
typedef struct {
    GUID nic_guid;      /* current non-null L0 hardware InterfaceGuid G */
    GUID owned_id;      /* owned_id(G) */
} HcnOwnedIdEntry;

typedef enum {
    HCN_SCAN_NOT_AVAILABLE,  /* enumerate export unresolved (decided once, before the walk) */
    HCN_SCAN_FAILED,         /* enumerate call/document failure: read-layer failure */
    HCN_SCAN_COMPLETE        /* call+document OK; per-object degradation via records */
} HcnScanState;

typedef struct {
    HcnScanRecord *records;      /* every deduplicated enumerated ID */
    size_t count;
    size_t total_enumerated;     /* pre-dedup count (diagnostics) */
    size_t vanished_count;
    size_t unproven_count;
    size_t transparent_count;
    BOOL enumeration_complete;   /* call OK + document complete + nothing vanished */
    HcnScanState state;
    HRESULT failure_hr;          /* originating error when state == HCN_SCAN_FAILED */
} HcnCandidateScan;

/* One read-only enumeration pass (the four-state model with per-object
   degradation). Requires hcn_init(). The scan is per-acquire state; no
   process-level caching. */
HRESULT hcn_scan_network_candidates(const HcnOwnedIdEntry *owned,
                                    size_t owned_count,
                                    HcnCandidateScan *scan);
void hcn_candidate_scan_free(HcnCandidateScan *scan);

/* ---- External network acquisition ---- */

/* Result of acquiring an External network for a VM. All three
   fields are set together on success and only on success; it is process
   memory, not configuration. delete_network_on_last_release is TRUE only
   for the owned network created or validated by this call; it is FALSE
   for every borrowed (user/third-party) external switch. Callers must
   never pass a FALSE-flag network_id to HcnDeleteNetwork. */
typedef struct {
    GUID network_id;                     /* actual HCN network ID */
    GUID adapter_interface_guid;         /* the selected physical NIC T */
    BOOL delete_network_on_last_release; /* TRUE only for owned */
} HcnExternalNetworkRef;

/* Acquire an External network.
   configured_adapter: FriendlyName from config (NULL/empty = Auto).
   out: validated and zeroed on entry; on any failure it stays zero (the
   caller's result is only published on success).
   reason/reason_cap: optional one-line diagnostic for the failing
   caller (truncated to fit; untouched on success and when not
   provided). It is display text, not configuration.
   Returns S_OK on success; otherwise the originating HRESULT with a
   distinct reason label. External acquisition failures must not be
   downgraded to NET_NONE by callers. */
HRESULT hcn_acquire_external_network(const wchar_t *configured_adapter,
                                     HcnExternalNetworkRef *out,
                                     wchar_t *reason, size_t reason_cap);

/* Owned-delete shield: pure binary ID comparison, no Name
   parsing, no HCN topology query. Refuses (S_FALSE, no HcnDeleteNetwork
   call) a NULL/zero network_id or adapter GUID, and any network_id that
   is not owned_id(adapter_interface_guid); otherwise deletes and returns
   the HcnDeleteNetwork HRESULT (exact delete-path not-found 0x80070490
   means already gone). The UUID helper stays single-sourced here -
   asb_core must not duplicate the formula. */
HRESULT hcn_delete_owned_external_network(const GUID *network_id,
                                          const GUID *adapter_interface_guid);

/* Create an endpoint on a network.
   network_id: the network to attach to (the actual acquisition network ID).
   endpoint_id: output GUID for the created endpoint (randomly generated).
   endpoint_guid_str: output string representation of endpoint GUID (for HCS JSON).
   nat_ip: for NAT networks, the static IP to assign (e.g. "172.20.0.2"); if
           NULL/empty, NAT endpoints default to "172.20.0.2". Ignored for
           Internal and External networks (which always use DHCP).
   mac_address: the VM's MAC address (17 chars, '-' or ':' separators) to
           preserve across restarts. Required: a NULL/invalid address is
           E_INVALIDARG.
   mark_borrowed: TRUE only for a borrowed External network
           (acquisition flag == FALSE). Adds the human-identifiable
           endpoint Name "AppSandboxBorrowedEndpoint-<same endpoint GUID>".
           The Name is a marker for logs/humans only - never an ownership
           or cleanup criterion. NAT/Internal/owned-External pass FALSE. */
HRESULT hcn_create_endpoint(const GUID *network_id, GUID *endpoint_id,
                            wchar_t *endpoint_guid_str, size_t str_len,
                            const char *nat_ip, const wchar_t *mac_address,
                            BOOL mark_borrowed);

/* Delete a network by GUID. */
ASB_API HRESULT hcn_delete_network(const GUID *network_id);

/* Delete an endpoint by GUID. */
ASB_API HRESULT hcn_delete_endpoint(const GUID *endpoint_id);

/* Enumerate network adapters suitable for External networking.
   Calls the callback for each adapter found. Returns count. */
typedef void (*HcnAdapterCallback)(const wchar_t *friendly_name, int if_type, void *ctx);
ASB_API int hcn_enum_adapters(HcnAdapterCallback cb, void *ctx);

#endif /* HCN_NETWORK_H */
