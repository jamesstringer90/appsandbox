/* selfcheck_tests.c - offline contract self-check for the External
   vSwitch reuse owned-ID data contracts.
 *
 * Golden vectors for the owned-network identity (UUID v5 ID derivation,
   Name emit, strict Name parse) and parser/classifier fixtures for the
   HCN network properties document and its directional binding
   classification. Everything runs offline: no HCN calls, no WMI, no
   host mutation - this binary links only the pure offline contract
   unit, so no acquire, WMI, or HCN-calling code is present in it at
   all (a structural guarantee, not just an unexercised path; the
   host-facing probes live in the parent hcn-selfcheck tool).
 *
 * It compiles the production hcn_parse.c (the pure contract unit:
 * owned identity, GUID tools, document parsing, classification) as its
 * own translation unit, so the UUID v5 helper, the Name emit, the
 * strict Name parse, and the parser/classifier are exercised through
 * the real production code - no duplicate implementation exists
 * anywhere. The production source logs through ui_log, which the
 * shared ui_log_stub.c supplies.
 *
   Exit code 0 only when every check passes; each check prints one
   line with its result. */

#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <objbase.h>
#include "hcn_network.h"
#include "hcn_private.h"
#include "asb_config.h"

static int g_failures = 0;

static void check(BOOL ok, const wchar_t *label)
{
    wprintf(L"%s  %s\n", ok ? L"PASS" : L"FAIL", label);
    if (!ok)
        g_failures++;
}

static void print_guid(const wchar_t *label, const GUID *g)
{
    wchar_t s[64];

    StringFromGUID2(g, s, 64);
    wprintf(L"      %s = %s\n", label, s);
}

/* ---- Parser/classifier fixtures ----
 *
 * The production parser (parse_hcn_network_properties) and directional
 * classifier (classify_binding_values) run against a hand-crafted full
 * interface table plus synthetic documents, and against a verbatim
 * captured foreign document from a Hyper-V Manager external switch
 * (build 26200, empty Policies, LayeredOn HNS-internal GUID - the
 * measured shape). */

static const GUID FIXT_OTHER = {            /* 11223344-5566-7788-99aa-bbccddeeff00 */
    0x11223344, 0x5566, 0x7788,
    { 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 }
};
static const GUID FIXT_X3 = {              /* aaaaaaaa-bbbb-cccc-dddd-eeeeffff0000 */
    0xAAAAAAAA, 0xBBBB, 0xCCCC,
    { 0xDD, 0xDD, 0xEE, 0xEE, 0xFF, 0xFF, 0x00, 0x00 }
};

#define FIXT_T_STR   L"c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f"   /* golden NIC */
#define FIXT_O_STR   L"11223344-5566-7788-99aa-bbccddeeff00"
#define FIXT_RENAMED L"99999999-9999-9999-9999-999999999999"   /* a connection NAME that is GUID-shaped */
#define FIXT_ABSENT  L"88888888-8888-8888-8888-888888888888"   /* matches nothing anywhere */

/* Verbatim capture of network 8764dd00-b7b1-496c-a187-1ba9975314b2
   (read-only; quotes escaped for C). */
static const wchar_t FIXTURE_HYPERV_SWITCH_JSON[] =
    L"{\"ActivityId\":\"60A35708-D66F-4465-81B5-C802A9662159\",\"AdditionalParams\":{},\"CurrentEndpointCount\":3,\"Extensions\":[{\"Id\":\"E7C3B2F0-F3C5-48DF-AF2B-10"
    L"FED6D72E7A\",\"IsEnabled\":false,\"Name\":\"Microsoft Windows Filtering Platform\"},{\"Id\":\"F74F241B-440F-4433-BB28-00F89EAD20D8\",\"IsEnabled\":false,\"Name"
    L"\":\"Microsoft Azure VFP Switch Filter Extension\"},{\"Id\":\"430BDADD-BAB0-41AB-A369-94B67FA5BE0A\",\"IsEnabled\":true,\"Name\":\"Microsoft NDIS Capture\"}],\""
    L"Flags\":2,\"Health\":{\"LastErrorCode\":0,\"LastUpdateTime\":134328525419934878},\"ID\":\"8764DD00-B7B1-496C-A187-1BA9975314B2\",\"IPv6\":false,\"LayeredOn\":\""
    L"543C3933-D6C7-4FC9-A872-371DDD7F689F\",\"MacPools\":[{\"EndMacAddress\":\"00-15-5D-74-6F-FF\",\"StartMacAddress\":\"00-15-5D-74-60-00\"}],\"MaxConcurrentEndpoin"
    L"ts\":4,\"Name\":\"I211\",\"Policies\":[],\"State\":1,\"SwitchGuid\":\"8764DD00-B7B1-496C-A187-1BA9975314B2\",\"TotalEndpoints\":38,\"Type\":\"Transparent\",\"Ve"
    L"rsion\":68719476736,\"Resources\":{\"AdditionalParams\":{},\"AllocationOrder\":0,\"CompartmentOperationTime\":0,\"Flags\":0,\"Health\":{\"LastErrorCode\":0,\"La"
    L"stUpdateTime\":134328525408419454},\"ID\":\"60A35708-D66F-4465-81B5-C802A9662159\",\"PortOperationTime\":0,\"State\":1,\"SwitchOperationTime\":0,\"VfpOperationT"
    L"ime\":0,\"parentId\":\"62A1B4F0-FB65-47DD-90D5-F6AE48774C91\"}}";

/* Verbatim capture of the created-network echo document (the create-exit
   verification's own input, preflight/echo-document.json): the binding
   arrives as a TOP-LEVEL OBJECT, not the string the parser originally
   assumed. Captured before 0.1.8 - a newer HNS echo may carry extra
   members, which the object walk skips structurally, so the assertions
   below stay pinned to the InterfaceGuid member. */
static const wchar_t FIXTURE_ECHO_JSON[] =
    L"{\"ActivityId\":\"CE1F2599-EF15-4C5E-8A09-BE203F1FA5AA\",\"AdditionalParams\":{},\"CurrentEndpointCount\":0,\"Extensions\":[{\"Id\":\"E7C3B2"
    L"F0-F3C5-48DF-AF2B-10FED6D72E7A\",\"IsEnabled\":false,\"Name\":\"Microsoft Windows Filtering Platform\"},{\"Id\":\"F74F241B-440F-4433-BB28-00"
    L"F89EAD20D8\",\"IsEnabled\":false,\"Name\":\"Microsoft Azure VFP Switch Filter Extension\"},{\"Id\":\"430BDADD-BAB0-41AB-A369-94B67FA5BE0A\","
    L"\"IsEnabled\":true,\"Name\":\"Microsoft NDIS Capture\"}],\"Flags\":2,\"Health\":{\"LastErrorCode\":0,\"LastUpdateTime\":134339245300219569},"
    L"\"ID\":\"8996D470-50B2-4241-A898-CA8F2A4804B3\",\"IPv6\":false,\"InterfaceConstraint\":{\"InterfaceGuid\":\"a874be21-2616-4e30-af31-340cb020"
    L"a595\"},\"LayeredOn\":\"9CD4C55A-42D2-4946-8069-56F61888B47D\",\"MacPools\":[{\"EndMacAddress\":\"00-15-5D-3B-DF-FF\",\"StartMacAddress\":\""
    L"00-15-5D-3B-D0-00\"}],\"MaxConcurrentEndpoints\":0,\"Name\":\"AppSandboxExternal-a874be21-2616-4e30-af31-340cb020a595\",\"Policies\":[],\"St"
    L"ate\":1,\"TotalEndpoints\":0,\"Type\":\"Transparent\",\"Version\":68719476736,\"Resources\":{\"AdditionalParams\":{},\"AllocationOrder\":0,\""
    L"CompartmentOperationTime\":0,\"Flags\":0,\"Health\":{\"LastErrorCode\":0,\"LastUpdateTime\":134339245392426845},\"ID\":\"CE1F2599-EF15-4C5E"
    L"-8A09-BE203F1FA5AA\",\"PortOperationTime\":0,\"State\":1,\"SwitchOperationTime\":0,\"VfpOperationTime\":0,\"parentId\":\"B28AF70E-EF3F-4930-"
    L"BC79-AFFFE39B5B3A\"}}";

/* The echo document's own InterfaceGuid member - its T for the ON_T
   fixture (the create-exit verification's own case). */
static const GUID FIXT_ECHO_T = {             /* a874be21-2616-4e30-af31-340cb020a595 */
    0xA874BE21, 0x2616, 0x4E30,
    { 0xAF, 0x31, 0x34, 0x0C, 0xB0, 0x20, 0xA5, 0x95 }
};

static HcnAdapterEntry g_fixture_entries[5];
static HcnAdapterInventory g_fixture_inv;

static const wchar_t *class_name(BindingClass c)
{
    switch (c) {
    case BINDING_ON_T:     return L"BINDING_ON_T";
    case BINDING_OFF_T:    return L"BINDING_OFF_T";
    case BINDING_CONFLICT: return L"BINDING_CONFLICT";
    default:               return L"BINDING_UNPROVEN";
    }
}

static HcnAdapterEntry *fixture_entry(int idx, const GUID *g,
                                      const wchar_t *friendly, const wchar_t *desc)
{
    HcnAdapterEntry *e = &g_fixture_entries[idx];

    ZeroMemory(e, sizeof(*e));
    e->interface_guid = *g;
    wcsncpy_s(e->friendly_name, HCN_PHYS_NAME_MAX, friendly, _TRUNCATE);
    wcsncpy_s(e->description, HCN_PHYS_DESC_MAX, desc, _TRUNCATE);
    return e;
}

static void check_class(const wchar_t *json, const GUID *target,
                        BindingClass expected, const wchar_t *label)
{
    HcnNetworkProps props;
    HRESULT hr;
    BindingClass got;

    hr = parse_hcn_network_properties(json, &props);
    if (FAILED(hr)) {
        check(FALSE, label);
        wprintf(L"      (parse unexpectedly failed: 0x%08X)\n", hr);
        return;
    }
    got = classify_binding_values(&props, &g_fixture_inv, target);
    if (got == expected) {
        check(TRUE, label);
    } else {
        check(FALSE, label);
        wprintf(L"      (expected %s, got %s)\n", class_name(expected),
                class_name(got));
    }
}

static void check_parse_fails(const wchar_t *json, const wchar_t *label)
{
    HcnNetworkProps props;

    check(FAILED(parse_hcn_network_properties(json, &props)), label);
}

