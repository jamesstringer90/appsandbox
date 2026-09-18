/* hcn_wmi.c - Hyper-V WMI read-only topology.
 *
 * The root\virtualization\v2 traversal for External acquisition:
 * COM/WMI session management, bounded associators queries, the two
 * port-to-switch walks (Ethernet port and wireless endpoint), the
 * one-shot topology inventory, the per-target association, the
 * unfiltered-token qualification gate, and the per-acquire
 * provider-existence probe. Read-only throughout: no WMI mutation
 * methods, no service starts. */

#include "hcn_network.h"
#include "hcn_private.h"
#include "ui.h"
#include <objbase.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "oleaut32.lib")

/* ---- COM/WMI read-only helpers ----
 *
 * Read-only Hyper-V WMI v2 (root\virtualization\v2) traversal for External
 * acquisition. No PowerShell, no Hyper-V cmdlets, no WMI mutation methods,
 * no service starts. Identity comes from binary GUIDs (DeviceID / the
 * switch's Name GUID); FriendlyName/ElementName are log fields and MAC is
 * cross-evidence only. The calling thread owns the COM apartment
 * lifecycle: this helper calls CoInitializeEx/CoUninitialize unless the
 * thread already had an apartment (RPC_E_CHANGED_MODE), so worker threads
 * and synchronous callers need no external COM initialization. All
 * COM/BSTR/VARIANT/enumerator resources are released on every exit path.
 * Queries use bounded waits (5s per Next, caller-supplied overall
 * deadline); a timeout is a hard error and never becomes NOT_FOUND.
 *
 * Association chain from a NIC-side EthernetPort to its switch:
 *   InternalEthernetPort --SAPImplementation--> LANEndpoint
 *     --ActiveConnection--> peer LANEndpoint
 *     --SAPImplementation--> Msvm_EthernetSwitchPort
 *     --SystemDevice--> Msvm_VirtualEthernetSwitch (Name = switch GUID) */

#define WMI_NEXT_TIMEOUT_MS     5000

#define WMI_MAX_ENUM_OBJECTS    64
#define WMI_REL_PATH_MAX        512
#define WMI_WQL_MAX             768

typedef enum {
    WMI_WALK_OK,            /* unique chain from a port to its switch */
    WMI_WALK_NOT_CONNECTED, /* authoritative: port has no active connection */
    WMI_WALK_ERROR          /* ambiguity, malformed topology, or query failure */
} WmiWalkResult;

/* Classify a WMI session-establishment failure: infrastructure or
 * namespace absence is PROVIDER_ABSENT; access denied, security setup,
 * COM initialization and every other failure is ERROR. */
TopologyResultKind wmi_classify_session_failure(HRESULT hr)
{
    if (hr == REGDB_E_CLASSNOTREG)          /* WMI infrastructure absent */
        return TOPOLOGY_PROVIDER_ABSENT;
    if (hr == WBEM_E_INVALID_NAMESPACE)     /* root\virtualization\v2 absent */
        return TOPOLOGY_PROVIDER_ABSENT;
    return TOPOLOGY_ERROR;
}

/* Open a WMI session with the three-branch COM rule:
   S_OK/S_FALSE -> own the reference and pair CoUninitialize;
   RPC_E_CHANGED_MODE -> use the caller's existing apartment, take no
   reference, do not CoUninitialize, and continue (not an error and not
   provider absence); any other failure -> ERROR, never PROVIDER_ABSENT. */
HRESULT wmi_session_open(WmiSession *s)
{
    IWbemLocator *locator = NULL;
    BSTR ns = NULL;
    HRESULT hr;

    s->svc = NULL;
    s->com_ref_owned = FALSE;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        s->com_ref_owned = TRUE;
    } else if (hr == RPC_E_CHANGED_MODE) {
        /* Thread already has an apartment from the caller: use it, do not
           take a reference and do not CoUninitialize. */
        s->com_ref_owned = FALSE;
    } else {
        ui_log(L"External: CoInitializeEx failed (0x%08X).", hr);
        return hr;
    }

    hr = CoInitializeSecurity(NULL, -1, NULL, NULL,
                              RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE,
                              NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
        ui_log(L"External: CoInitializeSecurity failed (0x%08X).", hr);
        if (s->com_ref_owned) CoUninitialize();
        s->com_ref_owned = FALSE;
        return hr;
    }

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&locator);
    if (SUCCEEDED(hr) && !locator) {
        /* S_OK with a NULL object is a contract failure, not success. */
        hr = E_FAIL;
    }
    if (FAILED(hr) || !locator) {
        ui_log(L"External: CoCreateInstance(WbemLocator) failed (0x%08X).", hr);
        if (locator) {
            /* A failed call that still returned an object owns it: release
               defensively, never leak it. */
            locator->lpVtbl->Release(locator);
            locator = NULL;
        }
        if (s->com_ref_owned) CoUninitialize();
        s->com_ref_owned = FALSE;
        return hr;
    }

    ns = SysAllocString(L"root\\virtualization\\v2");
    if (!ns) {
        locator->lpVtbl->Release(locator);
        if (s->com_ref_owned) CoUninitialize();
        s->com_ref_owned = FALSE;
        return E_OUTOFMEMORY;
    }

    hr = locator->lpVtbl->ConnectServer(locator, ns, NULL, NULL, NULL, 0,
                                        NULL, NULL, &s->svc);
    SysFreeString(ns);
    locator->lpVtbl->Release(locator);
    if (SUCCEEDED(hr) && !s->svc) {
        /* S_OK with a NULL service proxy is a contract failure. */
        hr = E_FAIL;
    }
    if (FAILED(hr) || !s->svc) {
        ui_log(L"External: WMI ConnectServer(root\\virtualization\\v2) failed (0x%08X).", hr);
        if (s->svc) {
            /* A failed ConnectServer that still wrote a service proxy owns
               it: release defensively, never leak it. */
            s->svc->lpVtbl->Release(s->svc);
            s->svc = NULL;
        }
        if (s->com_ref_owned) CoUninitialize();
        s->com_ref_owned = FALSE;
        return hr;
    }

    hr = CoSetProxyBlanket((IUnknown *)s->svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                           NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);
    if (FAILED(hr)) {
        ui_log(L"External: CoSetProxyBlanket failed (0x%08X).", hr);
        s->svc->lpVtbl->Release(s->svc);
        s->svc = NULL;
        if (s->com_ref_owned) CoUninitialize();
        s->com_ref_owned = FALSE;
        return hr;
    }

    return S_OK;
}

