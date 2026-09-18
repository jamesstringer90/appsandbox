"""
Strict-parse acceptance for the headless VM-response JSON.

The three production response kinds -- the /vms list, the single-VM
GET, and the PUT edit response -- must parse as complete JSON with
every network field a MEMBER of the VM object: one closing brace per
VM object, after every field, in the order
displayOpen, gpuId, gpuName, netAdapter. The assertions are strict
parses plus an exact key-order check on the parsed object; no
substring search.

The fixture is pinned by SIZE, not by story: the measured 1-VM
regression shape (a one-VM, empty-adapter response was invalid JSON
before the brace fix) plus a list of exactly N = 4 VMs (short names,
no adapter fields -- four is the smallest count that exercises the
loop, the per-VM separators, and the trailing member, and its encoded
size is a small fraction of the 524,288-byte response buffer, so the
fixture cannot drift into a capacity boundary). The rows are all
stopped and non-building, and carry no disk path (the rendered
diskDirectory is empty -- no on-disk VM folder is needed to render a
stopped row).

This script is the sole owner of the fixture environment's daemon
lifecycle (see daemon_fixture.py): backup, install fixture, start,
assert, stop, restore byte-for-byte -- both exit paths restore. Run it
from an elevated shell with no AppSandbox daemon or GUI already
running.

Exit 0 when every assertion passes.
"""
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # for asb.py
sys.path.insert(0, HERE)                            # for daemon_fixture.py
import daemon_fixture as df

# One VM, empty adapter field: the measured regression shape (invalid
# before the brace fix).
REGRESSION_ROWS = [
    "[VM]\r\nName=shape-1\r\nOsType=Linux\r\nNetworkMode=0\r\n\r\n",
]
# Exactly four VMs, short names, no adapter fields.
LIST_ROWS = [
    "[VM]\r\nName=shape-4a\r\nOsType=Linux\r\nNetworkMode=0\r\n\r\n",
    "[VM]\r\nName=shape-4b\r\nOsType=Windows\r\nNetworkMode=1\r\n\r\n",
    "[VM]\r\nName=shape-4c\r\nOsType=Linux\r\nNetworkMode=3\r\n\r\n",
    "[VM]\r\nName=shape-4d\r\nOsType=Windows\r\nNetworkMode=0\r\n\r\n",
]

# The trailing key sequence every VM object must end with (0.1.8 adds
# gpuId/gpuName; netAdapter lands INSIDE the object, before the brace).
VM_TAIL_KEYS = ["displayOpen", "gpuId", "gpuName", "netAdapter"]

_failures = []


def fail(msg):
    _failures.append(msg)
    print("FAIL  %s" % msg, flush=True)


def check(ok, label):
    if ok:
        print("PASS  %s" % label, flush=True)
    else:
        fail(label)


def assert_vm_shape(label, raw_text, expected_objects, tail_keys=VM_TAIL_KEYS):
    """Strict parse (never a substring search) + exact key order + exactly
    one closing brace per VM object."""
    try:
        obj = json.loads(raw_text, object_pairs_hook=list)
    except ValueError as e:
        check(False, "%s: strict parse failed: %s" % (label, e))
        return
    wrapped = False
    if (isinstance(obj, list) and len(obj) == 1 and isinstance(obj[0], tuple)
            and obj[0][0] == "vms"):
        wrapped = True
        obj = obj[0][1]                              # the {"vms": [...]} wrapper
    elif isinstance(obj, list) and obj and all(isinstance(e, tuple) for e in obj):
        obj = [obj]                                  # a bare VM object: the hook's
        # pair list IS one object, not a list of objects
    n = len(obj)
    if n != expected_objects:
        check(False, "%s: expected %d VM object(s), got %d"
              % (label, expected_objects, n))
        return
    braces = raw_text.count("}")
    expected_braces = expected_objects + (1 if wrapped else 0)
    if braces != expected_braces:
        check(False, "%s: expected exactly %d closing brace(s), got %d"
              % (label, expected_braces, braces))
        return
    for i, entry in enumerate(obj):
        keys = [k for k, _ in entry]
        if keys[-len(tail_keys):] != tail_keys:
            check(False, "%s: VM %d ends with %r, expected the fixed tail %r"
                  % (label, i, keys[-len(tail_keys):], tail_keys))
            return
    check(True, label)