static void run_parser_fixtures(const GUID *target)
{
    wchar_t doc[2048];

    /* Hand-crafted FULL interface table (name translation uses all
       rows, not the hardware-filtered subset). */
    fixture_entry(0, target,       L"Ethernet 3", L"Intel(R) Ethernet I211");
    fixture_entry(1, &FIXT_OTHER,  L"Ethernet 4", L"Intel(R) Ethernet I219");
    fixture_entry(2, &FIXT_X3,     L"DupNIC",     L"Duplicated name A");
    fixture_entry(3, &FIXT_OTHER,  L"DupNIC",     L"Duplicated name B");
    fixture_entry(4, &FIXT_X3,     FIXT_RENAMED,  L"Renamed GUID-shaped connection");
    g_fixture_inv.entries = g_fixture_entries;
    g_fixture_inv.count = 5;

    wprintf(L"\n[parser/classifier fixtures]\n");

    /* Captured foreign shape: no readable binding, unknown fields (LayeredOn)
       skipped structurally - unproven, never a parse failure. */
    check_class(FIXTURE_HYPERV_SWITCH_JSON, target, BINDING_UNPROVEN,
                L"verbatim captured Hyper-V switch doc classifies UNPROVEN "
                L"(empty Policies; LayeredOn is not binding evidence)");

    /* InterfaceConstraint policy payload under Settings (the owned-network
       write location) and under Data (read-side tolerance). */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"InterfaceConstraint\","
        L"\"Settings\":{\"InterfaceGuid\":\"%s\"}}]}", FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"InterfaceConstraint/Settings.InterfaceGuid == T -> ON_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"InterfaceConstraint\","
        L"\"Data\":{\"InterfaceGuid\":\"%s\"}}]}", FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"InterfaceConstraint/Data.InterfaceGuid == T -> ON_T (read-side Data tolerance)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"InterfaceConstraint\","
        L"\"Settings\":{\"InterfaceGuid\":\"%s\"}}]}", FIXT_O_STR);
    check_class(doc, target, BINDING_OFF_T,
                L"InterfaceConstraint/Settings.InterfaceGuid != T -> OFF_T");

    /* Cross-parent InterfaceGuid disagreement: not a parse conflict -
       both definite values flow to the directional classification. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"InterfaceConstraint\","
        L"\"Settings\":{\"InterfaceGuid\":\"%s\"},"
        L"\"Data\":{\"InterfaceGuid\":\"%s\"}}]}",
        FIXT_T_STR, FIXT_O_STR);
    check_class(doc, target, BINDING_CONFLICT,
                L"InterfaceGuid Settings=T + Data=other -> CONFLICT (never first-wins)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"InterfaceConstraint\","
        L"\"Settings\":{\"InterfaceGuid\":\"%s\"},"
        L"\"Data\":{\"InterfaceGuid\":\"%s\"}}]}",
        FIXT_O_STR, FIXT_ABSENT);
    check_class(doc, target, BINDING_UNPROVEN,
                L"InterfaceGuid Settings=other + Data=second other, disagreeing -> UNPROVEN relative to T");

    /* NetAdapterName translation through the full table. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"Ethernet 3\"}}]}");
    check_class(doc, target, BINDING_ON_T,
                L"NetAdapterName = unique FriendlyName of T -> ON_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"Ethernet 4\"}}]}");
    check_class(doc, target, BINDING_OFF_T,
                L"NetAdapterName = unique FriendlyName of another NIC -> OFF_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"DupNIC\"}}]}");
    check_class(doc, target, BINDING_UNPROVEN,
                L"NetAdapterName = ambiguous FriendlyName -> UNPROVEN (never first-match)");

    /* NetAdapterName translation through unique Description (the
       FriendlyName-then-Description ladder). */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"Intel(R) Ethernet I219\"}}]}");
    check_class(doc, target, BINDING_OFF_T,
                L"NetAdapterName = no FriendlyName match, unique Description match -> OFF_T");

    /* NetAdapterName payload under Data: not an accepted NAA location
       (the read-side Data tolerance is InterfaceConstraint's) -
       the slot is unreadable, fail-closed, never an off-T definite. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Data\":{\"NetworkAdapterName\":\"Ethernet 4\"}}]}");
    check_class(doc, target, BINDING_UNPROVEN,
                L"NetAdapterName under Data (no Settings) -> UNPROVEN (not an accepted NAA location)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{},\"Data\":{\"NetworkAdapterName\":\"Ethernet 4\"}}]}");
    check_class(doc, target, BINDING_UNPROVEN,
                L"NetAdapterName under Data with a Settings object present -> UNPROVEN (fail-closed, never OFF_T)");

    /* NetAdapterName GUID-shaped values: GUID evidence only when equal to
       T or a current interface; otherwise name translation. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"%s\"}}]}", FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"NetAdapterName GUID-shaped == T -> ON_T (GUID evidence)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"%s\"}}]}", FIXT_O_STR);
    check_class(doc, target, BINDING_OFF_T,
                L"NetAdapterName GUID-shaped == current L0 GUID -> OFF_T (GUID evidence)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"%s\"}}]}", FIXT_RENAMED);
    check_class(doc, target, BINDING_OFF_T,
                L"NetAdapterName GUID-shaped unknown GUID, unique connection NAME match -> OFF_T via name");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":[{\"Type\":\"NetAdapterName\","
        L"\"Settings\":{\"NetworkAdapterName\":\"%s\"}}]}", FIXT_ABSENT);
    check_class(doc, target, BINDING_UNPROVEN,
                L"NetAdapterName GUID-shaped unknown, no name match -> UNPROVEN (no fabricated off-T)");

    /* Top-level string InterfaceConstraint (old-branch handler, retained). */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":\"%s\"}", FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"top-level InterfaceConstraint == T -> ON_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":\"%s\"}", FIXT_O_STR);
    check_class(doc, target, BINDING_OFF_T,
                L"top-level InterfaceConstraint != T -> OFF_T");

    check_class(L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":7}", target,
                BINDING_UNPROVEN,
                L"top-level InterfaceConstraint non-string -> UNPROVEN (opaque marker)");

    /* Top-level object form (the measured echo shape). */
    check_class(FIXTURE_ECHO_JSON, &FIXT_ECHO_T, BINDING_ON_T,
                L"verbatim captured echo doc, T = its InterfaceGuid member -> ON_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{\"InterfaceGuid\":\"%s\"}}",
        FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"object-form InterfaceConstraint.InterfaceGuid == T -> ON_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{\"InterfaceGuid\":\"%s\"}}",
        FIXT_O_STR);
    check_class(doc, target, BINDING_OFF_T,
                L"object-form InterfaceConstraint.InterfaceGuid != T -> OFF_T "
                L"(never an unproven occupant: unproven_occupant is set only "
                L"from UNPROVEN, asserted here at the classification decision)");

    check_class(L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{}}", target,
                BINDING_UNPROVEN,
                L"object-form InterfaceConstraint with no member -> UNPROVEN (opaque marker)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{\"OtherGuid\":\"%s\"}}",
        FIXT_O_STR);
    check_class(doc, target, BINDING_UNPROVEN,
                L"object-form InterfaceConstraint without an InterfaceGuid member "
                L"-> UNPROVEN (unknown members skipped structurally, not evidence)");

    check_class(L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{\"InterfaceGuid\":7}}",
                target, BINDING_UNPROVEN,
                L"object-form non-string InterfaceGuid member -> UNPROVEN (opaque marker)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{\"InterfaceGuid\":\"%s\","
        L"\"InterfaceGuid\":\"%s\"}}", FIXT_T_STR, FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"object-form two agreeing InterfaceGuid values -> ON_T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":{\"InterfaceGuid\":\"%s\","
        L"\"InterfaceGuid\":\"%s\"}}", FIXT_T_STR, FIXT_O_STR);
    check_class(doc, target, BINDING_UNPROVEN,
                L"object-form two disagreeing InterfaceGuid values -> UNPROVEN "
                L"(whole field unreadable)");

    /* Unreadable-Type policy: unresolved, not ignored - it forbids off-T. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":\"%s\","
        L"\"Policies\":[{\"Settings\":{\"NetworkAdapterName\":\"Ethernet 4\"}}]}",
        FIXT_O_STR);
    check_class(doc, target, BINDING_UNPROVEN,
                L"definite off-T + policy with absent Type -> UNPROVEN (unresolved forbids off-T)");

    /* Readable unsupported policy type: ignored entirely. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":\"%s\","
        L"\"Policies\":[{\"Type\":\"QoS\",\"Settings\":{\"InterfaceGuid\":\"%s\"}}]}",
        FIXT_O_STR, FIXT_O_STR);
    check_class(doc, target, BINDING_OFF_T,
                L"definite off-T + readable unsupported policy type -> OFF_T (policy ignored entirely)");

    /* Directional aggregation rows. */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"InterfaceConstraint\":\"%s\","
        L"\"Policies\":[{\"Type\":\"NetAdapterName\",\"Settings\":{\"NetworkAdapterName\":5}}]}",
        FIXT_T_STR);
    check_class(doc, target, BINDING_ON_T,
                L"definite on-T + unreadable binding value -> ON_T (on-T stands)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":["
        L"{\"Type\":\"InterfaceConstraint\",\"Settings\":{\"InterfaceGuid\":\"%s\"}},"
        L"{\"Type\":\"InterfaceConstraint\",\"Settings\":{\"InterfaceGuid\":\"%s\"}}]}",
        FIXT_T_STR, FIXT_O_STR);
    check_class(doc, target, BINDING_CONFLICT,
                L"definite on-T + definite off-T -> CONFLICT (never first-wins)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":["
        L"{\"Type\":\"InterfaceConstraint\",\"Settings\":{\"InterfaceGuid\":\"%s\"}},"
        L"{\"Type\":\"InterfaceConstraint\",\"Settings\":{\"InterfaceGuid\":\"%s\"}}]}",
        FIXT_O_STR, FIXT_ABSENT);
    check_class(doc, target, BINDING_UNPROVEN,
                L"two disagreeing definite off-T values -> UNPROVEN relative to T");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"Type\":\"Transparent\",\"Policies\":["
        L"{\"Type\":\"InterfaceConstraint\",\"Settings\":{\"InterfaceGuid\":\"%s\"}},"
        L"{\"Type\":\"InterfaceConstraint\",\"Settings\":{\"InterfaceGuid\":\"%s\"}}]}",
        FIXT_O_STR, FIXT_O_STR);
    check_class(doc, target, BINDING_OFF_T,
                L"two agreeing definite off-T values, nothing unresolved -> OFF_T");

    check_class(L"{\"Type\":\"Transparent\",\"Policies\":[]}", target,
                BINDING_UNPROVEN,
                L"no definite value at all -> UNPROVEN");

    /* Document-level parse failures (record degrades to unproven at the
       scan level; the document itself never parses as success). */
    check_parse_fails(L"{\"Type\":\"Transp",
                      L"truncated document -> parse E_FAIL");
    check_parse_fails(L"{\"Type\":\"NAT\"} garbage",
                      L"trailing garbage -> parse E_FAIL");
    check_parse_fails(L"{\"ID\":\"notaguid\",\"Type\":\"NAT\"}",
                      L"non-GUID ID value -> parse E_FAIL");
    check_parse_fails(L"{\"Type\":5}",
                      L"non-string Type value -> parse E_FAIL");
}

/* ---- Config stream fixtures ----
 *
 * The vms.cfg parse and save loops live in asb_config.c (linked here as
 * the production TU); these fixtures drive the REAL loader and saver
 * through the production open helpers, against temporary files - never
 * the app's real config file.
 *
 * Two groups:
 *  - the extraction oracle: a copy of the PRE-move parse and save code
 *    (id assignment stripped) with its own globals, compared field-wise
 *    against the production stream functions and byte-wise on save;
 *  - the new InternalSwitch key's persistence contract: validation,
 *    round-trips, duplicate-key tri-state, neighbour direction.
 */

#define TEST_MAX_ROWS 8

static wchar_t g_cfg_tmp_dir[MAX_PATH];
static int g_cfg_tmp_seq = 0;

static void cfg_temp_path(wchar_t *out, size_t cap, const wchar_t *tag)
{
    if (!g_cfg_tmp_dir[0])
        GetTempPathW(MAX_PATH, g_cfg_tmp_dir);
    swprintf_s(out, (int)cap, L"%s\\asbtest-%d-%s.cfg", g_cfg_tmp_dir,
               g_cfg_tmp_seq++, tag);
}

static BOOL cfg_write_bytes(const wchar_t *path, const char *bytes, size_t len)
{
    FILE *f = NULL;
    BOOL ok;

    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
        return FALSE;
    ok = (fwrite(bytes, 1, len, f) == len);
    fclose(f);
    return ok;
}

enum {
    FX_UTF8_BOM,      /* EF BB BF + UTF-8 bytes, the canonical save form */
    FX_UTF8_NOBOM,    /* UTF-8/ANSI bytes without a BOM: the "r" (ANSI) path */
    FX_UTF16LE_BOM    /* FF FE + UTF-16LE code units */
};

static BOOL cfg_write_text(const wchar_t *path, const wchar_t *text, int form)
{
    char bytes[8192];
    size_t pos = 0;
    int n;

    if (form == FX_UTF16LE_BOM) {
        size_t wlen = wcslen(text);
        bytes[pos++] = '\xFF';
        bytes[pos++] = '\xFE';
        memcpy(bytes + pos, text, wlen * sizeof(wchar_t));
        pos += wlen * sizeof(wchar_t);
        return cfg_write_bytes(path, bytes, pos);
    }
    if (form == FX_UTF8_BOM) {
        bytes[pos++] = '\xEF';
        bytes[pos++] = '\xBB';
        bytes[pos++] = '\xBF';
    }
    n = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes + pos,
                            (int)(sizeof(bytes) - pos), NULL, NULL);
    if (n <= 0)
        return FALSE;
    /* n includes the terminator; fwrite excludes it */
    return cfg_write_bytes(path, bytes, pos + (size_t)n - 1);
}

static char *cfg_read_bytes(const wchar_t *path, size_t *out_len)
{
    FILE *f = NULL;
    char *buf;
    long len;

    *out_len = 0;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc(len ? (size_t)len : 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)len;
    return buf;
}

/* ---- The differential oracle ----
 *
 * A copy of the PRE-move parse and save loops with the id assignment
 * stripped (the id channel is the core wrapper's by design: the oracle
 * has no allocator, so the field-wise comparison excludes unique_id).
 * It carries its own copies of the globals the pre-move code indexed.
 *
 * Differential oracle - never synchronize with the production code: the
 * point of the diff is catching production drift from the pre-extraction
 * behavior, which a synchronized copy could not. */

static VmInstance o_vms[TEST_MAX_ROWS];
static TemplateInfo o_tpls[TEST_MAX_ROWS];
static int o_vm_count = 0;
static int o_tpl_count = 0;
static wchar_t o_last_iso[MAX_PATH];
static BOOL o_suppress = FALSE;

static void oracle_reset(void)
{
    ZeroMemory(o_vms, sizeof(o_vms));
    ZeroMemory(o_tpls, sizeof(o_tpls));
    o_vm_count = 0;
    o_tpl_count = 0;
    o_last_iso[0] = L'\0';
    o_suppress = FALSE;
}

static void oracle_parse(FILE *f, int vm_cap, int tpl_cap)
{
    wchar_t line[1024];
    VmInstance *vm = NULL;
    TemplateInfo *tpl = NULL;
    BOOL in_settings = FALSE;

    while (fgetws(line, 1024, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r'))
            line[--len] = L'\0';

        if (wcscmp(line, L"[Settings]") == 0) {
            in_settings = TRUE;
            vm = NULL;
            tpl = NULL;
            continue;
        }

        if (wcscmp(line, L"[VM]") == 0) {
            in_settings = FALSE;
            tpl = NULL;
            if (o_vm_count >= vm_cap) break;
            vm = &o_vms[o_vm_count];
            ZeroMemory(vm, sizeof(VmInstance));
            /* unique_id assignment stripped: the wrapper's channel */
            o_vm_count++;
            continue;
        }

        if (wcscmp(line, L"[Template]") == 0) {
            in_settings = FALSE;
            vm = NULL;
            tpl = NULL;
            if (o_tpl_count < tpl_cap) {
                tpl = &o_tpls[o_tpl_count++];
                ZeroMemory(tpl, sizeof(*tpl));
            }
            continue;
        }

        if (tpl) {
            if (wcsncmp(line, L"Name=", 5) == 0)
                wcsncpy_s(tpl->name, 256, line + 5, _TRUNCATE);
            else if (wcsncmp(line, L"OsType=", 7) == 0)
                wcsncpy_s(tpl->os_type, 32, line + 7, _TRUNCATE);
            else if (wcsncmp(line, L"ImagePath=", 10) == 0)
                wcsncpy_s(tpl->image_path, MAX_PATH, line + 10, _TRUNCATE);
            else if (wcsncmp(line, L"VhdxPath=", 9) == 0)
                wcsncpy_s(tpl->vhdx_path, MAX_PATH, line + 9, _TRUNCATE);
            continue;
        }

        if (in_settings) {
            if (wcsncmp(line, L"LastIsoPath=", 12) == 0)
                wcscpy_s(o_last_iso, MAX_PATH, line + 12);
            else if (wcsncmp(line, L"SuppressTrayWarn=", 17) == 0)
                o_suppress = (_wtoi(line + 17) != 0);
            continue;
        }

        if (!vm) continue;

        if (wcsncmp(line, L"Name=", 5) == 0)
            wcscpy_s(vm->name, 256, line + 5);
        else if (wcsncmp(line, L"OsType=", 7) == 0)
            wcscpy_s(vm->os_type, 32, line + 7);
        else if (wcsncmp(line, L"ImagePath=", 10) == 0)
            wcscpy_s(vm->image_path, MAX_PATH, line + 10);
        else if (wcsncmp(line, L"VhdxPath=", 9) == 0)
            wcscpy_s(vm->vhdx_path, MAX_PATH, line + 9);
        else if (wcsncmp(line, L"RamMB=", 6) == 0)
            vm->ram_mb = (DWORD)_wtoi(line + 6);
        else if (wcsncmp(line, L"HddGB=", 6) == 0)
            vm->hdd_gb = (DWORD)_wtoi(line + 6);
        else if (wcsncmp(line, L"CpuCores=", 9) == 0)
            vm->cpu_cores = (DWORD)_wtoi(line + 9);
        else if (wcsncmp(line, L"GpuMode=", 8) == 0)
            vm->gpu_mode = _wtoi(line + 8);
        else if (wcsncmp(line, L"GpuName=", 8) == 0)
            wcscpy_s(vm->gpu_name, 256, line + 8);
        else if (wcsncmp(line, L"GpuId=", 6) == 0)
            wcsncpy_s(vm->gpu_id, ARRAYSIZE(vm->gpu_id), line + 6, _TRUNCATE);
        else if (wcsncmp(line, L"GpuDevicePath=", 14) == 0)
            { /* ignored - backwards compat */ }
        else if (wcsncmp(line, L"NetworkMode=", 12) == 0)
            vm->network_mode = _wtoi(line + 12);
        else if (wcsncmp(line, L"NetAdapter=", 11) == 0)
            wcscpy_s(vm->net_adapter, 256, line + 11);
        else if (wcsncmp(line, L"MacAddress=", 11) == 0)
            wcsncpy_s(vm->mac_address, ARRAYSIZE(vm->mac_address), line + 11, _TRUNCATE);
        else if (wcsncmp(line, L"ResourcesIso=", 13) == 0)
            wcscpy_s(vm->resources_iso_path, MAX_PATH, line + 13);
        else if (wcsncmp(line, L"NatIp=", 6) == 0)
            WideCharToMultiByte(CP_UTF8, 0, line + 6, -1, vm->nat_ip, sizeof(vm->nat_ip), NULL, NULL);
        else if (wcsncmp(line, L"IsTemplate=", 11) == 0)
            vm->is_template = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"TestMode=", 9) == 0)
            vm->test_mode = (_wtoi(line + 9) != 0);
        else if (wcsncmp(line, L"AdminUser=", 10) == 0)
            wcscpy_s(vm->admin_user, 128, line + 10);
        else if (wcsncmp(line, L"SshEnabled=", 11) == 0)
            vm->ssh_enabled = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"SshPort=", 8) == 0)
            vm->ssh_port = (DWORD)_wtoi(line + 8);
        else if (wcsncmp(line, L"SshDeployKey=", 13) == 0)
            vm->ssh_deploy_key = (_wtoi(line + 13) != 0);
        else if (wcsncmp(line, L"SshPubKey=", 10) == 0)
            wcsncpy_s(vm->ssh_pubkey, 512, line + 10, _TRUNCATE);
        else if (wcsncmp(line, L"InstallComplete=", 16) == 0)
            vm->install_complete = (_wtoi(line + 16) != 0);
    }
}

