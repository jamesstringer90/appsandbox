/* hcn_internal_policy.c - the pure Internal vSwitch policy TU.
 *
 * The parse-TU precedent: E1's precedence ladder over RAW records, the
 * product-port skip, the tri-state-aware attribution helper, the pure
 * E0/E2 selector resolver, E3/E4's verdict over probe evidence, the
 * fixed-identity tri-state, the reason formatter, the shared BSTR-to-RAW
 * switch/iport name conversion, and the projection. No HCN or WMI I/O lives here
 * (the OLE string allocator behind SysStringLen is not I/O and is the
 * conversion's one system dependency) - which is what lets the offline
 * test binary link this TU without the acquire.
 *
 * Decision tables and the reason catalog are specified by the design
 * doc; the code keeps to them mechanically. */

#include "hcn_network.h"
#include "hcn_private.h"

#include <stdio.h>
#include <string.h>
#include <oleauto.h>

/* The two product network GUIDs as private comparison copies compiled
   from the single-source initializers: this TU links into the offline
   test binary, which does not link hcn_network.c. */
static const GUID s_product_nat_guid = APPSANDBOX_NAT_GUID_INIT;
static const GUID s_product_internal_guid = APPSANDBOX_INTERNAL_GUID_INIT;

/* ------------------------------------------------------------------ */
/* The fixed-identity tri-state                                        */
/* ------------------------------------------------------------------ */

/* MATCH requires a readable, untruncated Name+Type that says exactly
   AppSandboxInternal + ICS; MISMATCH requires readable Name+Type that
   provably says otherwise; INDETERMINATE is everything weaker - an
   unreadable identity is a data branch, never an assert. */
HcnFixedIdentity internal_fixed_identity(const HcnNetworkProps *props)
{
    if (!props->has_name || props->name_truncated)
        return HCN_ID_INDETERMINATE;
    if (!props->has_type || props->type_truncated)
        return HCN_ID_INDETERMINATE;
    if (wcscmp(props->name, L"AppSandboxInternal") == 0 &&
        wcscmp(props->type, L"ICS") == 0)
        return HCN_ID_MATCH;
    return HCN_ID_MISMATCH;
}

/* ------------------------------------------------------------------ */
/* E3/E4: the probe verdict                                            */
/* ------------------------------------------------------------------ */

/* Type membership in the joinable set. */
static BOOL type_is_joinable(const wchar_t *type)
{
    return wcscmp(type, L"Internal") == 0 || wcscmp(type, L"ICS") == 0;
}

/* Fill the type-specific line's <T>: the raw Type when readable,
   "(missing)" or "(truncated)" otherwise. */
static void fill_type_display(wchar_t *out, size_t cap,
                              const HcnNetworkProps *props)
{
    if (!props->has_type)
        wcsncpy_s(out, cap, L"(missing)", _TRUNCATE);
    else if (props->type_truncated)
        wcsncpy_s(out, cap, L"(truncated)", _TRUNCATE);
    else
        wcsncpy_s(out, cap, props->type, _TRUNCATE);
}

