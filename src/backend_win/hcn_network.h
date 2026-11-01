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
#include "asb_net_limits.h"

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

/* ---- Internal vSwitch census (the C shape) ----
   Internal contracts (no ASB_API): the classify TU and the acquire own
   the semantics; the private prototypes live in hcn_private.h. The
   member is sw_class (not "class" - a C++ keyword); the walk result
   enum and the RAW classifier inputs live in hcn_private.h below this
   header's include (no include cycle). */

typedef enum { HCN_CENSUS_OK, HCN_CENSUS_EMPTY,
               HCN_CENSUS_UNAVAILABLE } HcnCensusState;

typedef enum { HCN_CAND_OWNED, HCN_CAND_BORROWED,
               HCN_CAND_REJECTED, HCN_CAND_UNKNOWN } HcnCandidateVerdict;

typedef enum { HCN_SW_INTERNAL, HCN_SW_EXTERNAL,
               HCN_SW_PRIVATE,  HCN_SW_UNCLASSIFIED } HcnSwitchClass;

/* One value per catalog line; never reordered (the web side compares
   the payload's integer enums as ABI constants). */
typedef enum {
    HCN_IR_NOT_FOUND = 0,        /* no internal switch named "X" (N internal switches) */
    HCN_IR_DUPLICATE,            /* the switch name "X" is duplicated */
    HCN_IR_EXTERNAL_SWITCH,      /* "X" is an external switch */
    HCN_IR_NOT_INTERNAL,         /* "X" is not an internal switch (no host adapter) */
    HCN_IR_NO_HOST_NETWORK,      /* switch "X" has no host network object to attach to */
    HCN_IR_INVENTORY_UNAVAILABLE,/* host inventory unavailable */
    HCN_IR_CORRELATION_UNAVAILABLE, /* HCN correlation unavailable */
    HCN_IR_UNSUPPORTED_TYPE,     /* "X"'s network type is not Internal/ICS */
    HCN_IR_NAT,                  /* "X"'s network is the AppSandbox NAT network */
    HCN_IR_OWNED_EXTERNAL,       /* "X"'s network is an AppSandbox-owned external network */
    HCN_IR_FIXED_IDENTITY_CONFLICT,  /* the fixed internal GUID holds a foreign identity */
    HCN_IR_UNREADABLE_IDENTITY,  /* the fixed internal GUID holds an unreadable identity */
    HCN_IR_ENDPOINT_FAILED,      /* endpoint creation on switch "X" failed */
    HCN_IR_INVALID_STORED_NAME,  /* the stored switch name in vms.cfg is invalid */
    HCN_IR_INVALID_NAME,         /* interactive invalid-name (too long, CR/LF/NUL, unpaired surrogate, U+FFFF) */
    HCN_IR_AUTO_COLLISION,       /* Auto: the fixed internal GUID is occupied by a foreign network */
    HCN_IR_AUTO_NAME_SQUAT       /* Auto: a Hyper-V switch named AppSandboxInternal exists */
} HcnInternalReasonCode;

#define HCN_REASON_TEXT_CAP 1024   /* the FULL/LIST lines (aligned with the
                                      async channel; 256 could not hold the
                                      explanation + a name) */
#define HCN_CONCISE_REASON_CAP 64  /* the per-entry CONCISE clause - its own
                                      small cap so the entry budget is auditable */
#define HCN_TYPE_DISPLAY_MAX  (HCN_MAX_TYPE_CHARS + 16)

/* One classified switch. name_unusable is build-time (over-cap, a
   zero-length name, or the shared character class failing) and is the
   ONLY thing E2 excludes on; name_unreadable means the ElementName was
   missing - E0's fatal (b) reads it, and it is a different predicate.
   selectable/defer_eligible/in_duplicate_name_group/reason_* are
   projection outputs (project_for_ui). */
typedef struct {
    wchar_t name[INTERNAL_SWITCH_CAP];
    BOOL    name_truncated;
    GUID    switch_id;
    HcnSwitchClass sw_class;
    wchar_t type[HCN_MAX_TYPE_CHARS];   /* display-only (the ICS note) */
    BOOL    type_truncated;
    HcnCandidateVerdict verdict;   /* classify-time (probed entries) */
    BOOL    name_unusable;
    BOOL    name_unreadable;
    BOOL    in_duplicate_name_group;
    BOOL    defer_eligible;   /* class-level DEFER precondition; the OFFER
                                 additionally requires the entry to be THIS
                                 VM's own stored selector (a per-VM decision
                                 the shared census cannot carry) */
    HcnInternalReasonCode reason_code;  /* tests/debugging */
    wchar_t reason_text[HCN_CONCISE_REASON_CAP]; /* CONCISE clause, projection */
    BOOL    selectable;    /* DERIVED (census-side, VM-independent):
                              ((sw_class == INTERNAL) ||
                               (sw_class == PRIVATE && defer_eligible)) &&
                              !name_unusable && verdict != REJECTED &&
                              !auto_sentinel && !duplicate_group - the UI adds
                              the per-VM test for the PRIVATE arm */
} HcnInternalSwitchEntry;

