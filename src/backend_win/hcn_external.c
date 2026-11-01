/* hcn_external.c - External-mode network acquisition and lifecycle.
 *
 * The complete External-mode decision layer: the acquire ladder
 * (owned reuse, D0-D3 discovery with WMI correlation, borrow or
 * proven-free create with create-exit verification), the Auto and
 * Explicit walks with end-of-walk arbitration, the reason lines, the
 * owned-delete shield (binary-ID comparison only - a borrowed switch
 * is never deleted), and the startup sweep for stale owned networks. */

#include "hcn_network.h"
#include "hcn_private.h"
#include "ui.h"
#include <objbase.h>
#include <iphlpapi.h>
#include <string.h>

#pragma comment(lib, "iphlpapi.lib")   /* lookup_adapter_description */

/* ---- External network acquisition core ---- */

/* Exact delete-path not-found (measured): the delete and
   open paths return DIFFERENT not-found codes; never substitute one for
   the other. */
#ifndef HCN_DELETE_NOT_FOUND
#define HCN_DELETE_NOT_FOUND ((HRESULT)0x80070490L)
#endif

/* Official HCN error-family code: "An adapter was not found"
   (learn.microsoft.com, HCN HRESULT reference). Measured on the create
   path when HNS cannot resolve the InterfaceConstraint adapter: a
   media-disconnected NIC (its host network stack is not active), or a
   NIC already consumed by a foreign vSwitch (no TCPIP binding left).
   Adapter resolution precedes the 802.11 bridging check, so these
   shapes never reach the E_INVALIDARG rejection. */
#ifndef HCN_E_ADAPTER_NOT_FOUND
#define HCN_E_ADAPTER_NOT_FOUND ((HRESULT)0x803B0006L)
#endif

/* The liveness predicate form. Default is the two-condition
   form (OperStatus == Up AND NotMediaConnected == false). The contingency
   variant (OperStatus only) may be selected ONLY with recorded on-host
   evidence that the media flag lies on a connected bound NIC; build 26200
   did not trigger the contingency. */
#define EXTERNAL_LIVENESS_NO_MEDIA_CONTINGENCY 0

/* Wi-Fi auto-creation capability bit, applied at the
   FREE-selection point only. HNS rejects creating a native Transparent
   network on an 802.11 adapter with E_INVALIDARG - infrastructure Wi-Fi
   allows one MAC address per station, so L2 bridging is impossible and
   only a Hyper-V external switch (which works by MAC rewriting) can
   bridge the adapter. Auto skips the doomed create attempt; borrow and
   Explicit are unaffected (an Explicit create failure on a wireless
   target reports the Wi-Fi-specific reason line). */
#define EXTERNAL_WIFI_AUTO_CREATABLE 0

/* Reason labels: log/error-text categories, not public ABI. */
typedef enum {
    EXT_REASON_NONE = 0,
    EXT_REASON_EXPLICIT_MISSING,
    EXT_REASON_EXPLICIT_AMBIGUOUS,
    EXT_REASON_AUTO_UNRESOLVED,
    EXT_REASON_API_UNAVAILABLE,
    EXT_REASON_INVENTORY_FAILED,
    EXT_REASON_HCN_SCAN_FAILED,
    EXT_REASON_TARGET_UNKNOWN,
    EXT_REASON_TARGET_OCCUPIED,
    EXT_REASON_TOPOLOGY_CONFLICT,
    EXT_REASON_SET_UNSUPPORTED,
    EXT_REASON_WMI_FAILED,
    EXT_REASON_HCN_TARGET_UNAVAILABLE,
    EXT_REASON_CREATE_FAILED,
    EXT_REASON_CREATE_UNBOUND
} ExternalReasonKind;

/* WMI failure causes select distinct diagnostic lines. */
typedef enum {
    WMI_FAIL_INFRA,      /* server/RPC/service state: vmms hint */
    WMI_FAIL_DATA,       /* malformed topology data/chain: no service hint */
    WMI_FAIL_FILTERED,   /* filtered view or coverage disqualified: elevation required */
    WMI_FAIL_CAP         /* query exceeded the object cap */
} WmiFailCause;

/* End-of-walk reason precedence: WMI_FAILED > HCN_SCAN_FAILED > API_UNAVAILABLE >
   TARGET_UNKNOWN > OCCUPIED-class reasons. */
static int reason_precedence(ExternalReasonKind k)
{
    switch (k) {
    case EXT_REASON_WMI_FAILED:        return 5;
    case EXT_REASON_HCN_SCAN_FAILED:   return 4;
    case EXT_REASON_API_UNAVAILABLE:   return 3;
    case EXT_REASON_TARGET_UNKNOWN:    return 2;
    case EXT_REASON_TARGET_OCCUPIED:
    case EXT_REASON_TOPOLOGY_CONFLICT:
    case EXT_REASON_HCN_TARGET_UNAVAILABLE:
    case EXT_REASON_SET_UNSUPPORTED:   return 1;
    default:                           return 0;
    }
}

/* Per-target discovery outcomes (discovery never returns BORROWED or FREE;
   the terminal results before the gate are OWNED, OCCUPIED, WMI_ERROR,
   and failure). */
typedef enum {
    ACQ_FAILURE = 0,       /* hard: stop the walk at this NIC (fail_reason set) */
    ACQ_OWNED,             /* terminal: owned reuse, result filled */
    ACQ_BORROWED,          /* terminal: foreign borrow, result filled */
    ACQ_OCCUPIED,          /* per-T: skip T, continue the walk */
    ACQ_WMI_ERROR,         /* host-wide: downgrade (no borrow, no create, owned probe only) */
    ACQ_NO_POSITIVE,       /* no existing network on T (gate input) */
    ACQ_CREATE_BLOCKED,    /* gate: creation unavailable for the rest of the walk */
    ACQ_FREE               /* gate: T is proven free; create owned_id(T) */
} AcqOutcome;

/* The per-target view of the shared scan. */
typedef struct {
    int on_t_count;          /* definite on-T candidates (COMPLETE+Transparent) */
    GUID on_t_id;            /* valid when on_t_count >= 1 */
    int on_t_index;          /* scan record index, -1 when none */
    BOOL unproven_occupant;  /* a possible occupant of T remains */
    GUID unproven_first;     /* first blocking object */
    BOOL unproven_first_valid;
    int unproven_index;      /* first blocking object's scan record index, -1 when none */
} ScanView;

typedef struct {
    AcqOutcome outcome;
    ExternalReasonKind reason;     /* OCCUPIED shape's label / failure label */
    HRESULT hr;                    /* originating error for FAILURE / WMI_ERROR */
    GUID object_id;                /* named blocking object */
    BOOL object_valid;
    BOOL occupied_shape_c;         /* (c): the list is proven untrustworthy */
    BOOL authoritative_not_found;  /* authoritative NOT_FOUND for T (create-gate input) */
    ScanView view;                 /* per-target scan view (create-gate input) */
} TargetResult;

/* One acquisition context: every snapshot builds at most once
   per acquire; enumeration is lazy until a live NIC misses owned ID; the
   WMI topology is lazy, built once on first correlation need when the provider is
   installed. The recorded host-wide block is a single monotonic reason
   field with the first blocking object, WMI HRESULT, and scan state. */
typedef struct {
    /* L0/GAA inventory (required) */
    HcnAdapterInventory inv;
    HRESULT inv_hr;

    /* precomputed owned_id(G) set for current hardware InterfaceGuids */
    HcnOwnedIdEntry *owned_set;
    size_t owned_count;
    HRESULT owned_hr;

    /* shared HCN scan (lazy) */
    HcnCandidateScan scan;
    BOOL scan_decided;             /* scan built or NOT_AVAILABLE decided */
    BOOL enumerate_unavailable;    /* the enumerate export is unresolved */

    /* provider-existence snapshot (one cheap probe, at most once) */
    BOOL provider_probed;
    WmiProviderStatus provider_status;

    /* WMI (lazy) */
    WmiTopology topo;
    BOOL topo_built;               /* one build per acquire, even on failure */
    BOOL topo_usable;              /* the build SUCCEEDED: later correlations are lookups into it */
    BOOL provider_absent;          /* probe or topology build says absent */
    BOOL token_checked;
    BOOL token_unfiltered;

    /* host-wide block state (end-of-walk arbitration) */
    BOOL wmi_failed;
    HRESULT wmi_hr;
    WmiFailCause wmi_cause;
    BOOL create_blocked;
    ExternalReasonKind block_reason;
    GUID block_object;
    BOOL block_object_valid;
    BOOL occupied_seen;
    ExternalReasonKind occupied_reason;
    GUID occupied_object;
    BOOL occupied_object_valid;
    BOOL occupied_tail_report;      /* the failure IS the tail's per-T occupied summary (not a walk-stopping conflict) */
    BOOL list_untrustworthy;       /* the correlation ladder proved the list incomplete */

    /* failure output for the entry function */
    ExternalReasonKind fail_reason;
    HRESULT fail_hr;
    GUID fail_object;
    BOOL fail_object_valid;
    BOOL wifi_create_rejected;   /* the create hit a wireless platform
                                    failure (see build_reason_line) */
    BOOL target_not_connected;   /* the create target's media was down:
                                    adapter-resolution evidence for the
                                    wireless reason line */

    /* inputs */
    const wchar_t *configured_adapter;

    /* local result: published to the caller only on success */
    HcnExternalNetworkRef result;
} ExternalAcquireContext;

/* ---- Context lifecycle ---- */

static HRESULT ctx_build(ExternalAcquireContext *ctx)
{
    size_t i;

    ZeroMemory(ctx, sizeof(*ctx));

    ctx->inv_hr = build_adapter_inventory(&ctx->inv);
    if (FAILED(ctx->inv_hr))
        return ctx->inv_hr;

    /* The GAA overlay is required acquire input: its failure is an
       acquire error, never a successful empty inventory. */
    ctx->inv_hr = inventory_overlay_ipv4_gateways(ctx->inv.entries, ctx->inv.count);
    if (FAILED(ctx->inv_hr))
        return ctx->inv_hr;

    /* Precompute owned_id(G) for every current non-null hardware
       InterfaceGuid. owned networks are only ever created on
       eligible hardware NICs, so this narrowing loses no owned object. */
    ctx->owned_set = (HcnOwnedIdEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                  ctx->inv.count * sizeof(HcnOwnedIdEntry));
    if (!ctx->owned_set)
        return E_OUTOFMEMORY;
    ctx->owned_hr = S_OK;
    for (i = 0; i < ctx->inv.count; i++) {
        const HcnAdapterEntry *e = &ctx->inv.entries[i];
        if (!e->is_hardware_eligible)
            continue;
        if (IsEqualGUID(&e->interface_guid, &GUID_NULL))
            continue;
        ctx->owned_hr = hcn_external_owned_id(&e->interface_guid,
                                              &ctx->owned_set[ctx->owned_count].owned_id);
        if (FAILED(ctx->owned_hr)) {
            ui_log(L"External: owned-ID derivation failed (0x%08X).", ctx->owned_hr);
            return ctx->owned_hr;
        }
        ctx->owned_set[ctx->owned_count].nic_guid = e->interface_guid;
        ctx->owned_count++;
    }
    return S_OK;
}

static void ctx_release(ExternalAcquireContext *ctx)
{
    /* The one cleanup exit: closes/frees every context-owned
       resource on all paths. No HCN handles are held in the context -
       probe handles close inside their discovery steps; the WMI session
       closes inside the topology build; COM balances through the paired
       wmi_session_open/wmi_session_close calls. */
    wmi_topology_free(&ctx->topo);
    hcn_candidate_scan_free(&ctx->scan);
    adapter_inventory_free(&ctx->inv);
    if (ctx->owned_set) {
        HeapFree(GetProcessHeap(), 0, ctx->owned_set);
        ctx->owned_set = NULL;
    }
}

/* Look up owned_id(T) in the precomputed set. T comes from the eligible
   inventory, so its entry is always present; NULL return is a defensive
   programming error handled by the caller. */
