/* Offline contract tests for the real Internal/External owned-delete
 * wrappers and Internal census gates. HCN and WMI entry points are
 * replaced only by inert local stubs; the production lock, delete core,
 * wrappers, resolver, and census projection are linked unchanged. */

#include "hcn_private.h"
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            wprintf(L"FAIL line %d: %S\n", __LINE__, #expr); \
            return FALSE; \
        } \
    } while (0)

typedef enum {
    MOCK_CENSUS_EMPTY,
    MOCK_CENSUS_ONE_INTERNAL
} MockCensusMode;

static volatile LONG g_delete_calls;
static GUID g_last_deleted_id;
static volatile LONG g_block_delete;
static HRESULT g_mock_delete_result = S_OK;
static volatile LONG g_freed_error_records;
static HANDLE g_delete_entered;
static HANDLE g_delete_release;
static HANDLE g_lock_acquired;

static BOOL g_mock_host_visible = TRUE;
static BOOL g_mock_token_unfiltered = TRUE;
static MockCensusMode g_mock_census_mode = MOCK_CENSUS_EMPTY;
static LONG g_mock_builder_calls;
static LONG g_mock_topology_calls;
static LONG g_mock_session_open_calls;
static LONG g_mock_session_close_calls;
static LONG g_mock_topology_free_calls;
static ULONGLONG g_mock_topology_deadline;
static ULONGLONG g_mock_census_deadline;
static ULONGLONG g_mock_session_open_end;
static HANDLE g_mock_topology_cancel_event;
static HANDLE g_mock_census_cancel_event;
static DWORD g_mock_session_open_delay_ms;
static DWORD g_mock_topology_delay_ms;
static HRESULT g_mock_session_open_result = S_OK;
static HRESULT g_mock_topology_result = S_OK;
static HRESULT g_mock_census_result = S_OK;
static BOOL g_mock_partial_census_failure;
static HANDLE g_probe_cancel_event;
static BOOL g_signal_cancel_on_open;

static const GUID G_TEST_SWITCH = {
    0xC001CAFE, 0xA501, 0x4EAD, { 0x81, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 }
};

static GUID make_guid(DWORD data1)
{
    GUID g;
    ZeroMemory(&g, sizeof(g));
    g.Data1 = data1;
    g.Data2 = 0x1234;
    g.Data3 = 0x5678;
    g.Data4[0] = 0x9A;
    g.Data4[7] = 0xBC;
    return g;
}

/* UI logging has no observable product effect in this harness. */
void ui_log(const wchar_t *fmt, ...)
{
    (void)fmt;
}

/* HCN-owned records are COM task allocations. Fake deletes return none,
   but keep the real ownership contract available to the wrappers. */
void hcn_free_string(PWSTR value)
{
    if (value) {
        InterlockedIncrement(&g_freed_error_records);
        CoTaskMemFree(value);
    }
}

static HRESULT WINAPI mock_delete_network(REFGUID id, PWSTR *out_error_record)
{
    if (out_error_record) {
        *out_error_record = NULL;
        if (FAILED(g_mock_delete_result)) {
            static const wchar_t kMockError[] = L"offline mock error";
            size_t bytes = sizeof(kMockError);
            *out_error_record = (PWSTR)CoTaskMemAlloc(bytes);
            if (*out_error_record)
                memcpy(*out_error_record, kMockError, bytes);
        }
    }
    g_last_deleted_id = *id;
    InterlockedIncrement(&g_delete_calls);

    if (InterlockedCompareExchange(&g_block_delete, 0, 0) != 0) {
        SetEvent(g_delete_entered);
        WaitForSingleObject(g_delete_release, 5000);
    }
    return g_mock_delete_result;
}

/* The delete wrapper thread blocks inside the fake HcnDeleteNetwork. */
static DWORD WINAPI internal_delete_thread(LPVOID context)
{
    return (DWORD)hcn_delete_owned_internal_network((const GUID *)context);
}

