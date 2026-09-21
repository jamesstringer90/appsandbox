/* hcn-selfcheck.c - host-side HCN External networking self-check.
 *
 * Modes (both run against the live host):
 *   --enum   read-only dump of the selectable adapter list (L0-based,
 *            bound NICs visible, duplicate-name groups omitted; no HCN
 *            call, no WMI, no mutation).
 *   --acquire [name] --allow-create
 *            run the production acquire for real: Auto (no name) or
 *            Explicit (name). Create is part of acquire, so a FREE
 *            target gets an owned network and that NIC's host
 *            connectivity goes offline for the network's lifetime -
 *            which is why the mode refuses to run without the explicit
 *            --allow-create acknowledgement. HCN reads require
 *            elevation (measured: non-elevated enumerate gets 0x80070005).
 *
 * The offline contract fixtures (owned-ID/Name vectors, parser and
 * classifier fixtures) live in tests/ as a separate binary that links
 * only the pure offline contract unit (hcn_parse.c): no acquire, WMI,
 * or HCN-calling code is present in that binary at all, so it cannot
 * mutate a host.
 *
 * This tool compiles the production hcn_* sources (hcn_parse.c,
 * hcn_inventory.c, hcn_wmi.c, hcn_external.c, hcn_network.c) as its
 * own translation units, so the probes exercise the real production
 * code - no duplicate implementation exists anywhere. */

#include <winsock2.h>
#include <stdio.h>
#include <objbase.h>
#include "hcn_network.h"

/* Read-only enumeration dump: the L0-based list with
   bound NICs visible and duplicate-name groups omitted. Pure inventory
   reads - no HCN, no WMI, no mutation. */
static void enum_cb(const wchar_t *name, int if_type, void *ctx)
{
    int *count = (int *)ctx;
    wprintf(L"  [%2d] %-40s (if_type %d)\n", (*count)++, name, if_type);
}

static int run_enum_dump(void)
{
    int count = 0;

    wprintf(L"\n[adapter enumeration (read-only, L0-based, duplicate groups omitted)]\n");
    hcn_enum_adapters(enum_cb, &count);
    wprintf(L"  -> %d selectable adapter(s)\n", count);
    return 0;
}

/* Acquire smoke: runs the production hcn_acquire_external_network and
   prints the result or the one-line reason. NOT read-only in general:
   acquire CREATES an owned network when the walk judges a target FREE
   (creation is part of acquire), and that create takes the host's
   connectivity on the NIC offline for the network's lifetime. On a host
   whose candidate adapters are all bound to existing switches the walk
   terminates in a borrow (pure reads), but a FREE target makes this
   mode a host mutation, which is why wmain refuses to run it without an
   explicit --allow-create. HCN reads require elevation (measured:
   non-elevated enumerate gets 0x80070005). */
static int run_acquire_smoke(const wchar_t *adapter)
{
    HcnExternalNetworkRef ref;
    wchar_t reason[1024]; /* the Wi-Fi failure reason is multi-paragraph;
                             the app callers use the same capacity */
    HRESULT hr;
    ULONGLONG t0 = GetTickCount64();

    ZeroMemory(&ref, sizeof(ref));
    reason[0] = L'\0';

    if (!hcn_init()) {
        wprintf(L"[acquire smoke] HCN unavailable (computenetwork.dll)\n");
        return 2;
    }

    wprintf(L"[acquire smoke] mode=%s adapter=\"%s\"\n",
            (adapter && adapter[0]) ? L"Explicit" : L"Auto",
            (adapter && adapter[0]) ? adapter : L"(none)");
    hr = hcn_acquire_external_network(adapter, &ref, reason,
                                      ARRAYSIZE(reason));
    wprintf(L"[acquire smoke] hr=0x%08X elapsed=%lums\n", hr,
            (unsigned long)(GetTickCount64() - t0));
    if (SUCCEEDED(hr)) {
        wchar_t s[64];
        StringFromGUID2(&ref.network_id, s, 64);
        wprintf(L"[acquire smoke] network_id=%s\n", s);
        StringFromGUID2(&ref.adapter_interface_guid, s, 64);
        wprintf(L"[acquire smoke] adapter_interface_guid=%s\n", s);
        wprintf(L"[acquire smoke] delete_on_last_release=%s\n",
                ref.delete_network_on_last_release ? L"TRUE (owned)"
                                                   : L"FALSE (borrowed)");
        if (reason[0])
            wprintf(L"[acquire smoke] reason buffer (should be empty on success): \"%s\"\n", reason);
        return 0;
    }
    wprintf(L"[acquire smoke] failure reason: \"%s\"\n",
            reason[0] ? reason : L"(no reason line)");
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    /* --enum: read-only adapter-list dump.
       --acquire [name] --allow-create: acquisition smoke (Auto when no
       name is given). The smoke runs the REAL acquire, which creates an
       owned network on a FREE target - the mode is mutation-capable and
       refuses to run without the explicit --allow-create
       acknowledgement. */
    if (argc == 2 && wcscmp(argv[1], L"--enum") == 0)
        return run_enum_dump();
    if (argc >= 2 && wcscmp(argv[1], L"--acquire") == 0) {
        const wchar_t *adapter = NULL;
        if (argc < 3 || wcscmp(argv[argc - 1], L"--allow-create") != 0) {
            wprintf(L"[acquire smoke] refused: acquire creates an owned "
                    L"network on a FREE target and takes the host's "
                    L"connectivity on that NIC offline for the network's "
                    L"lifetime. Re-run with --allow-create on an authorized "
                    L"host only.\n");
            return 2;
        }
        if (argc >= 4)
            adapter = argv[2];
        return run_acquire_smoke(adapter);
    }
    wprintf(L"usage: hcn-selfcheck --enum\n"
            L"       hcn-selfcheck --acquire [adapter-name] --allow-create\n"
            L"offline contract fixtures: hcn-selfcheck-tests (tests\\)\n");
    return 2;
}