/* The full classified switch set plus the resolution-time evidence
   flags. EMPTY is a COUNT summary of a complete enumeration (zero
   INTERNAL-class switches), never a decision input: the resolver runs
   its name layers for OK and EMPTY alike. unattributed_host_port is
   the AGGREGATE diagnostic (set by attributed failures too, never the
   gate's input); has_unknown_owner_failure is E0's (c) input. */
typedef struct {
    HcnCensusState state;
    HcnInternalReasonCode reason_code;
    wchar_t reason_text[HCN_REASON_TEXT_CAP];  /* the full/list-level line */
    BOOL    unattributed_host_port;
    BOOL    has_unknown_owner_failure;
    BOOL    capped;
    HcnInternalSwitchEntry *entries;   /* the FULL classified set */
    size_t  count;
} HcnInternalSwitchCensus;

/* Caller-owned snapshot: distinct stored selectors of Internal-mode VMs
   in VM-list order, capped at probe_budget. The strings stay alive for
   the duration of the call (ui.c deep-copies them into worker-owned
   storage). cancel_event is a caller-owned manual-reset EVENT (or an
   equivalent observable), NULL when nobody cancels; it rides the WMI
   session and is checked at the two query points and in the HCN probe
   loop. May be observed from a worker thread. */
typedef struct {
    const wchar_t *const *priority_names;
    size_t priority_count;
    size_t probe_budget;   /* total HCN probes per build */
    BOOL   project_for_ui; /* apply display policy + render reason text in-core */
    void  *cancel_event;
} HcnInternalCensusRequest;

/* ---- Internal vSwitch acquisition and enumeration ----
   Internal contracts + the DLL's Internal surface: the acquire, the
   census enumeration, the delete shield, and the HCN-layer stale-ping
   notifier pair (non-ASB_API by this header's charter; the public
   asb_set_census_stale_callback in asb_core.h delegates). */

/* The acquire's result ref: process memory, not configuration.
   delete_network_on_last_release is TRUE only for the owned fixed
   AppSandboxInternal network; every borrowed switch is FALSE, and the
   invariant "never pass a FALSE-flag network_id to HcnDeleteNetwork"
   is scoped to this ref. switch_id is diagnostics and log identity
   only - NEVER an input to cleanup or shared-use comparison. */
typedef struct {
    GUID network_id;                     /* actual HCN network ID */
    GUID switch_id;                      /* diagnostics/log only */
    BOOL delete_network_on_last_release; /* TRUE only for owned */
} HcnInternalNetworkRef;

/* Acquire an Internal network. selector: the vSwitch ElementName
   (NULL/empty = Auto, the fixed AppSandboxInternal ICS network).
   out is zeroed on entry and stays zero on any failure (the caller
   publishes only on success); a non-empty selector's failure is
   fail-closed and never downgraded by the caller. reason/reason_cap:
   optional one-line diagnostic for the failing caller (display text,
   not configuration; untouched on success). The stale-ping notifier
   fires from inside the failure exit whenever the selector is
   non-empty - the single emission site. */
HRESULT hcn_acquire_internal_network(const wchar_t *selector,
                                     HcnInternalNetworkRef *out,
                                     wchar_t *reason, size_t reason_cap);

/* The request-driven census enumeration. Runs WMI/HCN I/O: call it on
   a worker (never the UI STA). On ANY return, *out is a valid,
   free-able census - a cancelled build returns
   HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED) with the UNAVAILABLE
   shape, never S_OK, and never OK with a partial entry list. ASB_API:
   the UI layer's census worker (in the exe) calls it. */
ASB_API HRESULT hcn_enum_internal_switches(const HcnInternalCensusRequest *req,
                                           HcnInternalSwitchCensus *out);
ASB_API void hcn_internal_switch_census_free(HcnInternalSwitchCensus *c);

/* Owned-delete shield: refuses (S_FALSE, no delete call) any
   network_id that is not the fixed APPSANDBOX_INTERNAL_GUID; otherwise
   deletes through the shared hcn_network.c binary-ID core (which holds
   the network-create lock across the comparison and HcnDeleteNetwork). */
HRESULT hcn_delete_owned_internal_network(const GUID *network_id);

/* The stale-ping notifier: the acquire's failure exit fires the
   registered callback (may be invoked from a worker thread - the
   consumer must marshal; never webview2_post from the callback).
   Registration is once, before any worker thread exists (the
   thread-creation barrier is the publication fence for the pair). */
void hcn_set_census_stale_callback(void (*cb)(void *user_data),
                                   void *user_data);
void hcn_notify_census_stale(void);

#endif /* HCN_NETWORK_H */