void wmi_session_close(WmiSession *s)
{
    if (s->svc) {
        s->svc->lpVtbl->Release(s->svc);
        s->svc = NULL;
    }
    if (s->com_ref_owned) {
        CoUninitialize();
        s->com_ref_owned = FALSE;
    }
}

/* Get a string property as a HeapAlloc'd copy (caller frees with
   HeapFree). Returns NULL when absent/not a string. */
static wchar_t *wmi_get_string_prop(IWbemClassObject *obj, const wchar_t *prop)
{
    VARIANT v;
    wchar_t *result = NULL;

    VariantInit(&v);
    if (SUCCEEDED(obj->lpVtbl->Get(obj, prop, 0, &v, NULL, NULL)) &&
        v.vt == VT_BSTR && v.bstrVal) {
        size_t len = wcslen(v.bstrVal) + 1;
        result = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
        if (result)
            wcscpy_s(result, len, v.bstrVal);
    }
    VariantClear(&v);
    return result;
}

static void wmi_free_prop_string(wchar_t *s)
{
    if (s) HeapFree(GetProcessHeap(), 0, s);
}

/* Interpret a WMI VARIANT as a boolean. Only an explicit, losslessly
 * boolean value is usable: VT_BOOL, or an integer exactly 0/1. VT_NULL,
 * VT_EMPTY, other types, out-of-range integers and absent data are
 * UNAVAILABLE - for External port IsBound that is a topology error, never
 * an implicit FALSE. */
typedef enum {
    WMI_BOOL_TRUE = 1,
    WMI_BOOL_FALSE = 2,
    WMI_BOOL_UNAVAILABLE = 3
} WmiBoolRead;

static WmiBoolRead wmi_interpret_bool_variant(const VARIANT *v)
{
    if (!v)
        return WMI_BOOL_UNAVAILABLE;
    switch (v->vt) {
    case VT_BOOL:
        return (v->boolVal != VARIANT_FALSE) ? WMI_BOOL_TRUE : WMI_BOOL_FALSE;
    case VT_I4:
    case VT_UI4:
        if (v->lVal == 0) return WMI_BOOL_FALSE;
        if (v->lVal == 1) return WMI_BOOL_TRUE;
        return WMI_BOOL_UNAVAILABLE;  /* not losslessly boolean */
    case VT_I2:
    case VT_UI2:
        if (v->iVal == 0) return WMI_BOOL_FALSE;
        if (v->iVal == 1) return WMI_BOOL_TRUE;
        return WMI_BOOL_UNAVAILABLE;
    default:
        return WMI_BOOL_UNAVAILABLE;  /* includes VT_NULL / VT_EMPTY */
    }
}

/* Read a boolean WMI property. Returns the three-state read result;
   *out is set to TRUE only for a legitimate TRUE. */
static WmiBoolRead wmi_get_bool_prop(IWbemClassObject *obj, const wchar_t *prop,
                                     BOOL *out)
{
    VARIANT v;
    WmiBoolRead r;

    if (out)
        *out = FALSE;
    VariantInit(&v);
    if (FAILED(obj->lpVtbl->Get(obj, prop, 0, &v, NULL, NULL))) {
        VariantClear(&v);
        return WMI_BOOL_UNAVAILABLE;
    }
    r = wmi_interpret_bool_variant(&v);
    VariantClear(&v);
    if (out)
        *out = (r == WMI_BOOL_TRUE);
    return r;
}

static BOOL wmi_get_relpath(IWbemClassObject *obj, wchar_t *out, size_t cap)
{
    VARIANT v;
    BOOL ok = FALSE;

    out[0] = L'\0';
    VariantInit(&v);
    if (SUCCEEDED(obj->lpVtbl->Get(obj, L"__RELPATH", 0, &v, NULL, NULL)) &&
        v.vt == VT_BSTR && v.bstrVal) {
        size_t len = wcslen(v.bstrVal);
        if (len + 1 <= cap) {
            memcpy(out, v.bstrVal, (len + 1) * sizeof(wchar_t));
            ok = TRUE;
        }
    }
    VariantClear(&v);
    return ok;
}

/* Extract a binary GUID from exactly one of four precise forms (after
 * trimming surrounding whitespace): "GUID", "{GUID}", "Microsoft:GUID",
 * "Microsoft:{GUID}". Anything else - arbitrary prefixes or suffixes,
 * embedded or multiple GUIDs - is rejected (no searching for the
 * first '{' inside arbitrary text). GUID comparisons always use the
 * normalized binary value, never the text form. */
static BOOL wmi_guid_from_microsoft_id(const wchar_t *text, GUID *out)
{
    const wchar_t *p;
    size_t len;

    if (!text)
        return FALSE;

    /* Trim surrounding whitespace only; the interior must be exact. */
    p = text;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
        p++;
    len = wcslen(p);
    while (len > 0 && (p[len - 1] == L' ' || p[len - 1] == L'\t' ||
                       p[len - 1] == L'\r' || p[len - 1] == L'\n'))
        len--;

    if (len >= 10 && _wcsnicmp(p, L"Microsoft:", 10) == 0) {
        p += 10;
        len -= 10;
    }

    if (len == 38 && p[0] == L'{' && p[37] == L'}')
        return parse_guid_value(p, 38, out);
    if (len == 36)
        return parse_guid_value(p, 36, out);
    return FALSE;
}

/* Normalize a MAC string to uppercase hex digits (no separators).
   Cross-evidence only - never an identity decision. */
static void wmi_normalize_mac(const wchar_t *src, wchar_t *out, size_t cap)
{
    size_t o = 0;

    out[0] = L'\0';
    for (; src && *src && o + 1 < cap; src++) {
        wchar_t c = *src;
        if (c >= L'a' && c <= L'f')
            c = (wchar_t)(c - L'a' + L'A');
        if ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'))
            out[o++] = c;
    }
    out[o] = L'\0';
}

/* Result set of one WMI query. */
typedef struct {
    IWbemClassObject **objects;
    size_t count;
} WmiObjects;

static void wmi_objects_free(WmiObjects *o)
{
    size_t i;

    if (!o)
        return;
    if (o->objects) {
        for (i = 0; i < o->count; i++) {
            if (o->objects[i])
                o->objects[i]->lpVtbl->Release(o->objects[i]);
        }
        HeapFree(GetProcessHeap(), 0, o->objects);
    }
    o->objects = NULL;
    o->count = 0;
}