static const GUID *ctx_owned_id_for(const ExternalAcquireContext *ctx, const GUID *T)
{
    size_t i;
    for (i = 0; i < ctx->owned_count; i++) {
        if (IsEqualGUID(&ctx->owned_set[i].nic_guid, T))
            return &ctx->owned_set[i].owned_id;
    }
    return NULL;
}

/* Record the host-wide WMI failure (top precedence; first hr kept;
   every occurrence logged; create blocked host-wide). */
static void ctx_set_wmi_error(ExternalAcquireContext *ctx, HRESULT hr,
                              WmiFailCause cause)
{
    if (!ctx->wmi_failed) {
        ctx->wmi_failed = TRUE;
        ctx->wmi_hr = hr;
        ctx->wmi_cause = cause;
        ctx->create_blocked = TRUE;
        ctx->block_reason = EXT_REASON_WMI_FAILED;
    }
    switch (cause) {
    case WMI_FAIL_INFRA:
        ui_log(L"External: installed WMI unavailable (0x%08X); Hyper-V Virtual "
               L"Machine Management service may be stopped; Auto continues for "
               L"owned networks only; no create or borrow this attempt.", hr);
        break;
    case WMI_FAIL_DATA:
        ui_log(L"External: WMI topology data malformed (0x%08X); no service "
               L"action indicated; no create or borrow this attempt.", hr);
        break;
    case WMI_FAIL_FILTERED:
        ui_log(L"External: WMI view filtered or coverage unqualified (elevation "
               L"required); owned reuse unaffected; no create or borrow this "
               L"attempt.");
        break;
    case WMI_FAIL_CAP:
        ui_log(L"External: installed WMI query exceeded the object cap; owned "
               L"reuse unaffected; no create or borrow this attempt.");
        break;
    }
}

/* Record a host-wide create block with monotonic reason
   precedence; the first blocking object is kept with its reason; every
   recorded reason is logged. */
static void ctx_record_block(ExternalAcquireContext *ctx, ExternalReasonKind kind,
                             const GUID *object)
{
    ctx->create_blocked = TRUE;
    if (reason_precedence(kind) > reason_precedence(ctx->block_reason)) {
        ctx->block_reason = kind;
        if (object) {
            ctx->block_object = *object;
            ctx->block_object_valid = TRUE;
        }
    }
    switch (kind) {
    case EXT_REASON_API_UNAVAILABLE:
        ui_log(L"External: HCN enumeration capability unavailable; "
               L"creation blocked host-wide for this attempt.");
        break;
    case EXT_REASON_HCN_SCAN_FAILED:
        if (object) {
            wchar_t q[64];
            guid_to_string(object, q, 64);
            ui_log(L"External: HCN scan incomplete or untrustworthy (object %s); "
                   L"creation blocked host-wide; retry may succeed.", q);
        } else {
            ui_log(L"External: HCN scan incomplete or untrustworthy; "
                   L"creation blocked host-wide; retry may succeed.");
        }
        break;
    case EXT_REASON_TARGET_UNKNOWN:
        /* The rich unresolved-occupant line (name/reason) is logged at
           the recording sites - the create gate and the walk tail - which
           hold the scan record; this switch stays silent for it. */
        break;
    default:
        break;
    }
}

/* Note a per-T OCCUPIED-class outcome (never host-wide by itself). */
static void ctx_note_occupied(ExternalAcquireContext *ctx, ExternalReasonKind kind,
                              const GUID *object)
{
    if (!ctx->occupied_seen) {
        ctx->occupied_seen = TRUE;
        ctx->occupied_reason = kind;
        if (object) {
            ctx->occupied_object = *object;
            ctx->occupied_object_valid = TRUE;
        }
    }
}

/* Mark the attempt failed; a failing HRESULT is forced for semantic
   failures (a distinct reason with no HRESULT of its own). */
static HRESULT ctx_fail(ExternalAcquireContext *ctx, ExternalReasonKind kind,
                        HRESULT hr, const GUID *object)
{
    ctx->fail_reason = kind;
    ctx->fail_hr = hr;
    if (object) {
        ctx->fail_object = *object;
        ctx->fail_object_valid = TRUE;
    }
    return FAILED(hr) ? hr : E_FAIL;
}

/* The unresolved-occupant line: names the blocking object with its Name
   (when the scan record retained one) and the reason it could not be
   located. rec may be NULL (record not tracked); id may be NULL. */
static void log_unresolved_occupant(const HcnScanRecord *rec, const GUID *id)
{
    const wchar_t *name = L"";
    const wchar_t *reason = L"unavailable";

    if (rec) {
        if (rec->state == HCN_REC_COMPLETE &&
            rec->props.has_name && !rec->props.name_truncated)
            name = rec->props.name;
        if (rec->degrade_reason[0])
            reason = rec->degrade_reason;
        else if (rec->state == HCN_REC_COMPLETE)
            reason = (rec->props.binding_count > 0)
                ? L"binding values unresolvable" : L"no binding value";
    }
    if (id) {
        wchar_t q[64];
        guid_to_string(id, q, 64);
        ui_log(L"External: unresolved occupant %s name=\"%s\" reason=%s; "
               L"blocks creation on all targets.", q, name, reason);
    } else {
        ui_log(L"External: unresolved occupant name=\"%s\" reason=%s; "
               L"blocks creation on all targets.", name, reason);
    }
}

/* Liveness (two-condition default; the named contingency constant
   selects the alternate form only with recorded on-host evidence). */
static BOOL adapter_is_live(const HcnAdapterEntry *e)
{
    if (e->oper_status != IfOperStatusUp)
        return FALSE;
#if EXTERNAL_LIVENESS_NO_MEDIA_CONTINGENCY
    return TRUE;
#else
    return !e->not_media_connected;
#endif
}

/* Ordinary Auto uplink: live + physical IPv4 gateway (the GAA
   overlay). Evaluated after read discovery and before the gate. */
static BOOL adapter_ordinary_usable(const HcnAdapterEntry *e)
{
    return adapter_is_live(e) && e->has_ipv4_gateway;
}

/* ---- Shared snapshot builders (each at most once per acquire) ---- */

/* Ensure the shared HCN scan. The NOT_AVAILABLE decision happens here at
   most once (the export unresolved: API_UNAVAILABLE recorded up front,
   create blocked, correlation continues without scan data). A FAILED
   scan is a read-layer failure the caller fails on. */
static HRESULT ctx_ensure_scan(ExternalAcquireContext *ctx)
{
    HRESULT hr;

    if (ctx->scan_decided)
        return S_OK;
    ctx->scan_decided = TRUE;

    if (!pfnEnumNet || !pfnOpenNet || !pfnCloseNet || !pfnQueryNetProps) {
        ctx->enumerate_unavailable = TRUE;
        ctx->scan.state = HCN_SCAN_NOT_AVAILABLE;
        ctx_record_block(ctx, EXT_REASON_API_UNAVAILABLE, NULL);
        return S_OK;
    }

    hr = hcn_scan_network_candidates(ctx->owned_set, ctx->owned_count, &ctx->scan);
    if (FAILED(hr) && ctx->scan.state != HCN_SCAN_NOT_AVAILABLE) {
        /* Read-layer failure: the caller fails this target (and the walk). */
        return hr;
    }
    return S_OK;
}

/* Decide the enumerate export's availability once, before the walk:
   API_UNAVAILABLE recorded, create blocked. Capability check
   only - the scan itself stays lazy until the first live NIC misses
   owned ID (a first-NIC owned hit pays no enumeration, and a
   transient enumeration failure fails the walk at the target that hits it,
   never silently). */
static void ctx_decide_enumerate_availability(ExternalAcquireContext *ctx)
{
    if (ctx->scan_decided)
        return;
    if (pfnEnumNet && pfnOpenNet && pfnCloseNet && pfnQueryNetProps)
        return;
    ctx->scan_decided = TRUE;
    ctx->enumerate_unavailable = TRUE;
    ctx->scan.state = HCN_SCAN_NOT_AVAILABLE;
    ctx_record_block(ctx, EXT_REASON_API_UNAVAILABLE, NULL);
}

/* Ensure the provider-existence snapshot: one cheap probe per
   acquire, at most once, at the first correlation need. */
static void ctx_ensure_provider(ExternalAcquireContext *ctx)
{
    if (ctx->provider_probed)
        return;
    ctx->provider_probed = TRUE;
    ctx->provider_status = wmi_probe_provider_installed();
    if (ctx->provider_status == WMI_PROVIDER_ABSENT)
        ctx->provider_absent = TRUE;
}

/* Ensure the one-shot shared WMI topology, built lazily on first
   correlation need when the provider is installed. A successfully built
   topology stays usable: every later correlation is an in-memory lookup
   into it (one build per acquire, never a second build or a fake
   failure). A PROVIDER_ABSENT build reclassifies this acquire's provider
   state (pure-HCN correlation branch); an ERROR build is the host-wide
   WMI failure, with the cause (cap/data/infra) selecting the distinct
   line. */
typedef struct {
    BOOL built;                    /* topology usable */
    BOOL absent;                   /* provider/namespace/class absent */
    HRESULT hr;                    /* failure HRESULT */
    WmiFailCause cause;            /* failure cause */
} TopologyBuildResult;

static void ctx_ensure_topology(ExternalAcquireContext *ctx, TopologyBuildResult *out)
{
    WmiSession session;
    TopologyResultKind kind;
    HRESULT hr;

    out->built = FALSE;
    out->absent = FALSE;
    out->hr = S_OK;
    out->cause = WMI_FAIL_INFRA;

    if (ctx->topo_usable) {
        /* The one successful build serves every later correlation. */
        out->built = TRUE;
        return;
    }
    if (ctx->topo_built || ctx->provider_absent)
        return;
    if (ctx->wmi_failed) {
        /* The WMI work is unusable for the rest of the walk. */
        out->hr = ctx->wmi_hr;
        out->cause = ctx->wmi_cause;
        return;
    }

    ctx->topo_built = TRUE;   /* one build per acquire, even on failure */

    hr = wmi_session_open(&session);
    if (FAILED(hr)) {
        if (wmi_classify_session_failure(hr) == TOPOLOGY_PROVIDER_ABSENT) {
            ctx->provider_absent = TRUE;
            out->absent = TRUE;
            return;
        }
        out->hr = hr;
        out->cause = WMI_FAIL_INFRA;
        return;
    }

    {
        ULONGLONG deadline = GetTickCount64() + WMI_DEFAULT_DEADLINE_MS;
        hr = wmi_build_topology(&session, deadline, &ctx->topo, &kind);
    }
    wmi_session_close(&session);

    if (SUCCEEDED(hr)) {
        ctx->topo_usable = TRUE;
        out->built = TRUE;
        return;
    }
    if (kind == TOPOLOGY_PROVIDER_ABSENT) {
        ctx->provider_absent = TRUE;
        out->absent = TRUE;
        return;
    }
    out->hr = hr;
    out->cause = (kind == TOPOLOGY_CAP_OVERFLOW)   ? WMI_FAIL_CAP
               : (kind == TOPOLOGY_DATA_MALFORMED) ? WMI_FAIL_DATA
               : WMI_FAIL_INFRA;
}

/* ---- Discovery: owned probe, scan view, correlation ---- */

/* Owned probe: open/query owned_id(T). Valid Transparent -> terminal OWNED;
   exact not-found -> the scan view; anything else fails. The route never
   re-verifies the current uplink and parse results never control flow. */
