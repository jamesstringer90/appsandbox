"""
Master test harness.

  1. Runs the no-VM static checks once (host vs Win32 + create-validation).
  2. Spawns ONE full per-VM suite thread per spec, CONCURRENTLY -- so multi-VM
     concurrency is tested by construction (the daemon juggles every spec's VM at
     once) and the entire suite is proven on each OS simultaneously. There is no
     separate multi-VM test. Edit SPECS to change thread count / create params.
  3. Runs the Windows template lifecycle (separate -- sysprep is a different
     lifecycle from a normal VM).

Then prints a per-thread summary + a coverage table (function -> where tested).
Launch as a background task. Self-cleaning: every VM is a brk-* throwaway.
"""
import sys, os, subprocess, threading
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # parent: for asb.py
sys.path.insert(0, HERE)                     # this dir: for vm_lifecycle + sibling test modules
import asb
from vm_lifecycle import run_vm_lifecycle

# The harness chooses how many threads and each thread's create params.
# Specs are per host platform: a Windows host (HCS) builds Linux + Windows
# guests concurrently; a macOS host boots macOS guests (Virtualization.framework)
# AND Windows guests (QEMU+HVF+ivshmem), so it runs macOS + Windows concurrently --
# multi-VM + cross-hypervisor concurrency is tested by construction. Same interface
# either way; feature differences are capability-gated inside vm_lifecycle.
# One uniform guest size everywhere: 4096 MB / 2 cores / 64 GB.
IS_MAC = sys.platform == "darwin"
if IS_MAC:
    import helpers
    CACHED_IPSW = os.path.join(helpers.ASB_DIR, "restore.ipsw")
    # Must KEEP the .ipsw extension: VZMacOSRestoreImage type-checks by
    # extension and rejects e.g. "restore.ipsw.orig" as "the wrong file type".
    ORIG_IPSW = os.path.join(helpers.ASB_DIR, "restore-orig.ipsw")
    # The IPSW DOWNLOAD path is part of the suite: set any cached restore
    # image aside so brk-mac1 (imageless) forces a fresh fetch from Apple,
    # while brk-mac2 installs concurrently from the preserved original.
    # The post-suite step below keeps the freshly downloaded image (it's the
    # newest) and removes the old one -- or restores it if the fetch failed.
    if os.path.exists(CACHED_IPSW) and not os.path.exists(ORIG_IPSW):
        os.rename(CACHED_IPSW, ORIG_IPSW)
        print("IPSW: moved cached restore.ipsw aside -> brk-mac1 will download fresh")
    SPECS = [
        {"name": "brk-mac1", "os_type": "macOS", "ram": 4096, "hdd": 64, "cores": 2,
         "image": ""},   # imageless -> daemon fetches the latest IPSW (download test)
        {"name": "brk-mac2", "os_type": "macOS", "ram": 4096, "hdd": 64, "cores": 2,
         # concurrent installer; if there was no original cache to install
         # from, gate on brk-mac1's REPORTED download progress (the daemon
         # streams it in installStatus) rather than racing the shared fetch.
         "image": ORIG_IPSW if os.path.exists(ORIG_IPSW) else "",
         "await_download_of": None if os.path.exists(ORIG_IPSW) else "brk-mac1"},
        {"name": "brk-win1", "os_type": "Windows", "ram": 4096, "hdd": 64, "cores": 2,
         # Windows-on-Mac: QEMU+HVF+ivshmem disk build from the ISO, runs CONCURRENTLY
         # with the two macOS guests. Image via ASB_ISO_WINDOWS (helpers.ISO_WIN).
         "image": helpers.ISO_WIN},
    ]
else:
    SPECS = [
        {"name": "brk-lin", "os_type": "Linux",   "ram": 4096, "hdd": 64, "cores": 2},
        {"name": "brk-win", "os_type": "Windows", "ram": 4096, "hdd": 64, "cores": 2},
    ]
RUN_STATIC = True
RUN_TEMPLATE = not IS_MAC   # template create -> sysprep -> from-template (Windows-only)

# function / endpoint -> where it's covered
COVERAGE = [
    ("GET /host (cores/ram vs Win32)",          "static"),
    ("GET /version, /templates",                "static, per-VM"),
    ("POST /vms create -- input validation",    "static (reject) + per-VM (duplicate)"),
    ("POST /vms create -- Linux & Windows",     "per-VM (one thread per OS, concurrently)"),
    ("POST /vms create -- template / from-tpl", "template"),
    ("GET /vms (list), /vms/{n} status",        "per-VM (all threads)"),
    ("PUT /vms/{n} edit + validation",          "per-VM (config-at-start + reject)"),
    ("config applied at start (ram/cpu/gpu/net)","per-VM"),
    ("internalSwitch fields + response capacity", "separate test_response_capacity.py"),
    ("POST .../start (+ from snapshot/branch)", "per-VM"),
    ("POST .../shutdown (graceful)",            "per-VM"),
    ("POST .../stop (force) + idempotency",     "per-VM"),
    ("POST .../delete (+ no orphan)",           "per-VM"),
    ("delete-during-build guard (409)",         "per-VM (each thread, mid-build)"),
    ("GET .../sshInfo",                         "per-VM"),
    ("SSH key auto-deploy + key-only login",    "per-VM (sshEnabled+sshDeployKey, each OS)"),
    ("GET/POST/DELETE .../display open+close",   "per-VM when online (Win+mac); skip on no-desktop / Linux-pre-push"),
    ("snapshots take/rename/delete + GET tree", "per-VM"),
    ("branches: start-from-snapshot + delete",  "per-VM"),
    ("GET /events (SSE) per VM",                "per-VM (each thread, concurrent SSE)"),
    ("POST /shutdown (+ active-VM guard 409)",  "per-VM (while its VM is running)"),
    ("DELETE /templates/{name}, /templates",    "template"),
    ("multi-VM concurrency (daemon juggles N)", "INHERENT: %d threads run at once" % len(SPECS)),
]

