/* hcn_internal.c - the Internal acquisition and census I/O TU.
 *
 * The acquire (A0/A1 for Auto, E0-E4 for Explicit), the E3 probe, the
 * request-driven census enumeration (topology + census builder + the
 * bounded probe layer + the projection delegation), the lock-inheriting
 * owned-ICS create, and the owned-delete shield. Classification,
 * selector resolution, reason formatting, name conversion, and census
 * projection live in hcn_internal_policy.c so the offline test binary
 * can exercise those rules without the acquire. The acquire's failure
 * exit is the single stale-ping emission site. */

#include "hcn_network.h"
#include "hcn_private.h"
#include "ui.h"

#include <objbase.h>
#include <stdio.h>
#include <string.h>

/* The measured name-keyed ALREADY_EXISTS from HcnCreateNetwork on an
   existing network: the A1 recovery branch's input. */
#ifndef HCN_E_NETWORK_ALREADY_EXISTS
#define HCN_E_NETWORK_ALREADY_EXISTS 0x803B0010
#endif

/* ---- A0/A1: the owned create ladder (the demoted create) ----
 *
 * The fixed-GUID exists-check+create region, inherited from the
 * upstream-synchronized create: A0's open and A1's create run as ONE
 * serial region under the shared network-create lock (a concurrent
 * second start must see the first's create, not race it), every return
 * path releasing. The ALREADY_EXISTS reopen arm stays as the recovery
 * for a cross-PROCESS creator the lock cannot exclude - in-process,
 * the lock makes the race it answers unreachable. */

/* The fixed-GUID identity arm over an OPEN network handle: query the
   document, parse, and run the shared tri-state. The arm's input
   contract is the FULL query side: only when the query runs, the call
   succeeds, and the document parses with a readable Name+Type can the
   verdict be MATCH or MISMATCH - anything less answers INDETERMINATE
   (today's existence-only semantics keep that shape owned). */
static HcnFixedIdentity fixed_guid_identity(void *network)
{
    HcnFixedIdentity ident = HCN_ID_INDETERMINATE;
    PWSTR json = NULL;
    HcnNetworkProps props;

    if (!pfnQueryNetProps)
        return HCN_ID_INDETERMINATE;
    if (FAILED(hcn_query_network_properties(network, &json)) || !json)
        return HCN_ID_INDETERMINATE;
    if (SUCCEEDED(parse_hcn_network_properties(json, &props)))
        ident = internal_fixed_identity(&props);
    hcn_free_string(json);
    return ident;
}

/* One open+identity+adjudication of the fixed GUID, shared by A0 and
   A1's reopen. */
typedef enum {
    FIXED_OPEN_ABSENT,      /* not found / open error: the caller decides */
    FIXED_OPEN_OWNED,       /* proven ours, or unreadably present */
    FIXED_OPEN_COLLISION    /* proven foreign: the actionable line is in the reason */
} FixedOpenOutcome;

static FixedOpenOutcome fixed_guid_open_and_adjudicate(wchar_t *reason,
                                                       size_t reason_cap)
{
    void *network = NULL;
    HRESULT hr = S_OK;
    HcnLookupKind kind;
    HcnFixedIdentity ident;

    kind = hcn_open_network_exact(&APPSANDBOX_INTERNAL_GUID, &network, &hr);
    if (kind != HCN_LOOKUP_FOUND) {
        /* NOT_FOUND / ERROR: existence unknown (A0 -> create; the A1
           reopen arm -> the create's original HRESULT). */
        return FIXED_OPEN_ABSENT;
    }

    ident = fixed_guid_identity(network);
    if (network && pfnCloseNet)
        pfnCloseNet(network);

    if (ident == HCN_ID_MISMATCH) {
        /* A real collision is proven: the actionable catalog line (the
           fixed network ID is occupied by a foreign object). */
        format_reason(HCN_IR_AUTO_COLLISION, HCN_RS_FULL, NULL, 0, 0, NULL,
                      reason, reason_cap);
        return FIXED_OPEN_COLLISION;
    }
    if (ident == HCN_ID_INDETERMINATE)
        ui_log(L"Internal: fixed network present with an unreadable "
               L"identity; reusing (existence-only).");
    return FIXED_OPEN_OWNED;
}