static HRESULT d0_owned_open(ExternalAcquireContext *ctx, const GUID *T,
                             const GUID *owned, TargetResult *out)
{
    void *network = NULL;
    PWSTR json = NULL;
    HcnNetworkProps props;
    HcnLookupKind k;
    HRESULT hr;

    k = hcn_open_network_exact(owned, &network, &hr);
    if (k == HCN_LOOKUP_ERROR)
        return ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, hr, owned);
    if (k == HCN_LOOKUP_NOT_FOUND)
        return S_FALSE;   /* miss: continue with the scan view */

    hr = hcn_query_network_properties(network, &json);
    if (network && pfnCloseNet) pfnCloseNet(network);
    if (FAILED(hr))
        return ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, hr, owned);

    hr = parse_hcn_network_properties(json, &props);
    hcn_free_string(json);
    if (FAILED(hr))
        return ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, hr, owned);

    if (!props.has_id || !IsEqualGUID(&props.id, owned) ||
        !props.has_type || props.type_truncated ||
        _wcsicmp(props.type, L"Transparent") != 0) {
        ui_log(L"External: owned network probe failed (0x%08X); HCN query "
               L"failure (Type/query mismatch).", E_FAIL);
        return ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, E_FAIL, owned);
    }

    /* Terminal owned reuse: publish network_id, T, flag TRUE. */
    ctx->result.network_id = props.id;
    ctx->result.adapter_interface_guid = *T;
    ctx->result.delete_network_on_last_release = TRUE;
    out->outcome = ACQ_OWNED;

    {
        /* Diagnostics: log "not verifying current uplink" and any
           policy-parseable binding - parse results never enter control
           flow here. */
        wchar_t q[64];
        BindingClass cls = classify_binding_values(&props, &ctx->inv, T);
        const wchar_t *cls_text;
        guid_to_string(owned, q, 64);
        switch (cls) {
        case BINDING_ON_T:     cls_text = L"on-target"; break;
        case BINDING_OFF_T:    cls_text = L"elsewhere"; break;
        case BINDING_CONFLICT: cls_text = L"conflicting"; break;
        default:               cls_text = L"unavailable"; break;
        }
        ui_log(L"External: owned reuse %s; uplink not re-verified; binding=%s.",
               q, cls_text);
    }
    return S_OK;
}

/* Scan view: the per-target view of the shared scan. Reserved-ID
   rules first (terminal OWNED or a hard target failure), then binding
   classification relative to T. Discovery never returns BORROWED or FREE. */
static HRESULT d1_scan_view(ExternalAcquireContext *ctx, const GUID *T,
                            ScanView *view, TargetResult *out)
{
    size_t i;

    view->on_t_count = 0;
    view->on_t_index = -1;
    view->unproven_occupant = FALSE;
    view->unproven_first_valid = FALSE;
    view->unproven_index = -1;

    if (ctx->scan.state == HCN_SCAN_NOT_AVAILABLE)
        return S_OK;   /* correlation continues without scan data */

    for (i = 0; i < ctx->scan.count; i++) {
        HcnScanRecord *rec = &ctx->scan.records[i];

        if (rec->has_derived_identity) {
            if (IsEqualGUID(&rec->derived_nic_guid, T)) {
                /* Reserved for this target: AppSandbox's own network. */
                if (rec->state == HCN_REC_COMPLETE && rec->transparent) {
                    /* Terminal OWNED under the same pure-ID rule as the
                       owned probe; the observed query ID is authoritative. */
                    ctx->result.network_id = rec->props.id;
                    ctx->result.adapter_interface_guid = *T;
                    ctx->result.delete_network_on_last_release = TRUE;
                    out->outcome = ACQ_OWNED;
                    {
                        wchar_t q[64];
                        guid_to_string(&rec->enumerated_id, q, 64);
                        ui_log(L"External: owned reuse %s via scan; no extra "
                               L"open or WMI needed.", q);
                    }
                    return S_OK;
                }
                /* Enumerated owned_id(T) whose query failed, ID disagrees,
                   or Type is invalid: fail for this target. Do not ignore
                   it as an unrelated network or convert it to FREE. */
                {
                    wchar_t q[64];
                    guid_to_string(&rec->enumerated_id, q, 64);
                    ui_log(L"External: reserved owned ID %s is unreadable or "
                           L"wrong-typed; failing this target.", q);
                }
                return ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, E_FAIL,
                                &rec->enumerated_id);
            }
            /* Derived identity for another NIC: off-T with OWNED_IDENTITY
               evidence regardless of query success - not an occupant of
               T and does not clear completeness. */
            continue;
        }

        if (rec->state == HCN_REC_VANISHED)
            continue;   /* not an occupant; the list is untrustworthy */

        if (rec->state == HCN_REC_UNPROVEN) {
            view->unproven_occupant = TRUE;
            if (!view->unproven_first_valid) {
                view->unproven_first = rec->enumerated_id;
                view->unproven_first_valid = TRUE;
                view->unproven_index = (int)i;
            }
            continue;
        }

        /* COMPLETE record. */
        if (!rec->transparent)
            continue;   /* complete non-Transparent: not an occupant of T */

        {
            BindingClass cls = classify_binding_values(&rec->props, &ctx->inv, T);
            if (cls == BINDING_CONFLICT) {
                /* Known contradictory binding fields resolving to T:
                   walk-stopping TOPOLOGY_CONFLICT, never first-wins. */
                wchar_t q[64];
                guid_to_string(&rec->props.id, q, 64);
                ui_log(L"External: topology conflict: %s binding fields "
                       L"disagree on target; no downgrade.", q);
                return ctx_fail(ctx, EXT_REASON_TOPOLOGY_CONFLICT, E_FAIL,
                                &rec->props.id);
            }
            if (cls == BINDING_ON_T) {
                view->on_t_count++;
                view->on_t_id = rec->props.id;
                view->on_t_index = (int)i;
            } else if (cls == BINDING_UNPROVEN) {
                view->unproven_occupant = TRUE;
                if (!view->unproven_first_valid) {
                    view->unproven_first = rec->props.id;
                    view->unproven_first_valid = TRUE;
                    view->unproven_index = (int)i;
                }
            }
            /* BINDING_OFF_T: definitely elsewhere - not an occupant. */
        }
    }

    if (view->on_t_count > 1) {
        /* Multiple definite on-T IDs: conflict, never pick one. */
        return ctx_fail(ctx, EXT_REASON_TOPOLOGY_CONFLICT, E_FAIL, &view->on_t_id);
    }

    return S_OK;
}

/* TRUE when the candidate scan listed this enumerated network ID. */
static BOOL scan_lists_id(const ExternalAcquireContext *ctx, const GUID *id)
{
    size_t j;
    for (j = 0; j < ctx->scan.count; j++) {
        if (IsEqualGUID(&ctx->scan.records[j].enumerated_id, id))
            return TRUE;
    }
    return FALSE;
}

/* Correlation: per-target classification. WMI work runs only when the
   provider is installed; the PROVIDER_ABSENT branch is pure HCN
   evidence. Returns the target outcome; no premature foreign success
   before correlation. */
static void d2_classify(ExternalAcquireContext *ctx, const HcnAdapterEntry *T_entry,
                        const GUID *owned, const ScanView *view, TargetResult *out)
{
    const GUID *T = &T_entry->interface_guid;
    TopologyBuildResult topo;

    out->outcome = ACQ_NO_POSITIVE;

    ctx_ensure_provider(ctx);
    if (ctx->provider_absent) {
        /* PROVIDER_ABSENT branch: pure HCN evidence (VMP-only mainline).
           A usable list + exactly one definite on-T candidate + no known
           conflict permits HCN-only borrow; unreadable/vanished entries
           never veto it. Otherwise no borrow; creatable targets proceed
           to the create gate. */
        ui_log(L"External: provider absent; using unique HCN-on-target evidence.");
        if (ctx->scan.state == HCN_SCAN_COMPLETE && view->on_t_count == 1) {
            GUID id = view->on_t_id;
            if (IsEqualGUID(&id, owned)) {
                ctx->result.network_id = id;
                ctx->result.adapter_interface_guid = *T;
                ctx->result.delete_network_on_last_release = TRUE;
                out->outcome = ACQ_OWNED;
            } else {
                ctx->result.network_id = id;
                ctx->result.adapter_interface_guid = *T;
                ctx->result.delete_network_on_last_release = FALSE;
                out->outcome = ACQ_BORROWED;
                {
                    wchar_t q[64];
                    guid_to_string(&id, q, 64);
                    ui_log(L"External: reusing network %s; lifecycle=borrowed.", q);
                }
            }
        }
        return;
    }

    /* Provider installed: the shared topology (lazy, once). */
    ctx_ensure_topology(ctx, &topo);
    if (!topo.built) {
        if (topo.absent) {
            /* The build reclassified the provider as absent: fall back
               to the pure-HCN correlation branch for this and later targets. */
            ctx->provider_absent = TRUE;
            d2_classify(ctx, T_entry, owned, view, out);
            return;
        }
        out->outcome = ACQ_WMI_ERROR;
        ctx_set_wmi_error(ctx, topo.hr, topo.cause);
        return;
    }

    /* Association: T -> unique bound external port -> parent switch. */
    {
        const WmiSwitchEntry *sw = NULL;
        TopologyResultKind assoc;

        ui_log(L"External: HCN candidate pending target WMI association check.");
        assoc = wmi_topology_switch_for_external_adapter(&ctx->topo, T, &sw);
        if (assoc == TOPOLOGY_CONFLICT) {
            /* Multiple owners of T: object-level failure. */
            out->outcome = ACQ_FAILURE;
            out->reason = EXT_REASON_TOPOLOGY_CONFLICT;
            ctx_fail(ctx, EXT_REASON_TOPOLOGY_CONFLICT, E_FAIL, NULL);
            return;
        }
        if (assoc == TOPOLOGY_ERROR) {
            /* Broken association: host-wide WMI ERROR. */
            out->outcome = ACQ_WMI_ERROR;
            ctx_set_wmi_error(ctx, E_FAIL, WMI_FAIL_DATA);
            return;
        }

        if (assoc == TOPOLOGY_FOUND) {
            /* Single-uplink check: a selected External requires exactly
               one bound physical uplink. */
            if (sw->bound_external_ports > 1) {
                out->outcome = ACQ_FAILURE;
                ctx_fail(ctx, EXT_REASON_SET_UNSUPPORTED, E_FAIL, &sw->id);
                return;
            }

            /* Correlation ladder (in order; step 1 precedes any list check). */
            {
                void *network = NULL;
                PWSTR json = NULL;
                HcnNetworkProps props;
                HcnLookupKind k;
                HRESULT hr = S_OK;
                BOOL borrowed = FALSE;
                GUID id = sw->id;

                if (view->on_t_count == 1 && !IsEqualGUID(&view->on_t_id, &id)) {
                    /* Distinct definite on-T IDs / conflicting
                       associations fail; never first-wins. */
                    wchar_t a[64], b[64];
                    guid_to_string(&view->on_t_id, a, 64);
                    guid_to_string(&id, b, 64);
                    ui_log(L"External: topology conflict: %s vs %s on target; "
                           L"no downgrade; the attempt fails at this target "
                           L"(no later adapter is considered).", a, b);
                    out->outcome = ACQ_FAILURE;
                    ctx_fail(ctx, EXT_REASON_TOPOLOGY_CONFLICT, E_FAIL, &id);
                    return;
                }

                k = hcn_open_network_exact(&id, &network, &hr);
                if (k == HCN_LOOKUP_ERROR) {
                    /* Hard open error: if the scan listed this ID its
                       record is unprovable (OCCUPIED shape (a)); an
                       unlisted hard error is a read-layer failure. */
                    BOOL listed = scan_lists_id(ctx, &id);
                    if (listed) {
                        out->outcome = ACQ_OCCUPIED;
                        out->reason = EXT_REASON_TARGET_OCCUPIED;
                        out->object_id = id;
                        out->object_valid = TRUE;
                        return;
                    }
                    out->outcome = ACQ_FAILURE;
                    ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, hr, &id);
                    return;
                }
                if (k == HCN_LOOKUP_NOT_FOUND) {
                    /* Exact not-found on direct open: step 2 (listed but
                       unqueryable -> OCCUPIED (a)) or step 3 (not in the
                       list -> OCCUPIED (c), the list is untrustworthy). */
                    BOOL listed = scan_lists_id(ctx, &id);
                    out->outcome = ACQ_OCCUPIED;
                    out->object_id = id;
                    out->object_valid = TRUE;
                    if (listed) {
                        out->reason = EXT_REASON_TARGET_OCCUPIED;
                    } else {
                        out->reason = EXT_REASON_HCN_TARGET_UNAVAILABLE;
                        out->occupied_shape_c = TRUE;
                        /* The list is proven untrustworthy (it missed a
                           WMI-visible switch): enumeration_complete is
                           forced FALSE - host-wide create block,
                           borrowing continues; never a wrapper/replacement
                           on T. */
                        ctx->scan.enumeration_complete = FALSE;
                        ctx->list_untrustworthy = TRUE;
                        ctx_record_block(ctx, EXT_REASON_HCN_SCAN_FAILED, &id);
                        {
                            wchar_t q[64];
                            guid_to_string(&id, q, 64);
                            ui_log(L"External: HCN list untrustworthy (WMI switch "
                                   L"%s has no HCN record); creation blocked "
                                   L"host-wide; borrowing continues.", q);
                        }
                    }
                    return;
                }

                /* Step 1: opened - must query as Transparent and must not
                   contradict binding to T. */
                ZeroMemory(&props, sizeof(props));
                hr = hcn_query_network_properties(network, &json);
                if (network && pfnCloseNet) pfnCloseNet(network);
                if (SUCCEEDED(hr))
                    hr = parse_hcn_network_properties(json, &props);
                hcn_free_string(json);
                if (FAILED(hr) ||
                    !props.has_id || !IsEqualGUID(&props.id, &id) ||
                    !props.has_type || props.type_truncated ||
                    _wcsicmp(props.type, L"Transparent") != 0) {
                    /* Query or correlation fails on the opened object:
                       OCCUPIED shape (a) - skip T, continue (per-T). */
                    out->outcome = ACQ_OCCUPIED;
                    out->reason = EXT_REASON_TARGET_OCCUPIED;
                    out->object_id = id;
                    out->object_valid = TRUE;
                    return;
                }
                {
                    BindingClass cls = classify_binding_values(&props, &ctx->inv, T);
                    if (cls == BINDING_CONFLICT) {
                        /* A field-level conflict whose values involve T
                           stops the walk - never an OCCUPIED
                           per-T skip. */
                        wchar_t q[64];
                        out->outcome = ACQ_FAILURE;
                        out->reason = EXT_REASON_TOPOLOGY_CONFLICT;
                        guid_to_string(&props.id, q, 64);
                        ui_log(L"External: topology conflict: binding fields "
                               L"disagree on target (object %s); no downgrade.",
                               q);
                        ctx_fail(ctx, EXT_REASON_TOPOLOGY_CONFLICT, E_FAIL,
                                 &props.id);
                        return;
                    }
                    if (cls == BINDING_OFF_T) {
                        /* Correlation fails: OCCUPIED shape
                           (a) - skip T, continue (per-T). */
                        out->outcome = ACQ_OCCUPIED;
                        out->reason = EXT_REASON_TARGET_OCCUPIED;
                        out->object_id = id;
                        out->object_valid = TRUE;
                        return;
                    }
                }

                /* Correlation may also use the definite on-T candidate:
                   the same ID observed through HCN and WMI is one
                   candidate (already checked for distinct-ID conflict
                   above). */
                borrowed = !IsEqualGUID(&id, owned);
                ctx->result.network_id = props.id;
                ctx->result.adapter_interface_guid = *T;
                ctx->result.delete_network_on_last_release = borrowed ? FALSE : TRUE;
                out->outcome = borrowed ? ACQ_BORROWED : ACQ_OWNED;
                {
                    wchar_t q[64];
                    guid_to_string(&props.id, q, 64);
                    ui_log(L"External: reusing network %s; lifecycle=%s.", q,
                           borrowed ? L"borrowed" : L"owned");
                }
                return;
            }
        }

        /* assoc == TOPOLOGY_NOT_FOUND: authority check first. */
        if (!ctx->token_checked) {
            ctx->token_checked = TRUE;
            ctx->token_unfiltered = hcn_coverage_token_unfiltered();
        }
        if (!ctx->topo.host_system_visible || !ctx->token_unfiltered) {
            /* A filtered or coverage-disqualified view is ERROR, not
               authoritative NOT_FOUND: host-wide downgrade. */
            out->outcome = ACQ_WMI_ERROR;
            ctx_set_wmi_error(ctx, E_FAIL, WMI_FAIL_FILTERED);
            return;
        }

        if (view->on_t_count >= 1) {
            /* Definite HCN on-T candidate contradicted by authoritative
               WMI NOT_FOUND: OCCUPIED-class per-T skip (TOPOLOGY_CONFLICT
               reason; T is unusable, never walk-fatal). */
            out->outcome = ACQ_OCCUPIED;
            out->reason = EXT_REASON_TOPOLOGY_CONFLICT;
            out->object_id = view->on_t_id;
            out->object_valid = TRUE;
            return;
        }

        /* No borrow; a creatable target carries the authoritative
           NOT_FOUND result into the create gate. */
        out->authoritative_not_found = TRUE;
        return;
    }
}