void hcn_internal_classify_props(const GUID *wmi_switch_guid,
                                 const HcnProbeEvidence *evidence,
                                 BOOL defer_adjudication,
                                 HcnInternalClassifyVerdict *out)
{
    const HcnNetworkProps *props = &evidence->props;

    ZeroMemory(out, sizeof(*out));

    /* E3's correlation branches first - the target's identity is the
       whole question on this path. */
    switch (evidence->kind) {
    case HCN_PROBE_OPEN_NOT_FOUND:
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = HCN_IR_NO_HOST_NETWORK;
        out->hr = evidence->hr;
        return;
    case HCN_PROBE_QUERY_FAILED:
    case HCN_PROBE_OPEN_ERROR:
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = HCN_IR_CORRELATION_UNAVAILABLE;
        out->hr = evidence->hr;
        return;
    case HCN_PROBE_PARSED:
        break;
    default:
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = HCN_IR_CORRELATION_UNAVAILABLE;
        out->hr = E_FAIL;
        return;
    }

    /* E3's identity checks on the parsed document: the ID must equal
       the WMI switch GUID; an absent SwitchGuid is OK, a present one
       must agree. A violation is a correlation failure (the measured
       shape never produces it - defensive, fail closed). */
    if (!props->has_id || !IsEqualGUID(&props->id, wmi_switch_guid)) {
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = HCN_IR_CORRELATION_UNAVAILABLE;
        out->hr = evidence->hr;
        return;
    }
    if (props->has_switch_guid && !IsEqualGUID(&props->switch_guid,
                                               wmi_switch_guid)) {
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = HCN_IR_CORRELATION_UNAVAILABLE;
        out->hr = evidence->hr;
        return;
    }

    /* E4's ordered sequence. */
    if (IsEqualGUID(&props->id, &s_product_internal_guid)) {
        HcnFixedIdentity ident = internal_fixed_identity(props);
        if (ident == HCN_ID_MATCH) {
            out->verdict = HCN_CAND_OWNED;
            out->network_id = props->id;
            return;
        }
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = (ident == HCN_ID_MISMATCH)
                               ? HCN_IR_FIXED_IDENTITY_CONFLICT
                               : HCN_IR_UNREADABLE_IDENTITY;
        return;
    }

    if (IsEqualGUID(&props->id, &s_product_nat_guid)) {
        out->verdict = HCN_CAND_REJECTED;
        out->reason_code = HCN_IR_NAT;
        return;
    }

    /* Owned-External identity: pure parse, no L0 inventory dependency. */
    if (props->has_name && !props->name_truncated) {
        GUID nic_guid;
        if (hcn_external_parse_owned_name(props->name, &nic_guid)) {
            GUID owned_id;
            if (SUCCEEDED(hcn_external_owned_id(&nic_guid, &owned_id)) &&
                IsEqualGUID(&owned_id, &props->id)) {
                out->verdict = HCN_CAND_REJECTED;
                out->reason_code = HCN_IR_OWNED_EXTERNAL;
                return;
            }
        }
    }

    /* Step 4: Type not in {Internal, ICS}. In the DEFER context a
       Type of Private answers with E2's no-host-adapter line (a private
       switch HAS no host adapter - the sentence stays accurate); every
       other non-joinable type takes the type-specific line. */
    if (!props->has_type || props->type_truncated ||
        !type_is_joinable(props->type)) {
        out->verdict = HCN_CAND_REJECTED;
        if (defer_adjudication && props->has_type && !props->type_truncated &&
            wcscmp(props->type, L"Private") == 0)
            out->reason_code = HCN_IR_NOT_INTERNAL;
        else {
            out->reason_code = HCN_IR_UNSUPPORTED_TYPE;
            fill_type_display(out->type_display, ARRAYSIZE(out->type_display),
                              props);
        }
        return;
    }

    out->verdict = HCN_CAND_BORROWED;
    out->network_id = props->id;
}

/* ------------------------------------------------------------------ */
/* E1: the ladder over RAW records                                     */
/* ------------------------------------------------------------------ */

/* The product-port skip: an iport whose WMI Name parses as a GUID equal
   to one of the product's two fixed network GUIDs is one of the
   product's own HNS network ports (measured for both families: an HNS
   network port's Name == its network's ID, and a real switch iport's
   Name is its own port GUID - never a network ID - so the skip cannot
   false-positive). Port-level exemption ONLY: the port contributes
   nothing, and the exemption never licenses inventing a complete
   census (a product port's walk still records walk-LEVEL
   UNAVAILABLE when it reaches a foreign GUID). */
static BOOL iport_is_product_port(const HcnRawIportResult *ip)
{
    if (!ip->has_name_id)
        return FALSE;
    return IsEqualGUID(&ip->iport_name_id, &s_product_internal_guid) ||
           IsEqualGUID(&ip->iport_name_id, &s_product_nat_guid);
}

static const HcnRawSwitchResult *find_switch_by_id(
    const HcnRawSwitchResult *switches, size_t count, const GUID *id)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (switches[i].has_id && IsEqualGUID(&switches[i].id, id))
            return &switches[i];
    return NULL;
}

/* E2's match predicate core: the label is attribution-safe and equals
   the switch's name case-insensitively. Unusable and unreadable names
   never match (one predicate everywhere). */
static BOOL label_matches_switch(const wchar_t *label,
                                 const HcnRawSwitchResult *sw)
{
    if (sw->name_unusable || sw->name_unreadable)
        return FALSE;
    return _wcsicmp(label, sw->name) == 0;
}

/* The per-switch working state: the E1 ladder's inputs accumulated
   from the iport census. */
