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

/* Single-source initializers for the two IDs the pure classify TU
   compares against: hcn_network.c's definitions AND the classify TU's
   private comparison copies (it links into the offline test binary,
   which does not link hcn_network.c) both compile from these, so the
   values cannot drift apart. */
#define APPSANDBOX_NAT_GUID_INIT \
    { 0xA5B01234, 0x5678, 0x9ABC, \
      { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 } }
#define APPSANDBOX_INTERNAL_GUID_INIT \
    { 0xA5B01234, 0x5678, 0x9ABC, \
      { 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x77 } }

#ifndef HCN_DELETE_NOT_FOUND
#define HCN_DELETE_NOT_FOUND ((HRESULT)0x80070490L)
#endif

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

/* ---- Owned-network ID comparison and delete (hcn_network.c) ---- */

/* Delete a network only when its actual ID equals the caller's expected
   owned ID (pure binary identity; no Name parsing or HCN topology query).
   External derives expected_owned_id from the adapter InterfaceGuid;
   Internal supplies its fixed AppSandboxInternal GUID. The comparison
   and HcnDeleteNetwork call run under the shared network lock so a
   last-user delete cannot interleave with a concurrent acquire's
   re-check-then-create. S_FALSE means refused (null/zero ID or mismatch);
   successful HCN statuses normalize to S_OK, while failed statuses stay
   raw. out_delete_attempted is TRUE only if the HCN export was called.
   out_error_record receives the HCN-owned error string, when any, and is
   initialized to NULL. */
HRESULT hcn_delete_network_if_owned(const GUID *network_id,
                                    const GUID *expected_owned_id,
                                    BOOL *out_delete_attempted,
                                    PWSTR *out_error_record);

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

/* Display-only census deadline: a VM start is user-initiated and
   retry-costly (it waits), while a dropdown consult must fail fast -
   the tolerances differ, so the constants do. The 5 s consult promise
   is enforced UI-side (the watchdog), not by the worker: ConnectServer
   passes no wait cap and each enumerator Next waits a fixed 5 s. */
#define WMI_CENSUS_DEADLINE_MS 5000

/* WM_DESTROY's single drain budget over every live census worker
   handle (the census deadline plus one second of slack). */
#define CENSUS_DRAIN_WAIT_MS 6000

/* The payload/display placeholder for a name that is unusable or
   unreadable: never matched (the flags gate every comparison), never
   offered, and safe to carry through JSON conversion even when the raw
   text held a lone surrogate or U+FFFF. Shared by the classify TU and
   the WMI builder. */
#define HCN_NAME_PLACEHOLDER L"(name unavailable)"

/* The walk outcome of a port-to-switch chain. Published here (below
   the hcn_network.h include) so the internal classifier's RAW records
   can carry it; the walker functions themselves stay static in
   hcn_wmi.c. Values are fixed: the tri-state-aware attribution helper
   distinguishes NOT_CONNECTED (associations PROVEN absent) from
   WALK_ERROR (could not tell). */
typedef enum {
    WMI_WALK_OK,            /* unique chain from a port to its switch */
    WMI_WALK_NOT_CONNECTED, /* authoritative: port has no active connection */
    WMI_WALK_ERROR          /* ambiguity, malformed topology, or query failure */
} WmiWalkResult;

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
    void *cancel_event;  /* nullable manual-reset event signalled by the
                            owner to stop a build between queries; NULL
                            (the default every session gets at open) skips
                            the checks. Checked at wmi_run_query's top, at
                            the host-visibility probe's top, and in the
                            census's HCN probe loop - nothing else reads it. */
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

/* ---- Internal vSwitch policy (hcn_internal_policy.c) ----
 *
 * The pure TU (the parse-TU precedent): no HCN/WMI I/O (the OLE string
 * allocator behind SysStringLen, linked via OleAut32.lib, is not I/O
 * and is the conversion's one system dependency). Declared here, not
 * in hcn_network.h: they are neither ASB_API nor public surface, and
 * hcn_internal.c plus the offline tests both call them.
 */

/* The shared tri-state for the fixed AppSandboxInternal identity:
   MATCH requires a readable Name+Type that says exactly
   AppSandboxInternal + ICS; MISMATCH requires readable Name+Type that
   provably says otherwise; INDETERMINATE is everything weaker (the
   Auto acquire treats it as owned - existence-only semantics - while
   the Explicit probe's E4 step treats it as the unreadable-identity
   refusal: the Explicit path's whole question is the identity). */
