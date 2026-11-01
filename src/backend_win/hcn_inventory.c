/* hcn_inventory.c - adapter and network inventory (the read layer).
 *
 * The L0 physical adapter inventory (NDIS interface rows merged with
 * SetupAPI driver identity, registry connection names, gateway
 * overlay), the HCN open/query primitives with exact absence
 * classification, and the read-only candidate network scan.
 * Everything here gathers facts; the acquire ladder lives in
 * hcn_external.c, topology attribution in hcn_wmi.c. */

#include "hcn_network.h"
#include "hcn_private.h"
#include "ui.h"
#include <objbase.h>
#include <setupapi.h>
#include <devguid.h>
#include <string.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")

/* ---- L0 read-only adapter inventory ----
 *
 * The authoritative adapter inventory comes from NDIS interface rows
 * (GetIfTable2) merged with SetupAPI driver identity. This is required
 * for correctness: a physical adapter owned by an external vSwitch has
 * no TCPIP binding, so it is invisible to GetAdaptersAddresses
 * (verified on this topology: a bound physical NIC has an NDIS
 * interface but no IPv4 interface row). FriendlyName (connection name)
 * comes from the registry Connection\Name value keyed by the device's
 * NetCfgInstanceId. All identity comparisons are binary InterfaceGuid;
 * FriendlyName is only a user selector and log field.
 *
 * The inventory records every interface row (the full table serves
 * foreign binding-evidence translation); is_hardware_eligible
 * marks the physical-adapter subset (selection, sweep set, UI enum). */


void adapter_inventory_free(HcnAdapterInventory *inv)
{
    if (inv && inv->entries) {
        HeapFree(GetProcessHeap(), 0, inv->entries);
        inv->entries = NULL;
    }
    if (inv)
        inv->count = 0;
}

/* Read a REG_SZ/REG_EXPAND_SZ value into out (null-terminated). */
static BOOL reg_read_sz(HKEY key, const wchar_t *value_name,
                        wchar_t *out, DWORD out_chars)
{
    DWORD size_bytes = (out_chars - 1) * sizeof(wchar_t);
    DWORD type = 0;
    LONG rc;

    if (out_chars == 0)
        return FALSE;
    out[0] = L'\0';
    rc = RegQueryValueExW(key, value_name, NULL, &type, (BYTE *)out, &size_bytes);
    if (rc != ERROR_SUCCESS)
        return FALSE;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return FALSE;
    out[size_bytes / sizeof(wchar_t)] = L'\0';
    return out[0] != L'\0';
}


/* Positive Hyper-V management-vNIC identity: driver ComponentId is
 * exactly vms_mp. FriendlyName (vEthernet prefix) is NEVER used as
 * identity evidence. */
static BOOL adapter_is_mgmt_vnic(const HcnAdapterEntry *e)
{
    return e->has_component_id && _wcsicmp(e->component_id, L"vms_mp") == 0;
}

/* Overlay SetupAPI PnP identity (NetCfgInstanceId -> ComponentId) and the
 * registry connection name onto the NDIS interface inventory. Read-only
 * driver-registry access; every handle is closed on every exit path. */
static void inventory_overlay_pnp_identity(HcnAdapterEntry *entries, size_t count)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_data;
    DWORD idx;

    devs = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE)
        return;

    dev_data.cbSize = sizeof(SP_DEVINFO_DATA);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_data); idx++) {
        HKEY drv_key;
        wchar_t netcfg[80];
        GUID guid;
        HcnAdapterEntry *entry;

        drv_key = SetupDiOpenDevRegKey(devs, &dev_data, DICS_FLAG_GLOBAL, 0,
                                       DIREG_DRV, KEY_READ);
        if (drv_key == INVALID_HANDLE_VALUE)
            continue;

        if (!reg_read_sz(drv_key, L"NetCfgInstanceId", netcfg, ARRAYSIZE(netcfg)) ||
            !parse_guid_value(netcfg, wcslen(netcfg), &guid)) {
            RegCloseKey(drv_key);
            continue;
        }

        entry = inventory_entry_by_guid(entries, count, &guid);
        if (!entry) {
            RegCloseKey(drv_key);
            continue;
        }

        if (reg_read_sz(drv_key, L"ComponentId", entry->component_id,
                        ARRAYSIZE(entry->component_id)))
            entry->has_component_id = TRUE;
        RegCloseKey(drv_key);

        /* Connection (friendly) name:
         * HKLM\SYSTEM\CCS\Control\Network\{devclass}\<NetCfgInstanceId>\Connection\Name */
        {
            wchar_t guid_str[64];
            wchar_t class_str[64];
            wchar_t path[256];
            HKEY conn_key;

            StringFromGUID2(&guid, guid_str, ARRAYSIZE(guid_str));
            StringFromGUID2(&GUID_DEVCLASS_NET, class_str, ARRAYSIZE(class_str));
            swprintf_s(path, ARRAYSIZE(path),
                L"SYSTEM\\CurrentControlSet\\Control\\Network\\%s\\%s\\Connection",
                class_str, guid_str);
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ,
                              &conn_key) == ERROR_SUCCESS) {
                wchar_t name[HCN_PHYS_NAME_MAX];
                if (reg_read_sz(conn_key, L"Name", name, ARRAYSIZE(name))) {
                    wcscpy_s(entry->friendly_name, HCN_PHYS_NAME_MAX, name);
                    entry->name_is_alias = FALSE;
                }
                RegCloseKey(conn_key);
            }
        }
    }

    SetupDiDestroyDeviceInfoList(devs);
}

