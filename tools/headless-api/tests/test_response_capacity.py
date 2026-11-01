"""
Headless VM response serialization and internal-switch API coverage.

This script drives the shared response builder with 32 Internal-mode
rows, each carrying a 255-wide-character selector made from a
non-ASCII BMP character. It checks exact field values and strict JSON
parsing for list, single-VM, and edit responses. This is representative
serialization coverage; the source-extraction harness separately
exercises builder growth and failure paths.

The edit scenarios verify that invalid selectors are rejected without
changing the row, that U+0001 (which the selector contract permits)
round-trips, and that a multi-field request with an invalid selector
does not apply the network-mode change.

This script is the sole owner of the fixture environment's daemon
lifecycle (see daemon_fixture.py): backup, install fixture, start,
assert, stop, restore byte-for-byte -- both exit paths restore. Run it
from an elevated Windows shell with no AppSandbox daemon or GUI
already running.

Exit 0 when every assertion passes.
"""
import csv
import ctypes
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # for asb.py
sys.path.insert(0, HERE)                            # for daemon_fixture.py
import daemon_fixture as df

# The representative payload: 32 rows, each a 255-wide-character BMP
# selector (a distinct prefix per row so a swapped/truncated field is
# visible, not just a missing one). All rows are stopped and
# non-building (the fixture rows carry no on-disk folder).
SELECTOR_LEN = 255
ROW_COUNT = 32

def selector_for(i):
    body = ("S%03d-" % i) + "\u4e00" * (SELECTOR_LEN - 5)
    assert len(body) == SELECTOR_LEN
    return body

CAPACITY_ROWS = []
for _i in range(ROW_COUNT):
    CAPACITY_ROWS.append(
        "[VM]\r\nName=cap-%03d\r\nOsType=Windows\r\nNetworkMode=3\r\n"
        "InternalSwitch=%s\r\n\r\n" % (_i, selector_for(_i)))

# A smaller round for the edit/atomicity scenarios: one Internal row
# with a valid short selector.
EDIT_ROWS = [
    "[VM]\r\nName=cap-edit\r\nOsType=Windows\r\nNetworkMode=3\r\n"
    "InternalSwitch=LabSwitch\r\n\r\n",
]

# An over-cap selector value (256+ wide characters): the shared rule
# rejects it at every entrance; the edit handler's top-of-handler
# validation must 400 it before any setter runs.
OVERLONG = "y" * 300

_failures = []

def check(ok, label):
    print("%s  %s" % ("PASS" if ok else "FAIL", label), flush=True)
    if not ok:
        _failures.append(label)

def is_internal_switch_invalid_response(code, body):
    error = body.get("error") if isinstance(body, dict) else None
    return (code == 400 and isinstance(error, dict) and
            error.get("code") == "invalid_arg" and
            isinstance(error.get("message"), str) and
            "internalSwitch" in error["message"])

def strict_parse(code, body, label):
    """The response must be a strict-parse JSON object (never a partial
    or truncated payload)."""
    if not isinstance(body, dict):
        check(False, "%s: body is not a JSON object" % label)
        return None
    check(True, "%s: strictly parses as JSON" % label)
    return body


def standalone_preflight():
    """Require elevation and an unused AppSandbox process slot.

    host.json alone misses a live GUI or a daemon with stale discovery
    data. Either process may hold the shared single-instance mutex.
    """
    if os.name != "nt":
        df.die("response capacity integration requires Windows")
    if not ctypes.windll.shell32.IsUserAnAdmin():
        df.die("response capacity integration requires an elevated shell")
    try:
        result = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq AppSandbox.exe", "/FO", "CSV", "/NH"],
            capture_output=True, text=True, check=False, timeout=15)
    except (OSError, subprocess.TimeoutExpired) as exc:
        df.die("cannot verify that the AppSandbox instance slot is free: %s" % exc)
    if result.returncode != 0:
        df.die("tasklist could not verify the AppSandbox instance slot: %s"
               % result.stderr.strip())
    try:
        rows = list(csv.reader(result.stdout.splitlines()))
    except csv.Error as exc:
        df.die("tasklist returned an unreadable process list: %s" % exc)
    app_processes = [row for row in rows
                     if row and row[0].casefold() == "appsandbox.exe"]
    if app_processes or df.daemon_alive():
        df.die("an AppSandbox GUI or daemon is already running; stop it before "
               "this standalone test (it owns the shared vms.cfg and instance lock)")