/* This thread must remain blocked until fake HcnDeleteNetwork returns,
   proving the production core holds g_network_lock across the call. */
static DWORD WINAPI lock_probe_thread(LPVOID context)
{
    (void)context;
    hcn_network_lock_acquire();
    SetEvent(g_lock_acquired);
    hcn_network_lock_release();
    return 0;
}

static BOOL test_owned_delete_wrappers(void)
{
    GUID borrowed = make_guid(0xB001);
    GUID foreign_id = make_guid(0xF001);
    GUID adapter = make_guid(0xA001);
    GUID external_owned;
    LONG calls;
    LONG freed;
    HRESULT hr;

    g_hcn_dll = (HMODULE)1; /* non-null dummy; no DLL is loaded or used */
    pfnDeleteNet = mock_delete_network;
    g_mock_delete_result = S_OK;
    InterlockedExchange(&g_delete_calls, 0);

    hr = hcn_delete_owned_internal_network(&APPSANDBOX_INTERNAL_GUID);
    CHECK(hr == S_OK);
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == 1);
    CHECK(IsEqualGUID(&g_last_deleted_id, &APPSANDBOX_INTERNAL_GUID));

    calls = InterlockedCompareExchange(&g_delete_calls, 0, 0);
    CHECK(hcn_delete_owned_internal_network(&borrowed) == S_FALSE);
    CHECK(hcn_delete_owned_internal_network(&foreign_id) == S_FALSE);
    CHECK(hcn_delete_owned_internal_network(&GUID_NULL) == S_FALSE);
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == calls);

    CHECK(SUCCEEDED(hcn_external_owned_id(&adapter, &external_owned)));
    hr = hcn_delete_owned_external_network(&external_owned, &adapter);
    CHECK(hr == S_OK);
    CHECK(IsEqualGUID(&g_last_deleted_id, &external_owned));
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == calls + 1);

    calls = InterlockedCompareExchange(&g_delete_calls, 0, 0);
    CHECK(hcn_delete_owned_external_network(&foreign_id, &adapter) == S_FALSE);
    CHECK(hcn_delete_owned_external_network(&external_owned, &GUID_NULL) == S_FALSE);
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == calls);

    g_mock_delete_result = HCN_DELETE_NOT_FOUND;
    freed = InterlockedCompareExchange(&g_freed_error_records, 0, 0);
    CHECK(hcn_delete_owned_external_network(&external_owned, &adapter) == S_FALSE);
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == calls + 1);
    CHECK(InterlockedCompareExchange(&g_freed_error_records, 0, 0) == freed + 1);

    g_mock_delete_result = E_ACCESSDENIED;
    calls = InterlockedCompareExchange(&g_delete_calls, 0, 0);
    freed = InterlockedCompareExchange(&g_freed_error_records, 0, 0);
    CHECK(hcn_delete_owned_external_network(&external_owned, &adapter) ==
          E_ACCESSDENIED);
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == calls + 1);
    CHECK(InterlockedCompareExchange(&g_freed_error_records, 0, 0) == freed + 1);

    pfnDeleteNet = NULL;
    calls = InterlockedCompareExchange(&g_delete_calls, 0, 0);
    CHECK(hcn_delete_owned_external_network(&external_owned, &adapter) ==
          E_NOT_VALID_STATE);
    CHECK(InterlockedCompareExchange(&g_delete_calls, 0, 0) == calls);
    pfnDeleteNet = mock_delete_network;
    g_mock_delete_result = S_OK;
    return TRUE;
}