typedef enum {
    HCN_ID_MATCH,
    HCN_ID_MISMATCH,
    HCN_ID_INDETERMINATE
} HcnFixedIdentity;

HcnFixedIdentity internal_fixed_identity(const HcnNetworkProps *props);

/* The E3/E4 probe evidence: the open outcome plus, when the document
   parsed, its properties. hr carries the original failure HRESULT
   (0 when the refusal carries none). */
typedef enum {
    HCN_PROBE_OPEN_NOT_FOUND,  /* HcnOpenNetwork exact not-found */
    HCN_PROBE_QUERY_FAILED,    /* opened; the Query export missing or the query call failed */
    HCN_PROBE_OPEN_ERROR,      /* any other open failure (hr carries the original) */
    HCN_PROBE_PARSED           /* opened + queried + the document parsed */
} HcnProbeKind;

typedef struct {
    HcnProbeKind kind;
    HRESULT hr;
    HcnNetworkProps props;     /* valid when kind == HCN_PROBE_PARSED */
} HcnProbeEvidence;

/* classify_props' typed verdict: E3's correlation checks then E4's
   ordered sequence. type_display is E4 step 4's <T> for the
   type-specific line (the raw Type, "(missing)" or "(truncated)");
   empty otherwise. defer_adjudication selects the Private-type arm's
   line (the DEFER context's Type == Private answers with the
   no-host-adapter line, not the type-specific one). */
typedef struct {
    HcnCandidateVerdict verdict;          /* OWNED / BORROWED / REJECTED */
    GUID    network_id;                   /* the actual HCN network ID (OWNED/BORROWED) */
    HcnInternalReasonCode reason_code;    /* REJECTED only */
    HRESULT hr;                           /* REJECTED only; 0 when none */
    wchar_t type_display[HCN_TYPE_DISPLAY_MAX];
} HcnInternalClassifyVerdict;

void hcn_internal_classify_props(const GUID *wmi_switch_guid,
                                 const HcnProbeEvidence *evidence,
                                 BOOL defer_adjudication,
                                 HcnInternalClassifyVerdict *out);

/* The RAW classifier inputs (builder-filled, hcn_wmi.c). */
typedef struct {
    GUID    id;                         /* the switch's WMI Name GUID */
    BOOL    has_id;
    wchar_t name[INTERNAL_SWITCH_CAP];  /* ElementName (the placeholder when
                                           unusable - never a truncated prefix) */
    BOOL    name_unusable;              /* over-cap / zero-length / CR / LF /
                                           NUL / unpaired surrogate / U+FFFF
                                           (the shared helper) */
    BOOL    name_unreadable;            /* missing ElementName */
    int     bound_external_ports;       /* E1 ladder step 1's input - a REAL
                                           switch record (the External override) */
} HcnRawSwitchResult;

typedef struct {
    wchar_t name[INTERNAL_SWITCH_CAP];  /* the iport's ElementName (placeholder
                                           when unusable-for-attribution) */
    BOOL    name_unreadable;
    BOOL    name_unusable_for_attribution; /* the source was READABLE but is not
                                           attribution-safe: over-cap OR the
                                           character class failing over the RAW
                                           SysStringLen length (an embedded NUL
                                           at minimum). Readable NO-match, never
                                           name_unreadable. */
    BOOL    has_name_id;                /* the WMI Name parses as a GUID */
    GUID    iport_name_id;              /* HNS network ports: the network ID;
                                           real switch iports: their own port
                                           GUID - the product-port skip's input */
    WmiWalkResult walk_result;          /* the enum, not an int */
    GUID    reached_switch_id;          /* valid only on WALK_OK */
} HcnRawIportResult;

/* E1's precedence ladder over the RAW records (globally enumerated
   flat arrays - the classifier associates iports to switches):
   product-port skip FIRST (every iport, WALK_OK included, port-level
   exemption only), the tri-state-aware attribution helper, fusion,
   the absent-GUID walk-level rule, the aggregate flag and
   has_unknown_owner_failure. out is zeroed and fully owned by the
   caller (entries are HeapAlloc'd; free with
   hcn_internal_switch_census_free). */
void hcn_internal_classify_switches(const HcnRawSwitchResult *switches,
                                    size_t switch_count,
                                    const HcnRawIportResult *iports,
                                    size_t iport_count,
                                    HcnInternalSwitchCensus *out);