/* discover_read: owned probe + scan view + correlation, shared by Auto
   and Explicit. Runs for every live NIC after an owned-probe miss - on
   VMP-only hosts too (classification-only mode). The scan view is
   returned for the create gate. */
static HRESULT discover_read(ExternalAcquireContext *ctx, const HcnAdapterEntry *T_entry,
                             TargetResult *out)
{
    const GUID *T = &T_entry->interface_guid;
    const GUID *owned;
    HRESULT hr;

    out->outcome = ACQ_NO_POSITIVE;
    out->reason = EXT_REASON_NONE;
    out->hr = E_FAIL;
    out->object_valid = FALSE;
    out->occupied_shape_c = FALSE;
    out->authoritative_not_found = FALSE;
    ZeroMemory(&out->view, sizeof(out->view));
    out->view.on_t_index = -1;

    owned = ctx_owned_id_for(ctx, T);
    if (!owned) {
        /* T comes from the eligible inventory whose owned IDs are
           precomputed; unreachable unless the set build dropped an
           entry. Fail closed. */
        return ctx_fail(ctx, EXT_REASON_INVENTORY_FAILED, E_FAIL, T);
    }

    /* Owned probe. */
    hr = d0_owned_open(ctx, T, owned, out);
    if (FAILED(hr))
        return hr;
    if (out->outcome == ACQ_OWNED)
        return S_OK;
    if (hr != S_FALSE)
        return E_FAIL;   /* defensive: S_FALSE is the only continue signal */

    /* Scan view: ensure the shared scan (lazy; NOT_AVAILABLE decided once). */
    hr = ctx_ensure_scan(ctx);
    if (FAILED(hr)) {
        /* Read-layer failure: the walk stops at this NIC. */
        return ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, hr, NULL);
    }

    hr = d1_scan_view(ctx, T, &out->view, out);
    if (FAILED(hr))
        return hr;
    if (out->outcome == ACQ_OWNED)
        return S_OK;

    /* Correlation. */
    d2_classify(ctx, T_entry, owned, &out->view, out);
    return S_OK;
}

/* ---- Create gate ---- */

/* The create gate. Runs only for a creatable target
   (Auto: ordinary_usable passed and create still available; Explicit:
   after NO_POSITIVE). All conditions must hold for FREE:
   owned exact not-found (the owned-probe miss that got here), complete
   enumeration, no definite on-T candidate, no remaining unproven
   occupant of T, no known conflict, and WMI authoritative NOT_FOUND or
   genuine PROVIDER_ABSENT. An unproven occupant or an untrustworthy
   scan alone is CREATE_BLOCKED (host-wide create block; the walk
   continues for OWNED/BORROWED). */
static void evaluate_create_gate(ExternalAcquireContext *ctx, const HcnAdapterEntry *T_entry,
                                 const ScanView *view, BOOL authoritative_not_found,
                                 TargetResult *out)
{
    out->outcome = ACQ_FAILURE;
    out->reason = EXT_REASON_NONE;
    out->hr = E_FAIL;
    out->object_valid = FALSE;
    out->occupied_shape_c = FALSE;
    out->authoritative_not_found = FALSE;

    /* Defensive only: a FOUND or OCCUPIED target never reaches the gate
       (correlation ran first for every live NIC); if it ever does, treat it as
       occupied and fail. */
    if (view->on_t_count > 0) {
        ctx_fail(ctx, EXT_REASON_TARGET_OCCUPIED, E_FAIL, &view->on_t_id);
        return;
    }

    /* Condition: enumeration_complete == true (list-level: call +
       document + no vanished entry; also forced FALSE by the correlation
       ladder). NOT_AVAILABLE was decided up front and blocks create via
       API_UNAVAILABLE - for Auto the create_available guard keeps the gate
       unreached; Explicit fails here with that reason. */
    if (ctx->scan.state == HCN_SCAN_NOT_AVAILABLE) {
        ctx_fail(ctx, EXT_REASON_API_UNAVAILABLE, E_NOT_VALID_STATE, NULL);
        return;
    }
    if (ctx->scan.state != HCN_SCAN_COMPLETE) {
        /* FAILED scan: the read layer is down - enumeration_
           completeness is unknown, so FREE is impossible (a
           missing/incomplete scan prevents FREE even with WMI NOT_FOUND).
           Defensive: the callers fail on the propagated HRESULT before
           the gate is reached. */
        ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED,
                 FAILED(ctx->scan.failure_hr) ? ctx->scan.failure_hr : E_FAIL,
                 NULL);
        return;
    }
    if (!ctx->scan.enumeration_complete) {
        /* Untrustworthy scan only (vanished entry or a WMI-visible switch
           the list missed): CREATE_BLOCKED. */
        ctx_record_block(ctx, EXT_REASON_HCN_SCAN_FAILED, NULL);
        out->outcome = ACQ_CREATE_BLOCKED;
        out->reason = EXT_REASON_HCN_SCAN_FAILED;
        return;
    }

    /* Condition: no remaining unproven object that might occupy T. A
       complete unfiltered WMI NOT_FOUND excludes unproven records as
       occupants of T; provider absence cannot. */
    if (view->unproven_occupant && !authoritative_not_found) {
        log_unresolved_occupant(
            (view->unproven_index >= 0 &&
             (size_t)view->unproven_index < ctx->scan.count)
                ? &ctx->scan.records[view->unproven_index] : NULL,
            view->unproven_first_valid ? &view->unproven_first : NULL);
        ctx_record_block(ctx, EXT_REASON_TARGET_UNKNOWN,
                         view->unproven_first_valid ? &view->unproven_first : NULL);
        out->outcome = ACQ_CREATE_BLOCKED;
        out->reason = EXT_REASON_TARGET_UNKNOWN;
        if (view->unproven_first_valid) {
            out->object_id = view->unproven_first;
            out->object_valid = TRUE;
        }
        return;
    }

    /* Condition: WMI = authoritative NOT_FOUND (host_system_visible and
       unfiltered token) or genuine PROVIDER_ABSENT. A
       provider-installed NO_POSITIVE without authority already
       downgraded to WMI_ERROR inside correlation, so reaching here means one of
       the two held; the branch is defensive. */
    if (!authoritative_not_found && !ctx->provider_absent) {
        ctx_set_wmi_error(ctx, E_FAIL, WMI_FAIL_FILTERED);
        out->outcome = ACQ_WMI_ERROR;
        return;
    }

    /* All conditions hold: FREE. */
    out->outcome = ACQ_FREE;
    out->object_id = T_entry->interface_guid;
    out->object_valid = TRUE;
}

/* ---- Create with create-exit binding verification ---- */

/* Create the owned network on T and verify this call's product before
   publication. Only a selected FREE target reaches here (creation
   is never a probe; no retry, alternative ID, or ALREADY_EXISTS
   catch-and-reopen). The identity travels in the binary REFGUID
   argument only (the house form); the NIC GUID belongs in
   InterfaceConstraint.Settings.InterfaceGuid. The created network
   handle is closed on both success and failure before the caller
   returns: the result carries IDs, never handles. */