static void oracle_save(FILE *f)
{
    int i;

    if (o_last_iso[0] != L'\0' || o_suppress) {
        fwprintf(f, L"[Settings]\n");
        if (o_last_iso[0] != L'\0')
            fwprintf(f, L"LastIsoPath=%s\n", o_last_iso);
        if (o_suppress)
            fwprintf(f, L"SuppressTrayWarn=1\n");
        fwprintf(f, L"\n");
    }

    for (i = 0; i < o_tpl_count; i++) {
        fwprintf(f, L"[Template]\n");
        fwprintf(f, L"Name=%s\n", o_tpls[i].name);
        fwprintf(f, L"OsType=%s\n", o_tpls[i].os_type);
        fwprintf(f, L"ImagePath=%s\n", o_tpls[i].image_path);
        fwprintf(f, L"VhdxPath=%s\n\n", o_tpls[i].vhdx_path);
    }

    for (i = 0; i < o_vm_count; i++) {
        if (o_vms[i].building_vhdx) continue;
        fwprintf(f, L"[VM]\n");
        fwprintf(f, L"Name=%s\n", o_vms[i].name);
        fwprintf(f, L"OsType=%s\n", o_vms[i].os_type);
        fwprintf(f, L"ImagePath=%s\n", o_vms[i].image_path);
        fwprintf(f, L"VhdxPath=%s\n", o_vms[i].vhdx_path);
        fwprintf(f, L"RamMB=%lu\n", o_vms[i].ram_mb);
        fwprintf(f, L"HddGB=%lu\n", o_vms[i].hdd_gb);
        fwprintf(f, L"CpuCores=%lu\n", o_vms[i].cpu_cores);
        fwprintf(f, L"GpuMode=%d\n", o_vms[i].gpu_mode);
        fwprintf(f, L"GpuName=%s\n", o_vms[i].gpu_name);
        if (o_vms[i].gpu_id[0])
            fwprintf(f, L"GpuId=%s\n", o_vms[i].gpu_id);
        fwprintf(f, L"NetworkMode=%d\n", o_vms[i].network_mode);
        if (o_vms[i].net_adapter[0] != L'\0')
            fwprintf(f, L"NetAdapter=%s\n", o_vms[i].net_adapter);
        if (o_vms[i].mac_address[0])
            fwprintf(f, L"MacAddress=%s\n", o_vms[i].mac_address);
        if (o_vms[i].resources_iso_path[0] != L'\0')
            fwprintf(f, L"ResourcesIso=%s\n", o_vms[i].resources_iso_path);
        if (o_vms[i].nat_ip[0] != '\0')
            fwprintf(f, L"NatIp=%S\n", o_vms[i].nat_ip);
        if (o_vms[i].is_template)
            fwprintf(f, L"IsTemplate=1\n");
        if (o_vms[i].test_mode)
            fwprintf(f, L"TestMode=1\n");
        if (o_vms[i].admin_user[0])
            fwprintf(f, L"AdminUser=%s\n", o_vms[i].admin_user);
        if (o_vms[i].ssh_enabled)
            fwprintf(f, L"SshEnabled=1\n");
        if (o_vms[i].ssh_port)
            fwprintf(f, L"SshPort=%lu\n", o_vms[i].ssh_port);
        if (o_vms[i].ssh_deploy_key)
            fwprintf(f, L"SshDeployKey=1\n");
        if (o_vms[i].ssh_pubkey[0])
            fwprintf(f, L"SshPubKey=%s\n", o_vms[i].ssh_pubkey);
        if (o_vms[i].install_complete)
            fwprintf(f, L"InstallComplete=1\n");
        fwprintf(f, L"\n");
    }
}

/* Field-wise row comparison EXCLUDING unique_id - the wrapper's channel,
   which the oracle deliberately does not model. Zero-initialized rows on
   both sides keep padding deterministic, so the memcmp is exact. */
static BOOL cfg_rows_equal(const VmInstance *a, const VmInstance *b)
{
    return memcmp((const char *)a + sizeof(UINT64),
                  (const char *)b + sizeof(UINT64),
                  sizeof(VmInstance) - sizeof(UINT64)) == 0;
}

/* The baseline's BOM sniff, replicated for the oracle's own opens (the
   oracle is a PRE-move copy and cannot use the production open helper). */
static const wchar_t *unicode_file_mode(const wchar_t *path)
{
    FILE *f = NULL;
    unsigned char bom[3] = { 0 };
    BOOL unicode_config;

    if (_wfopen_s(&f, path, L"rb") != 0 || !f)
        return L"r";
    (void)fread(bom, 1, sizeof(bom), f);
    fclose(f);
    unicode_config = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) ||
                     (bom[0] == 0xFF && bom[1] == 0xFE);
    return unicode_config ? L"r,ccs=UTF-8" : L"r";
}

/* One production load + save round against the oracle on the same
   fixture file. Checks: stream TRUE, counts equal, rows field-equal
   (excluding unique_id), templates equal, settings equal, and the
   production save produces the oracle's bytes. */
static BOOL cfg_diff_round(const wchar_t *path, const wchar_t *label)
{
    static VmInstance t_vms[TEST_MAX_ROWS];
    TemplateInfo t_tpls[TEST_MAX_ROWS];
    wchar_t t_last_iso[MAX_PATH];
    BOOL t_suppress = FALSE;
    int t_vm_count = 0, t_tpl_count = 0;
    wchar_t prod_path[MAX_PATH], orac_path[MAX_PATH];
    FILE *f;
    BOOL ok = TRUE, eq;
    size_t plen, olen;
    char *pbytes, *obytes;
    int i;

    ZeroMemory(t_vms, sizeof(t_vms));
    ZeroMemory(t_tpls, sizeof(t_tpls));
    t_last_iso[0] = L'\0';

    f = asb_config_open_read(path);
    if (!f) {
        wprintf(L"      (open_read failed for %s)\n", path);
        return FALSE;
    }
    eq = asb_load_vm_list_stream(f, t_vms, TEST_MAX_ROWS, &t_vm_count,
                                 t_tpls, TEST_MAX_ROWS, &t_tpl_count,
                                 t_last_iso, MAX_PATH, &t_suppress);
    fclose(f);
    if (!eq) {
        wprintf(L"      (production load returned FALSE)\n");
        ok = FALSE;
    }

    oracle_reset();
    if (_wfopen_s(&f, path, unicode_file_mode(path)) != 0) f = NULL;
    if (!f) {
        wprintf(L"      (oracle open failed)\n");
        return FALSE;
    }
    oracle_parse(f, TEST_MAX_ROWS, TEST_MAX_ROWS);
    fclose(f);

    if (t_vm_count != o_vm_count || t_tpl_count != o_tpl_count) {
        wprintf(L"      (counts differ: prod %d VMs / %d templates, oracle %d / %d)\n",
                t_vm_count, t_tpl_count, o_vm_count, o_tpl_count);
        ok = FALSE;
    } else {
        for (i = 0; i < t_vm_count; i++) {
            if (!cfg_rows_equal(&t_vms[i], &o_vms[i])) {
                wprintf(L"      (VM row %d differs from the oracle)\n", i);
                ok = FALSE;
            }
        }
        for (i = 0; i < t_tpl_count; i++) {
            if (memcmp(&t_tpls[i], &o_tpls[i], sizeof(TemplateInfo)) != 0) {
                wprintf(L"      (template row %d differs from the oracle)\n", i);
                ok = FALSE;
            }
        }
    }
    if (wcscmp(t_last_iso, o_last_iso) != 0 || t_suppress != o_suppress) {
        wprintf(L"      (settings differ from the oracle)\n");
        ok = FALSE;
    }

    /* Save bytes: production stream vs the pre-move saver. */
    cfg_temp_path(prod_path, MAX_PATH, L"save-prod");
    cfg_temp_path(orac_path, MAX_PATH, L"save-oracle");
    f = asb_config_open_write(prod_path);
    if (f) {
        if (!asb_save_vm_list_stream(f, t_vms, t_vm_count, t_tpls, t_tpl_count,
                                     t_last_iso, t_suppress)) {
            wprintf(L"      (production save returned FALSE)\n");
            ok = FALSE;
        }
        fclose(f);
    } else {
        wprintf(L"      (open_write failed)\n");
        ok = FALSE;
    }
    if (_wfopen_s(&f, orac_path, L"w,ccs=UTF-8") != 0) f = NULL;
    if (f) {
        oracle_save(f);
        fclose(f);
    } else {
        wprintf(L"      (oracle save open failed)\n");
        ok = FALSE;
    }

    pbytes = cfg_read_bytes(prod_path, &plen);
    obytes = cfg_read_bytes(orac_path, &olen);
    if (!pbytes || !obytes || plen != olen ||
        (plen > 0 && memcmp(pbytes, obytes, plen) != 0)) {
        wprintf(L"      (save bytes differ from the oracle: %lu vs %lu)\n",
                (unsigned long)plen, (unsigned long)olen);
        ok = FALSE;
    }
    free(pbytes);
    free(obytes);
    DeleteFileW(prod_path);
    DeleteFileW(orac_path);

    if (!ok)
        wprintf(L"      (fixture: %s)\n", label);
    return ok;
}