/* The owned create ladder. On success fills *out (network_id = the
   fixed GUID, flag TRUE) and returns S_OK. On failure returns the
   failing HRESULT with the reason buffer filled ONLY for the two
   alert-worthy branches (the proven collision and the name-squat arm)
   - the Auto failure policy is unchanged for everything else, and the
   dispatch sites key their extra alert on the non-empty reason. */
static HRESULT internal_create_owned_ics(HcnInternalNetworkRef *out,
                                         wchar_t *reason, size_t reason_cap)
{
    wchar_t settings[1024];
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    if (!g_hcn_dll || !pfnCreateNet)
        return E_NOT_VALID_STATE;

    out->network_id = APPSANDBOX_INTERNAL_GUID;
    out->switch_id = APPSANDBOX_INTERNAL_GUID;
    out->delete_network_on_last_release = TRUE;

    hcn_network_lock_acquire();

    /* A0: open the fixed GUID. A missing Open export, exact not-found,
       and any open error all mean "existence unknown" - the create
       decides (today's semantics). */
    switch (fixed_guid_open_and_adjudicate(reason, reason_cap)) {
    case FIXED_OPEN_OWNED:
        hcn_network_lock_release();
        return S_OK;
    case FIXED_OPEN_COLLISION:
        hcn_network_lock_release();
        return E_FAIL;         /* fail closed with the line */
    case FIXED_OPEN_ABSENT:
    default:
        break;                 /* fall through to A1's create */
    }

    /* A1: create with the current ICS document - unchanged JSON. */
    swprintf_s(settings, 1024,
        L"{"
        L"\"SchemaVersion\":{\"Major\":2,\"Minor\":0},"
        L"\"Name\":\"AppSandboxInternal\","
        L"\"Type\":\"ICS\""
        L"}");

    hr = pfnCreateNet(&APPSANDBOX_INTERNAL_GUID, settings, &network,
                      &error_record);
    if (error_record) {
        if (FAILED(hr))
            ui_log(L"HCN Internal error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (network && pfnCloseNet)
        pfnCloseNet(network);
    if (SUCCEEDED(hr)) {
        hcn_network_lock_release();
        return S_OK;            /* owned, created */
    }

    if (hr == HCN_E_NETWORK_ALREADY_EXISTS) {
        /* The name-keyed collision (measured). The reopen adjudicates:
           in-process the lock already excluded our own create, so this
           is a cross-PROCESS creator or a name-squatting VMMS switch. */
        if (!pfnOpenNet) {
            hcn_network_lock_release();
            return hr;          /* the create's original HRESULT, no line */
        }
        switch (fixed_guid_open_and_adjudicate(reason, reason_cap)) {
        case FIXED_OPEN_OWNED:
            hcn_network_lock_release();
            return S_OK;        /* owned via the reopen */
        case FIXED_OPEN_ABSENT:
            /* The reopen failed - INCLUDING exact not-found: the create
               was blocked by NAME (a user VMMS switch named
               AppSandboxInternal - measured-reachable), never by the ID.
               The name-squat line names the remedy (rename it, or start
               Explicit on that switch). */
            format_reason(HCN_IR_AUTO_NAME_SQUAT, HCN_RS_FULL, NULL, 0, 0,
                          NULL, reason, reason_cap);
            hcn_network_lock_release();
            return HCN_E_NETWORK_ALREADY_EXISTS;
        case FIXED_OPEN_COLLISION:
        default:
            /* The reopen proved the collision: the line is already in
               the buffer. */
            hcn_network_lock_release();
            return E_FAIL;
        }
    }

    /* Any other create failure: the raw-HRESULT arm (the Auto policy's
       unchanged downgrade path - the reason stays empty). */
    hcn_network_lock_release();
    return hr;
}

/* ---- E3: the single probe ---- */

/* ONE open, no retry (the New-VMSwitch registration gap is
   user-retryable by design), Type read ONCE from the open's query. A
   query that cannot run (missing export, call failure, parse failure)
   is the correlation-unavailable evidence - the deliberate asymmetry
   with A0: the target's identity is the whole question here. */
static void hcn_internal_probe(const GUID *wmi_switch_guid,
                               HcnProbeEvidence *evidence)
{
    void *network = NULL;
    HRESULT hr = S_OK;
    HcnLookupKind kind;

    ZeroMemory(evidence, sizeof(*evidence));

    kind = hcn_open_network_exact(wmi_switch_guid, &network, &hr);
    if (kind == HCN_LOOKUP_NOT_FOUND) {
        evidence->kind = HCN_PROBE_OPEN_NOT_FOUND;
        evidence->hr = hr;
        return;
    }
    if (kind != HCN_LOOKUP_FOUND) {
        evidence->kind = HCN_PROBE_OPEN_ERROR;
        evidence->hr = hr;
        return;
    }

    if (!pfnQueryNetProps) {
        evidence->kind = HCN_PROBE_QUERY_FAILED;
        evidence->hr = E_NOT_VALID_STATE;
    } else {
        PWSTR json = NULL;
        hr = hcn_query_network_properties(network, &json);
        if (FAILED(hr) || !json) {
            evidence->kind = HCN_PROBE_QUERY_FAILED;
            evidence->hr = FAILED(hr) ? hr : E_FAIL;
        } else {
            HRESULT phr = parse_hcn_network_properties(json, &evidence->props);
            if (FAILED(phr)) {
                evidence->kind = HCN_PROBE_QUERY_FAILED;
                evidence->hr = phr;
            } else {
                evidence->kind = HCN_PROBE_PARSED;
            }
            hcn_free_string(json);
        }
    }

    if (network && pfnCloseNet)
        pfnCloseNet(network);
}

/* ---- The request-driven census enumeration ---- */

/* Probe one entry in place: the E3 open + the E4 verdict, writing the
   entry's verdict and, for a rejection, its CONCISE clause (probed
   entries get clauses like every other non-selectable entry). The
   DEFER context is the resolution's own answer for this name. */
static void census_probe_entry(HcnInternalSwitchEntry *e, BOOL defer)
{
    HcnProbeEvidence evidence;
    HcnInternalClassifyVerdict v;

    hcn_internal_probe(&e->switch_id, &evidence);
    hcn_internal_classify_props(&e->switch_id, &evidence, defer, &v);

    e->verdict = v.verdict;
    if (evidence.kind == HCN_PROBE_PARSED && evidence.props.has_type) {
        wcsncpy_s(e->type, HCN_MAX_TYPE_CHARS, evidence.props.type, _TRUNCATE);
        e->type_truncated = evidence.props.type_truncated;
    }
    if (v.verdict == HCN_CAND_REJECTED) {
        e->reason_code = v.reason_code;
        format_reason(v.reason_code, HCN_RS_CONCISE, e->name, v.hr, 0,
                      v.type_display, e->reason_text,
                      ARRAYSIZE(e->reason_text));
    }
}

/* Build the UNAVAILABLE census shape (the free-able early-exit /
   return contract: out is ALWAYS a valid, free-able census, and a
   cancelled build never returns S_OK). */
static void census_set_unavailable(HcnInternalSwitchCensus *c)
{
    hcn_internal_switch_census_free(c);
    ZeroMemory(c, sizeof(*c));
    c->state = HCN_CENSUS_UNAVAILABLE;
    c->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
}

/* The census state can authorize name-level absence only when this same
   WMI snapshot proved the host-system class visible and the effective
   token can see the unfiltered Hyper-V provider view. In particular,
   ComputerSystem visibility alone cannot make an empty switch/port
   result authoritative under UAC filtering. Kept as one production gate
   so the UI census, Explicit acquire, and offline fixtures share it. */
BOOL hcn_internal_census_authority_gate(const WmiTopology *topology,
                                        BOOL token_unfiltered,
                                        HcnInternalSwitchCensus *out)
{
    if (topology && topology->host_system_visible && token_unfiltered)
        return TRUE;

    if (out)
        census_set_unavailable(out);
    return FALSE;
}

typedef enum {
    INTERNAL_CENSUS_DEADLINE_SHARED,
    INTERNAL_CENSUS_DEADLINE_PER_PHASE
} InternalCensusDeadlineMode;

typedef enum {
    INTERNAL_CENSUS_BUILD_SESSION_FAILED,
    INTERNAL_CENSUS_BUILD_TOPOLOGY_FAILED,
    INTERNAL_CENSUS_BUILD_AUTHORITY_FAILED,
    INTERNAL_CENSUS_BUILD_ENUMERATION_FAILED,
    INTERNAL_CENSUS_BUILD_COMPLETE
} InternalCensusBuildStage;

typedef struct {
    InternalCensusBuildStage stage;
    BOOL host_system_visible;
    BOOL token_unfiltered;
} InternalCensusBuildInfo;

/* Build one fresh Internal census for either the display worker or an
   Explicit start. The UI shares one deadline across session setup,
   topology, and census enumeration; the start gives topology and census
   separate full budgets. Both paths use their own WMI session and pass
   the UI cancellation event through the shared query layer when present. */
static HRESULT build_fresh_internal_census(
    HANDLE cancel_event, DWORD timeout_ms,
    InternalCensusDeadlineMode deadline_mode,
    HcnInternalSwitchCensus *out, InternalCensusBuildInfo *info)
{
    WmiSession session;
    WmiTopology topology;
    TopologyResultKind kind;
    ULONGLONG deadline = 0;
    HRESULT hr;

    ZeroMemory(info, sizeof(*info));
    info->stage = INTERNAL_CENSUS_BUILD_SESSION_FAILED;
    ZeroMemory(out, sizeof(*out));
    out->state = HCN_CENSUS_UNAVAILABLE;
    out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;

    /* Display's timeout includes COM/WMI session establishment. The
       Explicit start's first phase begins after the session is ready. */
    if (deadline_mode == INTERNAL_CENSUS_DEADLINE_SHARED)
        deadline = GetTickCount64() + timeout_ms;

    hr = wmi_session_open(&session);
    if (FAILED(hr))
        return hr;
    session.cancel_event = cancel_event;

    if (deadline_mode == INTERNAL_CENSUS_DEADLINE_PER_PHASE)
        deadline = GetTickCount64() + timeout_ms;

    hr = wmi_build_topology(&session, deadline, &topology, &kind);
    if (FAILED(hr)) {
        /* The topology builder disposes partial output on failure; only
           a successful topology becomes this helper's cleanup duty. */
        info->stage = INTERNAL_CENSUS_BUILD_TOPOLOGY_FAILED;
        goto close_session;
    }

    info->host_system_visible = topology.host_system_visible;
    info->token_unfiltered = hcn_coverage_token_unfiltered();
    if (!hcn_internal_census_authority_gate(&topology,
                                            info->token_unfiltered, out)) {
        info->stage = INTERNAL_CENSUS_BUILD_AUTHORITY_FAILED;
        hr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        wmi_topology_free(&topology);
        goto close_session;
    }

    /* Explicit starts retain their second full budget; display uses the
       same absolute deadline that was passed to the topology builder. */
    if (deadline_mode == INTERNAL_CENSUS_DEADLINE_PER_PHASE)
        deadline = GetTickCount64() + timeout_ms;

    hr = wmi_build_internal_census(&session, &topology, deadline, out);
    info->stage = FAILED(hr) ? INTERNAL_CENSUS_BUILD_ENUMERATION_FAILED
                             : INTERNAL_CENSUS_BUILD_COMPLETE;
    wmi_topology_free(&topology);

close_session:
    wmi_session_close(&session);
    if (FAILED(hr))
        census_set_unavailable(out);
    return hr;
}

HRESULT hcn_enum_internal_switches(const HcnInternalCensusRequest *req,
                                   HcnInternalSwitchCensus *out)
{
    InternalCensusBuildInfo build_info;
    HRESULT hr;
    size_t probed = 0, i, j;

    /* The display tolerance is one budget for the entire WMI census;
       the acquire path below intentionally grants each build phase its
       own deadline and always starts from its own fresh session. */
    hr = build_fresh_internal_census(req->cancel_event,
                                     WMI_CENSUS_DEADLINE_MS,
                                     INTERNAL_CENSUS_DEADLINE_SHARED,
                                     out, &build_info);
    if (FAILED(hr)) {
        switch (build_info.stage) {
        case INTERNAL_CENSUS_BUILD_SESSION_FAILED:
            ui_log(L"Internal: census WMI session failed (0x%08X).", hr);
            break;
        case INTERNAL_CENSUS_BUILD_TOPOLOGY_FAILED:
            ui_log(L"Internal: census topology build failed (0x%08X).", hr);
            break;
        case INTERNAL_CENSUS_BUILD_AUTHORITY_FAILED:
            ui_log(L"Internal: census view unavailable (host-system %s, "
                   L"Hyper-V token %s); an empty result is not authoritative.",
                   build_info.host_system_visible ? L"visible" : L"not visible",
                   build_info.token_unfiltered ? L"unfiltered"
                                               : L"filtered or unknown");
            break;
        case INTERNAL_CENSUS_BUILD_ENUMERATION_FAILED:
        default:
            ui_log(L"Internal: census build failed (0x%08X).", hr);
            break;
        }
        return hr;
    }

    /* The cancelled build's return contract: distinguishable, never
       S_OK, out a free-able UNAVAILABLE census. */
    if (req->cancel_event &&
        WaitForSingleObject((HANDLE)req->cancel_event, 0) == WAIT_OBJECT_0) {
        census_set_unavailable(out);
        return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    }

    /* The bounded probe layer: priority names first (each resolved
       through the production resolver - its PROCEED answer names the
       target and carries the DEFER context), then INTERNAL-class fills
       in census order, skipping already-probed names. Unprobed entries
       stay UNKNOWN (the classifier sets that default explicitly). */
    for (i = 0; i < req->priority_count && probed < req->probe_budget; i++) {
        const wchar_t *name = req->priority_names[i];
        HcnInternalResolution res;

        if (!name || !name[0])
            continue;
        /* Skip repeated names (a miss is a miss; a hit is probed once). */
        for (j = 0; j < i; j++) {
            if (req->priority_names[j] && req->priority_names[j][0] &&
                _wcsicmp(req->priority_names[j], name) == 0)
                break;
        }
        if (j < i)
            continue;

        if (req->cancel_event &&
            WaitForSingleObject((HANDLE)req->cancel_event, 0) == WAIT_OBJECT_0) {
            census_set_unavailable(out);
            return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        }

        hcn_internal_resolve_selector(out, name, &res);
        if (res.outcome != HCN_IRES_PROCEED)
            continue;   /* a REJECT carries no probe target */
        {
            HcnInternalSwitchEntry *e = NULL;
            for (j = 0; j < out->count; j++) {
                if (IsEqualGUID(&out->entries[j].switch_id, &res.probe_guid)) {
                    e = &out->entries[j];
                    break;
                }
            }
            if (!e || e->verdict != HCN_CAND_UNKNOWN)
                continue;   /* already probed or not present */
            census_probe_entry(e, res.defer_adjudication);
            probed++;
        }
    }
    for (i = 0; i < out->count && probed < req->probe_budget; i++) {
        HcnInternalSwitchEntry *e = &out->entries[i];

        if (e->sw_class != HCN_SW_INTERNAL || e->verdict != HCN_CAND_UNKNOWN)
            continue;

        if (req->cancel_event &&
            WaitForSingleObject((HANDLE)req->cancel_event, 0) == WAIT_OBJECT_0) {
            census_set_unavailable(out);
            return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        }

        census_probe_entry(e, FALSE);
        probed++;
    }

    /* The projection is delegated to the pure helper (in-core display
       policy: the web never owns catalog text and never re-derives a
       reason). It runs AFTER probing so the probed verdicts and type
       notes are in hand. */
    if (req->project_for_ui)
        internal_project_census(out);

    /* Last-probe race: a cancel may arrive while the final synchronous
       HcnOpenNetwork/Query completes. Check after projection as well, so
       no completed probe can make a cancelled build publish S_OK or a
       partial census. */
    if (req->cancel_event &&
        WaitForSingleObject((HANDLE)req->cancel_event, 0) == WAIT_OBJECT_0) {
        census_set_unavailable(out);
        return HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
    }

    return S_OK;
}

/* ---- The acquire ---- */

HRESULT hcn_acquire_internal_network(const wchar_t *selector,
                                     HcnInternalNetworkRef *out,
                                     wchar_t *reason, size_t reason_cap)
{
    InternalCensusBuildInfo build_info;
    HcnInternalSwitchCensus census;
    HcnInternalResolution res;
    HcnInternalClassifyVerdict verdict;
    HcnProbeEvidence evidence;
    HRESULT hr;

    if (out)
        ZeroMemory(out, sizeof(*out));
    if (reason && reason_cap)
        reason[0] = L'\0';

    if (!selector || !selector[0]) {
        /* Auto: the fixed-GUID ladder, unchanged semantics (the two
           alert-worthy branches fill the reason; everything else keeps
           today's downgrade policy). Auto initializes no COM apartment
           and does no WMI work; a caller may still run this synchronous
           acquire on its ordinary helper thread. */
        return internal_create_owned_ics(out, reason, reason_cap);
    }

    /* Explicit: E0 (a FRESH census on this worker - never the UI
       thread, never the UI cache) -> resolve -> probe -> adjudicate. */
    hr = build_fresh_internal_census(NULL, WMI_DEFAULT_DEADLINE_MS,
                                     INTERNAL_CENSUS_DEADLINE_PER_PHASE,
                                     &census, &build_info);
    if (FAILED(hr) &&
        build_info.stage == INTERNAL_CENSUS_BUILD_SESSION_FAILED) {
        ui_log(L"Internal: acquire WMI session failed (0x%08X).", hr);
        format_reason(HCN_IR_INVENTORY_UNAVAILABLE, HCN_RS_FULL, selector, 0,
                      0, NULL, reason, reason_cap);
        hcn_notify_census_stale();
        return hr;
    }
    if (FAILED(hr)) {
        if (build_info.stage == INTERNAL_CENSUS_BUILD_AUTHORITY_FAILED) {
            ui_log(L"Internal: acquire census view unavailable (host-system %s, "
                   L"Hyper-V token %s); explicit resolution is refused.",
                   build_info.host_system_visible ? L"visible" : L"not visible",
                   build_info.token_unfiltered ? L"unfiltered"
                                               : L"filtered or unknown");
        }
        ui_log(L"Internal: acquire census build failed (0x%08X).", hr);
    }

    /* E0/E2 through the production resolver; the probe runs only on
       PROCEED (a REJECT provably happens BEFORE any HCN call). */
    hcn_internal_resolve_selector(&census, selector, &res);
    hcn_internal_switch_census_free(&census);

    if (res.outcome == HCN_IRES_REJECT) {
        hr = FAILED(res.hr) ? res.hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        format_reason(res.reason_code, HCN_RS_FULL, selector, res.hr, res.n,
                      NULL, reason, reason_cap);
        hcn_notify_census_stale();
        return hr;
    }

    /* E3: the single open. E4: the verdict (the DEFER context is the
       resolver's own answer for this selector). */
    hcn_internal_probe(&res.probe_guid, &evidence);
    hcn_internal_classify_props(&res.probe_guid, &evidence,
                                res.defer_adjudication, &verdict);

    if (verdict.verdict == HCN_CAND_REJECTED) {
        hr = FAILED(verdict.hr) ? verdict.hr
                                : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        format_reason(verdict.reason_code, HCN_RS_FULL, selector, verdict.hr,
                      0, verdict.type_display, reason, reason_cap);
        hcn_notify_census_stale();
        return hr;
    }

    /* Owned (the defensive E4 step-1 shape) or borrowed: publish. */
    out->network_id = verdict.network_id;
    out->switch_id = res.probe_guid;
    out->delete_network_on_last_release =
        (verdict.verdict == HCN_CAND_OWNED) ? TRUE : FALSE;
    return S_OK;
}

/* ---- The owned-delete shield ---- */

HRESULT hcn_delete_owned_internal_network(const GUID *network_id)
{
    PWSTR error_record = NULL;
    BOOL delete_attempted = FALSE;
    HRESULT hr = hcn_delete_network_if_owned(network_id,
                                             &APPSANDBOX_INTERNAL_GUID,
                                             &delete_attempted,
                                             &error_record);

    if (!delete_attempted && hr == S_FALSE) {
        ui_log(L"Internal: owned-delete shield refused a non-owned network ID.");
        return S_FALSE;
    }
    if (!delete_attempted && hr == E_NOT_VALID_STATE) {
        ui_log(L"Internal: owned-delete shield: HcnDeleteNetwork export is "
               L"unavailable; no delete attempted.");
        return hr;
    }
    if (error_record) {
        if (FAILED(hr))
            ui_log(L"Internal: HCN delete error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (hr == HCN_DELETE_NOT_FOUND) {
        ui_log(L"Internal: owned network already deleted (not found).");
        return S_FALSE;
    }
    if (FAILED(hr)) {
        ui_log(L"Internal: owned network delete failed (0x%08X).", hr);
        return hr;
    }
    ui_log(L"Internal: owned network deleted.");
    return S_OK;
}