static HRESULT create_owned_external_network(ExternalAcquireContext *ctx,
                                             const HcnAdapterEntry *T_entry)
{
    const GUID *T = &T_entry->interface_guid;
    const GUID *owned = ctx_owned_id_for(ctx, T);
    wchar_t name[HCN_PHYS_NAME_MAX];
    wchar_t t_str[64];
    wchar_t settings[512];
    void *network = NULL;
    PWSTR error_record = NULL;
    PWSTR json = NULL;
    HcnNetworkProps props;
    TargetResult reuse_out;
    HRESULT hr;
    BOOL verify_ok = FALSE;
    HRESULT verify_hr = S_OK;

    if (!owned)
        return ctx_fail(ctx, EXT_REASON_INVENTORY_FAILED, E_FAIL, T);

    if (!pfnCreateNet) {
        /* No supported create route (API_UNAVAILABLE):
           fail the attempt, never a fallback. */
        return ctx_fail(ctx, EXT_REASON_API_UNAVAILABLE, E_NOT_VALID_STATE, T);
    }

    hr = hcn_external_owned_name(T, name, ARRAYSIZE(name));
    if (FAILED(hr))
        return ctx_fail(ctx, EXT_REASON_INVENTORY_FAILED, hr, T);

    guid_to_string(T, t_str, 64);
    /* Backend JSON construction with swprintf_s + guid_to_string;
       GUID strings need no JSON escaping. Never mix name/GUID binding
       policies; no top-level ID field (the binary REFGUID carries it). */
    swprintf_s(settings, ARRAYSIZE(settings),
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"%s\","
        L"\"Type\":\"Transparent\","
        L"\"Policies\":[{\"Type\":\"InterfaceConstraint\","
        L"\"Settings\":{\"InterfaceGuid\":\"%s\"}}]"
        L"}", name, t_str);

    /* The create serializes with the fixed-GUID create family and the
       owned-delete shield through the shared network lock: the gate
       above proved FREE against a pre-create topology, so a concurrent
       acquire for the same T could otherwise run its exists-check
       between that proof and this create - upstream d307a22 locks its
       own lookup+create for the same reason ("concurrently started VMs
       reuse the same network"), and this branch's create has no
       ALREADY_EXISTS recovery, so the un-serialized race fails closed.
       The scan/gate work stays outside the lock (long WMI/HCN reads). */
    hcn_network_lock_acquire();
    /* Re-check the owned ID under the lock: a racing acquire may have
       created the network since the gate ran. Present and
       identity-matching takes the owned-reuse outcome; still absent
       creates. */
    hr = d0_owned_open(ctx, T, owned, &reuse_out);
    if (hr == S_OK) {
        /* Owned reuse published by the re-check (nothing was created). */
        hcn_network_lock_release();
        return S_OK;
    }
    if (FAILED(hr)) {
        /* The re-check's own failure is already recorded (ctx_fail). */
        hcn_network_lock_release();
        return hr;
    }

    hr = pfnCreateNet(owned, settings, &network, &error_record);
    hcn_network_lock_release();
    if (error_record) {
        if (FAILED(hr))
            ui_log(L"External: HCN create error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (FAILED(hr)) {
        /* The platform behavior: HNS rejects a native Transparent create
           on a connected 802.11 adapter with E_INVALIDARG; a
           media-disconnected 802.11 adapter fails adapter resolution
           earlier, with HCN_E_ADAPTER_NOT_FOUND. Flag both so the
           reason line explains the cause and the way out instead of
           the bare HRESULT. */
        if (T_entry->if_type == IF_TYPE_IEEE80211 &&
            (hr == E_INVALIDARG || hr == HCN_E_ADAPTER_NOT_FOUND)) {
            ctx->wifi_create_rejected = TRUE;
            ctx->target_not_connected = T_entry->not_media_connected;
        }
        return ctx_fail(ctx, EXT_REASON_CREATE_FAILED, hr, owned);
    }
    if (!network) {
        /* Success without a network handle is a failed call contract. */
        ui_log(L"External: HcnCreateNetwork returned success without a handle.");
        return ctx_fail(ctx, EXT_REASON_CREATE_FAILED, E_FAIL, owned);
    }

    /* Create-exit binding verification: query the created network
       through the handle already held - the query-back document must
       carry T's InterfaceGuid in a supported binding field (the
       InterfaceConstraint echo at minimum). */
    hr = hcn_query_network_properties(network, &json);
    if (SUCCEEDED(hr))
        hr = parse_hcn_network_properties(json, &props);
    if (SUCCEEDED(hr)) {
        BindingClass cls = classify_binding_values(&props, &ctx->inv, T);
        verify_ok = (cls == BINDING_ON_T);
        if (!verify_ok)
            verify_hr = E_FAIL;
    } else {
        verify_hr = hr;
    }

    /* On a WMI-installed host, rebuild the shared topology once and
       re-run the association for T: it must return FOUND naming the
       created switch (the gate required an authoritative NOT_FOUND, so
       the topology predates the create). On VMP-only the check is
       echo-level only - a known, accepted residual. */
    if (verify_ok && !ctx->provider_absent && !ctx->wmi_failed) {
        WmiSession session;
        WmiTopology new_topo;
        TopologyResultKind kind;

        hr = wmi_session_open(&session);
        if (FAILED(hr)) {
            verify_ok = FALSE;
            verify_hr = hr;
        } else {
            ULONGLONG deadline = GetTickCount64() + WMI_DEFAULT_DEADLINE_MS;
            hr = wmi_build_topology(&session, deadline, &new_topo, &kind);
            wmi_session_close(&session);
            if (SUCCEEDED(hr)) {
                const WmiSwitchEntry *sw = NULL;
                TopologyResultKind assoc =
                    wmi_topology_switch_for_external_adapter(&new_topo, T, &sw);
                if (assoc != TOPOLOGY_FOUND || !sw ||
                    !IsEqualGUID(&sw->id, owned)) {
                    /* A semantic mismatch (NOT_FOUND/CONFLICT association,
                       or a switch that is not the created one) has no
                       originating HRESULT: fail with a project E_FAIL,
                       never a success code like the build's S_OK. */
                    verify_ok = FALSE;
                    verify_hr = E_FAIL;
                }
                wmi_topology_free(&new_topo);
            } else {
                verify_ok = FALSE;
                verify_hr = hr;
            }
        }
    }

    /* The handle closes on both success and failure before returning. */
    if (network && pfnCloseNet)
        pfnCloseNet(network);
    hcn_free_string(json);

    if (!verify_ok) {
        /* CREATE_UNBOUND: best-effort delete of this call's failed
           product - it has no endpoints, so this is in-call cleanup, the
           same class as the failed-start endpoint drain. Log if the
           delete fails: the object then remains for the startup sweep or
           owned reuse - recorded, never silently assumed away. */
        PWSTR del_err = NULL;
        HRESULT del_hr = pfnDeleteNet(owned, &del_err);
        if (del_err) {
            if (FAILED(del_hr))
                ui_log(L"External: HCN error: %s", del_err);
            hcn_free_string(del_err);
        }
        if (FAILED(del_hr) && del_hr != HCN_DELETE_NOT_FOUND) {
            wchar_t q[64];
            guid_to_string(owned, q, 64);
            ui_log(L"External: best-effort delete of the unbound created object "
                   L"%s failed (0x%08X); it remains for the startup sweep or "
                   L"owned reuse.", q, del_hr);
        }
        ui_log(L"External: create succeeded but binding verification failed "
               L"(0x%08X); created object deleted; no network published.",
               verify_hr);
        return ctx_fail(ctx, EXT_REASON_CREATE_UNBOUND, verify_hr, owned);
    }

    /* Publish: created ID, T, owned flag TRUE. */
    ctx->result.network_id = *owned;
    ctx->result.adapter_interface_guid = *T;
    ctx->result.delete_network_on_last_release = TRUE;
    ui_log(L"External: created owned network on %s; host connectivity on this "
           L"adapter goes offline for the network's lifetime; connectivity "
           L"returns when the network is deleted.", T_entry->friendly_name);
    return S_OK;
}

/* ---- Auto walk ---- */

/* The ordered walk set: Ethernet by ifIndex, then Wi-Fi by ifIndex,
   over hardware-eligible entries only. Returns a heap-allocated
   array of inventory indices; count via out_count. */
static size_t *build_auto_walk_order(const ExternalAcquireContext *ctx, size_t *out_count)
{
    size_t *order = (size_t *)HeapAlloc(GetProcessHeap(), 0,
                                        (ctx->inv.count ? ctx->inv.count : 1) * sizeof(size_t));
    size_t n = 0, pass, i;

    *out_count = 0;
    if (!order)
        return NULL;

    /* Two passes with a stable smallest-ifIndex-first selection: no
       silent truncation (the array holds every eligible entry). */
    for (pass = 0; pass < 2; pass++) {
        ULONG want_type = (pass == 0) ? IF_TYPE_ETHERNET_CSMACD : IF_TYPE_IEEE80211;
        for (;;) {
            int best = -1;
            for (i = 0; i < ctx->inv.count; i++) {
                const HcnAdapterEntry *e = &ctx->inv.entries[i];
                size_t j;
                BOOL already = FALSE;

                if (!e->is_hardware_eligible || e->if_type != want_type)
                    continue;
                for (j = 0; j < n; j++) {
                    if (order[j] == i) {
                        already = TRUE;
                        break;
                    }
                }
                if (already)
                    continue;
                if (best < 0 || e->ifindex < ctx->inv.entries[best].ifindex)
                    best = (int)i;
            }
            if (best < 0)
                break;
            order[n++] = (size_t)best;
        }
    }

    *out_count = n;
    return order;
}

/* Walk tail: the first unproven occupant (the gate never reached it), or
   NULL. Derived-identity records are AppSandbox's own networks or
   reserved IDs - off every other NIC, not occupants. A COMPLETE+
   Transparent record is an occupant when it has no binding value, or
   when its binding values are unresolvable for every target (ambiguous
   names, unknown GUID-shaped names, disagreeing values that all resolve
   elsewhere): probing the classifier against a non-NIC GUID (GUID_NULL
   never identifies a NIC) keeps the check T-independent - a definite
   value resolving to a real NIC is MISMATCH there (not an occupant of
   this walk), while unresolvable values classify UNPROVEN. */
static const HcnScanRecord *scan_first_plain_occupant(const ExternalAcquireContext *ctx)
{
    size_t j;
    for (j = 0; j < ctx->scan.count; j++) {
        const HcnScanRecord *rec = &ctx->scan.records[j];
        if (rec->has_derived_identity)
            continue;
        if (rec->state == HCN_REC_UNPROVEN)
            return rec;
        if (rec->state == HCN_REC_COMPLETE && rec->transparent &&
            (rec->props.binding_count == 0 ||
             classify_binding_values(&rec->props, &ctx->inv, &GUID_NULL) ==
                 BINDING_UNPROVEN))
            return rec;
    }
    return NULL;
}

static HRESULT acquire_auto(ExternalAcquireContext *ctx)
{
    size_t *order;
    size_t order_count, i;
    HRESULT hr;

    order = build_auto_walk_order(ctx, &order_count);
    if (!order)
        return ctx_fail(ctx, EXT_REASON_INVENTORY_FAILED, E_OUTOFMEMORY, NULL);

    /* The enumerate export's availability is decided once, before the
       walk: API_UNAVAILABLE recorded, create blocked; the walk
       still runs the owned probe and, with WMI, correlation. Capability
       check only - the scan builds lazily at the first scan miss (a
       first-NIC owned hit pays no enumeration), where a FAILED scan stops
       the walk with its originating HRESULT instead of being swallowed. */
    ctx_decide_enumerate_availability(ctx);

    for (i = 0; i < order_count; i++) {
        const HcnAdapterEntry *e = &ctx->inv.entries[order[i]];
        TargetResult r;
        BOOL is_wifi = (e->if_type == IF_TYPE_IEEE80211);

        if (!adapter_is_live(e)) {
            ui_log(L"External: skipped adapter: link down.");
            continue;
        }

        if (ctx->wmi_failed) {
            /* Host-wide WMI ERROR earlier: owned-probe-only continue. Later
               NICs pay the pure-ID probe only - owned reuse on a later
               NIC still works; the shared scan is not consulted further. */
            const GUID *T = &e->interface_guid;
            const GUID *owned = ctx_owned_id_for(ctx, T);
            HRESULT d0hr;
            TargetResult d0r;

            ZeroMemory(&d0r, sizeof(d0r));
            if (!owned) {
                hr = ctx_fail(ctx, EXT_REASON_INVENTORY_FAILED, E_FAIL, T);
                goto done;
            }
            d0hr = d0_owned_open(ctx, T, owned, &d0r);
            if (FAILED(d0hr)) {
                hr = d0hr;
                goto done;
            }
            if (d0r.outcome == ACQ_OWNED) {
                hr = S_OK;
                goto done;
            }
            /* Scan/correlation/gate are unusable: continue. */
            continue;
        }

        hr = discover_read(ctx, e, &r);
        if (FAILED(hr)) {
            /* Read-layer failure: stop the walk at this NIC. The
               in-loop failure reports its own reason; any recorded one
               was logged. */
            goto done;
        }

        if (r.outcome == ACQ_FAILURE) {
            /* A walk-stopping correlation finding (SET, conflict,
               read-layer error) is never treated
               as NO_POSITIVE and never reselects a later NIC. */
            hr = FAILED(ctx->fail_hr) ? ctx->fail_hr : E_FAIL;
            goto done;
        }

        if (r.outcome == ACQ_WMI_ERROR) {
            /* Host-wide WMI ERROR: record (top precedence), block create,
               continue with the owned probe only. */
            continue;
        }
        if (r.outcome == ACQ_OWNED || r.outcome == ACQ_BORROWED) {
            hr = S_OK;
            goto done;
        }
        if (r.outcome == ACQ_OCCUPIED) {
            /* Per-T skip, never host-wide by itself; shape (c)
               additionally marked the list untrustworthy inside correlation. */
            ctx_note_occupied(ctx, r.reason,
                              r.object_valid ? &r.object_id : NULL);
            continue;
        }

        /* NO_POSITIVE: no existing network on T. */
        if (!adapter_ordinary_usable(e)) {
            ui_log(L"External: skipped adapter: no physical IPv4 gateway "
                   L"(gate not run).");
            continue;
        }
        if (ctx->create_blocked) {
            /* Blocked host-wide earlier (the create_available guard). */
            continue;
        }

        evaluate_create_gate(ctx, e, &r.view, r.authoritative_not_found, &r);
        if (r.outcome == ACQ_WMI_ERROR) {
            /* Defensive only (the WMI_ERROR arm). */
            continue;
        }
        if (r.outcome == ACQ_FREE) {
            if (is_wifi && !EXTERNAL_WIFI_AUTO_CREATABLE) {
                ui_log(L"External: skipped adapter: a native external network "
                       L"cannot be created on a wireless adapter (use a Hyper-V "
                       L"external switch, NAT, or a wired adapter).");
                continue;
            }
            hr = create_owned_external_network(ctx, e);
            goto done;
        }
        if (r.outcome == ACQ_CREATE_BLOCKED) {
            /* Unlocatable occupant or untrustworthy scan: creation
               unavailable for the rest of the walk; continue for
               OWNED/BORROWED only. The block reason/object was recorded
               once inside the gate. */
            continue;
        }

        /* Other create-gate failures stop the walk. */
        hr = FAILED(ctx->fail_hr) ? ctx->fail_hr : E_FAIL;
        goto done;
    }

    /* End-of-walk arbitration (precedence order). */
    if (ctx->wmi_failed) {
        hr = ctx_fail(ctx, EXT_REASON_WMI_FAILED, ctx->wmi_hr, NULL);
    } else if (ctx->create_blocked) {
        hr = ctx_fail(ctx, ctx->block_reason,
                      (ctx->block_reason == EXT_REASON_API_UNAVAILABLE)
                          ? E_NOT_VALID_STATE : E_FAIL,
                      ctx->block_object_valid ? &ctx->block_object : NULL);
    } else if (ctx->scan.state == HCN_SCAN_COMPLETE && !ctx->scan.enumeration_complete) {
        hr = ctx_fail(ctx, EXT_REASON_HCN_SCAN_FAILED, E_FAIL, NULL);
    } else if (ctx->scan.state == HCN_SCAN_COMPLETE) {
        /* Unproven occupants remain and the gate never reached
           them: TARGET_UNKNOWN names the first blocking object. */
        const HcnScanRecord *occ = scan_first_plain_occupant(ctx);
        if (occ) {
            GUID first = (occ->state == HCN_REC_COMPLETE) ? occ->props.id
                                                          : occ->enumerated_id;
            log_unresolved_occupant(occ, &first);
            hr = ctx_fail(ctx, EXT_REASON_TARGET_UNKNOWN, E_FAIL, &first);
        } else if (ctx->occupied_seen) {
            /* The tail's per-T occupied summary: the walk DID continue
               past these targets - the reason line's "target skipped
               (per-T)" is accurate for this report alone. */
            ctx->occupied_tail_report = TRUE;
            hr = ctx_fail(ctx, ctx->occupied_reason, E_FAIL,
                          ctx->occupied_object_valid ? &ctx->occupied_object : NULL);
        } else {
            hr = ctx_fail(ctx, EXT_REASON_AUTO_UNRESOLVED, E_FAIL, NULL);
        }
    } else if (ctx->occupied_seen) {
        ctx->occupied_tail_report = TRUE;
        hr = ctx_fail(ctx, ctx->occupied_reason, E_FAIL,
                      ctx->occupied_object_valid ? &ctx->occupied_object : NULL);
    } else {
        hr = ctx_fail(ctx, EXT_REASON_AUTO_UNRESOLVED, E_FAIL, NULL);
    }

done:
    HeapFree(GetProcessHeap(), 0, order);
    return hr;
}

/* ---- Explicit ---- */

static HRESULT acquire_explicit(ExternalAcquireContext *ctx)
{
    const wchar_t *name = ctx->configured_adapter;
    const HcnAdapterEntry *match = NULL;
    int matches = 0;
    size_t i;
    TargetResult r;
    HRESULT hr;

    /* Resolve against the FULL eligible inventory, not a capped UI
       list. Case-insensitive; zero/multiple matches fail; no
       first-match, rename inference, or link/gateway/route gate. */
    for (i = 0; i < ctx->inv.count; i++) {
        const HcnAdapterEntry *e = &ctx->inv.entries[i];
        if (!e->is_hardware_eligible)
            continue;
        if (e->friendly_name[0] && _wcsicmp(e->friendly_name, name) == 0) {
            match = e;
            matches++;
        }
    }
    if (matches == 0) {
        ui_log(L"External: configured adapter \"%s\" not found.", name);
        return ctx_fail(ctx, EXT_REASON_EXPLICIT_MISSING, E_FAIL, NULL);
    }
    if (matches > 1) {
        ui_log(L"External: configured adapter \"%s\" matches %d adapters; "
               L"refusing to pick one.", name, matches);
        return ctx_fail(ctx, EXT_REASON_EXPLICIT_AMBIGUOUS, E_FAIL, NULL);
    }

    hr = discover_read(ctx, match, &r);
    if (FAILED(hr))
        return hr;

    if (r.outcome == ACQ_OWNED || r.outcome == ACQ_BORROWED)
        return S_OK;

    if (r.outcome == ACQ_WMI_ERROR) {
        return ctx_fail(ctx, EXT_REASON_WMI_FAILED, ctx->wmi_hr, NULL);
    }
    if (r.outcome == ACQ_FAILURE) {
        /* Correlation's walk-stopping failure (SET/conflict/read-layer):
           report the recorded reason, never a NO_POSITIVE fallback. */
        return FAILED(ctx->fail_hr) ? ctx->fail_hr : E_FAIL;
    }
    if (r.outcome == ACQ_OCCUPIED) {
        /* Explicit wording: there is no later NIC to walk. */
        if (r.object_valid) {
            wchar_t q[64];
            guid_to_string(&r.object_id, q, 64);
            ui_log(L"External: target occupied (WMI switch %s); cannot borrow "
                   L"or create on occupied adapter.", q);
        } else {
            ui_log(L"External: target occupied; cannot borrow or create on "
                   L"occupied adapter.");
        }
        return ctx_fail(ctx, r.reason, E_FAIL,
                        r.object_valid ? &r.object_id : NULL);
    }

    /* NO_POSITIVE: run the same gate (no live/gateway precondition). */
    evaluate_create_gate(ctx, match, &r.view, r.authoritative_not_found, &r);
    if (r.outcome == ACQ_FREE)
        return create_owned_external_network(ctx, match);
    if (r.outcome == ACQ_WMI_ERROR) {
        return ctx_fail(ctx, EXT_REASON_WMI_FAILED, ctx->wmi_hr, NULL);
    }
    if (r.outcome == ACQ_CREATE_BLOCKED) {
        /* There is no later NIC: fail with the recorded reason. */
        return ctx_fail(ctx, r.reason, E_FAIL,
                        r.object_valid ? &r.object_id : NULL);
    }

    /* The gate's failure reason/object was recorded by ctx_fail. */
    return FAILED(ctx->fail_hr) ? ctx->fail_hr : E_FAIL;
}

/* ---- Entry, publication, reason line ---- */

/* Build the one-line diagnostic for the failing caller (truncated to
   fit; display text, not configuration). */
static void build_reason_line(const ExternalAcquireContext *ctx,
                              wchar_t *out, size_t cap)
{
    ExternalReasonKind kind = ctx->fail_reason;
    wchar_t q[64];

    if (!out || cap == 0)
        return;
    out[0] = L'\0';

    if (ctx->fail_object_valid)
        guid_to_string(&ctx->fail_object, q, 64);

    switch (kind) {
    case EXT_REASON_EXPLICIT_MISSING:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: configured adapter \"%s\" not found; re-select the adapter.",
            ctx->configured_adapter ? ctx->configured_adapter : L"");
        break;
    case EXT_REASON_EXPLICIT_AMBIGUOUS:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: configured adapter \"%s\" matches multiple adapters; re-select.",
            ctx->configured_adapter ? ctx->configured_adapter : L"");
        break;
    case EXT_REASON_AUTO_UNRESOLVED:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: no usable External adapter found (link down, no IPv4 gateway, or occupied).");
        break;
    case EXT_REASON_API_UNAVAILABLE:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: HCN enumeration is unavailable (0x%08X); no scan-derived match or create this attempt.",
            ctx->fail_hr);
        break;
    case EXT_REASON_INVENTORY_FAILED:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: adapter inventory failed (0x%08X).", ctx->fail_hr);
        break;
    case EXT_REASON_HCN_SCAN_FAILED:
        if (ctx->scan.state == HCN_SCAN_COMPLETE && !ctx->scan.enumeration_complete)
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: HCN scan incomplete or untrustworthy; creation blocked host-wide; retry may succeed.");
        else
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: HCN scan failed (0x%08X).", ctx->fail_hr);
        break;
    case EXT_REASON_TARGET_UNKNOWN:
        if (ctx->fail_object_valid)
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: unresolved occupant %s; blocks creation on all targets.",
                q);
        else
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: unresolved occupant; blocks creation on all targets.");
        break;
    case EXT_REASON_TARGET_OCCUPIED:
        if (ctx->fail_object_valid)
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: target occupied (WMI switch %s); cannot borrow or create on occupied adapter.",
                q);
        else
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: target occupied; cannot borrow or create on occupied adapter.");
        break;
    case EXT_REASON_TOPOLOGY_CONFLICT:
        if (ctx->occupied_tail_report) {
            /* The tail's per-T occupied summary (NOT_FOUND + a definite
               HCN on-T candidate): the walk continued
               past these targets, so "skipped (per-T)" is accurate. */
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: topology conflict on target: %s; no downgrade; target skipped (per-T).",
                ctx->fail_object_valid ? q : L"(unknown)");
        } else {
            /* A walk-stopping conflict (distinct definite on-T IDs, or
               binding fields involving T): the attempt FAILED at this
               adapter - no later adapter is considered. The
               per-T tail wording does not apply to this shape. */
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: topology conflict on target: %s; no downgrade; the attempt fails at this adapter.",
                ctx->fail_object_valid ? q : L"(unknown)");
        }
        break;
    case EXT_REASON_SET_UNSUPPORTED:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: unsupported multi-uplink target switch; no member selection.");
        break;
    case EXT_REASON_WMI_FAILED:
        switch (ctx->wmi_cause) {
        case WMI_FAIL_DATA:
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: WMI topology data malformed (0x%08X); no service action indicated; no create or borrow this attempt.",
                ctx->wmi_hr);
            break;
        case WMI_FAIL_FILTERED:
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: WMI view filtered or coverage unqualified (elevation required); owned reuse unaffected; no create or borrow this attempt.");
            break;
        case WMI_FAIL_CAP:
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: installed WMI query exceeded the object cap; owned reuse unaffected; no create or borrow this attempt.");
            break;
        default:
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: installed WMI unavailable (0x%08X); Hyper-V Virtual Machine Management service may be stopped; no create or borrow this attempt.",
                ctx->wmi_hr);
            break;
        }
        break;
    case EXT_REASON_HCN_TARGET_UNAVAILABLE:
        if (ctx->fail_object_valid)
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: WMI switch %s has no usable HCN network object; creation blocked host-wide; borrowing continues.",
                q);
        else
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: WMI switch has no usable HCN network object; creation blocked host-wide; borrowing continues.");
        break;
    case EXT_REASON_CREATE_FAILED:
        if (ctx->wifi_create_rejected && ctx->fail_hr == E_INVALIDARG) {
            /* Multi-paragraph on purpose: the popup and log panel render
               line breaks; callers' reason buffers hold 1024+ wchars. */
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: an external network cannot be created on a "
                L"wireless adapter (0x%08X).\n"
                L"\n"
                L"Why:\n"
                L"A Transparent external network is a layer-2 Ethernet "
                L"bridge onto a wired physical adapter; 802.11 infrastructure "
                L"mode allows one MAC address per station, so a wireless "
                L"adapter cannot be bridged.\n"
                L"\n"
                L"How-to:\n"
                L"To use External on Wi-Fi: create a Hyper-V external switch "
                L"on the wireless adapter and keep this adapter selected - "
                L"App Sandbox borrows it automatically; or use NAT or a wired "
                L"adapter.\n"
                L"\n"
                L"https://learn.microsoft.com/windows-server/virtualization/hyper-v/get-started/create-a-virtual-switch-for-hyper-v-virtual-machines",
                ctx->fail_hr);
        } else if (ctx->wifi_create_rejected && ctx->target_not_connected) {
            /* The disconnected shape: HNS resolves the binding adapter
               through the host's active network stack, so a
               media-disconnected adapter is "not found" before the
               802.11 bridging check is ever reached. */
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: an external network cannot be created on this "
                L"wireless adapter (0x%08X).\n"
                L"\n"
                L"Why:\n"
                L"The host network service resolves adapters through the "
                L"host's active network stack, and the selected wireless "
                L"adapter is not connected, so it cannot be resolved (an "
                L"adapter was not found). Even a borrowed external switch "
                L"would give the VM no connectivity while the adapter "
                L"stays disconnected.\n"
                L"\n"
                L"How-to:\n"
                L"Connect the wireless adapter first. To use External on "
                L"Wi-Fi: create a Hyper-V external switch on the wireless "
                L"adapter and keep this adapter selected - App Sandbox "
                L"borrows it automatically; or use NAT or a wired adapter.\n"
                L"\n"
                L"https://learn.microsoft.com/windows-server/virtualization/hyper-v/get-started/create-a-virtual-switch-for-hyper-v-virtual-machines",
                ctx->fail_hr);
        } else if (ctx->wifi_create_rejected) {
            /* Connected but unresolvable (no measured shape): keep the
               explanation honest and generic. */
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: an external network cannot be created on this "
                L"wireless adapter (0x%08X).\n"
                L"\n"
                L"Why:\n"
                L"The host network service could not resolve the selected "
                L"adapter for binding; the adapter may be in a state the "
                L"host network stack cannot use.\n"
                L"\n"
                L"How-to:\n"
                L"Create a Hyper-V external switch on the wireless adapter "
                L"and keep this adapter selected - App Sandbox borrows it "
                L"automatically; or use NAT or a wired adapter.\n"
                L"\n"
                L"https://learn.microsoft.com/windows-server/virtualization/hyper-v/get-started/create-a-virtual-switch-for-hyper-v-virtual-machines",
                ctx->fail_hr);
        } else {
            _snwprintf_s(out, cap, _TRUNCATE,
                L"External: create failed (0x%08X); GUID binding parameters retained.",
                ctx->fail_hr);
        }
        break;
    case EXT_REASON_CREATE_UNBOUND:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: create succeeded but binding verification failed (0x%08X); created object deleted; no network published.",
            ctx->fail_hr);
        break;
    default:
        _snwprintf_s(out, cap, _TRUNCATE,
            L"External: acquisition failed (0x%08X).", ctx->fail_hr);
        break;
    }
}