static void run_config_extraction_fixtures(void)
{
    wchar_t path[MAX_PATH];

    wprintf(L"\n[config extraction fixtures - differential oracle]\n");

    /* Representative fixture: settings, two templates, three VMs,
       non-ASCII names, an empty string value, rich optional keys, CRLF
       line endings, UTF-8 BOM (the canonical save form). Contains no
       InternalSwitch lines - the pre-move/no-new-key shape. */
    {
        static const wchar_t rich[] =
            L"[Settings]\r\n"
            L"LastIsoPath=C:\\isos\\win11 \u00E9.iso\r\n"
            L"SuppressTrayWarn=1\r\n"
            L"\r\n"
            L"[Template]\r\n"
            L"Name=Win11 Base\r\n"
            L"OsType=Windows\r\n"
            L"ImagePath=C:\\isos\\win11.iso\r\n"
            L"VhdxPath=C:\\vms\\templates\\win11\\disk.vhdx\r\n"
            L"\r\n"
            L"[Template]\r\n"
            L"Name=Ubuntu Base\r\n"
            L"OsType=Linux\r\n"
            L"ImagePath=C:\\isos\\ubuntu-24.04.iso\r\n"
            L"VhdxPath=C:\\vms\\templates\\ubuntu\\disk.vhdx\r\n"
            L"\r\n"
            L"[VM]\r\n"
            L"Name=Lab \u00DCn\u00EFcode \u03A9\r\n"
            L"OsType=Windows\r\n"
            L"ImagePath=C:\\isos\\win11.iso\r\n"
            L"VhdxPath=C:\\vms\\lab1\\disk.vhdx\r\n"
            L"RamMB=4096\r\n"
            L"HddGB=64\r\n"
            L"CpuCores=4\r\n"
            L"GpuMode=1\r\n"
            L"GpuName=Default GPU\r\n"
            L"GpuId=PCI\\VEN_10DE&DEV_2684\r\n"
            L"NetworkMode=3\r\n"
            L"NetAdapter=\r\n"
            L"MacAddress=00-15-5D-00-00-01\r\n"
            L"AdminUser=admin\r\n"
            L"SshEnabled=1\r\n"
            L"SshPort=2222\r\n"
            L"SshDeployKey=1\r\n"
            L"SshPubKey=ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFixture appsandbox\r\n"
            L"InstallComplete=1\r\n"
            L"\r\n"
            L"[VM]\r\n"
            L"Name=vm-linux\r\n"
            L"OsType=Linux\r\n"
            L"VhdxPath=C:\\vms\\linux1\\disk.vhdx\r\n"
            L"NetworkMode=1\r\n"
            L"NatIp=172.20.0.2\r\n"
            L"TestMode=1\r\n"
            L"\r\n"
            L"[VM]\r\n"
            L"Name=minimal-no-trailing-newline\r\n"
            L"OsType=Windows\r\n"
            L"VhdxPath=C:\\vms\\min\\disk.vhdx";
        cfg_temp_path(path, MAX_PATH, L"rich");
        check(cfg_write_text(path, rich, FX_UTF8_BOM) &&
              cfg_diff_round(path, L"UTF-8 BOM + CRLF, multi-VM/templates/settings"),
              L"rich UTF-8 BOM fixture: production == oracle (fields, counts, save bytes)");
        DeleteFileW(path);
    }

    /* LF-only, no BOM (the ANSI "r" path), ASCII, final line without a
       newline. */
    {
        static const wchar_t lfonly[] =
            L"[VM]\n"
            L"Name=vmA\n"
            L"OsType=Linux\n"
            L"VhdxPath=C:\\vms\\a.vhdx\n"
            L"RamMB=2048\n"
            L"NetworkMode=0\n"
            L"\n"
            L"[VM]\n"
            L"Name=vmB\n"
            L"OsType=Windows\n"
            L"VhdxPath=C:\\vms\\b.vhdx";
        cfg_temp_path(path, MAX_PATH, L"lf");
        check(cfg_write_text(path, lfonly, FX_UTF8_NOBOM) &&
              cfg_diff_round(path, L"LF-only no-BOM (ANSI path)"),
              L"LF/ANSI fixture: production == oracle");
        DeleteFileW(path);
    }

    /* UTF-16LE BOM with non-ASCII (the FF-FE branch; BOM priority). */
    {
        static const wchar_t u16[] =
            L"[VM]\r\n"
            L"Name=Vm \u00DCnicode\r\n"
            L"OsType=Windows\r\n"
            L"VhdxPath=C:\\vms\\u.vhdx\r\n"
            L"RamMB=8192\r\n"
            L"NetworkMode=3\r\n"
            L"MacAddress=00-15-5D-00-00-07\r\n"
            L"\r\n"
            L"[Template]\r\n"
            L"Name=Tpl 16\r\n"
            L"OsType=Linux\r\n"
            L"ImagePath=C:\\isos\\t.iso\r\n"
            L"VhdxPath=C:\\vms\\t.vhdx\r\n";
        cfg_temp_path(path, MAX_PATH, L"u16");
        check(cfg_write_text(path, u16, FX_UTF16LE_BOM) &&
              cfg_diff_round(path, L"UTF-16LE BOM"),
              L"UTF-16LE fixture: production == oracle");
        DeleteFileW(path);
    }

    /* ---- The two caps, at their boundaries, through the same oracle ----
       Both caps move VERBATIM and behave DIFFERENTLY: the VM cap stops
       the stream (a later template is dropped with it); the template cap
       skips the block and keeps parsing (a later VM still loads). */

    {
        static const wchar_t vmcap[] =
            L"[VM]\r\nName=one\r\nNetworkMode=0\r\n\r\n"
            L"[VM]\r\nName=two\r\nNetworkMode=3\r\nInternalSwitch=Lab\r\n\r\n"
            L"[VM]\r\nName=three\r\nNetworkMode=0\r\n\r\n"
            L"[Template]\r\nName=dropped\r\n";
        static VmInstance t_vms[2];
        TemplateInfo t_tpls[1];
        wchar_t t_last_iso[MAX_PATH];
        BOOL t_suppress = FALSE;
        int t_vm_count = 0, t_tpl_count = 0;
        FILE *f;

        cfg_temp_path(path, MAX_PATH, L"vmcap");
        check(cfg_write_text(path, vmcap, FX_UTF8_BOM), L"vm-cap fixture written");

        ZeroMemory(t_vms, sizeof(t_vms));
        ZeroMemory(t_tpls, sizeof(t_tpls));
        t_last_iso[0] = L'\0';
        f = asb_config_open_read(path);
        check(f != NULL &&
              asb_load_vm_list_stream(f, t_vms, 2, &t_vm_count, t_tpls, 1,
                                      &t_tpl_count, t_last_iso, MAX_PATH,
                                      &t_suppress) &&
              fclose(f) == 0,
              L"vm cap boundary: stream returns TRUE");
        check(t_vm_count == 2 && t_tpl_count == 0 &&
              wcscmp(t_vms[1].name, L"two") == 0,
              L"vm cap STOPS the stream: the third VM and the later template are dropped");
        check(wcscmp(t_vms[1].internal_switch, L"Lab") == 0 &&
              !t_vms[1].internal_switch_invalid,
              L"vm cap: stopping at the next VM preserves the last accepted selector");
        DeleteFileW(path);

        /* The oracle, same caps, same file. */
        cfg_write_text(path, vmcap, FX_UTF8_BOM);
        oracle_reset();
        if (_wfopen_s(&f, path, L"r,ccs=UTF-8") != 0) f = NULL;
        oracle_parse(f, 2, 1);
        fclose(f);
        check(o_vm_count == 2 && o_tpl_count == 0,
              L"vm cap boundary: oracle agrees (stops the stream)");
        DeleteFileW(path);
    }

    {
        static const wchar_t tplcap[] =
            L"[Template]\r\nName=kept\r\nOsType=Windows\r\n\r\n"
            L"[Template]\r\nName=skipped\r\nOsType=Linux\r\n\r\n"
            L"[VM]\r\nName=after-cap\r\nNetworkMode=0\r\n";
        static VmInstance t_vms[2];
        TemplateInfo t_tpls[1];
        wchar_t t_last_iso[MAX_PATH];
        BOOL t_suppress = FALSE;
        int t_vm_count = 0, t_tpl_count = 0;
        FILE *f;

        cfg_temp_path(path, MAX_PATH, L"tplcap");
        check(cfg_write_text(path, tplcap, FX_UTF8_BOM), L"template-cap fixture written");

        ZeroMemory(t_vms, sizeof(t_vms));
        ZeroMemory(t_tpls, sizeof(t_tpls));
        t_last_iso[0] = L'\0';
        f = asb_config_open_read(path);
        check(f != NULL &&
              asb_load_vm_list_stream(f, t_vms, 2, &t_vm_count, t_tpls, 1,
                                      &t_tpl_count, t_last_iso, MAX_PATH,
                                      &t_suppress) &&
              fclose(f) == 0,
              L"template cap boundary: stream returns TRUE");
        check(t_tpl_count == 1 && t_vm_count == 1 &&
              wcscmp(t_tpls[0].name, L"kept") == 0 &&
              wcscmp(t_vms[0].name, L"after-cap") == 0,
              L"template cap SKIPS the block: the later VM still loads");
        DeleteFileW(path);

        cfg_write_text(path, tplcap, FX_UTF8_BOM);
        oracle_reset();
        if (_wfopen_s(&f, path, L"r,ccs=UTF-8") != 0) f = NULL;
        oracle_parse(f, 2, 1);
        fclose(f);
        check(o_tpl_count == 1 && o_vm_count == 1,
              L"template cap boundary: oracle agrees (skips the block)");
        DeleteFileW(path);
    }

    /* ---- The IN/OUT append rule: a non-zero initial count appends
       without touching existing rows. ---- */
    {
        static const wchar_t two[] =
            L"[VM]\r\nName=appendedA\r\nNetworkMode=0\r\n\r\n"
            L"[VM]\r\nName=appendedB\r\nNetworkMode=3\r\n";
        static VmInstance t_vms[TEST_MAX_ROWS];
        VmInstance before;
        TemplateInfo t_tpls[1];
        wchar_t t_last_iso[MAX_PATH];
        BOOL t_suppress = FALSE;
        int t_vm_count = 1, t_tpl_count = 0;
        FILE *f;

        cfg_temp_path(path, MAX_PATH, L"append");
        check(cfg_write_text(path, two, FX_UTF8_BOM), L"append fixture written");

        ZeroMemory(t_vms, sizeof(t_vms));
        wcscpy_s(t_vms[0].name, 256, L"existing");
        t_vms[0].network_mode = 2;
        wcscpy_s(t_vms[0].net_adapter, 256, L"Ethernet 3");
        t_vms[0].install_complete = TRUE;
        before = t_vms[0];
        t_last_iso[0] = L'\0';
        f = asb_config_open_read(path);
        check(f != NULL &&
              asb_load_vm_list_stream(f, t_vms, TEST_MAX_ROWS, &t_vm_count,
                                      t_tpls, 1, &t_tpl_count, t_last_iso,
                                      MAX_PATH, &t_suppress) &&
              fclose(f) == 0,
              L"append round: stream returns TRUE");
        check(t_vm_count == 3 &&
              cfg_rows_equal(&t_vms[0], &before) &&
              wcscmp(t_vms[1].name, L"appendedA") == 0 &&
              wcscmp(t_vms[2].name, L"appendedB") == 0,
              L"a non-zero initial count appends without touching existing rows");
        DeleteFileW(path);
    }

    /* ---- The FALSE path: a genuinely errored stream. ----
       Writes into a read-mode stream fail AND set ferror (measured);
       a locked byte range makes the read side fail the same way. The
       FALSE detects STREAM errors only - the rows parsed before the
       error still stand. */

    {
        static VmInstance t_vms[TEST_MAX_ROWS];
        TemplateInfo t_tpls[1];
        wchar_t t_last_iso[MAX_PATH];
        BOOL t_suppress = FALSE, r;
        int t_vm_count = 1, t_tpl_count = 0;
        FILE *f;
        static const wchar_t one[] = L"[VM]\r\nName=x\r\nNetworkMode=0\r\n";

        /* Save into a read-mode stream: the fwprintf calls fail and set
           ferror (measured). A row must be present so the loop actually
           writes - an empty save has nothing to fail on. */
        cfg_temp_path(path, MAX_PATH, L"false-save");
        check(cfg_write_text(path, one, FX_UTF8_BOM), L"FALSE-path fixture written");
        ZeroMemory(t_vms, sizeof(t_vms));
        wcscpy_s(t_vms[0].name, 256, L"x");
        t_last_iso[0] = L'\0';
        if (_wfopen_s(&f, path, L"r,ccs=UTF-8") != 0) f = NULL;
        check(f != NULL &&
              !asb_save_vm_list_stream(f, t_vms, t_vm_count, t_tpls, t_tpl_count,
                                       t_last_iso, t_suppress),
              L"save into a read-mode stream -> FALSE (ferror probe)");
        if (f) fclose(f);
        DeleteFileW(path);

        /* Load from an exclusively locked file: the first buffered read
           fails, fgetws stops, ferror is set. */
        cfg_temp_path(path, MAX_PATH, L"false-load");
        check(cfg_write_text(path, one, FX_UTF8_BOM), L"FALSE-path load fixture written");
        {
            HANDLE lock = CreateFileW(path, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (_wfopen_s(&f, path, L"r,ccs=UTF-8") != 0) f = NULL;
            if (lock != INVALID_HANDLE_VALUE && f) {
                OVERLAPPED ov;
                ZeroMemory(&ov, sizeof(ov));
                LockFileEx(lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                           0, 0x7FFFFFFF, 0x7FFFFFFF, &ov);
                ZeroMemory(t_vms, sizeof(t_vms));
                t_vm_count = 0;
                t_last_iso[0] = L'\0';
                r = asb_load_vm_list_stream(f, t_vms, TEST_MAX_ROWS, &t_vm_count,
                                            t_tpls, 1, &t_tpl_count, t_last_iso,
                                            MAX_PATH, &t_suppress);
                UnlockFileEx(lock, 0, 0x7FFFFFFF, 0x7FFFFFFF, &ov);
                check(!r,
                      L"load from an error-injected (locked) read stream -> FALSE");
            } else {
                check(f != NULL, L"FALSE-path load fixture opened (lock unavailable)");
            }
            if (lock != INVALID_HANDLE_VALUE)
                CloseHandle(lock);
            if (f) fclose(f);
        }
        DeleteFileW(path);
    }
}

/* ---- The new key's persistence contract (A8b) ---- */

/* Load row 0's InternalSwitch state from an existing fixture file. */
static BOOL load_switch_row_from_file(const wchar_t *path, wchar_t *out_sel,
                                       size_t sel_cap, BOOL *out_invalid,
                                       BOOL *out_stream_ok)
{
    static VmInstance t_vms[2];
    TemplateInfo t_tpls[1];
    wchar_t t_last_iso[MAX_PATH];
    BOOL t_suppress = FALSE, stream_ok;
    int t_vm_count = 0, t_tpl_count = 0;
    FILE *f;

    ZeroMemory(t_vms, sizeof(t_vms));
    t_last_iso[0] = L'\0';
    f = asb_config_open_read(path);
    if (!f)
        return FALSE;
    stream_ok = asb_load_vm_list_stream(f, t_vms, 2, &t_vm_count, t_tpls, 1,
                                        &t_tpl_count, t_last_iso, MAX_PATH,
                                        &t_suppress);
    fclose(f);

    wcsncpy_s(out_sel, sel_cap, t_vms[0].internal_switch, _TRUNCATE);
    *out_invalid = t_vms[0].internal_switch_invalid;
    *out_stream_ok = stream_ok;
    return t_vm_count == 1;
}

/* Write one [VM] body as a canonical UTF-8 BOM + CRLF fixture, then
   load its row 0. */
static BOOL load_switch_row(const wchar_t *body, wchar_t *out_sel,
                            size_t sel_cap, BOOL *out_invalid,
                            BOOL *out_stream_ok)
{
    wchar_t path[MAX_PATH];
    BOOL ok;

    cfg_temp_path(path, MAX_PATH, L"sw");
    if (!cfg_write_text(path, body, FX_UTF8_BOM))
        return FALSE;
    ok = load_switch_row_from_file(path, out_sel, sel_cap, out_invalid,
                                   out_stream_ok);
    DeleteFileW(path);
    return ok;
}

/* Read a saved file back as text (UTF-8, BOM skipped) for byte-shape
   assertions. */
static wchar_t *cfg_read_text(const wchar_t *path)
{
    size_t len;
    char *raw = cfg_read_bytes(path, &len);
    const char *bytes;
    wchar_t *text = NULL;
    int n;

    if (!raw)
        return NULL;
    bytes = raw;
    if (len >= 3 && raw[0] == '\xEF' && raw[1] == '\xBB' &&
        raw[2] == '\xBF') {
        bytes = raw + 3;
        len -= 3;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)len, NULL, 0);
    if (n > 0) {
        text = (wchar_t *)malloc((size_t)(n + 1) * sizeof(wchar_t));
        if (text) {
            MultiByteToWideChar(CP_UTF8, 0, bytes, (int)len, text, n);
            text[n] = L'\0';
        }
    }
    free(raw);
    return text;
}