/* Mark the hardware-eligible subset: loopback, tunnel,
 * vms_mp management adapters, and positively software-only interfaces
 * (HardwareInterface=false) are ineligible. One flag on one inventory,
 * not a second filtered array. */
static void inventory_mark_eligibility(HcnAdapterEntry *entries, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        HcnAdapterEntry *e = &entries[i];

        e->is_hardware_eligible =
            e->hw_interface &&
            e->if_type != IF_TYPE_SOFTWARE_LOOPBACK &&
            e->if_type != IF_TYPE_TUNNEL &&
            !adapter_is_mgmt_vnic(e);
    }
}

/* Build the adapter inventory from NDIS rows + PnP identity. The result
   holds every interface row; the hardware subset is the
   is_hardware_eligible flag. Failure returns the originating error -
   never a successful empty inventory. */
HRESULT build_adapter_inventory(HcnAdapterInventory *inv)
{
    PMIB_IF_TABLE2 table = NULL;
    HcnAdapterEntry *entries = NULL;
    size_t count = 0;
    DWORD i;
    DWORD ret;

    inv->entries = NULL;
    inv->count = 0;

    ret = GetIfTable2(&table);
    if (ret != NO_ERROR)
        return HRESULT_FROM_WIN32(ret);

    if (table->NumEntries > 0) {
        entries = (HcnAdapterEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                               table->NumEntries * sizeof(HcnAdapterEntry));
        if (!entries) {
            FreeMibTable(table);
            return E_OUTOFMEMORY;
        }

        for (i = 0; i < table->NumEntries; i++) {
            MIB_IF_ROW2 *row = &table->Table[i];
            HcnAdapterEntry *e = &entries[count];

            e->luid = row->InterfaceLuid;
            e->ifindex = row->InterfaceIndex;
            e->interface_guid = row->InterfaceGuid;
            e->if_type = row->Type;
            e->oper_status = row->OperStatus;
            e->not_media_connected =
                row->InterfaceAndOperStatusFlags.NotMediaConnected;
            e->hw_interface = row->InterfaceAndOperStatusFlags.HardwareInterface;
            e->mac_len = row->PhysicalAddressLength;
            if (e->mac_len > IF_MAX_PHYS_ADDRESS_LENGTH)
                e->mac_len = IF_MAX_PHYS_ADDRESS_LENGTH;
            if (e->mac_len > 0)
                memcpy(e->mac, row->PhysicalAddress, e->mac_len);
            /* Connection name is overlaid from the registry below; start
               with the NDIS alias so every entry stays loggable. */
            wcsncpy_s(e->friendly_name, HCN_PHYS_NAME_MAX, row->Alias, _TRUNCATE);
            e->name_is_alias = TRUE;
            wcsncpy_s(e->description, HCN_PHYS_DESC_MAX, row->Description, _TRUNCATE);
            count++;
        }
    }

    FreeMibTable(table);

    inventory_overlay_pnp_identity(entries, count);
    inventory_mark_eligibility(entries, count);

    inv->entries = entries;
    inv->count = count;
    return S_OK;
}

/* Overlay the physical IPv4 gateway predicate by InterfaceGuid: GAA's
   ANSI AdapterName is the interface GUID string. Retains the existing
   gateway interpretation (FirstGatewayAddress != NULL); no Internet
   probe, no default-route lookup, no GetIpForwardTable2. A GAA failure
   returns the originating error - required-GAA failure is an acquire
   error, never a silently empty overlay (the acquire context decides
   whether it requires this step; the UI enumeration does not). */