HRESULT hcn_acquire_external_network(const wchar_t *configured_adapter,
                                     HcnExternalNetworkRef *out,
                                     wchar_t *reason, size_t reason_cap)
{
    ExternalAcquireContext ctx;
    HRESULT hr;

    /* Entry: validate out; zero it; NULL/empty config = Auto. Work in
       a local result published only on success; failure leaves the
       caller's result zero. The reason buffer is written only on
       failure (truncated to fit; display text, not configuration). */
    if (!out)
        return E_POINTER;
    ZeroMemory(out, sizeof(*out));

    /* Every failure exit fills the one-line reason - the capability
       early returns too (API_UNAVAILABLE: "no supported
       discovery/create route"). */
    if (!g_hcn_dll || !pfnOpenNet || !pfnCloseNet) {
        if (reason && reason_cap) {
            ExternalAcquireContext empty;
            ZeroMemory(&empty, sizeof(empty));
            empty.fail_reason = EXT_REASON_API_UNAVAILABLE;
            empty.fail_hr = E_NOT_VALID_STATE;
            build_reason_line(&empty, reason, reason_cap);
        }
        return E_NOT_VALID_STATE;
    }

    /* Capability: HcnQueryNetworkProperties unavailable -> External
       acquisition unsupported (NAT/Internal keep working). */
    if (!pfnQueryNetProps) {
        if (reason && reason_cap) {
            ExternalAcquireContext empty;
            ZeroMemory(&empty, sizeof(empty));
            empty.fail_reason = EXT_REASON_API_UNAVAILABLE;
            empty.fail_hr = E_NOTIMPL;
            build_reason_line(&empty, reason, reason_cap);
        }
        return E_NOTIMPL;
    }

    hr = ctx_build(&ctx);
    if (FAILED(hr)) {
        ExternalReasonKind kind = EXT_REASON_INVENTORY_FAILED;
        ctx_release(&ctx);
        if (reason && reason_cap) {
            ExternalAcquireContext empty;
            ZeroMemory(&empty, sizeof(empty));
            empty.fail_reason = kind;
            empty.fail_hr = hr;
            empty.configured_adapter = configured_adapter;
            build_reason_line(&empty, reason, reason_cap);
        }
        return hr;
    }
    ctx.configured_adapter = configured_adapter;

    if (configured_adapter && configured_adapter[0])
        hr = acquire_explicit(&ctx);
    else
        hr = acquire_auto(&ctx);

    if (SUCCEEDED(hr)) {
        /* Publication: all three fields together, every successful route
           records T. */
        *out = ctx.result;
    } else if (reason && reason_cap) {
        build_reason_line(&ctx, reason, reason_cap);
    }

    /* All paths: one cleanup exit. */
    ctx_release(&ctx);
    return hr;
}