static BOOL test_delete_lock_boundary(void)
{
    HANDLE delete_thread;
    HANDLE probe_thread;
    DWORD wait_result;

    g_delete_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_delete_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lock_acquired = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(g_delete_entered && g_delete_release && g_lock_acquired);
    InterlockedExchange(&g_block_delete, 1);

    delete_thread = CreateThread(NULL, 0, internal_delete_thread,
                                 (LPVOID)&APPSANDBOX_INTERNAL_GUID, 0, NULL);
    CHECK(delete_thread != NULL);
    CHECK(WaitForSingleObject(g_delete_entered, 5000) == WAIT_OBJECT_0);

    probe_thread = CreateThread(NULL, 0, lock_probe_thread, NULL, 0, NULL);
    CHECK(probe_thread != NULL);
    wait_result = WaitForSingleObject(g_lock_acquired, 100);
    CHECK(wait_result == WAIT_TIMEOUT);

    SetEvent(g_delete_release);
    CHECK(WaitForSingleObject(delete_thread, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(probe_thread, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(g_lock_acquired, 0) == WAIT_OBJECT_0);

    InterlockedExchange(&g_block_delete, 0);
    CloseHandle(delete_thread);
    CloseHandle(probe_thread);
    CloseHandle(g_delete_entered);
    CloseHandle(g_delete_release);
    CloseHandle(g_lock_acquired);
    g_delete_entered = NULL;
    g_delete_release = NULL;
    g_lock_acquired = NULL;
    return TRUE;
}

/* ---- WMI/HCN read stubs used only by hcn_enum_internal_switches ---- */

HRESULT wmi_session_open(WmiSession *session)
{
    ZeroMemory(session, sizeof(*session));
    InterlockedIncrement(&g_mock_session_open_calls);
    if (g_mock_session_open_delay_ms)
        Sleep(g_mock_session_open_delay_ms);
    g_mock_session_open_end = GetTickCount64();
    return g_mock_session_open_result;
}

void wmi_session_close(WmiSession *session)
{
    (void)session;
    InterlockedIncrement(&g_mock_session_close_calls);
}

HRESULT wmi_build_topology(WmiSession *session, ULONGLONG deadline,
                           WmiTopology *out, TopologyResultKind *out_kind)
{
    InterlockedIncrement(&g_mock_topology_calls);
    g_mock_topology_deadline = deadline;
    g_mock_topology_cancel_event = session->cancel_event;
    if (g_mock_topology_delay_ms)
        Sleep(g_mock_topology_delay_ms);
    ZeroMemory(out, sizeof(*out));
    out->host_system_visible = g_mock_host_visible;
    if (FAILED(g_mock_topology_result)) {
        *out_kind = TOPOLOGY_ERROR;
        return g_mock_topology_result;
    }
    if (g_mock_census_mode == MOCK_CENSUS_ONE_INTERNAL) {
        out->switches = (WmiSwitchEntry *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WmiSwitchEntry));
        if (!out->switches)
            return E_OUTOFMEMORY;
        out->switch_count = 1;
        out->switches[0].id = G_TEST_SWITCH;
        out->switches[0].has_id = TRUE;
    }
    *out_kind = TOPOLOGY_NOT_FOUND;
    return S_OK;
}

void wmi_topology_free(WmiTopology *topology)
{
    if (!topology)
        return;
    InterlockedIncrement(&g_mock_topology_free_calls);
    if (topology->switches)
        HeapFree(GetProcessHeap(), 0, topology->switches);
    if (topology->external_ports)
        HeapFree(GetProcessHeap(), 0, topology->external_ports);
    ZeroMemory(topology, sizeof(*topology));
}

HRESULT wmi_build_internal_census(WmiSession *session,
                                  const WmiTopology *topology,
                                  ULONGLONG deadline,
                                  HcnInternalSwitchCensus *out)
{
    (void)topology;
    InterlockedIncrement(&g_mock_builder_calls);
    g_mock_census_deadline = deadline;
    g_mock_census_cancel_event = session->cancel_event;
    ZeroMemory(out, sizeof(*out));
    out->reason_code = HCN_IR_NOT_FOUND;
    if (FAILED(g_mock_census_result)) {
        if (g_mock_partial_census_failure) {
            out->entries = (HcnInternalSwitchEntry *)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY,
                sizeof(HcnInternalSwitchEntry));
            if (!out->entries)
                return E_OUTOFMEMORY;
            out->count = 1;
            out->state = HCN_CENSUS_OK;
            out->entries[0].switch_id = G_TEST_SWITCH;
        }
        return g_mock_census_result;
    }
    if (g_mock_census_mode == MOCK_CENSUS_EMPTY) {
        out->state = HCN_CENSUS_EMPTY;
        return S_OK;
    }

    out->entries = (HcnInternalSwitchEntry *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HcnInternalSwitchEntry));
    if (!out->entries)
        return E_OUTOFMEMORY;
    out->count = 1;
    out->state = HCN_CENSUS_OK;
    out->entries[0].switch_id = G_TEST_SWITCH;
    out->entries[0].sw_class = HCN_SW_INTERNAL;
    out->entries[0].verdict = HCN_CAND_UNKNOWN;
    wcscpy_s(out->entries[0].name, INTERNAL_SWITCH_CAP, L"Test Internal");
    return S_OK;
}

