"""
Template lifecycle (Windows-only). Create a Windows template (isTemplate=true),
wait for sysprep finalization (the template self-shuts-down and moves from /vms
to /templates), verify it, create a VM FROM it, then delete the template. LONG:
full Windows build + sysprep (~15-25 min). Throwaways (brk-tpl, brk-fromtpl);
self-cleaning; never touches any pre-existing VM or template.
"""
import sys, os, time, shutil
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # for asb.py (parent dir)
import asb

PD = os.environ.get("ProgramData", r"C:\ProgramData")
ASB_DIR = os.path.join(PD, "AppSandbox")
ISO_WIN = os.environ.get("ASB_ISO_WINDOWS", os.path.join(os.path.dirname(os.path.abspath(__file__)), "Win11_x64.iso"))
TPL, FROMTPL = "brk-tpl", "brk-fromtpl"
c = asb.connect()
_fails = []
def check(label, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + label + (("   " + detail) if detail else ""), flush=True)
    if not cond:
        _fails.append(label + (("  (" + detail + ")") if detail else ""))

def names():
    return {v["name"] for v in c.list()}
def tpl_names():
    return {t["name"] for t in c.templates()}
def snap(n):
    try: return c.status(n)
    except KeyError: return None
def reap(n):
    deadline = time.time() + 480
    while time.time() < deadline:
        s = snap(n)
        if s is None: break
        if s["building"]: time.sleep(5); continue
        if s["running"]: c.stop(n); time.sleep(3)
        c.delete_vm(n); time.sleep(2); break
    d = os.path.join(ASB_DIR, n)
    if os.path.isdir(d): shutil.rmtree(d, ignore_errors=True)
def reap_tpl(n):
    if n in tpl_names():
        c.delete_template(n); time.sleep(2)
    d = os.path.join(ASB_DIR, "templates", n)
    if os.path.isdir(d): shutil.rmtree(d, ignore_errors=True)

# sweep leftovers
for v in list(c.list()):
    if v["name"].startswith("brk-"): reap(v["name"])
reap_tpl(TPL)
base_tpls = tpl_names()
print("baseline templates:", sorted(base_tpls), flush=True)

try:
    print("=== create a Windows template ===")
    code, _ = c.create(name=TPL, osType="Windows", imagePath=ISO_WIN, ramMb=4000, hddGb=64,
                       cpuCores=4, gpuMode=1, networkMode=1, adminUser="user", adminPass="test123",
                       testMode=True, isTemplate=True, copyNvidiaSmi=True)
    check("template create -> 202", code == 202, "code=%s" % code)
    deadline = time.time() + 90
    while time.time() < deadline and TPL not in names():
        time.sleep(2)
    check("template appears in /vms while building", TPL in names())
    if TPL in names():
        check("template forces copyNvidiaSmi false", c.status(TPL).get("copyNvidiaSmi") is False)
    check("delete-during-build guard applies to template -> 409", c.delete_vm(TPL)[0] == 409)

    print("    waiting for sysprep finalization (template -> /templates, leaves /vms)...", flush=True)
    # Stall-based, not a fixed deadline: build progress / state transitions
    # (building % -> running-sysprep -> left /vms) reset the clock; only a
    # template making NO observable progress for 10 min fails the wait.
    STALL = 600
    last_sig = None
    last_change = time.time()
    while True:
        s = snap(TPL)
        sig = (s["state"], s.get("progress"), s.get("installStatus")) if s else ("(left /vms)",)
        if sig != last_sig:
            print("    [%s] %s = %s" % (time.strftime("%H:%M:%S"), TPL, sig[0]), flush=True)
            last_sig = sig
            last_change = time.time()
        if TPL in tpl_names() and TPL not in names():
            break
        if time.time() - last_change > STALL:
            print("    STALLED: no progress for %ds (last: %s)" % (STALL, last_sig), flush=True)
            break
        time.sleep(10)
    check("template finalized: now in /templates", TPL in tpl_names(), "templates=%s" % sorted(tpl_names()))
    check("template left the /vms list", TPL not in names())
    t = next((t for t in c.templates() if t["name"] == TPL), None)
    check("template osType == Windows", bool(t) and t.get("osType") == "Windows", "t=%s" % t)

    print("\n=== create a VM FROM the template ===")
    code, _ = c.create(name=FROMTPL, osType="Windows", templateName=TPL, ramMb=4000, hddGb=64,
                       cpuCores=2, adminUser="user", adminPass="test123", testMode=True)
    check("create-from-template -> 202", code == 202, "code=%s" % code)
    s = c.wait_online(FROMTPL, timeout=1800)   # boots from the generalized template all the way to online
    check("VM-from-template reaches online", s["state"] == "online", "state=%s" % s["state"])
    check("VM-from-template defaults copyNvidiaSmi false", s.get("copyNvidiaSmi") is False)
    # graceful, agent-mediated shutdown (force only as a fallback so the test can finish)
    gcode, _ = c.shutdown(FROMTPL)
    check("from-template graceful shutdown -> 202", gcode == 202, "code=%s" % gcode)
    try:
        c.wait(FROMTPL, "stopped", timeout=600); check("from-template graceful -> stopped", True)
    except Exception:
        c.stop(FROMTPL); c.wait(FROMTPL, "stopped", timeout=90)
        check("from-template graceful -> stopped", False, "did not reach stopped within 600s")
    check("delete VM-from-template -> 202", c.delete_vm(FROMTPL)[0] == 202)

    print("\n=== delete the template ===")
    code, _ = c.delete_template(TPL); check("delete template -> 2xx", code in (200, 202), "code=%s" % code)
    time.sleep(2)
    check("template removed from /templates", TPL not in tpl_names())
finally:
    reap(FROMTPL)
    reap_tpl(TPL)

print("\n=== collateral ===")
check("templates back to baseline", tpl_names() == base_tpls, "now=%s base=%s" % (sorted(tpl_names()), sorted(base_tpls)))
check("no brk-* VMs left", not any(n.startswith("brk-") for n in names()))

print("\n" + ("==== TEMPLATE: ALL PASS ====" if not _fails else "==== %d FAIL ====\n  - %s" % (len(_fails), "\n  - ".join(_fails))))
sys.exit(1 if _fails else 0)