static void run_new_key_fixtures(void)
{
    wchar_t sel[INTERNAL_SWITCH_CAP];
    BOOL invalid, stream_ok;
    wchar_t overlong[600];
    wchar_t path[MAX_PATH];
    int i;

    wprintf(L"\n[new-key persistence fixtures]\n");

    for (i = 0; i < 599; i++)
        overlong[i] = L'A';
    overlong[599] = L'\0';

    /* ---- The shared predicate (the one rule, every consumer) ---- */

    check(asb_internal_switch_value_valid(L"Lab", 3),
          L"predicate: an ordinary name is valid");
    check(asb_internal_switch_value_valid(L"", 0),
          L"predicate: the empty value (Auto) is valid");
    check(asb_internal_switch_value_valid(L"(Auto)", 6),
          L"predicate: the literal \"(Auto)\" is an ordinary valid name (never reserved)");
    check(asb_internal_switch_value_valid(L"a\tb\x0001", 4),
          L"predicate: control characters other than CR/LF/NUL are legal");
    check(asb_internal_switch_value_valid(overlong, 255),
          L"predicate: 255 wchars is within the cap (255 < 256)");
    check(!asb_internal_switch_value_valid(overlong, 256),
          L"predicate: a length at the cap is rejected (over-long)");
    check(!asb_internal_switch_value_valid(L"La\rb", 4),
          L"predicate: an embedded CR is rejected");
    check(!asb_internal_switch_value_valid(L"La\nb", 4),
          L"predicate: an embedded LF is rejected");
    check(!asb_internal_switch_value_valid(L"La\0b", 4),
          L"predicate: an embedded NUL (inside the length view) is rejected");
    check(!asb_internal_switch_value_valid(L"\xD800" L"abc", 4),
          L"predicate: a lone high surrogate in first position is rejected");
    check(!asb_internal_switch_value_valid(L"a\xD800" L"b", 4),
          L"predicate: a lone high surrogate mid-string is rejected");
    check(!asb_internal_switch_value_valid(L"abc\xD800", 4),
          L"predicate: a lone high surrogate in last position is rejected");
    check(!asb_internal_switch_value_valid(L"\xDC00" L"abc", 4),
          L"predicate: a lone low surrogate is rejected");
    check(!asb_internal_switch_value_valid(L"a\xD800" L"X", 3),
          L"predicate: a high half not followed by its low half is rejected");
    check(asb_internal_switch_value_valid(L"a\xD800\xDC00" L"b", 4),
          L"predicate: a correctly PAIRED surrogate is valid");
    check(!asb_internal_switch_value_valid(L"a\xFFFF" L"b", 3),
          L"predicate: U+FFFF is rejected (the baseline saver truncates there, measured)");

    /* ---- Loader tri-state, duplicate keys, marker interplay ---- */

    {
        wchar_t body[2048];
        swprintf_s(body, ARRAYSIZE(body),
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitch=%s\r\n",
            overlong);
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              stream_ok && invalid && sel[0] == L'\0',
              L"loader: an over-long value marks the row INVALID (selector empty; the stream itself is TRUE)");
    }

    {
        wchar_t body[2048];
        swprintf_s(body, ARRAYSIZE(body),
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitch=Lab\r\n"
            L"InternalSwitch=%s\r\n", overlong);
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              invalid && sel[0] == L'\0',
              L"duplicate keys: valid then invalid -> INVALID (the LAST occurrence wins)");
    }

    {
        wchar_t body[2048];
        swprintf_s(body, ARRAYSIZE(body),
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitch=%s\r\n"
            L"InternalSwitch=Lab\r\n", overlong);
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              !invalid && wcscmp(sel, L"Lab") == 0,
              L"duplicate keys: invalid then valid -> VALID (recovery)");
    }

    {
        static const wchar_t body[] =
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitch=Lab\r\n"
            L"InternalSwitch=\r\n";
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              !invalid && sel[0] == L'\0',
              L"duplicate keys: valid then explicit empty -> VALID empty (Auto)");
    }

    {
        static const wchar_t body[] =
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitch=Lab\r\n"
            L"InternalSwitchInvalid=1\r\n";
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              !invalid && wcscmp(sel, L"Lab") == 0,
              L"a valid value with a stale marker (marker AFTER the key): the value wins");
    }

    {
        static const wchar_t body[] =
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitchInvalid=1\r\n"
            L"InternalSwitch=Lab\r\n";
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              !invalid && wcscmp(sel, L"Lab") == 0,
              L"a valid value with a stale marker (marker BEFORE the key): the value wins");
    }

    {
        static const wchar_t body[] =
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitchInvalid=1\r\n"
            L"InternalSwitch=\r\n";
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              !invalid && sel[0] == L'\0',
              L"an explicit empty value clears a stale marker (the Auto recovery)");
    }

    {
        static const wchar_t body[] =
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitchInvalid=1\r\n";
        check(load_switch_row(body, sel, ARRAYSIZE(sel), &invalid, &stream_ok) &&
              invalid && sel[0] == L'\0',
              L"a marker alone (no value): the row stays INVALID, start refused");
    }

    /* ---- Round trip: load -> save -> load, plus the save byte shape ----
       One INVALID row and one VALID row: the INVALID row persists via
       its marker alone, the VALID row via its value. Consecutive blocks
       stay isolated; EOF finishes the last block. */

    {
        static VmInstance t_vms[3], r_vms[3];
        TemplateInfo t_tpls[1], r_tpls[1];
        wchar_t t_last_iso[MAX_PATH], r_last_iso[MAX_PATH];
        BOOL t_suppress = FALSE, r_suppress = FALSE;
        int t_vm_count = 0, r_vm_count = 0, t_tpl_count = 0, r_tpl_count = 0;
        wchar_t *full = (wchar_t *)malloc(sizeof(wchar_t) * 4096);
        FILE *f;
        wchar_t *saved_text;

        check(full != NULL, L"round-trip fixture allocation");
        if (full) {
            swprintf_s(full, 4096,
                L"[VM]\r\nName=first\r\nNetworkMode=3\r\nInternalSwitch=%s\r\n"
                L"InternalSwitchInvalid=1\r\n"
                L"\r\n"
                L"[VM]\r\nName=second\r\nNetworkMode=3\r\nInternalSwitch=Keep\r\n",
                overlong);
            cfg_temp_path(path, MAX_PATH, L"swround");
            check(cfg_write_text(path, full, FX_UTF8_BOM), L"round-trip fixture written");

            ZeroMemory(t_vms, sizeof(t_vms));
            t_last_iso[0] = L'\0';
            f = asb_config_open_read(path);
            check(f != NULL &&
                  asb_load_vm_list_stream(f, t_vms, 3, &t_vm_count, t_tpls, 1,
                                          &t_tpl_count, t_last_iso, MAX_PATH,
                                          &t_suppress) &&
                  fclose(f) == 0,
                  L"round-trip first load: stream TRUE (one invalid row is NOT a stream failure)");
            check(t_vm_count == 2 && t_vms[0].internal_switch_invalid &&
                  t_vms[0].internal_switch[0] == L'\0' &&
                  wcscmp(t_vms[1].internal_switch, L"Keep") == 0 &&
                  !t_vms[1].internal_switch_invalid,
                  L"consecutive blocks: the first row INVALID, the second VALID (EOF finishes it)");

            f = asb_config_open_write(path);
            check(f != NULL &&
                  asb_save_vm_list_stream(f, t_vms, t_vm_count, t_tpls, t_tpl_count,
                                          t_last_iso, t_suppress) &&
                  fclose(f) == 0,
                  L"round-trip save: stream TRUE");

            ZeroMemory(r_vms, sizeof(r_vms));
            r_last_iso[0] = L'\0';
            f = asb_config_open_read(path);
            check(f != NULL &&
                  asb_load_vm_list_stream(f, r_vms, 3, &r_vm_count, r_tpls, 1,
                                          &r_tpl_count, r_last_iso, MAX_PATH,
                                          &r_suppress) &&
                  fclose(f) == 0,
                  L"round-trip second load: stream TRUE");
            check(r_vm_count == 2 &&
                  r_vms[0].internal_switch_invalid &&
                  r_vms[0].internal_switch[0] == L'\0' &&
                  wcscmp(r_vms[1].internal_switch, L"Keep") == 0 &&
                  !r_vms[1].internal_switch_invalid,
                  L"round-trip: values equal after load->save->load (INVALID persists via the marker)");

            /* Save byte shape: the new key's line immediately after
               NetworkMode= (ahead of every unvalidated string key), the
               INVALID row persisting through the marker alone, CRLF,
               UTF-8 BOM (checked as text after the BOM skip). */
            saved_text = cfg_read_text(path);
            check(saved_text != NULL &&
                  wcsstr(saved_text,
                         L"NetworkMode=3\r\nInternalSwitch=Keep\r\n") != NULL,
                  L"save byte shape: the InternalSwitch line sits immediately after NetworkMode=");
            check(saved_text != NULL &&
                  wcsstr(saved_text, L"InternalSwitchInvalid=1\r\n") != NULL,
                  L"save byte shape: the INVALID row persists via the marker line alone (no value line)");
            free(saved_text);
            DeleteFileW(path);
            free(full);
        }
    }

    /* UTF-16LE file entrance: a lone surrogate code unit arrives intact
       (measured) and is rejected by the loader's check. */
    {
        static const wchar_t body[] =
            L"[VM]\r\nName=vm\r\nNetworkMode=3\r\nInternalSwitch=\xD800" L"Lab\r\n";
        cfg_temp_path(path, MAX_PATH, L"sw-u16");
        check(cfg_write_text(path, body, FX_UTF16LE_BOM) &&
              load_switch_row_from_file(path, sel, ARRAYSIZE(sel), &invalid,
                                        &stream_ok) &&
              stream_ok && invalid && sel[0] == L'\0',
              L"UTF-16LE entrance: a lone surrogate marks the row INVALID (stream TRUE)");
        DeleteFileW(path);
    }

    /* Neighbour direction: a NetAdapter value containing U+FFFF (an
       entrance this feature does not own) truncates its OWN line on
       save - the InternalSwitch line above it survives intact. */
    {
        static VmInstance t_vms[2];
        TemplateInfo t_tpls[1], r_tpls[1];
        wchar_t t_last_iso[MAX_PATH];
        BOOL t_suppress = FALSE;
        int t_vm_count = 1, t_tpl_count = 0;
        static VmInstance r_vms[2];
        wchar_t r_last_iso[MAX_PATH];
        BOOL r_suppress = FALSE;
        int r_vm_count = 0, r_tpl_count = 0;
        FILE *f;

        cfg_temp_path(path, MAX_PATH, L"neighbour");
        ZeroMemory(t_vms, sizeof(t_vms));
        ZeroMemory(t_tpls, sizeof(t_tpls));
        ZeroMemory(r_vms, sizeof(r_vms));
        ZeroMemory(r_tpls, sizeof(r_tpls));
        t_last_iso[0] = L'\0';
        wcscpy_s(t_vms[0].name, 256, L"nb");
        t_vms[0].network_mode = 3;
        wcscpy_s(t_vms[0].internal_switch, INTERNAL_SWITCH_CAP, L"LabSwitch");
        t_vms[0].net_adapter[0] = L'E';
        t_vms[0].net_adapter[1] = 0xFFFF;
        t_vms[0].net_adapter[2] = L'X';
        t_vms[0].net_adapter[3] = L'\0';

        f = asb_config_open_write(path);
        check(f != NULL &&
              asb_save_vm_list_stream(f, t_vms, t_vm_count, t_tpls, t_tpl_count,
                                      t_last_iso, t_suppress) &&
              fclose(f) == 0,
              L"neighbour fixture save: stream TRUE (ferror clean - the truncation is silent, measured)");
        {
            r_last_iso[0] = L'\0';
            f = asb_config_open_read(path);
            check(f != NULL &&
                  asb_load_vm_list_stream(f, r_vms, 2, &r_vm_count, r_tpls, 1,
                                          &r_tpl_count, r_last_iso, MAX_PATH,
                                          &r_suppress) &&
                  fclose(f) == 0,
                  L"neighbour fixture reload: stream TRUE");
            check(r_vm_count == 1 &&
                  wcscmp(r_vms[0].internal_switch, L"LabSwitch") == 0,
                  L"neighbour direction: InternalSwitch survives a U+FFFF NetAdapter value above it");
        }
        DeleteFileW(path);
    }
}

/* ---- Internal classification fixtures (A5b) ----
 *
 * All pure functions run through the production classify TU linked
 * here: E4's verdict over synthetic probe evidence (parsed through the
 * REAL document parser), E1's ladder over synthetic RAW records, the
 * E0/E2 resolver, the projection, and the reason formatter. Synthetic
 * GUIDs stand in for host switches; the product GUIDs are the real
 * values via the shared single-source initializers. */

static const GUID G_Q   = { 0x11111111, 0x1111, 0x1111,
                            { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 } };
static const GUID G_P   = { 0x22222222, 0x2222, 0x2222,
                            { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 } };
static const GUID G_P2  = { 0x32323232, 0x3232, 0x3232,
                            { 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32, 0x32 } };
static const GUID G_E   = { 0x33333333, 0x3333, 0x3333,
                            { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33 } };
static const GUID G_R   = { 0x44444444, 0x4444, 0x4444,
                            { 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44 } };
static const GUID G_DEF = { 0x55555555, 0x5555, 0x5555,
                            { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 } };
static const GUID G_X   = { 0x66666666, 0x6666, 0x6666,
                            { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66 } };
static const GUID G_U   = { 0x77777777, 0x7777, 0x7777,
                            { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 } };
static const GUID G_T   = { 0x88888888, 0x8888, 0x8888,
                            { 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 } };

#define STR_Q   L"11111111-1111-1111-1111-111111111111"
#define STR_P   L"22222222-2222-2222-2222-222222222222"
#define STR_E   L"33333333-3333-3333-3333-333333333333"
#define STR_DEF L"55555555-5555-5555-5555-555555555555"
#define STR_X   L"66666666-6666-6666-6666-666666666666"
#define STR_T   L"88888888-8888-8888-8888-888888888888"
#define STR_INT L"a5b01234-5678-9abc-def0-112233445577"   /* APPSANDBOX_INTERNAL_GUID */
#define STR_NAT L"a5b01234-5678-9abc-def0-112233445566"   /* APPSANDBOX_NAT_GUID */

static void census_free(HcnInternalSwitchCensus *c)
{
    if (c->entries) {
        HeapFree(GetProcessHeap(), 0, c->entries);
        c->entries = NULL;
    }
    c->count = 0;
}

/* A readable named switch record. */
static HcnRawSwitchResult mk_sw(const GUID *id, const wchar_t *name,
                                int bound_ports)
{
    HcnRawSwitchResult sw;
    ZeroMemory(&sw, sizeof(sw));
    sw.id = *id;
    sw.has_id = TRUE;
    wcsncpy_s(sw.name, INTERNAL_SWITCH_CAP, name, _TRUNCATE);
    sw.bound_external_ports = bound_ports;
    return sw;
}

/* A switch whose ElementName was missing (unreadable). */
static HcnRawSwitchResult mk_sw_unreadable(const GUID *id, int bound_ports)
{
    HcnRawSwitchResult sw;
    ZeroMemory(&sw, sizeof(sw));
    sw.id = *id;
    sw.has_id = TRUE;
    sw.name_unreadable = TRUE;
    sw.bound_external_ports = bound_ports;
    return sw;
}

/* A switch whose ElementName is present but rejected by the shared
   character rule (the builder's decision, replicated here through the
   SAME helper the builder uses). */
static HcnRawSwitchResult mk_sw_unusable(const GUID *id, const wchar_t *name,
                                         int bound_ports)
{
    HcnRawSwitchResult sw = mk_sw(id, name, bound_ports);
    sw.name_unusable =
        !asb_internal_switch_value_valid(name, wcslen(name));
    return sw;
}

/* An iport with a readable, attribution-safe label. */
static HcnRawIportResult mk_ip(const wchar_t *label, WmiWalkResult walk,
                               const GUID *reached)
{
    HcnRawIportResult ip;
    ZeroMemory(&ip, sizeof(ip));
    wcsncpy_s(ip.name, INTERNAL_SWITCH_CAP, label, _TRUNCATE);
    ip.walk_result = walk;
    if (reached)
        ip.reached_switch_id = *reached;
    return ip;
}

/* An iport whose ElementName was missing. */
static HcnRawIportResult mk_ip_unreadable(WmiWalkResult walk,
                                          const GUID *reached)
{
    HcnRawIportResult ip;
    ZeroMemory(&ip, sizeof(ip));
    ip.name_unreadable = TRUE;
    ip.walk_result = walk;
    if (reached)
        ip.reached_switch_id = *reached;
    return ip;
}

/* One of the product's own HNS network ports: the WMI Name parses as a
   GUID equal to a fixed product network ID (the product-port skip's
   input). */
static HcnRawIportResult mk_ip_product(const GUID *name_id, const wchar_t *label,
                                        WmiWalkResult walk, const GUID *reached)
{
    HcnRawIportResult ip = mk_ip(label, walk, reached);
    ip.has_name_id = TRUE;
    ip.iport_name_id = *name_id;
    return ip;
}

static const HcnInternalSwitchEntry *census_find(
    const HcnInternalSwitchCensus *c, const GUID *id)
{
    size_t i;
    for (i = 0; i < c->count; i++)
        if (IsEqualGUID(&c->entries[i].switch_id, id))
            return &c->entries[i];
    return NULL;
}

/* ---- E3/E4: classify_props over synthetic probe evidence ---- */

static HcnProbeEvidence mk_evidence(const wchar_t *json, HcnProbeKind kind,
                                    HRESULT hr)
{
    HcnProbeEvidence ev;
    ZeroMemory(&ev, sizeof(ev));
    ev.kind = kind;
    ev.hr = hr;
    if (kind == HCN_PROBE_PARSED) {
        HRESULT phr = parse_hcn_network_properties(json, &ev.props);
        if (FAILED(phr)) {
            wprintf(L"      (evidence parse failed: 0x%08X for %s)\n", phr,
                    json);
            ev.kind = HCN_PROBE_QUERY_FAILED;
        }
    }
    return ev;
}

static void check_props(const GUID *guid, const wchar_t *json, BOOL defer,
                        HcnCandidateVerdict want, HcnInternalReasonCode want_code,
                        const wchar_t *label)
{
    HcnProbeEvidence ev = mk_evidence(json, HCN_PROBE_PARSED, 0);
    HcnInternalClassifyVerdict v;

    hcn_internal_classify_props(guid, &ev, defer, &v);
    if (v.verdict == want && (want != HCN_CAND_REJECTED ||
                              v.reason_code == want_code))
        check(TRUE, label);
    else {
        check(FALSE, label);
        wprintf(L"      (verdict %d code %d)\n", (int)v.verdict,
                (int)v.reason_code);
    }
}