BOOL hcn_coverage_token_unfiltered(void)
{
    return g_mock_token_unfiltered;
}

HcnLookupKind hcn_open_network_exact(const GUID *id, void **out_handle,
                                     HRESULT *out_hr)
{
    (void)id;
    if (out_handle)
        *out_handle = NULL;
    if (out_hr)
        *out_hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (g_signal_cancel_on_open && g_probe_cancel_event)
        SetEvent(g_probe_cancel_event);
    return HCN_LOOKUP_NOT_FOUND;
}

HRESULT hcn_query_network_properties(void *network, PWSTR *out_json)
{
    (void)network;
    if (out_json)
        *out_json = NULL;
    return E_NOTIMPL;
}

HRESULT hcn_scan_network_candidates(const HcnOwnedIdEntry *owned,
                                    size_t owned_count,
                                    HcnCandidateScan *scan)
{
    (void)owned;
    (void)owned_count;
    ZeroMemory(scan, sizeof(*scan));
    scan->state = HCN_SCAN_NOT_AVAILABLE;
    scan->failure_hr = E_NOTIMPL;
    return E_NOTIMPL;
}

void hcn_candidate_scan_free(HcnCandidateScan *scan)
{
    if (scan && scan->records)
        HeapFree(GetProcessHeap(), 0, scan->records);
    if (scan)
        ZeroMemory(scan, sizeof(*scan));
}

HRESULT build_adapter_inventory(HcnAdapterInventory *inventory)
{
    ZeroMemory(inventory, sizeof(*inventory));
    return E_NOTIMPL;
}

void adapter_inventory_free(HcnAdapterInventory *inventory)
{
    if (inventory && inventory->entries)
        HeapFree(GetProcessHeap(), 0, inventory->entries);
    if (inventory)
        ZeroMemory(inventory, sizeof(*inventory));
}

HRESULT inventory_overlay_ipv4_gateways(HcnAdapterEntry *entries, size_t count)
{
    (void)entries;
    (void)count;
    return E_NOTIMPL;
}

TopologyResultKind wmi_classify_session_failure(HRESULT hr)
{
    (void)hr;
    return TOPOLOGY_ERROR;
}

TopologyResultKind wmi_topology_switch_for_external_adapter(
    const WmiTopology *topology, const GUID *adapter_guid,
    const WmiSwitchEntry **out_switch)
{
    (void)topology;
    (void)adapter_guid;
    if (out_switch)
        *out_switch = NULL;
    return TOPOLOGY_ERROR;
}

WmiProviderStatus wmi_probe_provider_installed(void)
{
    return WMI_PROVIDER_ERROR;
}