/* Classify a WMI query failure in initialization/capability context
 * (the provider-existence probe, the ComputerSystem probe): provider,
 * namespace or class absence is PROVIDER_ABSENT. Topology data queries
 * never use this - an unreadable class after the provider was
 * established is an error, never a fail-open downgrade. */
static TopologyResultKind wmi_classify_query_failure(HRESULT hr)
{
    if (hr == WBEM_E_INVALID_NAMESPACE)
        return TOPOLOGY_PROVIDER_ABSENT;
    if (hr == WBEM_E_INVALID_CLASS)
        return TOPOLOGY_PROVIDER_ABSENT;
    if (hr == REGDB_E_CLASSNOTREG)
        return TOPOLOGY_PROVIDER_ABSENT;
    return TOPOLOGY_ERROR;
}

/* Execute one WQL query (SELECT or ASSOCIATORS OF) and enumerate all
   results with a bounded wait. S_OK is returned ONLY when the
   enumeration completed; the kind is then FOUND (count > 0) or NOT_FOUND
   (count == 0). Query failures return the original HRESULT with the kind
   TOPOLOGY_ERROR (an error, never a provider-absence downgrade -
   these queries run after the provider was established), and an
   object-cap overflow returns TOPOLOGY_CAP_OVERFLOW so the WMI_FAILED
   line is the cap wording, not the service-state wording. */
static HRESULT wmi_run_query(WmiSession *s, const wchar_t *wql, size_t max_objects,
                             ULONGLONG deadline, WmiObjects *out,
                             TopologyResultKind *kind)
{
    IEnumWbemClassObject *enumerator = NULL;
    BSTR bstr_lang = NULL;
    BSTR bstr_query = NULL;
    IWbemClassObject **all = NULL;
    IWbemClassObject *batch[16];
    ULONG returned = 0;
    size_t count = 0, cap = 0;
    BOOL capped = FALSE;
    HRESULT hr;

    out->objects = NULL;
    out->count = 0;
    *kind = TOPOLOGY_ERROR;

    bstr_lang = SysAllocString(L"WQL");
    bstr_query = SysAllocString(wql);
    if (!bstr_lang || !bstr_query) {
        SysFreeString(bstr_lang);
        SysFreeString(bstr_query);
        return E_OUTOFMEMORY;
    }

    hr = s->svc->lpVtbl->ExecQuery(s->svc, bstr_lang, bstr_query,
                                   WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                   NULL, &enumerator);
    SysFreeString(bstr_lang);
    SysFreeString(bstr_query);
    if (SUCCEEDED(hr) && !enumerator) {
        /* S_OK with a NULL enumerator is a contract failure, never a
           usable empty result. */
        ui_log(L"External: WMI ExecQuery returned success without an enumerator.");
        hr = E_FAIL;
    }
    if (FAILED(hr) || !enumerator) {
        /* Provider/namespace/class absence is classified only in
           initialization/capability context (the provider probe and the
           session open). This query runs after the provider was
           established, so an unreadable class here is an error - never a
           downgrade that would fail-open a partial provider. */
        *kind = TOPOLOGY_ERROR;
        ui_log(L"External: WMI query failed (0x%08X): %s", hr, wql);
        if (enumerator) {
            /* A failed call that still returned an enumerator owns it:
               release defensively, never leak it. */
            enumerator->lpVtbl->Release(enumerator);
            enumerator = NULL;
        }
        return hr;
    }

    for (;;) {
        int b;
        ULONG i;

        /* Zero the batch so ownership is tracked by `returned` alone: a
           slot is an object only when Next reported it this round. */
        for (i = 0; i < ARRAYSIZE(batch); i++)
            batch[i] = NULL;
        returned = 0;

        hr = enumerator->lpVtbl->Next(enumerator, WMI_NEXT_TIMEOUT_MS,
                                      ARRAYSIZE(batch), batch, &returned);
        if (hr == WBEM_S_FALSE && returned == 0)
            break; /* enumeration complete */
        if (hr == WBEM_S_TIMEDOUT && returned == 0) {
            /* Bounded wait elapsed with no data: hard error, never
               NOT_FOUND. */
            ui_log(L"External: WMI enumeration timed out.");
            goto fail_batch;
        }
        if (FAILED(hr)) {
            ui_log(L"External: WMI enumeration failed (0x%08X).", hr);
            goto fail_batch;
        }
        if (GetTickCount64() > deadline) {
            ui_log(L"External: WMI topology query exceeded its deadline.");
            goto fail_batch;
        }

        for (b = 0; b < (int)returned; b++) {
            if (count >= max_objects) {
                /* More authoritative results than allowed: ambiguity. */
                int k;
                for (k = b; k < (int)returned; k++)
                    if (batch[k]) batch[k]->lpVtbl->Release(batch[k]);
                ui_log(L"External: WMI query returned more than %lu results; treating as ambiguous.",
                       (unsigned long)max_objects);
                capped = TRUE;
                goto fail_query;
            }
            if (count == cap) {
                IWbemClassObject **grown;
                size_t new_cap = cap ? cap * 2 : 16;
                grown = (IWbemClassObject **)HeapAlloc(GetProcessHeap(), 0,
                                                       new_cap * sizeof(IWbemClassObject *));
                if (!grown) {
                    int k;
                    for (k = b; k < (int)returned; k++)
                        if (batch[k]) batch[k]->lpVtbl->Release(batch[k]);
                    goto fail_query;
                }
                if (cap > 0)
                    memcpy(grown, all, cap * sizeof(IWbemClassObject *));
                if (all)
                    HeapFree(GetProcessHeap(), 0, all);
                all = grown;
                cap = new_cap;
            }
            all[count++] = batch[b];
        }
    }

    enumerator->lpVtbl->Release(enumerator);

    out->objects = all;
    out->count = count;
    *kind = (count > 0) ? TOPOLOGY_FOUND : TOPOLOGY_NOT_FOUND;
    return S_OK;

fail_batch:
    /* Objects the last Next reported but that were never transferred into
       all[] are owned by this call: release exactly those. */
    {
        ULONG k;
        for (k = 0; k < returned; k++)
            if (batch[k]) batch[k]->lpVtbl->Release(batch[k]);
    }
    /* fall through */
fail_query:
    {
        size_t i;
        for (i = 0; i < count; i++)
            all[i]->lpVtbl->Release(all[i]);
        if (all)
            HeapFree(GetProcessHeap(), 0, all);
    }
    enumerator->lpVtbl->Release(enumerator);
    *kind = capped ? TOPOLOGY_CAP_OVERFLOW : TOPOLOGY_ERROR;
    if (SUCCEEDED(hr))
        return E_FAIL;
    return hr;
}