try:
    _c = asb.connect()
    v = _c.version()
    print("daemon reachable: %s %s (%s)" % (v.get("product"), v.get("version"), v.get("hostOs")))
except Exception as e:
    print("PREFLIGHT FAIL: daemon not reachable (%s).\nStart it with: %s"
          % (e, asb.daemon_launch_hint()))
    sys.exit(2)

# Sweep leftover brk-* throwaways from a previous aborted run so a rerun is
# self-healing (every spec creates its VM from scratch).
import helpers as _helpers
for _vm in list(_c.list()):
    if _vm["name"].startswith("brk-"):
        print("sweeping leftover %s..." % _vm["name"], flush=True)
        _helpers.destroy_vm(_c, _vm["name"])

results = {}   # label -> list of failures ([] == pass)

if RUN_STATIC:
    print("\n" + "=" * 70 + "\nSTATIC (no-VM) CHECKS\n" + "=" * 70, flush=True)
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_host_and_validation.py")], cwd=HERE)
    results["static"] = [] if p.returncode == 0 else ["test_host_and_validation.py exit %d" % p.returncode]

print("\n" + "=" * 70 + "\n%d CONCURRENT PER-VM SUITES: %s\n" % (len(SPECS), ", ".join(s["name"] + "/" + s["os_type"] for s in SPECS)) + "=" * 70, flush=True)
threads = []
def worker(spec):
    # Optional gate: wait until another spec's VM has finished its restore-
    # image download, judged by the DAEMON'S OWN progress reporting
    # (installStatus leaves the "Fetching"/"Downloading"/"Resuming" phases),
    # so two imageless creates never race the one shared fetch. Stall-based:
    # download progress keeps the gate alive; a hung download fails it.
    gate = spec.get("await_download_of")
    if gate:
        import helpers
        print("    [%s] gating on %s's restore-image download..." % (spec["name"], gate), flush=True)
        def download_done(s):
            phase = (s.get("installStatus") or "").lower()
            return not any(w in phase for w in ("fetching", "downloading",
                                                "resuming", "reconnecting",
                                                "waiting for network"))
        helpers.wait_progress(asb.connect(), gate, download_done,
                              stall=600, label="%s gate" % spec["name"],
                              missing_ok=True)
    name, fails = run_vm_lifecycle(spec)
    results[name] = fails
for spec in SPECS:
    t = threading.Thread(target=worker, args=(spec,)); t.start(); threads.append(t)
for t in threads:
    t.join()

if IS_MAC:
    # IPSW housekeeping: keep exactly one cached image. Prefer the freshly
    # downloaded one (it's the latest Apple ships); fall back to the original
    # if the download never completed.
    if os.path.exists(ORIG_IPSW):
        if os.path.exists(CACHED_IPSW):
            os.remove(ORIG_IPSW)
            print("IPSW: kept the freshly downloaded restore.ipsw; removed the old cache")
        else:
            os.rename(ORIG_IPSW, CACHED_IPSW)
            print("IPSW: download did not complete; restored the original cache")

if RUN_TEMPLATE:
    print("\n" + "=" * 70 + "\nWINDOWS TEMPLATE LIFECYCLE\n" + "=" * 70, flush=True)
    p = subprocess.run([sys.executable, os.path.join(HERE, "test_windows_template.py")], cwd=HERE)
    results["template"] = [] if p.returncode == 0 else ["test_windows_template.py exit %d" % p.returncode]

print("\n" + "=" * 70 + "\nSUMMARY\n" + "=" * 70)
for label in sorted(results):
    f = results[label]
    print("  %-12s %s" % (label, "PASS" if not f else "FAIL"))
    for x in f:
        print("        - %s" % x)

ran = set(results)
failed = {k for k, f in results.items() if f}
def status_for(where):
    if "separate" in where:
        return "separate"
    toks = []
    if "static" in where: toks.append("static")
    if "per-VM" in where: toks.append("per-VM")
    if "template" in where: toks.append("template")
    if "INHERENT" in where: toks.append("per-VM")
    keys = set()
    for t in toks:
        if t == "static": keys.add("static")
        elif t == "template": keys.add("template")
        elif t == "per-VM": keys |= {s["name"] for s in SPECS}
    if failed & keys: return "FAIL"
    if keys & ran:    return "PASS"
    return "n/a"

print("\n" + "=" * 70 + "\nCOVERAGE  (function -> where tested)\n" + "=" * 70)
print("  %-44s %-6s %s" % ("function / endpoint", "status", "covered by"))
print("  " + "-" * 68)
for feature, where in COVERAGE:
    print("  %-44s %-6s %s" % (feature, status_for(where), where))

ok = not any(results.values())
print("\n%s" % ("ALL GREEN" if ok else "SOME FAILED"))
sys.exit(0 if ok else 1)