static BOOL test_filtered_view_and_final_cancel(void)
{
    HcnInternalCensusRequest req;
    HcnInternalSwitchCensus census;
    HcnInternalResolution resolution;
    HRESULT hr;
    LONG builders;
    HANDLE cancel_event;

    ZeroMemory(&req, sizeof(req));
    ZeroMemory(&census, sizeof(census));
    g_mock_host_visible = TRUE;
    g_mock_token_unfiltered = FALSE;
    g_mock_census_mode = MOCK_CENSUS_EMPTY;
    builders = InterlockedCompareExchange(&g_mock_builder_calls, 0, 0);

    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    CHECK(census.state == HCN_CENSUS_UNAVAILABLE);
    CHECK(census.count == 0 && census.entries == NULL);
    CHECK(InterlockedCompareExchange(&g_mock_builder_calls, 0, 0) == builders);
    hcn_internal_resolve_selector(&census, L"Missing", &resolution);
    CHECK(resolution.outcome == HCN_IRES_REJECT);
    CHECK(resolution.reason_code == HCN_IR_INVENTORY_UNAVAILABLE);
    CHECK(IsEqualGUID(&resolution.probe_guid, &GUID_NULL));

    /* A visible host plus a qualified token can publish a genuine EMPTY
       census; the same selector then resolves to definite not-found. */
    g_mock_token_unfiltered = TRUE;
    g_mock_builder_calls = 0;
    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == S_OK);
    CHECK(census.state == HCN_CENSUS_EMPTY);
    CHECK(InterlockedCompareExchange(&g_mock_builder_calls, 0, 0) == 1);
    hcn_internal_resolve_selector(&census, L"Missing", &resolution);
    CHECK(resolution.outcome == HCN_IRES_REJECT);
    CHECK(resolution.reason_code == HCN_IR_NOT_FOUND);
    hcn_internal_switch_census_free(&census);

    /* Signal cancellation from inside the only/final fake HCN open. The
       production post-probe check must return ABORTED and free the result. */
    g_mock_census_mode = MOCK_CENSUS_ONE_INTERNAL;
    cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(cancel_event != NULL);
    req.probe_budget = 1;
    req.cancel_event = cancel_event;
    g_probe_cancel_event = cancel_event;
    g_signal_cancel_on_open = TRUE;
    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
    CHECK(census.state == HCN_CENSUS_UNAVAILABLE);
    CHECK(census.count == 0 && census.entries == NULL);
    g_signal_cancel_on_open = FALSE;
    g_probe_cancel_event = NULL;
    CloseHandle(cancel_event);
    return TRUE;
}

static BOOL test_fresh_census_deadline_policies(void)
{
    HcnInternalCensusRequest req;
    HcnInternalSwitchCensus census;
    HcnInternalNetworkRef network_ref;
    wchar_t reason[256];
    HANDLE cancel_event;
    HRESULT hr;

    ZeroMemory(&req, sizeof(req));
    ZeroMemory(&census, sizeof(census));
    g_mock_host_visible = TRUE;
    g_mock_token_unfiltered = TRUE;
    g_mock_census_mode = MOCK_CENSUS_EMPTY;
    g_mock_session_open_calls = 0;
    g_mock_session_close_calls = 0;
    g_mock_topology_calls = 0;
    g_mock_builder_calls = 0;
    g_mock_session_open_delay_ms = 40;
    g_mock_topology_delay_ms = 0;

    cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(cancel_event != NULL);
    req.cancel_event = cancel_event;

    /* The UI census carries its cancel event and uses one deadline that
       starts before session setup and remains unchanged for both builds. */
    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == S_OK);
    CHECK(g_mock_topology_calls == 1 && g_mock_builder_calls == 1);
    CHECK(g_mock_session_open_calls == 1 && g_mock_session_close_calls == 1);
    CHECK(g_mock_topology_cancel_event == cancel_event &&
          g_mock_census_cancel_event == cancel_event);
    CHECK(g_mock_topology_deadline == g_mock_census_deadline);
    CHECK(g_mock_topology_deadline <
          g_mock_session_open_end + WMI_CENSUS_DEADLINE_MS);
    hcn_internal_switch_census_free(&census);
    CloseHandle(cancel_event);

    /* Explicit acquire uses a new WMI session with no UI cancel event;
       its topology deadline begins after session setup and its census
       deadline is reset after topology completes. The empty census
       rejects Missing before any HCN probe. */
    ZeroMemory(&network_ref, sizeof(network_ref));
    reason[0] = L'\0';
    g_mock_session_open_calls = 0;
    g_mock_session_close_calls = 0;
    g_mock_topology_calls = 0;
    g_mock_builder_calls = 0;
    g_mock_topology_delay_ms = 40;

    hr = hcn_acquire_internal_network(L"Missing", &network_ref,
                                      reason, ARRAYSIZE(reason));
    CHECK(hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    CHECK(g_mock_topology_calls == 1 && g_mock_builder_calls == 1);
    CHECK(g_mock_session_open_calls == 1 && g_mock_session_close_calls == 1);
    CHECK(g_mock_topology_cancel_event == NULL &&
          g_mock_census_cancel_event == NULL);
    CHECK(g_mock_topology_deadline >=
          g_mock_session_open_end + WMI_DEFAULT_DEADLINE_MS);
    CHECK(g_mock_census_deadline > g_mock_topology_deadline);
    CHECK(network_ref.network_id.Data1 == 0 && reason[0] != L'\0');

    g_mock_session_open_delay_ms = 0;
    g_mock_topology_delay_ms = 0;
    return TRUE;
}