/* One ASSOCIATORS OF step from an object, filtered by association class
   and (optionally) result class. Reuses wmi_run_query semantics. */
static HRESULT wmi_associators_step(WmiSession *s, IWbemClassObject *obj,
                                    const wchar_t *assoc_class,
                                    const wchar_t *result_class,
                                    ULONGLONG deadline, WmiObjects *out,
                                    TopologyResultKind *kind)
{
    wchar_t relpath[WMI_REL_PATH_MAX];
    wchar_t wql[WMI_WQL_MAX];

    if (!wmi_get_relpath(obj, relpath, WMI_REL_PATH_MAX)) {
        *kind = TOPOLOGY_ERROR;
        return E_FAIL;
    }
    if (result_class && result_class[0])
        swprintf_s(wql, WMI_WQL_MAX,
                   L"ASSOCIATORS OF {%s} WHERE AssocClass = %s ResultClass = %s",
                   relpath, assoc_class, result_class);
    else
        swprintf_s(wql, WMI_WQL_MAX,
                   L"ASSOCIATORS OF {%s} WHERE AssocClass = %s",
                   relpath, assoc_class);

    return wmi_run_query(s, wql, WMI_MAX_ENUM_OBJECTS, deadline, out, kind);
}

/* One associators hop in a port-to-switch chain: the association class,
   the expected result class, and the label used in the ambiguity log. */
typedef struct {
    const wchar_t *assoc_class;
    const wchar_t *result_class;
    const wchar_t *label;
} WmiHop;

#define WMI_WALK_MAX_HOPS 4

/* Walk a chain of associators hops from a port-side object to its owning
 * switch. Cardinality must be exactly 1 at every hop: zero results is
 * NOT_CONNECTED ("not connected" - authoritative; the CALLER decides
 * whether that is expected (unbound port/endpoint: diagnostics) or
 * inconsistent (bound port, or the caller's own consistency rules)), and
 * more than one result is an ambiguity error (logged, never first-wins).
 * The single result of the last hop is the switch; on OK its identity
 * comes from the switch object's Name GUID (the authoritative WMI switch
 * ID; on current Windows builds this equals the HCN network ID). All hop
 * result batches stay alive until the walk completes - each hop's input
 * is the previous hop's single result object. */
static WmiWalkResult wmi_walk_chain(WmiSession *s, IWbemClassObject *start,
                                    const WmiHop *hops, size_t hop_count,
                                    ULONGLONG deadline, GUID *out_switch_id)
{
    WmiObjects steps[WMI_WALK_MAX_HOPS];
    TopologyResultKind kind;
    WmiWalkResult result = WMI_WALK_ERROR;
    size_t i;

    ZeroMemory(steps, sizeof(steps));
    if (hop_count == 0 || hop_count > WMI_WALK_MAX_HOPS)
        return WMI_WALK_ERROR;
    for (i = 0; i < hop_count; i++) {
        IWbemClassObject *from = (i == 0) ? start : steps[i - 1].objects[0];
        if (FAILED(wmi_associators_step(s, from, hops[i].assoc_class,
                                        hops[i].result_class, deadline,
                                        &steps[i], &kind)))
            goto done;
        if (steps[i].count == 0) {
            result = WMI_WALK_NOT_CONNECTED;
            goto done;
        }
        if (steps[i].count > 1) {
            ui_log(L"External: %s association count %lu; ambiguous.",
                   hops[i].label, (unsigned long)steps[i].count);
            goto done;
        }
    }
    {
        wchar_t *sw_guid_text = wmi_get_string_prop(steps[hop_count - 1].objects[0],
                                                    L"Name");
        if (!sw_guid_text || !wmi_guid_from_microsoft_id(sw_guid_text, out_switch_id)) {
            ui_log(L"External: switch Name is not an authoritative GUID.");
            wmi_free_prop_string(sw_guid_text);
            goto done;
        }
        wmi_free_prop_string(sw_guid_text);
    }
    result = WMI_WALK_OK;

done:
    for (i = 0; i < WMI_WALK_MAX_HOPS; i++)
        wmi_objects_free(&steps[i]);
    return result;
}

/* Walk one NIC-side Msvm_ExternalEthernetPort to its owning external
   switch through this association chain:
   Port --Msvm_EthernetDeviceSAPImplementation--> LANEndpoint
   --Msvm_ActiveConnection--> peer LANEndpoint (switch port side)
   --Msvm_EthernetDeviceSAPImplementation--> Msvm_EthernetSwitchPort
   --Msvm_SystemDevice--> Msvm_VirtualEthernetSwitch */
static WmiWalkResult wmi_walk_port_to_switch(WmiSession *s, IWbemClassObject *port,
                                             ULONGLONG deadline,
                                             GUID *out_switch_id)
{
    static const WmiHop hops[] = {
        { L"Msvm_EthernetDeviceSAPImplementation", L"Msvm_LANEndpoint",
          L"port LANEndpoint" },
        { L"Msvm_ActiveConnection", L"Msvm_LANEndpoint",
          L"ActiveConnection" },
        { L"Msvm_EthernetDeviceSAPImplementation", L"Msvm_EthernetSwitchPort",
          L"switch-port" },
        { L"Msvm_SystemDevice", L"Msvm_VirtualEthernetSwitch",
          L"switch" },
    };
    return wmi_walk_chain(s, port, hops, ARRAYSIZE(hops), deadline,
                          out_switch_id);
}

/* Walk one Msvm_WiFiEndpoint (the wireless adapter's protocol endpoint) to
   its owning external switch. A wireless binding has no
   Msvm_ExternalEthernetPort row - the endpoint itself is the adapter-side
   end of the chain, so the walk starts at ActiveConnection:
   WiFiEndpoint -> ActiveConnection -> peer LANEndpoint -> SAP ->
   EthernetSwitchPort -> SystemDevice -> VirtualEthernetSwitch (the hops
   after ActiveConnection are the ones the Ethernet walk uses). Zero
   results at any hop means "not bound" (a free wireless adapter has no
   switch-side peer). */
static WmiWalkResult wmi_walk_wifi_endpoint_to_switch(WmiSession *s, IWbemClassObject *endpoint,
                                                      ULONGLONG deadline,
                                                      GUID *out_switch_id)
{
    static const WmiHop hops[] = {
        { L"Msvm_ActiveConnection", L"Msvm_LANEndpoint",
          L"wireless endpoint ActiveConnection" },
        { L"Msvm_EthernetDeviceSAPImplementation", L"Msvm_EthernetSwitchPort",
          L"wireless endpoint switch-port" },
        { L"Msvm_SystemDevice", L"Msvm_VirtualEthernetSwitch",
          L"wireless endpoint switch" },
    };
    return wmi_walk_chain(s, endpoint, hops, ARRAYSIZE(hops), deadline,
                          out_switch_id);
}