/* The pure E0/E2 resolver: layer 1 is the census state (only OK and
   EMPTY continue to the name layers), layer 2 is E0's name-relative
   gate (the premise + (a)/(b)/(c) + the DEFER arm), layer 3 is E2's
   ordered match/class selection. The selector MUST be non-empty (an
   empty selector is Auto, which never resolves); *out is zeroed at
   entry and only the live arm is filled. A REJECT carries no probe
   target (probe_guid == GUID_NULL). */
typedef enum { HCN_IRES_REJECT, HCN_IRES_PROCEED } HcnResolveOutcome;

typedef struct {
    HcnResolveOutcome outcome;
    /* PROCEED: */
    GUID    probe_guid;             /* the unique E2-matched switch */
    BOOL    defer_adjudication;     /* TRUE iff the match is a PRIVATE entry
                                       under the flag and NOT blocked by (b) */
    /* REJECT: */
    HcnInternalReasonCode reason_code;  /* the census's OWN code for a state
                                           refusal, E0's, or E2's */
    HRESULT hr;                     /* the failing HRESULT; 0 when the
                                       refusal carries none */
    size_t  n;                      /* the INTERNAL-class count after E1 */
} HcnInternalResolution;

void hcn_internal_resolve_selector(const HcnInternalSwitchCensus *census,
                                   const wchar_t *selector,
                                   HcnInternalResolution *out);

/* The reason catalog's formatter. FULL (alerts/logs), LIST (the
   census-level line - omits the "; cannot resolve" clause), CONCISE
   (the per-entry clause). n feeds the not-found line only; type_str
   is E4's <T> (NULL otherwise). The rendered name is truncated to fit
   out_cap. */
typedef enum { HCN_RS_FULL, HCN_RS_LIST, HCN_RS_CONCISE } HcnReasonStyle;

void format_reason(HcnInternalReasonCode code, HcnReasonStyle style,
                   const wchar_t *selector, HRESULT hr, size_t n,
                   const wchar_t *type_str, wchar_t *out, size_t out_cap);

/* The shared BSTR-to-RAW ElementName conversion for switch and iport
   records. Length and character checks use SysStringLen over the source
   (an embedded NUL hides from wcslen); an unusable value writes the
   placeholder before any prefix can be copied. NULL is unreadable, while
   an empty or invalid readable string is unusable. Switch records retain
   an empty name for a missing property; iport records use the placeholder. */
void internal_switch_name_to_raw(BSTR source, HcnRawSwitchResult *out);
void internal_iport_name_to_raw(BSTR source, HcnRawIportResult *out);

/* The pure projection (the enum wrapper delegates to it behind
   project_for_ui): duplicate groups over {INTERNAL ∪ UNCLASSIFIED}
   excluding name_unusable, defer_eligible and every readable PRIVATE
   entry's CONCISE reason derived from per-name
   hcn_internal_resolve_selector calls (no second copy of the
   flag/duplicate/(b) precedence rules), selectable per the derived
   rule, product fixed-GUID entries marked OWNED (de-offered), the
   sentinel-named entry de-offered, and unusable/unreadable names
   replaced by the fixed placeholder for the payload. Mutates the
   census's entries in place. */
void internal_project_census(HcnInternalSwitchCensus *census);

/* The census sibling builder (hcn_wmi.c): WMI-only, re-enumerates
   Msvm_VirtualEthernetSwitch for Name GUID + ElementName (joined to
   the topology by GUID) and Msvm_InternalEthernetPort for the iport
   census, walking each iport to its switch; never writes out_kind,
   never frees the topology, no HCN I/O, no request struct. deadline
   is CALLER-SUPPLIED: the acquire passes WMI_DEFAULT_DEADLINE_MS, the
   ui.c worker WMI_CENSUS_DEADLINE_MS. */
HRESULT wmi_build_internal_census(WmiSession *s, const WmiTopology *topology,
                                  ULONGLONG deadline,
                                  HcnInternalSwitchCensus *out);

/* The production census authority gate shared by the UI census and the
   fresh Explicit acquire. A filtered token or an invisible host-system
   class cannot authorize an empty switch list as complete; on refusal,
   out is reset to the free-able UNAVAILABLE shape. */
BOOL hcn_internal_census_authority_gate(const WmiTopology *topology,
                                        BOOL token_unfiltered,
                                        HcnInternalSwitchCensus *out);

#endif /* HCN_PRIVATE_H */