typedef struct {
    BOOL walk_ok;        /* >=1 WALK_OK iport chain reached this switch (fusion) */
    BOOL failure_signal; /* >=1 attributed iport failure signal */
} SwitchSignals;

void hcn_internal_classify_switches(const HcnRawSwitchResult *switches,
                                    size_t switch_count,
                                    const HcnRawIportResult *iports,
                                    size_t iport_count,
                                    HcnInternalSwitchCensus *out)
{
    SwitchSignals *signals = NULL;
    size_t i, j;
    size_t internal_count = 0;
    BOOL walk_level_unavailable = FALSE;

    ZeroMemory(out, sizeof(*out));

    if (switch_count > 0) {
        signals = (SwitchSignals *)HeapAlloc(GetProcessHeap(),
                                             HEAP_ZERO_MEMORY,
                                             switch_count * sizeof(*signals));
        if (!signals) {
            out->state = HCN_CENSUS_UNAVAILABLE;
            out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
            return;
        }
    }

    /* Pass 1 over the iports: the absent-GUID walk-level rule, the
       product-port skip, the tri-state-aware attribution helper, and
       fusion credit. */
    for (i = 0; i < iport_count; i++) {
        const HcnRawIportResult *ip = &iports[i];
        size_t sw_idx;

        if (ip->walk_result == WMI_WALK_OK) {
            /* The 4th hop returned a real Msvm_VirtualEthernetSwitch:
               the absent-GUID rule runs BEFORE the product-port skip -
               the skip exempts the port's CONTRIBUTIONS, it does not
               license inventing a complete census. The inconsistent
               iport (a mid-build race) is omitted entirely. */
            const HcnRawSwitchResult *reached =
                find_switch_by_id(switches, switch_count, &ip->reached_switch_id);
            if (!reached) {
                walk_level_unavailable = TRUE;
                continue;
            }
            if (iport_is_product_port(ip))
                continue;   /* no credit, no signal, no flag */
            sw_idx = (size_t)(reached - switches);
            signals[sw_idx].walk_ok = TRUE;
            continue;
        }

        if (iport_is_product_port(ip))
            continue;   /* a failed product port: omitted entirely */

        /* A failed walk. The attribution helper's tri-state: a readable
           attribution-safe label either matches (failure signal to every
           matched switch + the aggregate flag) or does not (NOT_CONNECTED
           omits - the owner's associations are PROVEN absent; WALK_ERROR
           sets the unknown-owner flag - the owner could be a live renamed
           switch whose association query independently failed). An
           unreadable label sets both flags whatever the failure kind. A
           readable-but-unusable label (truncated prefix class) is
           readable NO-match: never a match, never unreadable. */
        if (ip->name_unreadable) {
            out->unattributed_host_port = TRUE;
            out->has_unknown_owner_failure = TRUE;
            continue;
        }
        if (ip->name_unusable_for_attribution) {
            if (ip->walk_result == WMI_WALK_ERROR) {
                out->unattributed_host_port = TRUE;
                out->has_unknown_owner_failure = TRUE;
            }
            /* NOT_CONNECTED: omit entirely. */
            continue;
        }

        /* Readable, attribution-safe label: match against every switch
           (never first-wins - a multi-match must not erase duplicate
           uncertainty). */
        {
            BOOL matched = FALSE;
            for (j = 0; j < switch_count; j++) {
                if (label_matches_switch(ip->name, &switches[j])) {
                    signals[j].failure_signal = TRUE;
                    matched = TRUE;
                }
            }
            if (matched) {
                out->unattributed_host_port = TRUE;
                continue;
            }
            if (ip->walk_result == WMI_WALK_ERROR) {
                out->unattributed_host_port = TRUE;
                out->has_unknown_owner_failure = TRUE;
            }
            /* NOT_CONNECTED, no match: omit. */
        }
    }

    /* Pass 2: the per-switch ladder and the entries. */
    if (switch_count > 0) {
        out->entries = (HcnInternalSwitchEntry *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            switch_count * sizeof(HcnInternalSwitchEntry));
        if (!out->entries) {
            HeapFree(GetProcessHeap(), 0, signals);
            out->state = HCN_CENSUS_UNAVAILABLE;
            out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
            return;
        }
        out->count = switch_count;
    }

    for (i = 0; i < switch_count; i++) {
        HcnInternalSwitchEntry *e = &out->entries[i];
        const HcnRawSwitchResult *sw = &switches[i];

        e->switch_id = sw->id;
        /* Zeroed storage reads verdict as the enum's zero value (OWNED),
           so the UNKNOWN default is set explicitly: an unprobed or
           probe-failed entry is UNKNOWN, never a fabricated verdict. */
        e->verdict = HCN_CAND_UNKNOWN;
        e->name_unusable = sw->name_unusable;
        e->name_unreadable = sw->name_unreadable;
        /* Verbatim copy: the payload placeholder rule (an unusable row's
           AND an unreadable row's name) belongs to the projection; the
           flags gate every comparison in the meantime. */
        wcsncpy_s(e->name, INTERNAL_SWITCH_CAP, sw->name, _TRUNCATE);

        if (sw->bound_external_ports > 0)
            e->sw_class = HCN_SW_EXTERNAL;      /* terminal: bound ports */
        else if (signals[i].walk_ok)
            e->sw_class = HCN_SW_INTERNAL;      /* fusion: a successful chain wins,
                                                   later failures never demote */
        else if (signals[i].failure_signal)
            e->sw_class = HCN_SW_UNCLASSIFIED;
        else
            e->sw_class = HCN_SW_PRIVATE;       /* zero evidence */

        if (e->sw_class == HCN_SW_INTERNAL)
            internal_count++;
    }

    HeapFree(GetProcessHeap(), 0, signals);

    /* List-level escalation: an unreadable name on an INTERNAL- or
       UNCLASSIFIED-class entry - scoping requires a readable name, and
       without one the entry could be the target or its second match.
       On EXTERNAL/PRIVATE the entry is merely not offered; the state
       stays authoritative and E0's gate handles the resolution-time
       question. */
    for (i = 0; i < out->count; i++) {
        if (out->entries[i].name_unreadable &&
            (out->entries[i].sw_class == HCN_SW_INTERNAL ||
             out->entries[i].sw_class == HCN_SW_UNCLASSIFIED)) {
            walk_level_unavailable = TRUE;
            break;
        }
    }

    if (walk_level_unavailable) {
        out->state = HCN_CENSUS_UNAVAILABLE;
        out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
        return;
    }

    out->state = (internal_count == 0) ? HCN_CENSUS_EMPTY : HCN_CENSUS_OK;
}