/* ---- WMI network topology inventory ---- */

void wmi_topology_free(WmiTopology *t)
{
    if (!t)
        return;
    if (t->switches) HeapFree(GetProcessHeap(), 0, t->switches);
    if (t->external_ports) HeapFree(GetProcessHeap(), 0, t->external_ports);
    ZeroMemory(t, sizeof(*t));
}

/* Read identity fields of an external port object. IsBound must be
 * legitimately readable: a missing, NULL, wrong-typed or non-boolean
 * value makes the topology untrustworthy and fails the read - only a
 * real false is false. */
static BOOL wmi_read_port_entry(IWbemClassObject *obj, WmiPortEntry *entry,
                                BOOL is_external)
{
    wchar_t *devid = wmi_get_string_prop(obj, L"DeviceID");
    wchar_t *mac = wmi_get_string_prop(obj, L"PermanentAddress");
    BOOL ok = FALSE;

    ZeroMemory(entry, sizeof(*entry));
    if (devid) {
        if (wmi_guid_from_microsoft_id(devid, &entry->adapter_guid))
            entry->has_guid = TRUE;
        wmi_free_prop_string(devid);
    }
    if (mac) {
        wmi_normalize_mac(mac, entry->mac, ARRAYSIZE(entry->mac));
        wmi_free_prop_string(mac);
    }
    if (is_external) {
        WmiBoolRead br = wmi_get_bool_prop(obj, L"IsBound", &entry->is_bound);
        if (br == WMI_BOOL_UNAVAILABLE) {
            ui_log(L"External: WMI external port IsBound is missing or not a "
                   L"readable boolean; topology is untrustworthy.");
            return FALSE;
        }
    }

    ok = entry->has_guid;
    if (!ok)
        ui_log(L"External: WMI port object without a parseable DeviceID GUID.");
    return ok;
}

/* Find a switch entry by its authoritative GUID. */
static WmiSwitchEntry *wmi_topology_find_switch_by_id(const WmiTopology *t, const GUID *id)
{
    size_t i;
    for (i = 0; i < t->switch_count; i++) {
        if (t->switches[i].has_id && IsEqualGUID(&t->switches[i].id, id))
            return &t->switches[i];
    }
    return NULL;
}

/* TRUE when a switch entry with this identity already exists.
   Duplicate identities make the topology ambiguous (never pick first). */
static BOOL wmi_topology_has_duplicate_switch_id(const WmiTopology *t, const GUID *id)
{
    return wmi_topology_find_switch_by_id(t, id) != NULL;
}

/* TRUE when a port entry with this adapter GUID already exists. */
static BOOL wmi_topology_has_duplicate_port_guid(const WmiPortEntry *ports,
                                                 size_t count, const GUID *guid)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (ports[i].has_guid && IsEqualGUID(&ports[i].adapter_guid, guid))
            return TRUE;
    }
    return FALSE;
}

/* Visibility probe for Msvm_ComputerSystem with first-object-wins
   semantics: the probe's only output is a boolean, so more
   results than any cap is a busy host, not ambiguity - the cap-overflow
   disqualification would block create on VM-heavy hosts while borrowing
   still works. A probe failure disqualifies coverage without failing the
   topology build. */
static BOOL wmi_probe_host_system_visible(WmiSession *s, ULONGLONG deadline)
{
    IEnumWbemClassObject *enumerator = NULL;
    BSTR bstr_lang = NULL;
    BSTR bstr_query = NULL;
    IWbemClassObject *batch[1];
    ULONG returned = 0;
    HRESULT hr;
    BOOL visible = FALSE;

    bstr_lang = SysAllocString(L"WQL");
    bstr_query = SysAllocString(L"SELECT * FROM Msvm_ComputerSystem");
    if (!bstr_lang || !bstr_query) {
        SysFreeString(bstr_lang);
        SysFreeString(bstr_query);
        return FALSE;
    }

    hr = s->svc->lpVtbl->ExecQuery(s->svc, bstr_lang, bstr_query,
                                   WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                   NULL, &enumerator);
    SysFreeString(bstr_lang);
    SysFreeString(bstr_query);
    if (SUCCEEDED(hr) && !enumerator)
        hr = E_FAIL;
    if (FAILED(hr) || !enumerator) {
        ui_log(L"External: WMI host system probe failed (0x%08X).", hr);
        if (enumerator)
            enumerator->lpVtbl->Release(enumerator);
        return FALSE;
    }

    /* One bounded Next: any object at all means visible. */
    batch[0] = NULL;
    hr = enumerator->lpVtbl->Next(enumerator, WMI_NEXT_TIMEOUT_MS, 1, batch,
                                  &returned);
    if (SUCCEEDED(hr) && returned >= 1 && batch[0]) {
        visible = TRUE;
        batch[0]->lpVtbl->Release(batch[0]);
    } else {
        ui_log(L"External: WMI host system (Msvm_ComputerSystem) not "
               L"visible; coverage cannot be qualified.");
    }
    (void)deadline;  /* one bounded Next; the overall deadline is the caller's */
    enumerator->lpVtbl->Release(enumerator);
    return visible;
}

/* Build the one-shot read-only WMI network topology inventory. Enumerates
 * switches and external ports, and chains every port to its owning switch
 * while the enumeration objects are alive. Unbound external ports are
 * kept (diagnostics only, never owners). Duplicate switch or port
 * identities fail the build (never pick first). External/non-External
 * classification is by bound external ports only - the internal
 * port census is deliberately not built. Fails (ERROR/PROVIDER_ABSENT)
 * on any enumeration or chain problem; never truncates silently. A host
 * with zero identifiable switches yields a complete empty inventory, not
 * an error - whether that inventory can PROVE absence is the
 * authority question (host_system_visible + unfiltered token). */