def capacity_round():
    """The representative fixture through all three response kinds."""
    df.install_fixture(CAPACITY_ROWS)
    proc = df.start_daemon(df.workdir_for("resp-capacity"))
    c = None
    try:
        c = df.wait_ready(proc)

        # The list: every selector intact, every field a member of the VM
        # object, the whole response strictly parseable.
        code, body = c._req("GET", "/vms", timeout=20)
        check(code == 200, "capacity: GET /vms -> 200")
        if code != 200:
            return
        body = strict_parse(code, body, "capacity: list")
        if body is None:
            return
        vms = body.get("vms", [])
        check(len(vms) == ROW_COUNT,
              "capacity: exactly %d VM(s) listed" % ROW_COUNT)
        by_name = {v.get("name"): v for v in vms if isinstance(v, dict)}
        check(len(by_name) == ROW_COUNT,
              "capacity: list contains every uniquely named fixture row")
        exact = 0
        for i in range(ROW_COUNT):
            v = by_name.get("cap-%03d" % i)
            if v is None:
                continue
            if v.get("internalSwitch") == selector_for(i):
                exact += 1
            check(v.get("internalSwitchInvalid") is False,
                  "capacity: %s internalSwitchInvalid member false"
                  % v.get("name"))
        check(exact == ROW_COUNT,
              "capacity: every 255-wchar BMP selector is exact in the list "
              "(%d/%d)" % (exact, ROW_COUNT))

        # The single-VM GET.
        code, single = c._req("GET", "/vms/cap-017", timeout=10)
        check(code == 200, "capacity: GET /vms/cap-017 -> 200")
        if code == 200:
            single = strict_parse(code, single, "capacity: single")
            if single is not None:
                check(single.get("name") == "cap-017",
                      "capacity: single-VM name member")
                sel = single.get("internalSwitch")
                check(isinstance(sel, str) and len(sel) == SELECTOR_LEN and
                      sel == selector_for(17),
                      "capacity: single-VM selector intact and exact")

        # The edit response (a no-op edit on a stopped row: same selector).
        code, edited = c._req("PUT", "/vms/cap-017",
                              {"internalSwitch": selector_for(17)},
                              timeout=10)
        check(code == 200, "capacity: PUT edit (same selector) -> 200")
        if code == 200:
            edited = strict_parse(code, edited, "capacity: edit")
            if edited is not None:
                sel = edited.get("internalSwitch")
                check(isinstance(sel, str) and len(sel) == SELECTOR_LEN and
                      sel == selector_for(17),
                      "capacity: edit response selector intact and exact")
    finally:
        if c is not None:
            df.stop_daemon(proc, c, workdir=df.workdir_for("resp-capacity"))
        else:
            df.kill_if_alive(proc)                   # never became ready