static void reset_census_build_mocks(void)
{
    g_mock_session_open_calls = 0;
    g_mock_session_close_calls = 0;
    g_mock_topology_free_calls = 0;
    g_mock_topology_calls = 0;
    g_mock_builder_calls = 0;
    g_mock_session_open_delay_ms = 0;
    g_mock_topology_delay_ms = 0;
    g_mock_session_open_result = S_OK;
    g_mock_topology_result = S_OK;
    g_mock_census_result = S_OK;
    g_mock_partial_census_failure = FALSE;
    g_mock_host_visible = TRUE;
    g_mock_token_unfiltered = TRUE;
    g_mock_census_mode = MOCK_CENSUS_EMPTY;
}

static BOOL census_is_unavailable_and_empty(const HcnInternalSwitchCensus *census)
{
    return census->state == HCN_CENSUS_UNAVAILABLE &&
           census->reason_code == HCN_IR_INVENTORY_UNAVAILABLE &&
           census->count == 0 && census->entries == NULL;
}

static BOOL test_fresh_census_failure_cleanup(void)
{
    HcnInternalCensusRequest req;
    HcnInternalSwitchCensus census;
    HRESULT hr;

    ZeroMemory(&req, sizeof(req));
    ZeroMemory(&census, sizeof(census));
    reset_census_build_mocks();

    /* Session-open failure owns no session handle and must not close it. */
    g_mock_session_open_result = E_FAIL;
    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == E_FAIL);
    CHECK(g_mock_session_open_calls == 1 && g_mock_session_close_calls == 0);
    CHECK(g_mock_topology_calls == 0 && g_mock_builder_calls == 0);
    CHECK(census_is_unavailable_and_empty(&census));

    /* Once open succeeds, topology failure still closes that session once. */
    hcn_internal_switch_census_free(&census);
    reset_census_build_mocks();
    g_mock_topology_result = E_FAIL;
    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == E_FAIL);
    CHECK(g_mock_session_open_calls == 1 && g_mock_session_close_calls == 1);
    CHECK(g_mock_topology_calls == 1 && g_mock_builder_calls == 0);
    CHECK(census_is_unavailable_and_empty(&census));

    /* A census builder may fail after allocating partial entries. The
       wrapper must free the topology and session, then publish only the
       free-able UNAVAILABLE shape. */
    hcn_internal_switch_census_free(&census);
    reset_census_build_mocks();
    g_mock_census_result = E_FAIL;
    g_mock_partial_census_failure = TRUE;
    hr = hcn_enum_internal_switches(&req, &census);
    CHECK(hr == E_FAIL);
    CHECK(g_mock_session_open_calls == 1 && g_mock_session_close_calls == 1);
    CHECK(g_mock_topology_calls == 1 && g_mock_builder_calls == 1);
    CHECK(g_mock_topology_free_calls == 1);
    CHECK(census_is_unavailable_and_empty(&census));
    hcn_internal_switch_census_free(&census);

    reset_census_build_mocks();
    return TRUE;
}