HRESULT wmi_build_topology(WmiSession *s, ULONGLONG deadline,
                                  WmiTopology *out, TopologyResultKind *out_kind)
{
    WmiObjects objs = {0};
    TopologyResultKind kind;
    size_t i;
    HRESULT hr;

    ZeroMemory(out, sizeof(*out));
    *out_kind = TOPOLOGY_ERROR;

    out->host_system_visible = wmi_probe_host_system_visible(s, deadline);

    /* Switches */
    hr = wmi_run_query(s, L"SELECT * FROM Msvm_VirtualEthernetSwitch",
                       WMI_MAX_ENUM_OBJECTS, deadline, &objs, &kind);
    if (FAILED(hr)) {
        *out_kind = kind;
        return hr;
    }
    if (objs.count > 0) {
        out->switches = (WmiSwitchEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                    objs.count * sizeof(WmiSwitchEntry));
        if (!out->switches) {
            wmi_objects_free(&objs);
            *out_kind = TOPOLOGY_ERROR;
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < objs.count; i++) {
            WmiSwitchEntry *sw = &out->switches[out->switch_count];
            wchar_t *guid_text = wmi_get_string_prop(objs.objects[i], L"Name");
            if (guid_text && wmi_guid_from_microsoft_id(guid_text, &sw->id))
                sw->has_id = TRUE;
            wmi_free_prop_string(guid_text);
            if (sw->has_id) {
                /* Duplicate switch identity: ambiguous, never pick first. */
                if (wmi_topology_has_duplicate_switch_id(out, &sw->id)) {
                    ui_log(L"External: duplicate WMI switch identity; "
                           L"topology is ambiguous.");
                    wmi_objects_free(&objs);
                    wmi_topology_free(out);
                    *out_kind = TOPOLOGY_DATA_MALFORMED;
                    return E_FAIL;
                }
                out->switch_count++;
            } else {
                /* A switch without a GUID Name is malformed and
                   unidentifiable: fail closed. */
                ui_log(L"External: WMI switch without authoritative Name GUID.");
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            }
        }
    }
    wmi_objects_free(&objs);

    if (out->switch_count == 0) {
        /* No switches exist: an empty inventory. Per-target association
           returns NOT_FOUND from it; authority is the
           visibility-and-token condition. */
        *out_kind = TOPOLOGY_NOT_FOUND;
        return S_OK;
    }

    /* External ports: read identity and walk to the owning switch in the
       same enumeration pass. */
    hr = wmi_run_query(s, L"SELECT * FROM Msvm_ExternalEthernetPort",
                       WMI_MAX_ENUM_OBJECTS, deadline, &objs, &kind);
    if (FAILED(hr)) {
        *out_kind = kind;
        wmi_topology_free(out);
        return hr;
    }
    if (objs.count > 0) {
        out->external_ports = (WmiPortEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                        objs.count * sizeof(WmiPortEntry));
        if (!out->external_ports) {
            wmi_objects_free(&objs);
            wmi_topology_free(out);
            *out_kind = TOPOLOGY_ERROR;
            return E_OUTOFMEMORY;
        }
        for (i = 0; i < objs.count; i++) {
            WmiPortEntry *pe = &out->external_ports[out->external_port_count];
            GUID switch_id;
            WmiWalkResult walk;

            if (!wmi_read_port_entry(objs.objects[i], pe, TRUE)) {
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            }
            {
                /* Duplicate port identity: ambiguous, never pick first. */
                if (wmi_topology_has_duplicate_port_guid(out->external_ports,
                                                         out->external_port_count,
                                                         &pe->adapter_guid)) {
                    ui_log(L"External: duplicate WMI external port identity; "
                           L"topology is ambiguous.");
                    wmi_objects_free(&objs);
                    wmi_topology_free(out);
                    *out_kind = TOPOLOGY_DATA_MALFORMED;
                    return E_FAIL;
                }
            }
            walk = wmi_walk_port_to_switch(s, objs.objects[i], deadline,
                                           &switch_id);
            if (walk == WMI_WALK_OK) {
                pe->has_switch = TRUE;
                pe->switch_id = switch_id;
            } else if (walk == WMI_WALK_ERROR) {
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            } else if (pe->is_bound) {
                /* A BOUND external port without an association chain is
                   inconsistent topology: fail closed. */
                ui_log(L"External: bound external port has no association chain.");
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            }
            /* Unbound port, not connected: diagnostics only. */
            if (pe->has_switch && !wmi_topology_find_switch_by_id(out, &pe->switch_id)) {
                /* The walked switch is not in the enumerated switch set:
                   inconsistent inventory, fail closed. */
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            }
            out->external_port_count++;
        }
    }
    wmi_objects_free(&objs);

    /* Wireless uplinks: a Wi-Fi adapter bound to an external switch has NO
       Msvm_ExternalEthernetPort row (measured) - Hyper-V models the binding
       as an Msvm_WiFiEndpoint whose Name is Microsoft:{adapter GUID}. The
       census walks each endpoint; bound-ness is the walk reaching a switch
       (the endpoint's own Associated/Connected properties are 802.11
       association state, not binding evidence). A walk that errors or is
       ambiguous degrades only that endpoint (logged), never the whole
       topology: the endpoint population is not restricted to inventory
       adapters, and a bound entry is only ever claimed from a completed
       walk - a degraded target falls through to the create path, where
       HNS's own adapter-occupancy check rejects a bound adapter. */
    hr = wmi_run_query(s, L"SELECT * FROM Msvm_WiFiEndpoint",
                       WMI_MAX_ENUM_OBJECTS, deadline, &objs, &kind);
    if (SUCCEEDED(hr)) {
        if (objs.count > 0) {
            WmiPortEntry *grown;
            if (out->external_ports) {
                grown = (WmiPortEntry *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    out->external_ports,
                    (out->external_port_count + objs.count) * sizeof(WmiPortEntry));
            } else {
                /* No Ethernet external-port rows were enumerated (a
                   wireless-only host): the array was never allocated, and
                   HeapReAlloc cannot grow a NULL block - allocate it. */
                grown = (WmiPortEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                    (out->external_port_count + objs.count) * sizeof(WmiPortEntry));
            }
            if (!grown) {
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_ERROR;
                return E_OUTOFMEMORY;
            }
            out->external_ports = grown;
        }
        for (i = 0; i < objs.count; i++) {
            WmiPortEntry *pe = &out->external_ports[out->external_port_count];
            GUID switch_id;
            WmiWalkResult walk;
            wchar_t *ep_name, *ep_mac;

            ep_name = wmi_get_string_prop(objs.objects[i], L"Name");
            if (!ep_name || !wmi_guid_from_microsoft_id(ep_name, &pe->adapter_guid)) {
                /* Unidentifiable endpoint (Name must be Microsoft:{GUID}):
                   the same fail-closed treatment as an unparseable
                   Ethernet DeviceID, never a silent skip - a skipped
                   endpoint could be a bound uplink, which would make a
                   later NOT_FOUND falsely authoritative. */
                ui_log(L"External: WMI wireless endpoint without a parseable Name GUID.");
                wmi_free_prop_string(ep_name);
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            }
            wmi_free_prop_string(ep_name);
            pe->has_guid = TRUE;
            pe->is_wifi = TRUE;

            ep_mac = wmi_get_string_prop(objs.objects[i], L"MACAddress");
            if (ep_mac) {
                wmi_normalize_mac(ep_mac, pe->mac, ARRAYSIZE(pe->mac));
                wmi_free_prop_string(ep_mac);
            }

            if (wmi_topology_has_duplicate_port_guid(out->external_ports,
                                                     out->external_port_count,
                                                     &pe->adapter_guid)) {
                /* Duplicate identity (an Ethernet port row and a wireless
                   endpoint for one GUID would land here): ambiguous. */
                ui_log(L"External: duplicate wireless endpoint identity; "
                       L"topology is ambiguous.");
                wmi_objects_free(&objs);
                wmi_topology_free(out);
                *out_kind = TOPOLOGY_DATA_MALFORMED;
                return E_FAIL;
            }

            walk = wmi_walk_wifi_endpoint_to_switch(s, objs.objects[i], deadline,
                                                    &switch_id);
            if (walk == WMI_WALK_OK) {
                pe->is_bound = TRUE;
                pe->has_switch = TRUE;
                pe->switch_id = switch_id;
                if (!wmi_topology_find_switch_by_id(out, &pe->switch_id)) {
                    /* The walked switch is not in the enumerated switch set:
                       inconsistent inventory, fail closed. */
                    ui_log(L"External: wireless endpoint switch missing from switch inventory.");
                    wmi_objects_free(&objs);
                    wmi_topology_free(out);
                    *out_kind = TOPOLOGY_DATA_MALFORMED;
                    return E_FAIL;
                }
            } else if (walk == WMI_WALK_ERROR) {
                /* Degrade this endpoint only (see the block comment above). */
                wchar_t q[64];
                guid_to_string(&pe->adapter_guid, q, 64);
                ui_log(L"External: wireless endpoint %s could not be resolved "
                       L"to a switch; entry degraded.", q);
            }
            /* Not connected: a free wireless adapter - diagnostics only. */
            out->external_port_count++;
        }
    } else if (hr != WBEM_E_INVALID_CLASS) {
        *out_kind = kind;
        wmi_topology_free(out);
        return hr;
    }
    /* WBEM_E_INVALID_CLASS: this host has no wireless endpoint modeling -
       nothing to census (the namespace schema normally defines the class
       regardless of hardware; its absence is not a provider failure). */
    wmi_objects_free(&objs);

    /* Per-switch bound-physical-uplink counting: a selected
       External requires exactly one bound physical uplink. */
    {
        size_t pi;
        for (pi = 0; pi < out->external_port_count; pi++) {
            if (out->external_ports[pi].is_bound && out->external_ports[pi].has_switch) {
                WmiSwitchEntry *sw = wmi_topology_find_switch_by_id(out,
                    &out->external_ports[pi].switch_id);
                if (sw)
                    sw->bound_external_ports++;
            }
        }
    }

    *out_kind = TOPOLOGY_FOUND;
    return S_OK;
}