def edit_validation_round():
    """The 400 scenarios: an invalid selector is rejected before any
    setter runs, and the row is left completely untouched."""
    df.install_fixture(EDIT_ROWS)
    proc = df.start_daemon(df.workdir_for("resp-capacity"))
    c = None
    try:
        c = df.wait_ready(proc)

        # Baseline: the row's mode and selector.
        code, before = c._req("GET", "/vms/cap-edit", timeout=10)
        if code != 200:
            check(False, "edit-400: baseline GET -> %d" % code)
            return
        check(before.get("networkMode") == 3 and
              before.get("internalSwitch") == "LabSwitch",
              "edit-400: baseline row is Internal with the LabSwitch selector")

        # An over-long selector: 400, row untouched.
        code, body = c._req("PUT", "/vms/cap-edit",
                            {"internalSwitch": OVERLONG}, timeout=10)
        check(code == 400, "edit-400: over-long selector -> 400")
        code, after = c._req("GET", "/vms/cap-edit", timeout=10)
        check(after.get("networkMode") == 3 and
              after.get("internalSwitch") == "LabSwitch",
              "edit-400: over-long rejected leaves the row untouched")

        # Other control characters are legal; exercise a real U+0001
        # through the client's JSON encoder, then verify it round-trips.
        permitted_control = "Lab\u0001X"
        code, _ = c._req("PUT", "/vms/cap-edit",
                         {"internalSwitch": permitted_control}, timeout=10)
        check(code == 200, "selector: U+0001 is permitted -> 200")
        code, after = c._req("GET", "/vms/cap-edit", timeout=10)
        check(code == 200 and after.get("internalSwitch") == permitted_control,
              "selector: U+0001 round-trips through the JSON response")
        code, _ = c._req("PUT", "/vms/cap-edit",
                         {"internalSwitch": "LabSwitch"}, timeout=10)
        check(code == 200, "selector: restore the baseline name after control test")

        # Invalid selectors return 400 before any setter and leave the row
        # untouched. The client serializes each value as a JSON escape.
        invalid_values = [
            ("over-long", OVERLONG),
            ("carriage return", "Lab\rX"),
            ("line feed", "Lab\nX"),
            ("U+FFFF", "Lab\uffffX"),
            ("unpaired high surrogate", "Lab\ud800X"),
        ]
        for label, value in invalid_values:
            code, _ = c._req("PUT", "/vms/cap-edit",
                             {"internalSwitch": value}, timeout=10)
            check(code == 400, "edit-400: %s selector -> 400" % label)
            code, after = c._req("GET", "/vms/cap-edit", timeout=10)
            check(code == 200 and after.get("networkMode") == 3 and
                  after.get("internalSwitch") == "LabSwitch",
                  "edit-400: %s rejection leaves the row untouched" % label)

        # Multi-field atomicity: one PUT carrying networkMode and an
        # INVALID selector mutates NEITHER (the validation runs before
        # any setter, so there is no half-applied configuration to save).
        code, body = c._req("PUT", "/vms/cap-edit",
                            {"networkMode": 1, "internalSwitch": OVERLONG},
                            timeout=10)
        check(code == 400,
              "atomicity: mode+invalid-selector -> 400 (no half-apply)")
        code, after = c._req("GET", "/vms/cap-edit", timeout=10)
        check(after.get("networkMode") == 3 and
              after.get("internalSwitch") == "LabSwitch",
              "atomicity: neither the mode nor the selector changed")

        # A VALID multi-field request: both arms run (mode first, then
        # the selector; two independent blocks, not an if/else chain).
        code, body = c._req("PUT", "/vms/cap-edit",
                            {"networkMode": 3, "internalSwitch": "NewLab"},
                            timeout=10)
        check(code == 200, "atomicity: mode+valid-selector -> 200")
        code, after = c._req("GET", "/vms/cap-edit", timeout=10)
        check(after.get("networkMode") == 3 and
              after.get("internalSwitch") == "NewLab",
              "atomicity: both fields applied (mode 3 with NewLab)")

        # The (Auto) recovery: an explicit empty selector clears the value.
        code, body = c._req("PUT", "/vms/cap-edit",
                            {"internalSwitch": ""}, timeout=10)
        check(code == 200, "recovery: empty selector (Auto) -> 200")
        code, after = c._req("GET", "/vms/cap-edit", timeout=10)
        check(after.get("internalSwitch") == "",
              "recovery: the empty value is stored (Auto)")

        # Create with an overlong selector: reject-on-false decode (the
        # same key-present semantics; the create entry's own gate).
        code, body = c._req("POST", "/vms",
                            {"name": "cap-create", "osType": "Windows",
                             "ramMb": 4096, "hddGb": 16, "cpuCores": 2,
                             "networkMode": 3,
                             "internalSwitch": OVERLONG,
                             "adminUser": "tester",
                             "adminPass": "Test123456!"}, timeout=10)
        check(is_internal_switch_invalid_response(code, body),
              "create-400: over-long selector rejected as internalSwitch invalid_arg")
        code, missing = c._req("GET", "/vms/cap-create", timeout=10)
        check(code == 404, "create-400: rejected request did not create a VM")

        create_invalid_values = [
            ("CR", "Lab\rX"),
            ("LF", "Lab\nX"),
            ("U+FFFF", "Lab\uffffX"),
            ("unpaired surrogate", "Lab\ud800X"),
        ]
        for index, (label, value) in enumerate(create_invalid_values):
            name = "ci%d" % index  # within the Windows VM-name limit
            code, body = c._req("POST", "/vms",
                             {"name": name, "osType": "Windows",
                              "ramMb": 4096, "hddGb": 16, "cpuCores": 2,
                              "networkMode": 3, "internalSwitch": value,
                              "adminUser": "tester", "adminPass": "Test123456!"},
                             timeout=10)
            check(is_internal_switch_invalid_response(code, body),
                  "create-400: %s rejected as internalSwitch invalid_arg" % label)
            code, _ = c._req("GET", "/vms/" + name, timeout=10)
            check(code == 404,
                  "create-400: %s rejection has no VM side effect" % label)
    finally:
        if c is not None:
            df.stop_daemon(proc, c, workdir=df.workdir_for("resp-capacity"))
        else:
            df.kill_if_alive(proc)                   # never became ready


def main():
    print("Headless VM response capacity and selector validation",
          flush=True)
    if not os.path.isfile(df.DAEMON_EXE):
        df.die("daemon binary not found: %s (build AppSandbox Debug first)"
               % df.DAEMON_EXE)
    standalone_preflight()

    bak = df.backup_path_for("response-capacity")
    had_original = df.backup_or_die(bak)
    try:
        capacity_round()
        edit_validation_round()
    finally:
        df.restore_or_die(bak, had_original)

    print("\n%d failure(s)." % len(_failures), flush=True)
    sys.exit(1 if _failures else 0)


if __name__ == "__main__":
    main()