static void init_projection_entry(HcnInternalSwitchEntry *entry,
                                  DWORD id,
                                  HcnSwitchClass sw_class,
                                  const wchar_t *name)
{
    ZeroMemory(entry, sizeof(*entry));
    entry->switch_id = make_guid(id);
    entry->sw_class = sw_class;
    entry->verdict = HCN_CAND_UNKNOWN;
    wcscpy_s(entry->name, INTERNAL_SWITCH_CAP, name);
}

static BOOL test_projection_reasons(void)
{
    HcnInternalSwitchCensus census;
    HcnInternalSwitchEntry *entries;

    ZeroMemory(&census, sizeof(census));
    census.state = HCN_CENSUS_OK;
    census.has_unknown_owner_failure = TRUE;
    census.count = 6;
    entries = (HcnInternalSwitchEntry *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        census.count * sizeof(HcnInternalSwitchEntry));
    CHECK(entries != NULL);
    census.entries = entries;

    init_projection_entry(&entries[0], 1, HCN_SW_EXTERNAL, L"External");
    init_projection_entry(&entries[1], 2, HCN_SW_UNCLASSIFIED, L"Unclassified");
    init_projection_entry(&entries[2], 3, HCN_SW_INTERNAL, L"Duplicate");
    init_projection_entry(&entries[3], 4, HCN_SW_INTERNAL, L"Duplicate");
    init_projection_entry(&entries[4], 5, HCN_SW_INTERNAL, L"ProbeRejected");
    init_projection_entry(&entries[5], 6, HCN_SW_PRIVATE, L"Private");
    entries[4].verdict = HCN_CAND_REJECTED;
    entries[4].reason_code = HCN_IR_UNSUPPORTED_TYPE;
    format_reason(HCN_IR_UNSUPPORTED_TYPE, HCN_RS_CONCISE, L"ProbeRejected",
                  0, 0, NULL, entries[4].reason_text,
                  ARRAYSIZE(entries[4].reason_text));

    internal_project_census(&census);

    CHECK(wcscmp(entries[0].reason_text, L"external switch") == 0);
    CHECK(wcscmp(entries[1].reason_text, L"classification incomplete") == 0);
    CHECK(wcscmp(entries[2].reason_text, L"name duplicated") == 0);
    CHECK(wcscmp(entries[3].reason_text, L"name duplicated") == 0);
    CHECK(wcscmp(entries[4].reason_text, L"unsupported type") == 0);
    CHECK(entries[5].defer_eligible == TRUE);
    CHECK(entries[0].defer_eligible == FALSE &&
          entries[1].defer_eligible == FALSE &&
          entries[2].defer_eligible == FALSE &&
          entries[3].defer_eligible == FALSE &&
          entries[4].defer_eligible == FALSE);
    CHECK(entries[0].selectable == FALSE && entries[1].selectable == FALSE &&
          entries[2].selectable == FALSE && entries[3].selectable == FALSE &&
          entries[4].selectable == FALSE && entries[5].selectable == TRUE);

    hcn_internal_switch_census_free(&census);
    return TRUE;
}

int wmain(void)
{
    if (!test_owned_delete_wrappers() ||
        !test_delete_lock_boundary() ||
        !test_filtered_view_and_final_cancel() ||
        !test_fresh_census_deadline_policies() ||
        !test_fresh_census_failure_cleanup() ||
        !test_projection_reasons())
        return 1;

    wprintf(L"PASS: owned-delete wrappers, lock boundary, census authority/cancel/deadlines, projection reasons\n");
    return 0;
}