static void run_internal_classify_fixtures(void)
{
    wchar_t doc[1024];

    wprintf(L"\n[internal classify_props fixtures]\n");

    /* E3's branches. */
    {
        HcnProbeEvidence ev;
        HcnInternalClassifyVerdict v;

        ZeroMemory(&ev, sizeof(ev));
        ev.kind = HCN_PROBE_OPEN_NOT_FOUND;
        ev.hr = 0x803B0005;
        hcn_internal_classify_props(&G_T, &ev, FALSE, &v);
        check(v.verdict == HCN_CAND_REJECTED &&
              v.reason_code == HCN_IR_NO_HOST_NETWORK,
              L"E3: open exact not-found -> the no-host-network-object line");

        ev.kind = HCN_PROBE_QUERY_FAILED;
        ev.hr = E_FAIL;
        hcn_internal_classify_props(&G_T, &ev, FALSE, &v);
        check(v.verdict == HCN_CAND_REJECTED &&
              v.reason_code == HCN_IR_CORRELATION_UNAVAILABLE,
              L"E3: query export missing / query failed -> correlation-unavailable");

        ev.kind = HCN_PROBE_OPEN_ERROR;
        ev.hr = 0x80070005;
        hcn_internal_classify_props(&G_T, &ev, FALSE, &v);
        check(v.verdict == HCN_CAND_REJECTED &&
              v.reason_code == HCN_IR_CORRELATION_UNAVAILABLE &&
              v.hr == 0x80070005,
              L"E3: other open error -> the original HRESULT channel (correlation family)");
    }

    /* E4's ordered sequence, through the real parser. The probe opens
       the network BY the WMI switch GUID, so the E3 identity check
       needs the document's ID to equal the same GUID. */
    {
        static const GUID s_internal_guid = APPSANDBOX_INTERNAL_GUID_INIT;
        static const GUID s_nat_guid = APPSANDBOX_NAT_GUID_INIT;
        static const GUID s_owned_id = {
            0x46322DE4, 0x4D9E, 0x576C,
            { 0x90, 0x46, 0x85, 0x49, 0x59, 0x0C, 0xA0, 0x28 }
        };

        swprintf_s(doc, ARRAYSIZE(doc),
            L"{\"ID\":\"%s\",\"Name\":\"AppSandboxInternal\",\"Type\":\"ICS\"}",
            STR_INT);
        check_props(&s_internal_guid, doc, FALSE, HCN_CAND_OWNED, 0,
                    L"E4: fixed internal GUID + exact AppSandboxInternal/ICS -> OWNED");

        swprintf_s(doc, ARRAYSIZE(doc),
            L"{\"ID\":\"%s\",\"Name\":\"ForeignNet\",\"Type\":\"ICS\"}", STR_INT);
        check_props(&s_internal_guid, doc, FALSE, HCN_CAND_REJECTED,
                    HCN_IR_FIXED_IDENTITY_CONFLICT,
                    L"E4: fixed internal GUID + readable foreign identity -> conflict (MISMATCH)");

        swprintf_s(doc, ARRAYSIZE(doc),
            L"{\"ID\":\"%s\",\"Name\":\"AppSandboxInternal\"}", STR_INT);
        check_props(&s_internal_guid, doc, FALSE, HCN_CAND_REJECTED,
                    HCN_IR_UNREADABLE_IDENTITY,
                    L"E4: fixed internal GUID + missing Type -> unreadable identity (INDETERMINATE)");

        swprintf_s(doc, ARRAYSIZE(doc),
            L"{\"ID\":\"%s\",\"Name\":\"Whatever\",\"Type\":\"ICS\"}", STR_NAT);
        check_props(&s_nat_guid, doc, FALSE, HCN_CAND_REJECTED, HCN_IR_NAT,
                    L"E4: the NAT GUID -> the NAT line");

        swprintf_s(doc, ARRAYSIZE(doc),
            L"{\"ID\":\"46322de4-4d9e-576c-9046-8549590ca028\","
            L"\"Name\":\"AppSandboxExternal-{c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f}\","
            L"\"Type\":\"Transparent\"}");
        check_props(&s_owned_id, doc, FALSE, HCN_CAND_REJECTED, HCN_IR_OWNED_EXTERNAL,
                    L"E4: owned-external identity (Name parse + owned_id equality) -> the owned line");
    }

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"Internal\"}", STR_T);
    check_props(&G_T, doc, FALSE, HCN_CAND_BORROWED, 0,
                L"E4: Type Internal -> BORROWED");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"ICS\"}", STR_DEF);
    check_props(&G_DEF, doc, FALSE, HCN_CAND_BORROWED, 0,
                L"E4: Type ICS -> BORROWED (network_id = the actual ID)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"Transparent\"}", STR_T);
    check_props(&G_T, doc, FALSE, HCN_CAND_REJECTED, HCN_IR_UNSUPPORTED_TYPE,
                L"E4: Type Transparent -> the type-specific line");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\"}", STR_T);
    check_props(&G_T, doc, FALSE, HCN_CAND_REJECTED, HCN_IR_UNSUPPORTED_TYPE,
                L"E4: Type missing -> the type-specific line with (missing)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"Bat\"}", STR_T);
    check_props(&G_T, doc, FALSE, HCN_CAND_REJECTED, HCN_IR_UNSUPPORTED_TYPE,
                L"E4: an unjoinable type -> the type-specific line with the raw <T>");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"Private\"}", STR_T);
    check_props(&G_T, doc, TRUE, HCN_CAND_REJECTED, HCN_IR_NOT_INTERNAL,
                L"E4 DEFER context: Type Private -> E2's no-host-adapter line (not the type line)");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"Private\"}", STR_T);
    check_props(&G_T, doc, FALSE, HCN_CAND_REJECTED, HCN_IR_UNSUPPORTED_TYPE,
                L"E4 non-DEFER context: Type Private -> the type-specific line");

    /* E3's identity checks on the parsed document (defensive, fail closed). */
    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"Name\":\"Borrowed\",\"Type\":\"Internal\"}", STR_X);
    check_props(&G_T, doc, FALSE, HCN_CAND_REJECTED,
                HCN_IR_CORRELATION_UNAVAILABLE,
                L"E3: a parsed ID != the WMI switch GUID -> correlation failure");

    swprintf_s(doc, ARRAYSIZE(doc),
        L"{\"ID\":\"%s\",\"SwitchGuid\":\"%s\",\"Name\":\"Borrowed\","
        L"\"Type\":\"Internal\"}", STR_T, STR_X);
    check_props(&G_T, doc, FALSE, HCN_CAND_REJECTED,
                HCN_IR_CORRELATION_UNAVAILABLE,
                L"E3: a present SwitchGuid disagreeing with the switch GUID -> correlation failure");

    /* The fixed-identity tri-state, driven directly. */
    {
        HcnNetworkProps props;
        ZeroMemory(&props, sizeof(props));
        check(internal_fixed_identity(&props) == HCN_ID_INDETERMINATE,
              L"fixed identity: no Name/Type -> INDETERMINATE");

        props.has_name = TRUE; props.has_type = TRUE;
        wcscpy_s(props.name, ARRAYSIZE(props.name), L"AppSandboxInternal");
        wcscpy_s(props.type, ARRAYSIZE(props.type), L"ICS");
        check(internal_fixed_identity(&props) == HCN_ID_MATCH,
              L"fixed identity: AppSandboxInternal + ICS -> MATCH");

        wcscpy_s(props.type, ARRAYSIZE(props.type), L"Internal");
        check(internal_fixed_identity(&props) == HCN_ID_MISMATCH,
              L"fixed identity: readable but foreign -> MISMATCH");

        props.has_type = FALSE;
        check(internal_fixed_identity(&props) == HCN_ID_INDETERMINATE,
              L"fixed identity: missing Type -> INDETERMINATE (never an assert)");
    }
}

/* ---- E1: the ladder over RAW records ---- */