/* ---- Owned-delete shield ---- */

/* Log-only adapter identity for diagnostic lines: reverse-looks the
   interface table by InterfaceGuid for its Description. Purely
   diagnostic - the shield decides on binary IDs only (no Name
   parsing, no HCN topology query) and this lookup never controls flow.
   Any failure leaves buf empty; the caller falls back to the GUID
   string. */
static void lookup_adapter_description(const GUID *iface, wchar_t *buf, size_t cap)
{
    PMIB_IF_TABLE2 table = NULL;
    ULONG i;

    buf[0] = L'\0';
    if (!iface || GetIfTable2(&table) != NO_ERROR || !table)
        return;
    for (i = 0; i < table->NumEntries; i++) {
        if (IsEqualGUID(&table->Table[i].InterfaceGuid, iface)) {
            wcsncpy_s(buf, cap, table->Table[i].Description, _TRUNCATE);
            break;
        }
    }
    FreeMibTable(table);
}

/* External wrapper: derive the per-adapter owned ID here, then let the
   shared binary-ID core serialize the comparison and actual delete.
   Logging and the measured delete-not-found policy stay External-local. */
HRESULT hcn_delete_owned_external_network(const GUID *network_id,
                                          const GUID *adapter_interface_guid)
{
    GUID owned_id;
    PWSTR error_record = NULL;
    BOOL delete_attempted = FALSE;
    HRESULT hr;

    if (!network_id || !adapter_interface_guid ||
        IsEqualGUID(network_id, &GUID_NULL) ||
        IsEqualGUID(adapter_interface_guid, &GUID_NULL))
        return S_FALSE;

    if (FAILED(hcn_external_owned_id(adapter_interface_guid, &owned_id))) {
        ui_log(L"External: owned-delete shield could not derive the owned ID.");
        return S_FALSE;
    }

    hr = hcn_delete_network_if_owned(network_id, &owned_id,
                                     &delete_attempted, &error_record);
    if (!delete_attempted && hr == S_FALSE) {
        /* A borrowed or foreign ID must never reach HcnDeleteNetwork,
           including startup/error/teardown. */
        wchar_t q[64];
        guid_to_string(network_id, q, ARRAYSIZE(q));
        ui_log(L"External: owned-delete shield refused network %s "
               L"(not the derived owned ID for this adapter).", q);
        return S_FALSE;
    }
    if (!delete_attempted && hr == E_NOT_VALID_STATE) {
        /* The delete export resolved away between init and release: a
           loud programming-state error, never a silent skip. */
        ui_log(L"External: owned-delete shield: HcnDeleteNetwork export is "
               L"unavailable; no delete attempted.");
        return hr;
    }

    if (error_record) {
        if (FAILED(hr))
            ui_log(L"External: HCN error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (hr == HCN_DELETE_NOT_FOUND) {
        /* Exact delete-path not-found (measured 0x80070490):
           already gone. */
        ui_log(L"External: owned network already deleted (not found).");
        return S_FALSE;
    }
    if (FAILED(hr)) {
        /* Best-effort semantics: report the original error; no
           cross-start retry obligation is created. The line names the
           adapter so the operator knows which NIC stays offline. */
        wchar_t adapter[HCN_PHYS_DESC_MAX];
        lookup_adapter_description(adapter_interface_guid, adapter,
                                   ARRAYSIZE(adapter));
        if (!adapter[0]) {
            guid_to_string(adapter_interface_guid, adapter,
                           ARRAYSIZE(adapter));
        }
        ui_log(L"External: owned delete failed (0x%08X); adapter \"%s\" stays "
               L"offline until the next App Sandbox start or manual "
               L"removal.", hr, adapter);
        return hr;
    }
    ui_log(L"External: owned network deleted.");
    return S_OK;
}

/* Delete all AppSandbox networks left over from a previous run. Fast - no enumeration.
   Call only at startup to clean up stale networks from a previous run; never
   from per-VM create paths, or you'll rip the network out from under any
   already-running VM that's attached to it.

   The historical fixed IDs (NAT, Internal, External) are deleted first.
   Per-NIC owned recognition: the hardware set
   S = { owned_id(G) : G in current non-null L0 hardware InterfaceGuids }
   is computed independently of any acquire. When the enumeration is
   usable (export present, call succeeds, document parses completely),
   each deduplicated enumerated ID is deleted by set membership (renamed
   network, NIC present) or through the strict owned-Name parse to a G
   PLUS actual-ID equality with owned_id(G) (NIC absent, Name intact).
   Otherwise the blind fallback attempts every owned_id(G) in S.
   Best-effort: failures are logged and independent IDs continue; no
   persistent retry state. Log deletion success only on actual success. */
void hcn_cleanup_stale_networks(void)
{
    PWSTR er = NULL;
    ULONGLONG started = GetTickCount64();
    int fixed_deleted = 0, set_deleted = 0, name_deleted = 0, blind_deleted = 0;
    int attempted = 0, query_count = 0, enum_count = 0;
    HcnAdapterInventory inv;
    HcnOwnedIdEntry *owned_set = NULL;
    size_t owned_count = 0;
    HRESULT inv_hr;
    GUID *ids = NULL;
    size_t id_count = 0;
    BOOL ids_valid = FALSE;
    GUID done_ids[64];   /* dedup of attempted/skipped IDs this sweep */
    size_t done_count = 0;

    if (!pfnDeleteNet) return;

    /* Historical fixed-ID deletions. */
    er = NULL;
    if (SUCCEEDED(pfnDeleteNet(&APPSANDBOX_NAT_GUID, &er))) fixed_deleted++;
    if (er) { hcn_free_string(er); er = NULL; }
    if (SUCCEEDED(pfnDeleteNet(&APPSANDBOX_INTERNAL_GUID, &er))) fixed_deleted++;
    if (er) { hcn_free_string(er); er = NULL; }
    if (SUCCEEDED(pfnDeleteNet(&APPSANDBOX_EXTERNAL_GUID, &er))) fixed_deleted++;
    if (er) { hcn_free_string(er); er = NULL; }

    /* S = { owned_id(G) : G in current non-null L0 hardware
       InterfaceGuids } - the same hardware-set definition the acquire
       context precomputes, but computed here independently. */
    ZeroMemory(&inv, sizeof(inv));
    inv_hr = build_adapter_inventory(&inv);
    if (SUCCEEDED(inv_hr)) {
        size_t i;
        owned_set = (HcnOwnedIdEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                 (inv.count ? inv.count : 1) *
                                                     sizeof(HcnOwnedIdEntry));
        if (owned_set) {
            for (i = 0; i < inv.count; i++) {
                const HcnAdapterEntry *e = &inv.entries[i];
                if (!e->is_hardware_eligible)
                    continue;
                if (IsEqualGUID(&e->interface_guid, &GUID_NULL))
                    continue;
                if (FAILED(hcn_external_owned_id(&e->interface_guid,
                                                 &owned_set[owned_count].owned_id))) {
                    ui_log(L"External: startup owned-ID derivation failed for "
                           L"one adapter; skipping its sweep entry.");
                    continue;
                }
                owned_set[owned_count].nic_guid = e->interface_guid;
                owned_count++;
            }
        } else {
            ui_log(L"External: startup sweep could not allocate the owned-ID "
                   L"set; per-NIC recognition is skipped this run.");
        }
    } else {
        /* Startup L0 build failure: historical deletions already ran; the
           enumeration route continues with an empty S; the blind loop
           cannot run. */
        ui_log(L"External: startup L0 inventory failed (0x%08X); per-NIC sweep "
               L"runs with an empty hardware set (Name route only).", inv_hr);
    }

    /* Enumeration usability: export present, call succeeds, and the result
       document parses completely. A failed
       call or incomplete document falls back to the blind loop. */
    if (pfnEnumNet && pfnOpenNet && pfnCloseNet && pfnQueryNetProps) {
        PWSTR enum_json = NULL;
        PWSTR enum_er = NULL;
        HRESULT hr = pfnEnumNet(NULL, &enum_json, &enum_er);
        if (enum_er) {
            hcn_free_string(enum_er);
            enum_er = NULL;
        }
        if (SUCCEEDED(hr) && enum_json) {
            hr = parse_hcn_network_id_array(enum_json, &ids, &id_count);
            hcn_free_string(enum_json);
            if (SUCCEEDED(hr)) {
                ids_valid = TRUE;
                enum_count = (int)id_count;
            } else {
                ui_log(L"External: startup enumeration document incomplete "
                       L"(0x%08X); using the blind per-NIC fallback.", hr);
            }
        } else {
            if (enum_json) hcn_free_string(enum_json);
            ui_log(L"External: startup enumeration unusable (0x%08X); using "
                   L"the blind per-NIC fallback.", hr);
        }
    } else {
        ui_log(L"External: HCN enumeration capability unavailable at startup; "
               L"using the blind per-NIC fallback.");
    }

    if (ids_valid) {
        /* Enumeration route: per deduplicated enumerated ID. */
        size_t i;
        for (i = 0; i < id_count; i++) {
            GUID id = ids[i];
            void *network = NULL;
            PWSTR json = NULL;
            HcnLookupKind k;
            HRESULT hr;
            size_t j;
            BOOL in_set = FALSE;
            BOOL already = FALSE;

            for (j = 0; j < done_count; j++) {
                if (IsEqualGUID(&done_ids[j], &id)) {
                    already = TRUE;
                    break;
                }
            }
            if (already)
                continue;
            if (done_count < ARRAYSIZE(done_ids))
                done_ids[done_count++] = id;

            for (j = 0; j < owned_count; j++) {
                if (IsEqualGUID(&owned_set[j].owned_id, &id)) {
                    in_set = TRUE;
                    break;
                }
            }
            if (in_set) {
                /* Renamed owned network, NIC present: delete by the
                   derived ID (set match). */
                HRESULT del_hr;
                attempted++;
                er = NULL;
                del_hr = pfnDeleteNet(&id, &er);
                if (SUCCEEDED(del_hr)) {
                    set_deleted++;
                    {
                        wchar_t q[64];
                        guid_to_string(&id, q, 64);
                        ui_log(L"External: startup deleted owned network %s "
                               L"(NIC present, derived-ID match).", q);
                    }
                } else if (del_hr == HCN_DELETE_NOT_FOUND) {
                    /* Already gone between enumerate and delete: the goal
                       state - counted silently like the blind route. */
                    set_deleted++;
                } else {
                    wchar_t q[64];
                    guid_to_string(&id, q, 64);
                    ui_log(L"External: startup owned-ID delete failed for %s "
                           L"(0x%08X); left for the next start or manual "
                           L"removal.", q, del_hr);
                }
                if (er) { hcn_free_string(er); er = NULL; }
                continue;
            }

            /* Not in S: strict owned-Name parse to a candidate G, then
               actual-ID equality with owned_id(G) before any delete.
               Name supplies G only; Name AND actual derived-ID equality
               are required. */
            k = hcn_open_network_exact(&id, &network, &hr);
            if (k == HCN_LOOKUP_ERROR) {
                wchar_t q[64];
                guid_to_string(&id, q, 64);
                ui_log(L"External: startup could not open enumerated network "
                       L"%s (0x%08X); skipped.", q, hr);
                continue;
            }
            if (k == HCN_LOOKUP_NOT_FOUND) {
                wchar_t q[64];
                guid_to_string(&id, q, 64);
                ui_log(L"External: startup enumerated network %s vanished "
                       L"before open; skipped (not an occupant).", q);
                continue;
            }

            query_count++;
            hr = hcn_query_network_properties(network, &json);
            if (network && pfnCloseNet) {
                /* The query handle is closed before deletion and on every
                   skip path (whether an open handle actually blocks
                   deletion is an unverified assumption, not a codebase
                   fact). */
                pfnCloseNet(network);
                network = NULL;
            }
            if (SUCCEEDED(hr)) {
                HcnNetworkProps props;
                hr = parse_hcn_network_properties(json, &props);
                if (SUCCEEDED(hr) && props.has_id &&
                    IsEqualGUID(&props.id, &id) &&
                    props.has_name && !props.name_truncated) {
                    GUID g, oid;
                    BOOL name_parsed = hcn_external_parse_owned_name(props.name, &g);
                    BOOL id_owned = FALSE;
                    if (name_parsed)
                        id_owned = SUCCEEDED(hcn_external_owned_id(&g, &oid)) &&
                                   IsEqualGUID(&oid, &props.id);
                    if (name_parsed && id_owned) {
                        /* Name parses to G and the actual ID equals
                           owned_id(G): delete the actual ID (NIC absent,
                           original Name intact). */
                        HRESULT del_hr;
                        attempted++;
                        er = NULL;
                        del_hr = pfnDeleteNet(&props.id, &er);
                        if (SUCCEEDED(del_hr)) {
                            name_deleted++;
                            {
                                wchar_t q[64];
                                guid_to_string(&props.id, q, 64);
                                ui_log(L"External: startup deleted owned "
                                       L"network %s (Name+actual-ID match, "
                                       L"NIC absent).", q);
                            }
                        } else if (del_hr == HCN_DELETE_NOT_FOUND) {
                            /* Already gone between query and delete: the
                               goal state - counted silently like the
                               blind route. */
                            name_deleted++;
                        } else {
                            wchar_t q[64];
                            guid_to_string(&props.id, q, 64);
                            ui_log(L"External: startup owned delete failed "
                                   L"for %s (0x%08X); left for the next "
                                   L"start or manual removal.", q, del_hr);
                        }
                        if (er) { hcn_free_string(er); er = NULL; }
                    } else if (name_parsed) {
                        /* Matching foreign Name: the Name
                           parses but the actual ID is not owned - never
                           deleted. */
                        wchar_t q[64];
                        guid_to_string(&id, q, 64);
                        ui_log(L"External: startup skipped matching Name "
                               L"with non-owned actual ID (%s).", q);
                    }
                    /* else: not an AppSandboxExternal Name at all (a
                       foreign network's own name) - an ordinary skip, no
                       log (the matching-Name case is the only one
                       logged). */
                } else if (SUCCEEDED(hr)) {
                    wchar_t q[64];
                    guid_to_string(&id, q, 64);
                    ui_log(L"External: startup query mismatch for %s; "
                           L"skipped.", q);
                } else {
                    wchar_t q[64];
                    guid_to_string(&id, q, 64);
                    ui_log(L"External: startup properties unreadable for %s "
                           L"(0x%08X); skipped.", q, hr);
                }
            } else {
                wchar_t q[64];
                guid_to_string(&id, q, 64);
                ui_log(L"External: startup properties query failed for %s "
                       L"(0x%08X); skipped.", q, hr);
            }
            hcn_free_string(json);
        }
        free(ids);
        ids = NULL;
    } else {
        /* Blind fallback: enumerate missing, call failed, or the
           document incomplete - for every G in S, attempt the derived ID.
           The coverage does not depend on the enumeration. The exact
           delete-path not-found (0x80070490) is the expected
           "nothing there" outcome for most NICs and is counted silently;
           every other failure is logged and the loop continues. */
        size_t j;
        for (j = 0; j < owned_count; j++) {
            GUID id = owned_set[j].owned_id;
            size_t d;
            BOOL already = FALSE;
            HRESULT del_hr;

            for (d = 0; d < done_count; d++) {
                if (IsEqualGUID(&done_ids[d], &id)) {
                    already = TRUE;
                    break;
                }
            }
            if (already)
                continue;
            if (done_count < ARRAYSIZE(done_ids))
                done_ids[done_count++] = id;

            attempted++;
            er = NULL;
            del_hr = pfnDeleteNet(&id, &er);
            if (SUCCEEDED(del_hr)) {
                blind_deleted++;
                {
                    wchar_t q[64];
                    guid_to_string(&id, q, 64);
                    ui_log(L"External: startup deleted owned network %s "
                           L"(blind per-NIC fallback).", q);
                }
            } else if (del_hr != HCN_DELETE_NOT_FOUND) {
                wchar_t q[64];
                guid_to_string(&id, q, 64);
                ui_log(L"External: startup blind delete failed for %s "
                       L"(0x%08X); left for the next start or manual "
                       L"removal.", q, del_hr);
            }
            if (er) { hcn_free_string(er); er = NULL; }
        }
    }

    adapter_inventory_free(&inv);
    if (owned_set) {
        HeapFree(GetProcessHeap(), 0, owned_set);
        owned_set = NULL;
    }

    /* Report sweep counts and elapsed time through the logs. */
    ui_log(L"External: startup sweep done in %lums: fixed=%d, set=%d, "
           L"name=%d, blind=%d, attempted=%d, enumerated=%d, queried=%d.",
           (unsigned long)(GetTickCount64() - started), fixed_deleted,
           set_deleted, name_deleted, blind_deleted, attempted, enum_count,
           query_count);
}