/* Per-target association: the switch owning the unique BOUND
 * external port for the given physical adapter GUID. Unbound ports with
 * the same GUID or MAC are diagnostics only and never create ambiguity.
 * Zero bound matches -> NOT_FOUND (adapter is not owned by an external
 * switch). More than one bound match -> ERROR. A bound match whose chain
 * did not complete -> ERROR (inconsistent topology). FOUND names the
 * switch whose bound_external_ports count the caller checks for the
 * single-uplink requirement. */
TopologyResultKind wmi_topology_switch_for_external_adapter(
    const WmiTopology *t, const GUID *adapter_guid, const WmiSwitchEntry **out)
{
    const WmiPortEntry *bound_match = NULL;
    int bound_matches = 0;
    size_t i;

    *out = NULL;
    for (i = 0; i < t->external_port_count; i++) {
        const WmiPortEntry *pe = &t->external_ports[i];
        if (!pe->has_guid || !IsEqualGUID(&pe->adapter_guid, adapter_guid))
            continue;
        if (pe->is_bound) {
            bound_match = pe;
            bound_matches++;
        }
    }
    if (bound_matches == 0)
        return TOPOLOGY_NOT_FOUND;
    if (bound_matches > 1) {
        /* Multiple owners of T: an object-level topology failure
           - SET_UNSUPPORTED/TOPOLOGY_CONFLICT class, never host-wide
           ERROR. It stops the walk; the caller decides the label. */
        ui_log(L"External: %d bound WMI external ports match the physical adapter GUID.",
               bound_matches);
        return TOPOLOGY_CONFLICT;
    }
    if (!bound_match->has_switch) {
        ui_log(L"External: bound external port has no association chain to a switch.");
        return TOPOLOGY_ERROR;
    }
    {
        const WmiSwitchEntry *sw = wmi_topology_find_switch_by_id(t, &bound_match->switch_id);
        if (!sw) {
            ui_log(L"External: external port switch missing from switch inventory.");
            return TOPOLOGY_ERROR;
        }
        if (bound_match->is_wifi)
            ui_log(L"External: adapter association resolved through the "
                   L"wireless endpoint binding.");
        *out = sw;
        return TOPOLOGY_FOUND;
    }
}

/* ---- Token qualification gate ----
 *
 * A filtered token, access denial, or incomplete view is not absence:
 * NOT_FOUND authority requires host_system_visible AND an
 * unfiltered view. The gate reads ONE effective token: elevated, or a
 * member of the built-in Hyper-V Administrators group. */

typedef struct {
    BOOL ok;
    BOOL elevated;
    BOOL hv_admin;
} TokenQualification;

/* Build the built-in Hyper-V Administrators SID (S-1-5-32-578) into the
   caller's buffer via the well-known-SID API. The buffer is caller-owned
   stack storage; it is never passed to FreeSid (that deallocator is only
   valid for AllocateAndInitializeSid results). Returns the SID pointer,
   or NULL when the SID could not be built. */
static PSID hcn_build_hv_admin_sid(BYTE *buf, DWORD *in_out_size)
{
    if (!buf || !in_out_size)
        return NULL;
    if (!CreateWellKnownSid(WinBuiltinHyperVAdminsSid, NULL, (PSID)buf, in_out_size))
        return NULL;
    return (PSID)buf;
}