static void run_internal_resolver_fixtures(void)
{
    HcnInternalSwitchCensus c;
    HcnRawSwitchResult sws[6];
    HcnRawIportResult ips[6];
    HcnInternalResolution res;

    wprintf(L"\n[internal classify_switches / resolver fixtures]\n");

    /* The healthy host: Q internal (WALK_OK), E external (bound ports),
       P private (no iports). */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Q", 0);
        sws[1] = mk_sw(&G_E, L"E", 1);
        sws[2] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        hcn_internal_classify_switches(sws, 3, ips, 1, &c);
        check(c.state == HCN_CENSUS_OK && c.count == 3 &&
              census_find(&c, &G_Q)->sw_class == HCN_SW_INTERNAL &&
              census_find(&c, &G_E)->sw_class == HCN_SW_EXTERNAL &&
              census_find(&c, &G_P)->sw_class == HCN_SW_PRIVATE &&
              !c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"E1 ladder: WALK_OK -> INTERNAL, bound ports -> EXTERNAL, nothing -> PRIVATE");
        check(census_find(&c, &G_Q)->verdict == HCN_CAND_UNKNOWN,
              L"verdict default: an unprobed entry is UNKNOWN (never the zero value OWNED)");
        hcn_internal_resolve_selector(&c, L"Q", &res);
        check(res.outcome == HCN_IRES_PROCEED && !res.defer_adjudication &&
              IsEqualGUID(&res.probe_guid, &G_Q) && res.n == 1,
              L"E2: one INTERNAL match -> PROCEED (the terminal match, no defer)");
        hcn_internal_resolve_selector(&c, L"Absent", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_NOT_FOUND &&
              res.n == 1 && IsEqualGUID(&res.probe_guid, &GUID_NULL),
              L"E2: zero matches anywhere -> not-found (n = the INTERNAL count; no probe target)");
        census_free(&c);
    }

    /* (alpha) The product-port skip: a failed iport whose ElementName
       matches user switch U but whose WMI Name == the internal product
       GUID is omitted - no signal, no flag; U classifies by its own
       WALK_OK chain. */
    {
        static const GUID s_internal_guid = APPSANDBOX_INTERNAL_GUID_INIT;
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_U, L"U", 0);
        ips[0] = mk_ip(L"UPort", WMI_WALK_OK, &G_U);
        ips[1] = mk_ip_product(&s_internal_guid, L"U", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 2, &c);
        check(c.state == HCN_CENSUS_OK &&
              census_find(&c, &G_U)->sw_class == HCN_SW_INTERNAL &&
              !c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"product-port skip (alpha): the product port's failed walk injects no signal");
        census_free(&c);
    }

    /* (beta) A WALK_OK product iport whose reached GUID is absent from
       the switch set: the port is omitted AND the census records
       walk-LEVEL UNAVAILABLE (the skip exempts contributions, not
       completeness). */
    {
        static const GUID s_nat_guid = APPSANDBOX_NAT_GUID_INIT;
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Q", 0);
        ips[0] = mk_ip_product(&s_nat_guid, L"Q", WMI_WALK_OK, &G_X);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(c.state == HCN_CENSUS_UNAVAILABLE &&
              c.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"product-port skip (beta): a walk reaching a foreign GUID -> walk-LEVEL UNAVAILABLE");
        hcn_internal_resolve_selector(&c, L"Q", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"resolver layer 1: an UNAVAILABLE census refuses before any name layer");
        census_free(&c);
    }

    /* (gamma) A WALK_OK product iport reaching the Default Switch: the
       Default Switch classifies INTERNAL from its OWN iport; the
       product port contributes nothing. */
    {
        static const GUID s_internal_guid = APPSANDBOX_INTERNAL_GUID_INIT;
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_DEF, L"Default Switch", 0);
        ips[0] = mk_ip_product(&s_internal_guid, L"Default Switch",
                               WMI_WALK_OK, &G_DEF);
        ips[1] = mk_ip(L"Default Switch", WMI_WALK_OK, &G_DEF);
        hcn_internal_classify_switches(sws, 1, ips, 2, &c);
        check(c.state == HCN_CENSUS_OK &&
              census_find(&c, &G_DEF)->sw_class == HCN_SW_INTERNAL &&
              !c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"product-port skip (gamma): the Default Switch classifies INTERNAL from its own iport");
        census_free(&c);
    }

    /* The attribution helper's tri-state: NOT_CONNECTED no-match omits;
       WALK_ERROR no-match sets the unknown-owner flag. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip(L"Orphan", WMI_WALK_NOT_CONNECTED, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(c.state == HCN_CENSUS_EMPTY &&
              census_find(&c, &G_P)->sw_class == HCN_SW_PRIVATE &&
              !c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"attribution: NOT_CONNECTED no-match -> omitted entirely (no flag)");

        ZeroMemory(&c, sizeof(c));
        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(c.has_unknown_owner_failure && c.unattributed_host_port &&
              census_find(&c, &G_P)->sw_class == HCN_SW_PRIVATE,
              L"attribution: WALK_ERROR no-match -> has_unknown_owner_failure (the owner is unknown)");

        /* (i): the unrelated Private stays PRIVATE; a missing selector
           is NOT fatal (not-found, the flag clear case). */
        ips[0] = mk_ip(L"Orphan", WMI_WALK_NOT_CONNECTED, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"Absent", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_NOT_FOUND,
              L"E0 (i): NOT_CONNECTED bystander, flag clear -> not-found (NOT fatal)");
        census_free(&c);
    }

    /* (ii): a failed iport matching Private P -> P UNCLASSIFIED + the
       aggregate flag (attributed, NOT unknown-owner); resolving P is
       fatal (premise: zero-evidence match). */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip(L"P", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(census_find(&c, &G_P)->sw_class == HCN_SW_UNCLASSIFIED &&
              c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"E1: an attributed failure signal -> UNCLASSIFIED (aggregate flag only)");
        hcn_internal_resolve_selector(&c, L"P", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"E0 (ii): an UNCLASSIFIED match is fatal by the scoping rule, flag or not");
        census_free(&c);
    }

    /* (iii): fusion - a failed iport matching Internal T which also
       has a WALK_OK chain: fusion keeps INTERNAL, the flag is set, and
       resolving T proceeds (terminal-evidence match, premise fails). */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_T, L"T", 0);
        ips[0] = mk_ip(L"TPort", WMI_WALK_OK, &G_T);
        ips[1] = mk_ip(L"T", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 2, &c);
        check(census_find(&c, &G_T)->sw_class == HCN_SW_INTERNAL &&
              c.unattributed_host_port,
              L"E1 fusion: a successful chain wins, later failures never demote");
        hcn_internal_resolve_selector(&c, L"T", &res);
        check(res.outcome == HCN_IRES_PROCEED && !res.defer_adjudication,
              L"E0 (iii): fusion match -> PROCEED (never gated)");
        census_free(&c);
    }

    /* (iv): a failed iport attributed to an unrelated readable name;
       the selector is an EXTERNAL switch (bound ports) - the external
       line stands, NOT fatal. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_E, L"I211", 1);
        sws[1] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip(L"Z", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"I211", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_EXTERNAL_SWITCH,
              L"E0 (iv): a terminal EXTERNAL match -> the external line (never gated)");
        census_free(&c);
    }

    /* (v)/(v-bis): an absent selector with unknown-owner evidence of
       both flavors is fatal. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip_unreadable(WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"Absent", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"E0 (v): an unreadable failed iport + an absent selector -> inventory-unavailable");

        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"Absent", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"E0 (v-bis): a WALK_ERROR no-match iport + an absent selector -> inventory-unavailable (the owner could be the target)");
        census_free(&c);
    }

    /* (vi): the PRIVATE arm - a unique readable PRIVATE match with
       unknown-owner evidence of EITHER flavor -> NOT fatal: the DEFER
       arm routes it to E3. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip_unreadable(WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"P", &res);
        check(res.outcome == HCN_IRES_PROCEED && res.defer_adjudication &&
              IsEqualGUID(&res.probe_guid, &G_P),
              L"E0 (vi): unique PRIVATE match + unknown-owner evidence (unreadable flavor) -> DEFER");

        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"P", &res);
        check(res.outcome == HCN_IRES_PROCEED && res.defer_adjudication,
              L"E0 (vi): unique PRIVATE match + unknown-owner evidence (WALK_ERROR no-match flavor) -> DEFER");
        census_free(&c);
    }

    /* (vii): an unreadable-named PRIVATE row is fatal ONLY under the
       premise - a healthy readable INTERNAL match is never gated. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Q", 0);
        sws[1] = mk_sw_unreadable(&G_R, 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        ips[1] = mk_ip(L"Q", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 2, &c);
        hcn_internal_resolve_selector(&c, L"Q", &res);
        check(res.outcome == HCN_IRES_PROCEED,
              L"E0 (vii): a terminal match + an unreadable PRIVATE bystander -> NOT fatal");
        census_free(&c);
    }

    /* (viii): fusion + self-attribution - an INTERNAL switch with one
       WALK_OK iport and one failed iport named like itself. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_T, L"T", 0);
        ips[0] = mk_ip(L"TPort", WMI_WALK_OK, &G_T);
        ips[1] = mk_ip(L"T", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 2, &c);
        check(census_find(&c, &G_T)->sw_class == HCN_SW_INTERNAL &&
              c.unattributed_host_port && c.has_unknown_owner_failure == FALSE,
              L"E0 (viii): fusion + self-attribution -> INTERNAL stays, aggregate flag set");
        hcn_internal_resolve_selector(&c, L"T", &res);
        check(res.outcome == HCN_IRES_PROCEED,
              L"E0 (viii): the gate does not fire for a fusion'd match");
        census_free(&c);
    }

    /* The paired counterexample: P(Private) + Q(Internal, WALK_OK) with
       a failed iport whose label is Q (attributed) vs OldQ (WALK_ERROR
       no-match) - one boolean cannot carry the distinction. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Q", 0);
        sws[1] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        ips[1] = mk_ip(L"Q", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 2, &c);
        check(c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"evidence fields: an ATTRIBUTED failure sets the aggregate only");
        hcn_internal_resolve_selector(&c, L"P", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_NOT_INTERNAL,
              L"paired counterexample (attributed): resolving P -> the no-host-adapter line");

        ips[1] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 2, &c);
        check(c.has_unknown_owner_failure,
              L"evidence fields: an UNATTRIBUTED WALK_ERROR sets has_unknown_owner_failure");
        hcn_internal_resolve_selector(&c, L"Absent", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"paired counterexample (unattributed): an absent selector -> inventory-unavailable");
        census_free(&c);
    }

    /* E2's ordered rules: the duplicate rule is flag-INDEPENDENT and
       E0's (b) precedes it. */
    {
        /* flag clear + two same-named readable PRIVATES -> duplicate. */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"Same", 0);
        sws[1] = mk_sw(&G_P2, L"Same", 0);
        hcn_internal_classify_switches(sws, 2, NULL, 0, &c);
        hcn_internal_resolve_selector(&c, L"Same", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_DUPLICATE,
              L"E2 ordered: flag CLEAR + duplicate PRIVATEs -> the duplicate line (never the class line)");

        /* flag set + duplicate + no unreadable row -> duplicate. */
        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"Same", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_DUPLICATE,
              L"E2 ordered: flag SET + duplicate (no unreadable row) -> the duplicate line");

        /* flag set + duplicate + an unreadable-named PRIVATE row ->
           E0's (b) precedes E2's duplicate line. */
        sws[2] = mk_sw_unreadable(&G_R, 0);
        hcn_internal_classify_switches(sws, 3, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"Same", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"E2 ordered: flag SET + duplicate + unreadable PRIVATE row -> E0(b)'s classification-incomplete code");
        census_free(&c);
    }

    /* More than one INTERNAL-class match -> the duplicate reason. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Same", 0);
        sws[1] = mk_sw(&G_T, L"Same", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        ips[1] = mk_ip(L"TPort", WMI_WALK_OK, &G_T);
        hcn_internal_classify_switches(sws, 2, ips, 2, &c);
        hcn_internal_resolve_selector(&c, L"Same", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_DUPLICATE,
              L"E2: two INTERNAL matches -> the duplicate line (fail-closed)");
        census_free(&c);
    }

    /* E2 rule 3: exactly one INTERNAL match + a same-named readable
       PRIVATE row -> still proceed (the terminal match is what the
       name resolves to). */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Shared", 0);
        sws[1] = mk_sw(&G_P, L"Shared", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        hcn_internal_classify_switches(sws, 2, ips, 1, &c);
        hcn_internal_resolve_selector(&c, L"Shared", &res);
        check(res.outcome == HCN_IRES_PROCEED && !res.defer_adjudication &&
              IsEqualGUID(&res.probe_guid, &G_Q),
              L"E2 rule 2: one INTERNAL + one same-named PRIVATE -> PROCEED (terminal wins; the PROCEED-without-defer cell)");
        census_free(&c);
    }

    /* The (b) fixture: an unreadable-named PRIVATE row blocks a
       selector whose best match is non-terminal, while a terminal
       match is never gated. Also the unusable-name control. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Q", 0);
        sws[1] = mk_sw(&G_P, L"P", 0);
        sws[2] = mk_sw_unreadable(&G_R, 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        ips[1] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 3, ips, 2, &c);
        hcn_internal_resolve_selector(&c, L"P", &res);
        check(res.outcome == HCN_IRES_REJECT &&
              res.reason_code == HCN_IR_INVENTORY_UNAVAILABLE &&
              IsEqualGUID(&res.probe_guid, &GUID_NULL),
              L"E0 (b): an unreadable-named PRIVATE row blocks the DEFER (no probe target)");
        hcn_internal_resolve_selector(&c, L"Q", &res);
        check(res.outcome == HCN_IRES_PROCEED,
              L"E0 (b): Q still PROCEEDs (the premise fails for a terminal match)");

        /* Replace R with a readable-but-UNUSABLE row: P returns to
           DEFER-eligible (the two flags are distinct predicates). */
        sws[2] = mk_sw_unusable(&G_R, L"Overlong", 0);
        sws[2].name_unusable = TRUE;
        hcn_internal_classify_switches(sws, 3, ips, 2, &c);
        hcn_internal_resolve_selector(&c, L"P", &res);
        check(res.outcome == HCN_IRES_PROCEED && res.defer_adjudication,
              L"E0 (b): an UNUSABLE (not unreadable) row does NOT block the DEFER");
        census_free(&c);
    }

    /* EMPTY is a count summary, never a decision input: the resolver
       runs E0/E2 exactly as for OK, and adding an unrelated INTERNAL
       changes none of the four answers. */
    {
        HcnInternalResolution r0[4];
        HcnInternalResolution r1[4];
        static const wchar_t *sels[4] = { L"P", L"E", L"Same", L"Absent" };
        int i;

        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        sws[1] = mk_sw(&G_E, L"E", 1);
        sws[2] = mk_sw(&G_P, L"Same", 0);   /* hmm: same GUID as P? use P2 */
        census_free(&c);
        /* rebuild with distinct GUIDs */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        sws[1] = mk_sw(&G_E, L"E", 1);
        sws[2] = mk_sw(&G_P2, L"Same", 0);
        sws[3] = mk_sw(&G_R, L"Same", 0);
        hcn_internal_classify_switches(sws, 4, NULL, 0, &c);
        check(c.state == HCN_CENSUS_EMPTY,
              L"state: zero INTERNAL-class switches -> EMPTY (a count summary)");
        for (i = 0; i < 4; i++)
            hcn_internal_resolve_selector(&c, sels[i], &r0[i]);
        check(r0[0].outcome == HCN_IRES_REJECT &&
              r0[0].reason_code == HCN_IR_NOT_INTERNAL && r0[0].n == 0,
              L"EMPTY census: P -> the no-host-adapter line (n = 0)");
        check(r0[1].outcome == HCN_IRES_REJECT &&
              r0[1].reason_code == HCN_IR_EXTERNAL_SWITCH,
              L"EMPTY census: E -> the external line");
        check(r0[2].outcome == HCN_IRES_REJECT &&
              r0[2].reason_code == HCN_IR_DUPLICATE,
              L"EMPTY census: the same-named PRIVATE pair -> the duplicate line");
        check(r0[3].outcome == HCN_IRES_REJECT &&
              r0[3].reason_code == HCN_IR_NOT_FOUND,
              L"EMPTY census: an absent selector -> not-found");

        /* Add an unrelated INTERNAL Q: state becomes OK; none of the
           four answers changes. */
        sws[4] = mk_sw(&G_Q, L"Q", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        hcn_internal_classify_switches(sws, 5, ips, 1, &c);
        check(c.state == HCN_CENSUS_OK,
              L"state: adding one INTERNAL -> OK");
        for (i = 0; i < 4; i++)
            hcn_internal_resolve_selector(&c, sels[i], &r1[i]);
        check(r0[0].outcome == r1[0].outcome &&
              r0[0].reason_code == r1[0].reason_code &&
              r0[1].outcome == r1[1].outcome &&
              r0[1].reason_code == r1[1].reason_code &&
              r0[2].outcome == r1[2].outcome &&
              r0[2].reason_code == r1[2].reason_code &&
              r0[3].outcome == r1[3].outcome &&
              r0[3].reason_code == r1[3].reason_code,
              L"EMPTY invariance: adding an unrelated INTERNAL changes NONE of the four answers");
        census_free(&c);
    }

    /* List-level escalation: an unreadable name on an INTERNAL- or
       UNCLASSIFIED-class entry -> UNAVAILABLE; on PRIVATE -> state ok. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw_unreadable(&G_R, 0);
        ips[0] = mk_ip(L"RPort", WMI_WALK_OK, &G_R);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(c.state == HCN_CENSUS_UNAVAILABLE,
              L"escalation: an unreadable INTERNAL-class name -> census UNAVAILABLE");

        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw_unreadable(&G_R, 0);
        hcn_internal_classify_switches(sws, 1, NULL, 0, &c);
        check(c.state == HCN_CENSUS_EMPTY &&
              census_find(&c, &G_R)->sw_class == HCN_SW_PRIVATE,
              L"escalation: an unreadable PRIVATE name -> state stays authoritative (not offered only)");
        census_free(&c);
    }

    /* The iport name-completeness fixtures: a 256-char label sharing a
       255-char name's prefix, and Lab<NUL>Old - both driven through the
       PRODUCTION BSTR conversion (never a pre-truncated or pre-flagged
       record). */
    {
        wchar_t long_name[300];
        BSTR bstr;
        HcnRawIportResult raw;
        HcnRawIportResult missing_raw;
        HcnRawSwitchResult switch_raw;
        int i;

        for (i = 0; i < 255; i++)
            long_name[i] = L'A';
        long_name[255] = L'\0';

        /* The 256-char label: over-cap, placeholder, never a prefix. */
        bstr = SysAllocStringLen(long_name, 256);   /* 256: shares the prefix, over the cap */
        check(bstr != NULL, L"fixture: 256-char BSTR allocated");
        internal_iport_name_to_raw(bstr, &raw);
        internal_switch_name_to_raw(bstr, &switch_raw);
        SysFreeString(bstr);
        check(raw.name_unusable_for_attribution && !raw.name_unreadable &&
              wcscmp(raw.name, L"(name unavailable)") == 0,
              L"iport conversion: a 256-char label -> unusable-for-attribution, name = the placeholder");
        check(switch_raw.name_unusable && !switch_raw.name_unreadable &&
              wcscmp(switch_raw.name, L"(name unavailable)") == 0,
              L"switch conversion: a 256-char label -> unusable, name = the placeholder");

        internal_switch_name_to_raw(NULL, &switch_raw);
        check(switch_raw.name_unreadable && !switch_raw.name_unusable &&
              switch_raw.name[0] == L'\0',
              L"switch conversion: a missing ElementName stays unreadable with its empty raw name");
        internal_iport_name_to_raw(NULL, &missing_raw);
        check(missing_raw.name_unreadable && !missing_raw.name_unusable_for_attribution &&
              wcscmp(missing_raw.name, L"(name unavailable)") == 0,
              L"iport conversion: a missing ElementName stays unreadable with its placeholder");

        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, long_name, 0);
        ips[0] = raw;
        ips[0].walk_result = WMI_WALK_ERROR;
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(census_find(&c, &G_P)->sw_class == HCN_SW_PRIVATE &&
              c.has_unknown_owner_failure,
              L"completeness: WALK_ERROR + an over-cap label -> NO prefix match, unknown-owner flag");

        ips[0].walk_result = WMI_WALK_NOT_CONNECTED;
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(!c.unattributed_host_port && !c.has_unknown_owner_failure,
              L"completeness: NOT_CONNECTED + an over-cap label -> omitted");

        ips[0].walk_result = WMI_WALK_OK;
        ips[0].reached_switch_id = G_P;
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(census_find(&c, &G_P)->sw_class == HCN_SW_INTERNAL,
              L"completeness: WALK_OK -> success evidence from reached_switch_id (whatever the label)");

        /* Lab<NUL>Old: 7 code units; every NUL-terminated comparison
           sees only "Lab" - the flag must prevent the prefix
           attribution. Control: a legal "Lab" label. */
        bstr = SysAllocStringLen(L"Lab\0Old", 7);
        check(bstr != NULL, L"fixture: embedded-NUL BSTR allocated");
        internal_iport_name_to_raw(bstr, &raw);
        internal_switch_name_to_raw(bstr, &switch_raw);
        SysFreeString(bstr);
        check(raw.name_unusable_for_attribution &&
              wcscmp(raw.name, L"(name unavailable)") == 0,
              L"iport conversion: an embedded NUL over the RAW length -> unusable (placeholder, never the prefix)");
        check(switch_raw.name_unusable && !switch_raw.name_unreadable &&
              wcscmp(switch_raw.name, L"(name unavailable)") == 0,
              L"switch conversion: an embedded NUL over the RAW length -> unusable (placeholder, never the prefix)");

        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"Lab", 0);
        ips[0] = raw;
        ips[0].walk_result = WMI_WALK_ERROR;
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(census_find(&c, &G_P)->sw_class == HCN_SW_PRIVATE,
              L"completeness: Lab<NUL>Old WALK_ERROR -> NO signal into the Lab switch");

        ips[0] = mk_ip(L"Lab", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(census_find(&c, &G_P)->sw_class == HCN_SW_UNCLASSIFIED,
              L"completeness control: a legal Lab label WALK_ERROR -> the signal lands (the control)");
        census_free(&c);
    }

    /* The census-side rejection set vs the setter's: a readable unique
       INTERNAL entry whose name carries U+FFFF is name_unusable through
       the SAME shared helper - excluded from the match set, so the
       census can never offer a name the entrances refuse. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw_unusable(&G_Q, L"Lab\xFFFF" L"X", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        check(census_find(&c, &G_Q)->name_unusable &&
              census_find(&c, &G_Q)->sw_class == HCN_SW_INTERNAL,
              L"rejection set: a U+FFFF name -> name_unusable (the same shared helper)");
        hcn_internal_resolve_selector(&c, L"Lab\xFFFF" L"X", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_NOT_FOUND,
              L"rejection set: an unusable name is excluded from the match set (not-found)");
        census_free(&c);
    }

    /* A zero-length PRIVATE name is name_unusable (the never-matchable
       class): never fed to the resolver, never DEFER-eligible. The
       resolver's selector contract is non-empty (an empty selector is
       Auto, which never resolves), so the assertion is the flag and
       the exclusion, not a resolution. */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"", 0);
        sws[0].name_unusable = TRUE;
        hcn_internal_classify_switches(sws, 1, NULL, 0, &c);
        check(census_find(&c, &G_P)->name_unusable,
              L"zero-length: a present-but-empty ElementName is name_unusable");
        hcn_internal_resolve_selector(&c, L"Lab", &res);
        check(res.outcome == HCN_IRES_REJECT && res.reason_code == HCN_IR_NOT_FOUND,
              L"zero-length: the unusable name is never matched (a storable selector sees not-found)");
        census_free(&c);
    }
}

/* ---- The projection ---- */