/* ------------------------------------------------------------------ */
/* The pure E0/E2 resolver                                             */
/* ------------------------------------------------------------------ */

/* Case-insensitive selector-vs-entry match: the ONE predicate E2, the
   duplicate grouping, and the UI's stored-name matching all share.
   Unusable and unreadable names never match. */
static BOOL entry_matches_selector(const HcnInternalSwitchEntry *e,
                                   const wchar_t *selector)
{
    if (e->name_unusable || e->name_unreadable)
        return FALSE;
    return _wcsicmp(e->name, selector) == 0;
}

void hcn_internal_resolve_selector(const HcnInternalSwitchCensus *census,
                                   const wchar_t *selector,
                                   HcnInternalResolution *out)
{
    size_t i;
    size_t internal_matches = 0, private_matches = 0, external_matches = 0;
    size_t unclassified_matches = 0, all_matches = 0;
    size_t internal_count = 0;
    const HcnInternalSwitchEntry *internal_match = NULL;
    const HcnInternalSwitchEntry *private_match = NULL;
    BOOL has_unreadable_private = FALSE;

    ZeroMemory(out, sizeof(*out));

    /* Layer 1: the census state. Only the state REFUSAL bypasses the
       name layers; EMPTY is a count summary (the same enumeration can
       carry PRIVATE/EXTERNAL rows and the evidence flags), so it runs
       E0/E2 exactly like OK. */
    if (census->state != HCN_CENSUS_OK && census->state != HCN_CENSUS_EMPTY) {
        out->outcome = HCN_IRES_REJECT;
        out->reason_code = census->reason_code;
        out->hr = 0;
        for (i = 0; i < census->count; i++)
            if (census->entries[i].sw_class == HCN_SW_INTERNAL)
                internal_count++;
        out->n = internal_count;
        return;
    }

    /* The match census, in one pass. */
    for (i = 0; i < census->count; i++) {
        const HcnInternalSwitchEntry *e = &census->entries[i];
        if (e->sw_class == HCN_SW_INTERNAL)
            internal_count++;
        if (e->sw_class == HCN_SW_PRIVATE && e->name_unreadable)
            has_unreadable_private = TRUE;
        if (!entry_matches_selector(e, selector))
            continue;
        all_matches++;
        switch (e->sw_class) {
        case HCN_SW_INTERNAL:
            internal_matches++;
            internal_match = e;
            break;
        case HCN_SW_PRIVATE:
            private_matches++;
            private_match = e;
            break;
        case HCN_SW_EXTERNAL:
            external_matches++;
            break;
        case HCN_SW_UNCLASSIFIED:
        default:
            unclassified_matches++;
            break;
        }
    }
    out->n = internal_count;

    /* Layer 2: E0's name-relative gate. (a) is the scoping rule: an
       UNCLASSIFIED match carries failure evidence attributed DIRECTLY
       to that switch - fatal, flag or not (unlike PRIVATE, whose zero
       evidence defers). */
    if (unclassified_matches > 0) {
        out->outcome = HCN_IRES_REJECT;
        out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
        return;
    }

    /* The premise: the selector lacks a readable terminal-evidence
       match (fusion or bound ports - ladder-terminal, undemotable). */
    if (internal_matches == 0 && external_matches == 0) {
        /* (b): an unreadable-named PRIVATE row exists - it could be the
           target or a second same-named candidate. Fires under the
           premise only; E0's layer runs before E2, so this beats the
           duplicate clause when both would apply. */
        if (has_unreadable_private) {
            out->outcome = HCN_IRES_REJECT;
            out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
            return;
        }
        /* (c): the unknown-owner flag with NOTHING readable matching -
           the absent target could be the unknown owner. */
        if (census->has_unknown_owner_failure && all_matches == 0) {
            out->outcome = HCN_IRES_REJECT;
            out->reason_code = HCN_IR_INVENTORY_UNAVAILABLE;
            return;
        }
        /* The DEFER arm: a unique readable PRIVATE match under the flag
           (and not blocked by (b)) - an iport of any flavor cannot be a
           Private switch's port, so the census's PRIVATE was a
           failed-chain INTERNAL and E3's Type adjudicates. */
        if (census->has_unknown_owner_failure && private_matches == 1 &&
            all_matches == 1) {
            out->outcome = HCN_IRES_PROCEED;
            out->probe_guid = private_match->switch_id;
            out->defer_adjudication = TRUE;
            return;
        }
        /* Flag clear + unique PRIVATE match, or a non-unique PRIVATE
           match: E2's own ordered rules take it from here. */
    }

    /* Layer 3: E2's ordered rules. */
    /* 1. The duplicate rule (flag-independent: the DEFER arm needs a
       unique match, and without uniqueness the acquire cannot know
       which switch the name means). */
    if (internal_matches > 1 ||
        (internal_matches == 0 && private_matches > 1)) {
        out->outcome = HCN_IRES_REJECT;
        out->reason_code = HCN_IR_DUPLICATE;
        return;
    }
    /* 2. Exactly one INTERNAL-class match: proceed (a same-named
       readable PRIVATE row does not change it - the terminal match is
       what the name resolves to). */
    if (internal_matches == 1) {
        out->outcome = HCN_IRES_PROCEED;
        out->probe_guid = internal_match->switch_id;
        out->defer_adjudication = FALSE;
        return;
    }
    /* 3. Zero INTERNAL matches; the name exists in the full classified
       set: the class-specific lines (external wins a shared name - an
       external switch HAS a host adapter; the merged line would be
       factually wrong for the measured I211). A PRIVATE match under
       the flag never reaches here - E0's DEFER routed it above, and
       E0's fatal (b) likewise precedes this list. */
    if (external_matches > 0) {
        out->outcome = HCN_IRES_REJECT;
        out->reason_code = HCN_IR_EXTERNAL_SWITCH;
        return;
    }
    if (private_matches > 0) {
        out->outcome = HCN_IRES_REJECT;
        out->reason_code = HCN_IR_NOT_INTERNAL;
        return;
    }
    /* 4. Zero matches anywhere, census complete: not-found (definite -
       reached only after E0/E2 found no applicable match, so a PRIVATE/
       EXTERNAL row took its class-specific line whatever the INTERNAL
       count was). */
    out->outcome = HCN_IRES_REJECT;
    out->reason_code = HCN_IR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* The reason formatter                                                */
/* ------------------------------------------------------------------ */

void format_reason(HcnInternalReasonCode code, HcnReasonStyle style,
                   const wchar_t *selector, HRESULT hr, size_t n,
                   const wchar_t *type_str, wchar_t *out, size_t out_cap)
{
    size_t pos = 0;

    out[0] = L'\0';

    /* The selector's role: appended in the "cannot resolve" form for
       the inventory/correlation refusals (omitted in the LIST census
       level), quoted in the name-level lines, absent from the rest.
       The name is truncated to fit the cap - the clause order puts the
       explanation first so a long name never eats it. */
#define APPEND(...)                                              \
    do {                                                          \
        int w = _snwprintf_s(out + pos, out_cap - pos, _TRUNCATE, \
                             __VA_ARGS__);                        \
        if (w > 0) pos += (size_t)w;                              \
    } while (0)

    if (style == HCN_RS_CONCISE) {
        switch (code) {
        case HCN_IR_NOT_FOUND:            APPEND(L"not found"); break;
        case HCN_IR_DUPLICATE:            APPEND(L"name duplicated"); break;
        case HCN_IR_EXTERNAL_SWITCH:      APPEND(L"external switch"); break;
        case HCN_IR_NOT_INTERNAL:         APPEND(L"no host adapter"); break;
        case HCN_IR_NO_HOST_NETWORK:      APPEND(L"no host network object"); break;
        case HCN_IR_INVENTORY_UNAVAILABLE:APPEND(L"classification incomplete"); break;
        case HCN_IR_CORRELATION_UNAVAILABLE: APPEND(L"correlation unavailable"); break;
        case HCN_IR_UNSUPPORTED_TYPE:     APPEND(L"unsupported type"); break;
        case HCN_IR_NAT:                  APPEND(L"NAT network"); break;
        case HCN_IR_OWNED_EXTERNAL:       APPEND(L"AppSandbox-owned"); break;
        case HCN_IR_FIXED_IDENTITY_CONFLICT: APPEND(L"fixed network identity conflict"); break;
        case HCN_IR_UNREADABLE_IDENTITY:  APPEND(L"unreadable identity"); break;
        case HCN_IR_ENDPOINT_FAILED:      APPEND(L"endpoint creation failed"); break;
        case HCN_IR_INVALID_STORED_NAME:  APPEND(L"invalid stored name"); break;
        case HCN_IR_INVALID_NAME:         APPEND(L"invalid name"); break;
        case HCN_IR_AUTO_COLLISION:       APPEND(L"fixed network occupied"); break;
        case HCN_IR_AUTO_NAME_SQUAT:      APPEND(L"AppSandboxInternal name in use"); break;
        default:                          APPEND(L"unavailable"); break;
        }
        return;
    }

    APPEND(L"Internal: ");
    switch (code) {
    case HCN_IR_NOT_FOUND:
        APPEND(L"no internal switch named \"%s\" on this host (%lu internal switches enumerated).",
               selector ? selector : L"", (unsigned long)n);
        break;
    case HCN_IR_DUPLICATE:
        APPEND(L"the switch name \"%s\" is duplicated; give the switches unique names.",
               selector ? selector : L"");
        break;
    case HCN_IR_EXTERNAL_SWITCH:
        APPEND(L"\"%s\" is an external switch; Internal can only join an internal switch.",
               selector ? selector : L"");
        break;
    case HCN_IR_NOT_INTERNAL:
        APPEND(L"\"%s\" is not an internal switch (no host adapter on the switch).",
               selector ? selector : L"");
        break;
    case HCN_IR_NO_HOST_NETWORK:
        APPEND(L"switch \"%s\" has no host network object to attach to; retry shortly if the switch was just created.",
               selector ? selector : L"");
        break;
    case HCN_IR_INVENTORY_UNAVAILABLE:
        APPEND(L"host inventory unavailable (WMI filtered, incomplete, or the Hyper-V WMI provider is absent)");
        if (style == HCN_RS_FULL)
            APPEND(L"; cannot resolve \"%s\".", selector ? selector : L"");
        else
            APPEND(L".");
        break;
    case HCN_IR_CORRELATION_UNAVAILABLE:
        APPEND(L"HCN correlation unavailable on this host");
        if (style == HCN_RS_FULL)
            APPEND(L"; cannot resolve \"%s\".", selector ? selector : L"");
        else
            APPEND(L".");
        break;
    case HCN_IR_UNSUPPORTED_TYPE:
        if (type_str && wcscmp(type_str, L"Transparent") == 0)
            APPEND(L"\"%s\"'s network is Transparent (external).",
                   selector ? selector : L"");
        else
            APPEND(L"\"%s\"'s network has unsupported HCN network type \"%s\".",
                   selector ? selector : L"",
                   (type_str && type_str[0]) ? type_str : L"(unknown)");
        break;
    case HCN_IR_NAT:
        APPEND(L"\"%s\"'s network is the AppSandbox NAT network; it cannot be joined.",
               selector ? selector : L"");
        break;
    case HCN_IR_OWNED_EXTERNAL:
        APPEND(L"\"%s\"'s network is an AppSandbox-owned external network; it cannot be joined.",
               selector ? selector : L"");
        break;
    case HCN_IR_FIXED_IDENTITY_CONFLICT:
        APPEND(L"the network at AppSandbox's internal GUID has a foreign identity; refusing to join.");
        break;
    case HCN_IR_UNREADABLE_IDENTITY:
        APPEND(L"the network at AppSandbox's internal GUID has an unreadable identity; refusing to join.");
        break;
    case HCN_IR_ENDPOINT_FAILED:
        APPEND(L"endpoint creation on switch \"%s\" failed (0x%08X); the switch was not modified.",
               selector ? selector : L"", (unsigned)hr);
        break;
    case HCN_IR_INVALID_STORED_NAME:
        APPEND(L"the stored switch name in vms.cfg is invalid; re-select the switch.");
        break;
    case HCN_IR_INVALID_NAME:
        APPEND(L"the internal switch name is invalid (too long, or it contains CR/LF/NUL, an unpaired surrogate, or U+FFFF); re-select the switch.");
        break;
    case HCN_IR_AUTO_COLLISION:
        APPEND(L"the fixed AppSandboxInternal network ID is occupied by a foreign network; delete it (via an HNS tool) to restore Auto Internal.");
        break;
    case HCN_IR_AUTO_NAME_SQUAT:
        APPEND(L"a Hyper-V switch named \"AppSandboxInternal\" already exists; rename it so App Sandbox can create its built-in network, or start the VM in Explicit on that switch.");
        break;
    default:
        APPEND(L"unavailable.");
        break;
    }

#undef APPEND
}

/* ------------------------------------------------------------------ */
/* The shared BSTR-to-RAW switch/iport name conversion                 */
/* ------------------------------------------------------------------ */

static void internal_name_to_raw(BSTR source,
                                 wchar_t *name,
                                 BOOL *name_unreadable,
                                 BOOL *name_unusable,
                                 BOOL placeholder_when_missing)
{
    *name_unreadable = FALSE;
    *name_unusable = FALSE;
    name[0] = L'\0';

    if (!source) {
        /* A missing property is UNREADABLE - a different predicate from
           unusable (E0's (b) reads it; an unusable name can never equal a
           storable selector, so it can be neither). */
        *name_unreadable = TRUE;
        if (placeholder_when_missing)
            wcsncpy_s(name, INTERNAL_SWITCH_CAP, HCN_NAME_PLACEHOLDER,
                      _TRUNCATE);
        return;
    }

    {
        UINT len = SysStringLen(source);

        /* Decided from the SOURCE length BEFORE the copy: a label too
           long for the buffer, a zero-length label, or one failing the
           shared character class over the RAW length (an embedded NUL
           prefix-matches exactly like a truncated over-length label) can
           never equal a storable selector - it must not contribute a
           name match, and the placeholder (never the prefix) lands in
           name[] so a consumer that forgets the flag still cannot match
           by prefix. */
        if (len == 0 || len >= INTERNAL_SWITCH_CAP ||
            !asb_internal_switch_value_chars_ok(source, len)) {
            *name_unusable = TRUE;
            wcsncpy_s(name, INTERNAL_SWITCH_CAP, HCN_NAME_PLACEHOLDER,
                      _TRUNCATE);
            return;
        }

        wcsncpy_s(name, INTERNAL_SWITCH_CAP, source, _TRUNCATE);
    }
}

void internal_switch_name_to_raw(BSTR source, HcnRawSwitchResult *out)
{
    if (!out)
        return;
    internal_name_to_raw(source, out->name, &out->name_unreadable,
                         &out->name_unusable, FALSE);
}

void internal_iport_name_to_raw(BSTR source, HcnRawIportResult *out)
{
    if (!out)
        return;
    internal_name_to_raw(source, out->name, &out->name_unreadable,
                         &out->name_unusable_for_attribution, TRUE);
}

/* ------------------------------------------------------------------ */
/* The projection                                                      */
/* ------------------------------------------------------------------ */

void internal_project_census(HcnInternalSwitchCensus *census)
{
    size_t i, j;

    if (!census)
        return;

    /* (i) Duplicate groups over {INTERNAL ∪ UNCLASSIFIED} entries
       excluding name_unusable: E2's match set (a truncated prefix
       cannot collide with a storable name). */
    for (i = 0; i < census->count; i++) {
        HcnInternalSwitchEntry *e = &census->entries[i];
        e->defer_eligible = FALSE;
        if (e->name_unusable || e->name_unreadable)
            continue;
        if (e->sw_class != HCN_SW_INTERNAL && e->sw_class != HCN_SW_UNCLASSIFIED)
            continue;
        for (j = i + 1; j < census->count; j++) {
            HcnInternalSwitchEntry *o = &census->entries[j];
            if (o->name_unusable || o->name_unreadable)
                continue;
            if (o->sw_class != HCN_SW_INTERNAL &&
                o->sw_class != HCN_SW_UNCLASSIFIED)
                continue;
            if (_wcsicmp(e->name, o->name) == 0) {
                e->in_duplicate_name_group = TRUE;
                o->in_duplicate_name_group = TRUE;
            }
        }
    }

    /* Every readable, usable entry asks the PRODUCTION resolver with
       its own name - the exact question "what would the acquire say
       about this name?". This gives disabled external, unclassified,
       and duplicate entries their real reason instead of a UI-level
       "not present" fallback. Only PRIVATE entries can be
       defer_eligible. PROCEED leaves any E3 probe rejection reason
       untouched; no second copy of the flag/duplicate/(b) precedence
       rules exists here. A zero-length name is name_unusable, so the
       resolver's non-empty contract holds. */
    for (i = 0; i < census->count; i++) {
        HcnInternalSwitchEntry *e = &census->entries[i];
        HcnInternalResolution res;

        if (e->name_unusable || e->name_unreadable)
            continue;

        hcn_internal_resolve_selector(census, e->name, &res);
        if (e->sw_class == HCN_SW_PRIVATE) {
            e->defer_eligible =
                (res.outcome == HCN_IRES_PROCEED && res.defer_adjudication);
        }
        if (res.outcome == HCN_IRES_REJECT) {
            e->reason_code = res.reason_code;
            format_reason(res.reason_code, HCN_RS_CONCISE, e->name, res.hr,
                          res.n, NULL, e->reason_text,
                          ARRAYSIZE(e->reason_text));
        }
    }

    /* (ii)-(v): the derived selectable rule, the product fixed-GUID
       de-offer (marked OWNED - they stay in the census, never offered),
       the sentinel-named entry de-offer, and the name placeholders. */
    for (i = 0; i < census->count; i++) {
        HcnInternalSwitchEntry *e = &census->entries[i];

        if (e->name_unusable || e->name_unreadable) {
            /* Payload safety: the raw text must not become a variable
               in whether the census message survives conversion. */
            wcsncpy_s(e->name, INTERNAL_SWITCH_CAP, HCN_NAME_PLACEHOLDER,
                      _TRUNCATE);
        }

        if (IsEqualGUID(&e->switch_id, &s_product_internal_guid) ||
            IsEqualGUID(&e->switch_id, &s_product_nat_guid)) {
            /* Measured-defensive: HNS networks register no VMMS
               switches, so this shape is refuted - the exclusion stays
               cheap defense. OWNED marks it for the UI's walk. */
            e->verdict = HCN_CAND_OWNED;
            e->selectable = FALSE;
            continue;
        }

        if (e->name_unusable || e->name_unreadable) {
            e->selectable = FALSE;
            continue;
        }

        if (e->sw_class == HCN_SW_PRIVATE && !e->defer_eligible) {
            e->selectable = FALSE;
            continue;
        }

        e->selectable = (e->sw_class == HCN_SW_INTERNAL ||
                         (e->sw_class == HCN_SW_PRIVATE && e->defer_eligible))
                            ? TRUE
                            : FALSE;
        if (e->verdict == HCN_CAND_REJECTED || e->in_duplicate_name_group ||
            _wcsicmp(e->name, L"(Auto)") == 0)
            e->selectable = FALSE;
    }
}