HRESULT inventory_overlay_ipv4_gateways(HcnAdapterEntry *entries, size_t count)
{
    ULONG buf_len = 15000;
    PIP_ADAPTER_ADDRESSES addrs, cur;
    DWORD ret;

    addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
    if (!addrs)
        return E_OUTOFMEMORY;

    ret = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        NULL, addrs, &buf_len);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        HeapFree(GetProcessHeap(), 0, addrs);
        addrs = (PIP_ADAPTER_ADDRESSES)HeapAlloc(GetProcessHeap(), 0, buf_len);
        if (!addrs)
            return E_OUTOFMEMORY;
        ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
            NULL, addrs, &buf_len);
    }
    if (ret != ERROR_SUCCESS) {
        HeapFree(GetProcessHeap(), 0, addrs);
        return HRESULT_FROM_WIN32(ret);
    }

    for (cur = addrs; cur != NULL; cur = cur->Next) {
        wchar_t wname[80];
        GUID guid;
        HcnAdapterEntry *e;
        size_t j, n;

        if (!cur->AdapterName)
            continue;
        n = strlen(cur->AdapterName);
        if (n >= ARRAYSIZE(wname))
            continue;
        for (j = 0; j <= n; j++)   /* include the NUL */
            wname[j] = (wchar_t)(unsigned char)cur->AdapterName[j];
        if (!parse_guid_value(wname, n, &guid))
            continue;

        e = inventory_entry_by_guid(entries, count, &guid);
        if (e)
            e->has_ipv4_gateway = (cur->FirstGatewayAddress != NULL);
    }

    HeapFree(GetProcessHeap(), 0, addrs);
    return S_OK;
}

/* ---- HCN open/query helpers (acquisition paths) ---- */

/* Exact HCN network-absence code (from the HCN error family). Only this
   HRESULT from the actual open operation means "network does not exist";
   access denied, service/RPC failures and unknown codes are hard errors,
   never absence. */
#ifndef HCN_E_NETWORK_NOT_FOUND
#define HCN_E_NETWORK_NOT_FOUND ((HRESULT)0x803B0001L)
#endif

/* Free an HCN-returned string (properties JSON, enumeration result, error
   record) from the acquisition/query paths, per the HCN API memory
   contract. */
void hcn_free_string(PWSTR s)
{
    if (s) CoTaskMemFree(s);
}

static BOOL hcn_is_network_not_found(HRESULT hr)
{
    return hr == HCN_E_NETWORK_NOT_FOUND;
}

/* Open a network by exact GUID and classify the outcome. On
   HCN_LOOKUP_FOUND, *out_handle owns an open network handle the caller
   must close with pfnCloseNet. On error the original HRESULT is logged
   with the HCN error record (if any) and returned via *out_hr, so callers
   can propagate the original failure verbatim. */