static void run_internal_projection_fixtures(void)
{
    HcnInternalSwitchCensus c;
    HcnRawSwitchResult sws[5];
    HcnRawIportResult ips[3];

    wprintf(L"\n[internal projection fixtures]\n");

    /* DEFER-eligibility and the CONCISE reason come from the production
       resolver: one shared census drives every cell of the (b)/flag/
       uniqueness cross product, asserted per entry. */
    {
        const HcnInternalSwitchEntry *e;

        /* flag SET + unique readable PRIVATE + no unreadable PRIVATE
           row -> DEFER-eligible, no clause. */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 1, ips, 1, &c);
        internal_project_census(&c);
        e = census_find(&c, &G_P);
        check(e->defer_eligible && e->selectable && e->reason_text[0] == L'\0',
              L"projection: flag SET + unique PRIVATE -> deferEligible, selectable, no clause");
        census_free(&c);

        /* flag CLEAR + unique -> not eligible, E2's no-host-adapter
           clause. */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        hcn_internal_classify_switches(sws, 1, NULL, 0, &c);
        internal_project_census(&c);
        e = census_find(&c, &G_P);
        check(!e->defer_eligible && !e->selectable &&
              e->reason_code == HCN_IR_NOT_INTERNAL &&
              wcscmp(e->reason_text, L"no host adapter") == 0,
              L"projection: flag CLEAR + unique PRIVATE -> the no-host-adapter clause");
        census_free(&c);

        /* flag CLEAR + duplicate -> the duplicate clause (E2's class
           list runs AFTER its duplicate rule). */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"Same", 0);
        sws[1] = mk_sw(&G_P2, L"Same", 0);
        hcn_internal_classify_switches(sws, 2, NULL, 0, &c);
        internal_project_census(&c);
        check(census_find(&c, &G_P)->reason_code == HCN_IR_DUPLICATE &&
              wcscmp(census_find(&c, &G_P)->reason_text, L"name duplicated") == 0,
              L"projection: flag CLEAR + duplicate -> the duplicate clause");
        census_free(&c);

        /* flag SET + unique + an unreadable-named PRIVATE row ->
           E0(b)'s classification-incomplete clause. */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        sws[1] = mk_sw_unreadable(&G_R, 0);
        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 1, &c);
        internal_project_census(&c);
        e = census_find(&c, &G_P);
        check(!e->defer_eligible && e->reason_code == HCN_IR_INVENTORY_UNAVAILABLE &&
              wcscmp(e->reason_text, L"classification incomplete") == 0,
              L"projection: flag SET + (b) -> the classification-incomplete clause (never no-host-adapter)");

        /* The (b)-blocked unreadable row itself: placeholder name,
           never offered. */
        check(census_find(&c, &G_R)->name_unreadable &&
              wcscmp(census_find(&c, &G_R)->name, L"(name unavailable)") == 0 &&
              !census_find(&c, &G_R)->selectable,
              L"projection: an unreadable row's payload name is the placeholder");
        census_free(&c);

        /* flag SET + duplicate + the unreadable row -> E0(b) beats the
           duplicate clause. */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"Same", 0);
        sws[1] = mk_sw(&G_P2, L"Same", 0);
        sws[2] = mk_sw_unreadable(&G_R, 0);
        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 3, ips, 1, &c);
        internal_project_census(&c);
        check(census_find(&c, &G_P)->reason_code == HCN_IR_INVENTORY_UNAVAILABLE,
              L"projection: flag SET + duplicate + (b) -> E0(b)'s code (the duplicate line is never emitted)");
        census_free(&c);

        /* PROCEED without the DEFER context: a readable PRIVATE shadowed
           by a same-named INTERNAL - not defer-eligible, no reason; the
           INTERNAL answer is unchanged. */
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Shared", 0);
        sws[1] = mk_sw(&G_P, L"Shared", 0);
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        hcn_internal_classify_switches(sws, 2, ips, 1, &c);
        internal_project_census(&c);
        e = census_find(&c, &G_P);
        check(!e->defer_eligible && e->reason_text[0] == L'\0' &&
              census_find(&c, &G_Q)->selectable,
              L"projection: a PRIVATE shadowed by a same-named INTERNAL -> not DEFER-eligible, no clause");
        census_free(&c);
    }

    /* Duplicate groups over {INTERNAL ∪ UNCLASSIFIED} excluding
       name_unusable; the product fixed-GUID and sentinel de-offers; the
       selectable rule. */
    {
        static const GUID s_internal_guid = APPSANDBOX_INTERNAL_GUID_INIT;
        static const GUID s_nat_guid = APPSANDBOX_NAT_GUID_INIT;
        const HcnInternalSwitchEntry *prod, *sent, *dup1, *dup2, *plain;

        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_Q, L"Q", 0);
        sws[1] = mk_sw(&G_T, L"Q", 0);              /* duplicate name */
        sws[2] = mk_sw(&G_DEF, L"Default Switch", 0);
        sws[3] = mk_sw(&s_internal_guid, L"AppSandboxInternal", 0);  /* product GUID */
        sws[4] = mk_sw(&G_X, L"(Auto)", 0);         /* sentinel-named */
        ips[0] = mk_ip(L"QPort", WMI_WALK_OK, &G_Q);
        ips[1] = mk_ip(L"TPort", WMI_WALK_OK, &G_T);
        ips[2] = mk_ip(L"DefPort", WMI_WALK_OK, &G_DEF);
        hcn_internal_classify_switches(sws, 5, ips, 3, &c);
        internal_project_census(&c);

        dup1 = census_find(&c, &G_Q);
        dup2 = census_find(&c, &G_T);
        check(dup1->in_duplicate_name_group && dup2->in_duplicate_name_group &&
              !dup1->selectable && !dup2->selectable,
              L"projection: a duplicate INTERNAL name group -> in_duplicate_group, not selectable");

        plain = census_find(&c, &G_DEF);
        check(plain->selectable && !plain->in_duplicate_name_group,
              L"projection: a unique INTERNAL entry -> selectable");

        prod = census_find(&c, &s_internal_guid);
        check(prod->verdict == HCN_CAND_OWNED && !prod->selectable,
              L"projection: the product fixed-GUID entry -> verdict OWNED, de-offered (measured-defensive)");

        sent = census_find(&c, &G_X);
        check(!sent->selectable,
              L"projection: the sentinel-named \"(Auto)\" entry -> de-offered (never selectable)");
        (void)s_nat_guid;
        census_free(&c);
    }

    /* The projection-vs-resolver agreement (a regression guard: it
       fails only if the projection starts re-deriving a reason instead
       of reading the resolver's). */
    {
        ZeroMemory(&c, sizeof(c));
        sws[0] = mk_sw(&G_P, L"P", 0);
        sws[1] = mk_sw(&G_R, L"R", 0);
        ips[0] = mk_ip(L"OldQ", WMI_WALK_ERROR, NULL);
        hcn_internal_classify_switches(sws, 2, ips, 1, &c);
        internal_project_census(&c);
        {
            HcnInternalResolution res;
            const HcnInternalSwitchEntry *e;
            hcn_internal_resolve_selector(&c, L"P", &res);
            e = census_find(&c, &G_P);
            check((res.outcome == HCN_IRES_PROCEED && res.defer_adjudication &&
                   e->defer_eligible) ||
                  (res.outcome == HCN_IRES_REJECT &&
                   e->reason_code == res.reason_code),
                  L"projection guard: the entry's answer mirrors the resolver's");
        }
        census_free(&c);
    }
}

/* ---- format_reason ---- */

static void run_internal_format_reason_fixtures(void)
{
    wchar_t buf[HCN_REASON_TEXT_CAP];
    wchar_t smallbuf[24];

    wprintf(L"\n[internal format_reason fixtures]\n");

    format_reason(HCN_IR_NOT_FOUND, HCN_RS_FULL, L"Lab", 0, 4, NULL, buf,
                  ARRAYSIZE(buf));
    check(wcsstr(buf, L"no internal switch named \"Lab\"") != NULL &&
          wcsstr(buf, L"4 internal switches") != NULL,
          L"format FULL: the not-found line consumes n");

    format_reason(HCN_IR_DUPLICATE, HCN_RS_FULL, L"Lab", 0, 0, NULL, buf,
                  ARRAYSIZE(buf));
    check(wcsstr(buf, L"the switch name \"Lab\" is duplicated") != NULL,
          L"format FULL: the duplicate line");

    format_reason(HCN_IR_INVENTORY_UNAVAILABLE, HCN_RS_FULL, L"Lab", 0, 0,
                  NULL, buf, ARRAYSIZE(buf));
    check(wcsstr(buf, L"host inventory unavailable") != NULL &&
          wcsstr(buf, L"cannot resolve \"Lab\"") != NULL,
          L"format FULL: inventory-unavailable carries the cannot-resolve clause");

    format_reason(HCN_IR_INVENTORY_UNAVAILABLE, HCN_RS_LIST, NULL, 0, 0, NULL,
                  buf, ARRAYSIZE(buf));
    check(wcsstr(buf, L"host inventory unavailable") != NULL &&
          wcsstr(buf, L"cannot resolve") == NULL,
          L"format LIST: omits the cannot-resolve clause");

    format_reason(HCN_IR_CORRELATION_UNAVAILABLE, HCN_RS_LIST, NULL, 0, 0,
                  NULL, buf, ARRAYSIZE(buf));
    check(wcsstr(buf, L"HCN correlation unavailable") != NULL,
          L"format LIST: the correlation line");

    format_reason(HCN_IR_UNSUPPORTED_TYPE, HCN_RS_FULL, L"Lab", 0, 0,
                   L"Transparent", buf, ARRAYSIZE(buf));
    check(wcsstr(buf, L"is Transparent (external)") != NULL,
          L"format FULL: Type Transparent gets the external wording");

    format_reason(HCN_IR_UNSUPPORTED_TYPE, HCN_RS_FULL, L"Lab", 0, 0, L"Bat",
                  buf, ARRAYSIZE(buf));
    check(wcsstr(buf, L"unsupported HCN network type \"Bat\"") != NULL,
          L"format FULL: a generic type gets the quoted <T> wording");

    format_reason(HCN_IR_ENDPOINT_FAILED, HCN_RS_FULL, L"Lab", 0x80070005, 0,
                  NULL, buf, ARRAYSIZE(buf));
    check(wcsstr(buf, L"0x80070005") != NULL,
          L"format FULL: the endpoint line carries the HRESULT");

    format_reason(HCN_IR_INVALID_NAME, HCN_RS_FULL, NULL, 0, 0, NULL, buf,
                  ARRAYSIZE(buf));
    check(wcsstr(buf, L"too long") != NULL &&
          wcsstr(buf, L"unpaired surrogate") != NULL &&
          wcsstr(buf, L"U+FFFF") != NULL,
          L"format FULL: the interactive invalid-name line leads with too long");

    format_reason(HCN_IR_AUTO_NAME_SQUAT, HCN_RS_FULL, NULL, 0, 0, NULL, buf,
                  ARRAYSIZE(buf));
    check(wcsstr(buf, L"rename it") != NULL &&
          wcsstr(buf, L"Explicit") != NULL,
          L"format FULL: the name-squat line includes the Explicit workaround");

    format_reason(HCN_IR_NOT_INTERNAL, HCN_RS_CONCISE, L"Lab", 0, 0, NULL, buf,
                  ARRAYSIZE(buf));
    check(wcscmp(buf, L"no host adapter") == 0,
          L"format CONCISE: the no-host-adapter clause");

    format_reason(HCN_IR_INVENTORY_UNAVAILABLE, HCN_RS_CONCISE, L"Lab", 0, 0,
                  NULL, buf, ARRAYSIZE(buf));
    check(wcscmp(buf, L"classification incomplete") == 0,
          L"format CONCISE: the classification-incomplete clause");

    format_reason(HCN_IR_DUPLICATE, HCN_RS_CONCISE, L"Lab", 0, 0, NULL, buf,
                  ARRAYSIZE(buf));
    check(wcscmp(buf, L"name duplicated") == 0,
          L"format CONCISE: the duplicate clause");

    /* The line is name-truncated per out_cap - never an overflow, and
       the prefix (the explanation's head) survives. */
    format_reason(HCN_IR_DUPLICATE, HCN_RS_FULL,
                  L"VeryLongSwitchNameThatCannotFit", 0, 0, NULL, smallbuf,
                  ARRAYSIZE(smallbuf));
    check(wcslen(smallbuf) < ARRAYSIZE(smallbuf) &&
          wcsncmp(smallbuf, L"Internal: the swi", 16) == 0,
          L"format: the line is name-truncated per out_cap (no overflow)");

    /* Every code renders SOMETHING in every style. */
    {
        int code;
        BOOL all_ok = TRUE;
        for (code = 0; code <= (int)HCN_IR_AUTO_NAME_SQUAT; code++) {
            format_reason((HcnInternalReasonCode)code, HCN_RS_FULL, L"Lab",
                          0x80070005, 2, L"Bat", buf, ARRAYSIZE(buf));
            if (buf[0] == L'\0' || wcslen(buf) >= ARRAYSIZE(buf))
                all_ok = FALSE;
            format_reason((HcnInternalReasonCode)code, HCN_RS_CONCISE, L"Lab",
                          0, 0, NULL, buf, ARRAYSIZE(buf));
            if (buf[0] == L'\0')
                all_ok = FALSE;
        }
        check(all_ok,
              L"format: every catalog code renders non-empty FULL and CONCISE lines");
    }
}
int wmain(int argc, wchar_t **argv)
{
    /* Golden vector. */
    static const GUID golden_nic = {
        0xC644EF22, 0x7A1B, 0x4E3D,
        { 0x9F, 0x08, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F }
    };
    static const GUID golden_id = {
        0x46322DE4, 0x4D9E, 0x576C,
        { 0x90, 0x46, 0x85, 0x49, 0x59, 0x0C, 0xA0, 0x28 }
    };
    static const wchar_t golden_name[] =
        L"AppSandboxExternal-{c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f}";

    GUID id, id_again, parsed;
    wchar_t name[128];
    wchar_t variant[128];
    wchar_t short_name[8];
    HRESULT hr;

    (void)argc;
    (void)argv;

    /* --- owned_id: UUID v5 golden vector --- */

    hr = hcn_external_owned_id(&golden_nic, &id);
    check(SUCCEEDED(hr) && IsEqualGUID(&id, &golden_id),
          L"owned_id(golden NIC) == golden Id 46322de4-4d9e-576c-9046-8549590ca028");
    if (SUCCEEDED(hr))
        print_guid(L"computed Id", &id);

    hr = hcn_external_owned_id(&golden_nic, &id_again);
    check(SUCCEEDED(hr) && IsEqualGUID(&id_again, &golden_id),
          L"owned_id is deterministic (same input, same Id)");

    /* --- owned_name: exact golden Name --- */

    hr = hcn_external_owned_name(&golden_nic, name, (size_t)sizeof(name) / sizeof(name[0]));
    check(SUCCEEDED(hr) && wcscmp(name, golden_name) == 0,
          L"owned_name(golden NIC) == \"AppSandboxExternal-{c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f}\"");
    wprintf(L"      emitted Name = %s\n", name);

    hr = hcn_external_owned_name(&golden_nic, short_name, (size_t)sizeof(short_name) / sizeof(short_name[0]));
    check(FAILED(hr),
          L"owned_name rejects an undersized buffer (no silent truncation)");

    /* --- strict Name parse: round trip and tolerance --- */

    check(hcn_external_parse_owned_name(golden_name, &parsed)
              && IsEqualGUID(&parsed, &golden_nic),
          L"parse(owned_name(golden NIC)) round-trips to the golden NIC");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-{C644EF22-7A1B-4E3D-9F08-1A2B3C4D5E6F}");
    check(hcn_external_parse_owned_name(variant, &parsed)
              && IsEqualGUID(&parsed, &golden_nic),
          L"parse accepts uppercase GUID hex (binary result unchanged)");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-{c644eF22-7A1b-4e3D-9f08-1a2b3C4d5e6f}");
    check(hcn_external_parse_owned_name(variant, &parsed)
              && IsEqualGUID(&parsed, &golden_nic),
          L"parse accepts mixed-case GUID hex");

    /* --- strict Name parse: rejection shapes --- */

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]), golden_name);
    wcscat_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]), L"2");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects a suffix after the closing brace");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-{c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects a missing closing brace");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects a brace-less GUID body");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-{{c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f}}");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects double braces");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxInternal-{c644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects a wrong prefix");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-{g644ef22-7a1b-4e3d-9f08-1a2b3c4d5e6f}");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects a non-hex GUID character");

    wcscpy_s(variant, (size_t)sizeof(variant) / sizeof(variant[0]),
             L"AppSandboxExternal-{c644ef227a1b-4e3d-9f08-1a2b3c4d5e6f}");
    check(!hcn_external_parse_owned_name(variant, &parsed),
          L"parse rejects a displaced dash (wrong group shape)");

    check(!hcn_external_parse_owned_name(NULL, &parsed),
          L"parse rejects a NULL name");

    check(!hcn_external_parse_owned_name(golden_name, NULL),
          L"parse rejects a NULL out GUID");

    /* ---- Parser/classifier fixtures ---- */
    run_parser_fixtures(&golden_nic);

    /* ---- Config stream fixtures (extraction oracle + the new key) ---- */
    run_config_extraction_fixtures();
    run_new_key_fixtures();

    /* ---- Internal classification fixtures (A5b) ---- */
    run_internal_classify_fixtures();
    run_internal_resolver_fixtures();
    run_internal_projection_fixtures();
    run_internal_format_reason_fixtures();

    wprintf(L"\n%d failure(s).\n", g_failures);
    return g_failures ? 1 : 0;
}