def run_round(label, rows, expected_count):
    df.install_fixture(rows)
    proc = df.start_daemon(df.workdir_for("json-shape"))
    c = None
    try:
        c = df.wait_ready(proc)
        code, body = c._req("GET", "/vms", timeout=10)
        check(code == 200, "%s: GET /vms -> 200" % label)
        if code != 200 or not body.get("vms"):
            return
        assert_vm_shape("%s: list parses with the fixed tail"
                        % label, json.dumps(body), expected_count)
        names = [v["name"] for v in body.get("vms", [])]
        check(len(names) == expected_count,
              "%s: exactly %d VM(s) listed" % (label, expected_count))

        first = body["vms"][0]["name"]
        code, single = c._req("GET", "/vms/" + first, timeout=10)
        check(code == 200, "%s: GET /vms/{n} -> 200" % label)
        if code == 200:
            assert_vm_shape("%s: single-VM response shape" % label,
                            json.dumps(single), 1)

        code, edited = c._req("PUT", "/vms/" + first,
                              {"networkMode": 1}, timeout=10)
        check(code == 200, "%s: PUT edit on a stopped VM -> 200" % label)
        if code == 200:
            assert_vm_shape("%s: edit response shape" % label,
                            json.dumps(edited), 1)
            check(edited.get("netAdapter", "missing") == "",
                  "%s: edited VM keeps an empty netAdapter member" % label)
    finally:
        if c is not None:
            df.stop_daemon(proc, c, workdir=df.workdir_for("json-shape"))
        else:
            df.kill_if_alive(proc)                   # never became ready


def building_negative():
    """A VM mid-build must return the documented 409 for PUT edit --
    confirm the negative is not miscounted as a successful edit. The
    name is unique per run (the create's staging folder on disk outlives
    the config restore) and the folder is removed after the round."""
    name = "brk-shape-bld-%d" % os.getpid()
    vm_dir = os.path.join(df.ASB_DIR, name)
    df.install_fixture(REGRESSION_ROWS)
    proc = df.start_daemon(df.workdir_for("json-shape"))
    c = None
    try:
        c = df.wait_ready(proc)
        code, body = c.create(name=name, osType="Linux",
                              imagePath=r"C:\nonexistent-installer.iso",
                              ramMb=4096, hddGb=32, cpuCores=2,
                              networkMode=0, adminUser="tester",
                              adminPass="Test123456!", sshEnabled=True)
        if code not in (200, 201, 202):
            fail("building negative: create -> %d (%r)" % (code, body))
            return
        # Building is set synchronously by the create; the polling loop
        # only guards against a pathological fast-clear.
        saw_building = False
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                st = c.status(name)
                if st.get("building"):
                    saw_building = True
                    break
            except KeyError:
                break
            time.sleep(0.1)
        check(saw_building,
              "building negative: created VM reports building=true")
        if saw_building:
            code, body = c.edit(name, networkMode=1)
            check(code == 409 and body.get("error", {}).get("code") == "vm_building",
                  "building negative: PUT edit -> 409 vm_building "
                  "(never a successful-edit count)")
            code, body = c.delete_vm(name)
            check(code == 409,
                  "building negative: delete while building -> 409")
    finally:
        # The build worker may still hold the staging dir: force shutdown
        # terminates it, then the fixture config is discarded by restore
        # and this run's staging folder is removed.
        if c is not None:
            df.stop_daemon(proc, c, force=True,
                           workdir=df.workdir_for("json-shape"))
        else:
            df.kill_if_alive(proc)
        shutil.rmtree(vm_dir, ignore_errors=True)


def main():
    print("S6: headless VM-response JSON strict-parse shape", flush=True)
    if not os.path.isfile(df.DAEMON_EXE):
        df.die("daemon binary not found: %s (build AppSandbox Debug first)"
               % df.DAEMON_EXE)
    if df.daemon_alive():
        df.die("an AppSandbox daemon is already answering at %s -- this "
               "script owns its own daemon; stop the running one first"
               % df.HOST_JSON)

    bak = df.backup_path_for("s6")
    had_original = df.backup_or_die(bak)
    try:
        run_round("1-VM regression", REGRESSION_ROWS, 1)
        run_round("N=4 list", LIST_ROWS, 4)
        building_negative()
    finally:
        df.restore_or_die(bak, had_original)

    print("\n%d failure(s)." % len(_failures), flush=True)
    sys.exit(1 if _failures else 0)


if __name__ == "__main__":
    main()
