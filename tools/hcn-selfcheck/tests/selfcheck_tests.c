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
#include <objbase.h>
#include "hcn_network.h"

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

    wprintf(L"\n%d failure(s).\n", g_failures);
    return g_failures ? 1 : 0;
}