/* Fill the token qualification snapshot from ONE effective token handle:
   the calling thread's impersonation token when the thread has one, else a
   duplicate of the process primary token converted to an impersonation
   token (CheckTokenMembership requires an impersonation token; passing a
   primary handle fails, so the conversion is load-bearing). Both the
   TokenElevation query and the Hyper-V Administrators membership query run
   against that one handle, so the two verdicts can never come from
   different tokens. Any failure anywhere leaves ok == FALSE, which the
   gate reads as unqualified (fail closed). */
static void hcn_read_token_qualification(TokenQualification *out)
{
    HANDLE thread_token = NULL;
    HANDLE primary = NULL;
    HANDLE effective = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    BOOL is_member = FALSE;
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid_buf);
    PSID hv_sid;

    ZeroMemory(out, sizeof(*out));

    hv_sid = hcn_build_hv_admin_sid(sid_buf, &sid_size);
    if (!hv_sid)
        return; /* cannot even build the SID: unqualified */

    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread_token)) {
        effective = thread_token;
    } else if (GetLastError() == ERROR_NO_TOKEN) {
        /* No thread token: fall back to the process token, converted to an
           impersonation token so CheckTokenMembership accepts it. */
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE,
                              &primary))
            return;
        if (!DuplicateTokenEx(primary, TOKEN_QUERY, NULL, SecurityImpersonation,
                              TokenImpersonation, &effective)) {
            CloseHandle(primary);
            return;
        }
        CloseHandle(primary);
    } else {
        /* A thread token exists but cannot be opened: fail closed. */
        return;
    }

    if (!GetTokenInformation(effective, TokenElevation, &elevation,
                             sizeof(elevation), &size)) {
        CloseHandle(effective);
        return; /* elevation query failed: unqualified */
    }
    out->elevated = (elevation.TokenIsElevated != 0);

    if (!CheckTokenMembership(effective, hv_sid, &is_member)) {
        CloseHandle(effective);
        return; /* membership query failed: unqualified */
    }
    out->hv_admin = is_member;

    out->ok = TRUE;
    CloseHandle(effective);
}

/* TRUE when the effective token can read Hyper-V WMI classes without UAC
   filtering (elevated token or Hyper-V Administrators membership).
   Measured on build 26200: a filtered token sees Msvm_ComputerSystem
   while the switch and port classes return empty against a host that
   demonstrably has switches - so ComputerSystem visibility alone does
   not prove an unfiltered view. */
BOOL hcn_coverage_token_unfiltered(void)
{
    TokenQualification tq;
    ZeroMemory(&tq, sizeof(tq));
    hcn_read_token_qualification(&tq);
    return tq.ok && (tq.elevated || tq.hv_admin);
}

/* ---- Per-acquire provider-existence probe ---- */



/* One cheap class-existence probe per acquire - not a topology build
 * (VMP-only hosts pay only this). Opens a session, executes one
 * bounded query against Msvm_VirtualEthernetSwitch, and stops after the
 * first batch: the object count is irrelevant, only whether the class is
 * queryable. Provider-unavailable HRESULTs (REGDB_E_CLASSNOTREG,
 * WBEM_E_INVALID_NAMESPACE, WBEM_E_INVALID_CLASS) classify as PROVIDER_ABSENT
 * in capability context; every other failure is ERROR - never provider
 * absence. */
WmiProviderStatus wmi_probe_provider_installed(void)
{
    WmiSession session;
    IEnumWbemClassObject *enumerator = NULL;
    BSTR bstr_lang = NULL;
    BSTR bstr_query = NULL;
    IWbemClassObject *batch[1];
    ULONG returned = 0;
    TopologyResultKind kind;
    WmiProviderStatus result;
    HRESULT hr;

    hr = wmi_session_open(&session);
    if (FAILED(hr)) {
        kind = wmi_classify_session_failure(hr);
        return (kind == TOPOLOGY_PROVIDER_ABSENT) ? WMI_PROVIDER_ABSENT
                                                   : WMI_PROVIDER_ERROR;
    }

    bstr_lang = SysAllocString(L"WQL");
    bstr_query = SysAllocString(L"SELECT * FROM Msvm_VirtualEthernetSwitch");
    if (!bstr_lang || !bstr_query) {
        SysFreeString(bstr_lang);
        SysFreeString(bstr_query);
        wmi_session_close(&session);
        return WMI_PROVIDER_ERROR;
    }

    hr = session.svc->lpVtbl->ExecQuery(session.svc, bstr_lang, bstr_query,
                                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                        NULL, &enumerator);
    SysFreeString(bstr_lang);
    SysFreeString(bstr_query);
    if (SUCCEEDED(hr) && !enumerator)
        hr = E_FAIL;
    if (FAILED(hr) || !enumerator) {
        if (enumerator) {
            enumerator->lpVtbl->Release(enumerator);
            enumerator = NULL;
        }
        wmi_session_close(&session);
        kind = wmi_classify_query_failure(hr);
        if (kind == TOPOLOGY_PROVIDER_ABSENT) {
            ui_log(L"External: Hyper-V WMI provider absent (0x%08X).", hr);
            return WMI_PROVIDER_ABSENT;
        }
        ui_log(L"External: WMI provider-existence probe failed (0x%08X).", hr);
        return WMI_PROVIDER_ERROR;
    }

    /* Drain one bounded batch: success means the class is queryable. */
    batch[0] = NULL;
    hr = enumerator->lpVtbl->Next(enumerator, WMI_NEXT_TIMEOUT_MS, 1, batch,
                                  &returned);
    if (SUCCEEDED(hr) && returned >= 1 && batch[0])
        batch[0]->lpVtbl->Release(batch[0]);
    enumerator->lpVtbl->Release(enumerator);
    wmi_session_close(&session);

    if (hr == WBEM_S_TIMEDOUT) {
        ui_log(L"External: WMI provider-existence probe timed out.");
        return WMI_PROVIDER_ERROR;
    }
    if (FAILED(hr)) {
        ui_log(L"External: WMI provider-existence probe failed (0x%08X).", hr);
        kind = wmi_classify_query_failure(hr);
        return (kind == TOPOLOGY_PROVIDER_ABSENT) ? WMI_PROVIDER_ABSENT
                                                  : WMI_PROVIDER_ERROR;
    }

    result = WMI_PROVIDER_INSTALLED;
    ui_log(L"External: Hyper-V WMI provider installed.");
    return result;
}