HcnLookupKind hcn_open_network_exact(const GUID *id, void **out_handle,
                                            HRESULT *out_hr)
{
    void *network = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;
    wchar_t guid_str[64];

    *out_handle = NULL;
    if (out_hr) *out_hr = S_OK;

    if (!pfnOpenNet || !pfnCloseNet) {
        if (out_hr) *out_hr = E_NOT_VALID_STATE;
        return HCN_LOOKUP_ERROR;
    }

    hr = pfnOpenNet(id, &network, &error_record);
    if (SUCCEEDED(hr) && network) {
        hcn_free_string(error_record);
        *out_handle = network;
        return HCN_LOOKUP_FOUND;
    }
    if (SUCCEEDED(hr) && !network) {
        /* S_OK with a NULL handle is a contract failure - neither
           success, absence, nor a propagable S_OK. */
        ui_log(L"External: HcnOpenNetwork returned success without a network handle.");
        hcn_free_string(error_record);
        if (out_hr) *out_hr = E_FAIL;
        return HCN_LOOKUP_ERROR;
    }
    /* Failing call returned a handle after all: release it defensively. */
    if (network && pfnCloseNet) {
        pfnCloseNet(network);
        network = NULL;
    }

    guid_to_string(id, guid_str, 64);
    if (hcn_is_network_not_found(hr)) {
        hcn_free_string(error_record);
        return HCN_LOOKUP_NOT_FOUND;
    }

    /* Hard error: log the original HRESULT (and error record) and keep it
       for the caller. Never fold this into NOT_FOUND. */
    ui_log(L"External: HcnOpenNetwork(%s) failed (0x%08X).", guid_str, hr);
    if (error_record) {
        ui_log(L"External: HCN error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (out_hr) *out_hr = hr;
    return HCN_LOOKUP_ERROR;
}

/* Query properties JSON for an open network handle. On success the caller
   owns *out_json and must free it with hcn_free_string(). The HCN error
   record, if any, is freed on every exit path - it is diagnostic output
   the caller never owns. */
HRESULT hcn_query_network_properties(void *network, PWSTR *out_json)
{
    PWSTR json = NULL;
    PWSTR error_record = NULL;
    HRESULT hr;

    *out_json = NULL;

    if (!pfnQueryNetProps)
        return E_NOTIMPL; /* capability checked separately by callers */

    hr = pfnQueryNetProps(network, NULL, &json, &error_record);
    if (FAILED(hr)) {
        ui_log(L"External: HcnQueryNetworkProperties failed (0x%08X).", hr);
        if (error_record) {
            ui_log(L"External: HCN error: %s", error_record);
            hcn_free_string(error_record);
        }
        hcn_free_string(json);
        return hr;
    }
    if (error_record) {
        /* A successful call may still carry an error record: log and free
           it; the caller only owns the properties JSON. */
        ui_log(L"External: HCN error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (!json) {
        ui_log(L"External: HcnQueryNetworkProperties returned no properties (0x%08X).", hr);
        return E_FAIL;
    }

    *out_json = json;
    return S_OK;
}

/* ---- HCN candidate scan: four states, per-object degradation ----
 *
 * One read-only enumeration pass. List-level states: NOT_AVAILABLE (the
 * enumerate export is unresolved - the acquire context decides this once
 * before the walk; this is the defensive in-scan decision), FAILED (the
 * HcnEnumerateNetworks call fails or its result document does not parse
 * completely - a read-layer failure), COMPLETE (call and document
 * succeeded). Within COMPLETE, each enumerated network is recorded:
 * a hard open/query/parse failure or an enumerated-vs-queried ID
 * mismatch degrades that ID to unproven (a possible occupant that cannot
 * be located) and the scan continues; an exact HCN_E_NETWORK_NOT_FOUND
 * on open is a vanished ID - not an occupant, but the list named a
 * nonexistent object so enumeration_complete becomes FALSE. Retain the
 * usable records alongside the flags; never pretend the scan was empty.
 * Duplicate enumerated IDs are one object (dedup) and do not reduce
 * completeness. Derived identity (owned_id set membership, or the
 * owned-Name + actual-ID equality for a NIC that left L0) is marked per
 * record for the per-target classification; the scan consumes the
 * caller-provided owned-ID set and never computes it. */

HRESULT hcn_scan_network_candidates(const HcnOwnedIdEntry *owned,
                                    size_t owned_count,
                                    HcnCandidateScan *scan)
{
    PWSTR enum_json = NULL;
    PWSTR error_record = NULL;
    GUID *ids = NULL;
    size_t id_count = 0;
    HRESULT hr;
    size_t i;

    ZeroMemory(scan, sizeof(*scan));
    scan->state = HCN_SCAN_FAILED;
    scan->failure_hr = E_NOT_VALID_STATE;

    if (!pfnEnumNet || !pfnOpenNet || !pfnCloseNet || !pfnQueryNetProps) {
        ui_log(L"External: HCN enumeration/query capability is not available; "
               L"scan not run (NOT_AVAILABLE).");
        scan->state = HCN_SCAN_NOT_AVAILABLE;
        return E_NOT_VALID_STATE;
    }

    hr = pfnEnumNet(NULL, &enum_json, &error_record);
    if (error_record) {
        ui_log(L"External: HcnEnumerateNetworks error: %s", error_record);
        hcn_free_string(error_record);
    }
    if (FAILED(hr)) {
        /* A failed call that still returned a result document owns it:
           free defensively, never leak it. */
        hcn_free_string(enum_json);
        scan->failure_hr = hr;
        return hr;
    }
    if (!enum_json) {
        ui_log(L"External: HcnEnumerateNetworks returned no result document.");
        scan->failure_hr = E_FAIL;
        return E_FAIL;
    }

    hr = parse_hcn_network_id_array(enum_json, &ids, &id_count);
    hcn_free_string(enum_json);
    if (FAILED(hr)) {
        ui_log(L"External: HCN enumeration result could not be parsed completely (0x%08X).",
               hr);
        scan->failure_hr = hr;
        return hr;
    }

    scan->total_enumerated = id_count;
    /* The call and the document succeeded; a vanished entry clears it. */
    scan->enumeration_complete = TRUE;

    if (id_count > 0) {
        scan->records = (HcnScanRecord *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                   id_count * sizeof(HcnScanRecord));
        if (!scan->records) {
            free(ids);
            scan->failure_hr = E_OUTOFMEMORY;
            return E_OUTOFMEMORY;
        }
    }

    for (i = 0; i < id_count; i++) {
        void *network = NULL;
        PWSTR json = NULL;
        HcnLookupKind open_kind;
        HRESULT open_hr = S_OK;
        HcnNetworkProps props;
        HcnScanRecord *rec;
        size_t j;
        BOOL dup = FALSE;

        /* Dedup: a duplicate enumerated ID is one object. */
        for (j = 0; j < scan->count; j++) {
            if (IsEqualGUID(&scan->records[j].enumerated_id, &ids[i])) {
                dup = TRUE;
                break;
            }
        }
        if (dup) {
            wchar_t q[64];
            guid_to_string(&ids[i], q, 64);
            ui_log(L"External: duplicate ID %s in the enumeration result; "
                   L"treated as one object.", q);
            continue;
        }

        rec = &scan->records[scan->count];
        ZeroMemory(rec, sizeof(*rec));
        rec->enumerated_id = ids[i];
        rec->state = HCN_REC_UNPROVEN;

        /* Derived identity from the caller-provided owned-ID set: the
           enumerated ID is the network's identity, so this needs no
           properties query and holds regardless of query success. */
        for (j = 0; j < owned_count; j++) {
            if (IsEqualGUID(&ids[i], &owned[j].owned_id)) {
                rec->has_derived_identity = TRUE;
                rec->derived_nic_guid = owned[j].nic_guid;
                break;
            }
        }

        open_kind = hcn_open_network_exact(&ids[i], &network, &open_hr);
        if (open_kind == HCN_LOOKUP_ERROR) {
            /* Hard error (not exact not-found): this ID is unproven and
               the scan continues (per-object degradation). */
            swprintf_s(rec->degrade_reason, ARRAYSIZE(rec->degrade_reason),
                       L"open failed (0x%08X)", open_hr);
            scan->unproven_count++;
            scan->count++;
            continue;
        }
        if (open_kind == HCN_LOOKUP_NOT_FOUND) {
            /* The enumeration listed a nonexistent object: not an
               occupant, but the list is untrustworthy once it names
               objects that do not exist. */
            wchar_t q[64];
            guid_to_string(&ids[i], q, 64);
            ui_log(L"External: enumerated network %s vanished before open; "
                   L"HCN inventory changed during the scan.", q);
            rec->state = HCN_REC_VANISHED;
            scan->vanished_count++;
            scan->enumeration_complete = FALSE;
            scan->count++;
            continue;
        }

        hr = hcn_query_network_properties(network, &json);
        if (FAILED(hr)) {
            if (network && pfnCloseNet) pfnCloseNet(network);
            swprintf_s(rec->degrade_reason, ARRAYSIZE(rec->degrade_reason),
                       L"properties query failed (0x%08X)", hr);
            scan->unproven_count++;
            scan->count++;
            continue;
        }

        hr = parse_hcn_network_properties(json, &props);
        hcn_free_string(json);
        if (network && pfnCloseNet) pfnCloseNet(network);
        if (FAILED(hr)) {
            ui_log(L"External: network properties malformed for an enumerated "
                   L"network (0x%08X); recorded unproven.", hr);
            swprintf_s(rec->degrade_reason, ARRAYSIZE(rec->degrade_reason),
                       L"properties unparsable (0x%08X)", hr);
            scan->unproven_count++;
            scan->count++;
            continue;
        }

        if (!props.has_id || !IsEqualGUID(&props.id, &ids[i])) {
            wchar_t q[64], a[64];
            guid_to_string(&ids[i], q, 64);
            if (props.has_id)
                guid_to_string(&props.id, a, 64);
            else
                wcscpy_s(a, 64, L"(missing)");
            ui_log(L"External: HCN query identity mismatch (queried %s, properties "
                   L"report %s); recorded unproven.", q, a);
            wcscpy_s(rec->degrade_reason, ARRAYSIZE(rec->degrade_reason),
                     L"identity mismatch");
            scan->unproven_count++;
            scan->count++;
            continue;
        }

        if (!props.has_type || props.type_truncated) {
            ui_log(L"External: network Type missing or truncated; recorded unproven.");
            wcscpy_s(rec->degrade_reason, ARRAYSIZE(rec->degrade_reason),
                     L"Type missing or truncated");
            scan->unproven_count++;
            scan->count++;
            continue;
        }

        rec->state = HCN_REC_COMPLETE;
        rec->props = props;
        rec->transparent = (_wcsicmp(props.type, L"Transparent") == 0);
        if (rec->transparent)
            scan->transparent_count++;

        /* Name-derived identity, covering a NIC that has left L0:
           a Name that strictly parses to G with the actual ID equal to
           owned_id(G). Derived identity outranks a contradicting parsed
           binding; the per-target classification consumes the mark. */
        if (!rec->has_derived_identity && props.has_name && !props.name_truncated) {
            GUID nic_g, oid;
            if (hcn_external_parse_owned_name(props.name, &nic_g) &&
                SUCCEEDED(hcn_external_owned_id(&nic_g, &oid)) &&
                IsEqualGUID(&oid, &props.id)) {
                rec->has_derived_identity = TRUE;
                rec->derived_nic_guid = nic_g;
            }
        }
        scan->count++;
    }

    free(ids);

    scan->state = HCN_SCAN_COMPLETE;
    scan->failure_hr = S_OK;
    ui_log(L"External: HCN enumeration pass complete; networks=%lu, transparent=%lu, "
           L"unproven=%lu, vanished=%lu, complete=%d.",
           (unsigned long)scan->total_enumerated,
           (unsigned long)scan->transparent_count,
           (unsigned long)scan->unproven_count,
           (unsigned long)scan->vanished_count,
           scan->enumeration_complete ? 1 : 0);
    return S_OK;
}

void hcn_candidate_scan_free(HcnCandidateScan *scan)
{
    if (!scan)
        return;
    if (scan->records) {
        HeapFree(GetProcessHeap(), 0, scan->records);
        scan->records = NULL;
    }
    scan->count = 0;
    scan->total_enumerated = 0;
    scan->vanished_count = 0;
    scan->unproven_count = 0;
    scan->transparent_count = 0;
    scan->enumeration_complete = FALSE;
    scan->state = HCN_SCAN_FAILED;
    scan->failure_hr = E_NOT_VALID_STATE;
}

int hcn_enum_adapters(HcnAdapterCallback cb, void *ctx)
{
    /* List eligible L0 physical NICs without WMI, gateway, or Up
       filtering; bound NICs remain visible. FriendlyName is the Explicit
       label/key. An entire same-name group is omitted from selectable
       entries here at the backend - a persisted lookup by FriendlyName
       must stay unambiguous, and no suffixes are manufactured to make
       stored names unique - so every consumer emits the same safe list.
       On inventory failure the list is empty: the UI shows only (Auto)
       The dropdown can consequently show fewer NICs than Auto
       walks; Auto still walks the omitted NICs. */
    HcnAdapterInventory inv;
    int count = 0;
    size_t i, j;
    HRESULT hr;

    ZeroMemory(&inv, sizeof(inv));
    hr = build_adapter_inventory(&inv);
    if (FAILED(hr)) {
        ui_log(L"External: adapter inventory failed (0x%08X); the adapter "
               L"list is (Auto) only.", hr);
        return 0;
    }

    for (i = 0; i < inv.count; i++) {
        const HcnAdapterEntry *e = &inv.entries[i];
        BOOL seen_earlier = FALSE;
        BOOL dup_group = FALSE;

        if (!e->is_hardware_eligible)
            continue;
        if (!e->friendly_name[0])
            continue;   /* a NIC with no connection name cannot be a key */

        for (j = 0; j < inv.count; j++) {
            const HcnAdapterEntry *o = &inv.entries[j];
            if (j == i)
                continue;
            if (!o->is_hardware_eligible || !o->friendly_name[0])
                continue;
            if (_wcsicmp(o->friendly_name, e->friendly_name) != 0)
                continue;
            if (j < i)
                seen_earlier = TRUE;
            dup_group = TRUE;
        }
        if (dup_group) {
            if (!seen_earlier) {
                /* Log each omitted group once. */
                ui_log(L"External: omitted duplicate-name group from "
                       L"Explicit list: \"%s\".", e->friendly_name);
            }
            continue;
        }

        cb(e->friendly_name, (int)e->if_type, ctx);
        count++;
    }

    adapter_inventory_free(&inv);
    return count;
}
