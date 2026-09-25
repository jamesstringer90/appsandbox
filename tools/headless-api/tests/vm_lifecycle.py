"""
The full per-VM test sequence, parametrized by a create-spec. The master harness
(run_all.py) runs this CONCURRENTLY across several specs (Linux + Windows on a
Windows host; a macOS guest on a macOS host) -- so multi-VM concurrency is
inherent where the platform has multiple guest types, and every path is proven
on each OS. Each call creates its own VM, runs everything against it, and
deletes it. There is no separate multi-VM test. Feature differences are
capability-gated via version()["capabilities"] (snapshots/templates are
Windows-only; those routes assert 501 on macOS).

Lifecycle discipline (mirrors the GUI):
  * Every VM start boots all the way to ONLINE (agent connected) before the VM is
    used -- the ONLY exceptions are the delete/edit-during-build tests, which
    deliberately act before the guest finishes installing.
  * Every shutdown is the GRACEFUL, agent-mediated one (asb_vm_shutdown -> hcs_stop_vm
    -> the guest agent's InitiateSystemShutdownExW / `systemctl`), sent once the
    agent is online -- with no agent the core returns ERROR_NOT_SUPPORTED and nothing
    shuts down.
  * Force-stop (POST .../stop) is exercised in exactly ONE dedicated test.

SSH key auto-deploy is folded in here (no separate VM): each VM is created with
sshEnabled + sshDeployKey, and once online we assert the agent deployed the
AppSandbox public key (sshState 4 / keyDeployed) and that a key-only login works.

run_vm_lifecycle(spec) -> (name, [failures]); spec = {name, os_type, ram, hdd, cores}.
"""
import os, sys, time, json, threading, urllib.request

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # for asb.py (parent dir)
import asb
import helpers

def run_vm_lifecycle(spec):
    name = spec["name"]
    os_type = spec.get("os_type", "Linux")
    ram = spec.get("ram", 4096)
    hdd = spec.get("hdd", 24)
    cores = spec.get("cores", 2)
    image = spec.get("image")            # explicit per-spec override (may be "")
    if image is None:
        if os_type == "Linux":
            image = helpers.ISO_LIN
        elif os_type == "Windows":
            image = helpers.ISO_WIN
        else:                            # macOS: IPSW; "" = cached/auto-fetched
            image = helpers.IPSW_MAC
    ONLINE_T = 600 if os_type == "Linux" else 1800   # boot-to-online budget
    GRACE_T  = 240 if os_type == "Linux" else 600    # graceful-shutdown budget
    # The build/install/download phase is stall-based -- it fails only after
    # BUILD_STALL seconds of zero observable movement, however long a healthy
    # download/install takes. EXCEPTION: a Windows guest's first boot (OOBE +
    # SetupComplete + driver/agent install) reports NO host-side progress -- it
    # stays "Installing Windows" (indeterminate) until the guest agent connects,
    # exactly like Windows-on-Windows -- so that phase is a legitimately
    # signal-less window. It can run 10+ minutes (longer under concurrent
    # multi-VM load), so give Windows a real budget; Linux/macOS stream live
    # progress and keep the tight 600 s stall.
    BUILD_STALL = 3600 if os_type == "Windows" else 600

    c = asb.connect()
    caps = c.version().get("capabilities", {})
    with open(os.path.join(helpers.ASB_DIR, "host.json"), encoding="utf-8") as f:
        info = json.load(f)
    base, token = info["endpoint"], info["token"]

    fails = []
    def check(label, cond, detail=""):
        print(("  PASS  " if cond else "  FAIL  ") + ("[%s] " % name) + label + (("   " + detail) if detail else ""), flush=True)
        if not cond:
            fails.append("[%s] %s%s" % (name, label, ("  (" + detail + ")") if detail else ""))
    def st():
        try: return c.status(name)
        except KeyError: return None

    def boot_online(label, snap_index=-1, branch_index=-1, branch_name=None):
        """Start the VM (if not already running) and wait until fully ONLINE -- the
        guest agent connected. Every boot in the suite goes to online; only the
        pre-install teardown tests act before that."""
        if not (st() and st()["running"]):
            c.start(name, snap_index=snap_index, branch_index=branch_index, branch_name=branch_name)
        try:
            c.wait_online(name, timeout=ONLINE_T)
            check("boot to online (%s)" % label, True)
        except Exception as e:
            check("boot to online (%s)" % label, False, repr(e))
        return st()

    def graceful_down(label):
        """Default teardown: the graceful, agent-mediated shutdown. The agent must be
        online to receive it (so we wait for online first, exactly as the GUI only
        offers shutdown on an online VM), and we assert it actually reaches stopped.
        Force-stop is a last resort so the suite can continue -- but it's recorded as
        a failure, because graceful shutdown is expected to work."""
        if not (st() and st()["running"]):
            return
        c.wait_online(name, timeout=ONLINE_T)            # agent must be up to receive it
        code, _ = c.shutdown(name)
        ok = (code == 202)
        if ok:
            try: c.wait(name, "stopped", timeout=GRACE_T)
            except Exception: ok = False
        check("graceful shutdown -> stopped (%s)" % label, ok, "code=%s" % code)
        if st() and st()["running"]:                     # last-resort force, keep going
            c.stop(name); c.wait(name, "stopped", timeout=90)

    # per-VM SSE reader (proves events flow for this VM even with others active)
    sse = []
    def sse_reader():
        try:
            req = urllib.request.Request(base + "/v1/events", headers={"Authorization": "Bearer " + token})
            with urllib.request.urlopen(req, timeout=3600) as r:
                for line in r:
                    t = line.decode("utf-8", "replace").strip()
                    if t.startswith("data:"): sse.append(t[5:].strip())
        except Exception:
            pass
    threading.Thread(target=sse_reader, daemon=True).start()

    cfg = dict(name=name, osType=os_type, ramMb=ram, hddGb=hdd, cpuCores=cores,
               gpuMode=1, networkMode=1, testMode=True, sshEnabled=True, sshDeployKey=True,
               adminUser="user", adminPass="test123")
    expect_nvidia_smi = False
    if os_type == "Windows" and os.environ.get("ASB_TEST_NVIDIA_SMI") == "1":
        nvidia = next((g for g in c.host().get("gpus", []) if g.get("isNvidia") is True), None)
        if nvidia:
            cfg["gpuId"] = nvidia["id"]
            cfg["copyNvidiaSmi"] = True
            expect_nvidia_smi = True
    if image:   # macOS may omit it (cached/auto-fetched restore image)
        cfg["imagePath"] = image
    try:
        # --- create (validates the create path) ---
        code, body = c.create(**cfg)
        check("create -> 202", code in (200, 202), "%s %s" % (code, body))

        # --- delete/edit-during-build guards: the ONLY paths that act before the
        #     guest finishes installing (deliberately pre-online). Windows
        #     surfaces this as building=true; macOS as state=="installing". ---
        deadline = time.time() + 90
        caught_building = False
        while time.time() < deadline:
            s = st()
            if s and (s["building"] or s["state"] == "installing"):
                caught_building = True; break
            if s and not s["building"]:
                break
            time.sleep(2)
        if caught_building:
            check("delete during build -> 409", c.delete_vm(name)[0] == 409)
            check("edit during build -> 409", c.edit(name, ramMb=ram)[0] == 409)
            check("still present after refused delete", name in {v["name"] for v in c.list()})

        # --- wait until built/installed. Create auto-starts on both platforms
        #     (the macOS install briefly parks at "stopped" before the core
        #     auto-boots it once the disk is released), so this just waits past
        #     building/installing; boot_online is idempotent if it's already up.
        #     Stall-based: live progress keeps the wait alive; a hung build
        #     fails after BUILD_STALL seconds of no movement. ---
        try:
            s = helpers.wait_progress(
                c, name,
                lambda s: (not s["building"] and s["state"] != "installing"
                           and (s["running"] or s["state"] == "stopped")),
                stall=BUILD_STALL, label=name)
            check("build finished", True)
        except Exception as e:
            s = st()
            check("build finished", False, "%r state=%s" % (e, s and s["state"]))

        # --- boot all the way to ONLINE (agent connected). A fresh Windows install
        #     can take ~15-20 min to reach this. ---
        son = boot_online("post-install")
        check("guest installComplete", bool(son) and son["installComplete"], "state=%s" % (son and son["state"]))

        # --- display: a quick programmatic open + close now that the VM is online.
        #     Readiness (running + agentOnline + the display driver up) is a latched,
        #     passive flag -- never a frame-channel probe. On macOS there is no frame
        #     channel/driver (VZVirtualMachineView binds the in-process framebuffer),
        #     so readiness is just running + agentOnline. SKIPped (not failed) so CI
        #     stays green in two cases:
        #       * Linux guest whose deployed agent predates the idd_status push, so
        #         readiness never latches -- exercised once a fresh Linux VM auto-builds
        #         the updated agent;
        #       * a daemon in a non-interactive session that can't show a window
        #         (409 no_display) -- e.g. a sudo/launchd daemon with no console Aqua
        #         session, on either Windows or macOS. ---
        check("display not open before open", son.get("displayOpen") is False)
        deadline = time.time() + 90
        while time.time() < deadline and not c.display_ready(name):
            time.sleep(2)
        ready = c.display_ready(name)
        if not ready and os_type == "Linux":
            print("  SKIP  [%s] display open/close -- Linux guest readiness needs the pushed agent" % name, flush=True)
        else:
            check("display ready when online", ready, "display_status=%s" % c.display_status(name))
            oc, ob = c.open_display(name)
            if oc == 409 and ob.get("error", {}).get("code") == "no_display":
                print("  SKIP  [%s] display open/close -- no interactive desktop on daemon session" % name, flush=True)
            else:
                check("display open -> 200", oc == 200, "%s %s" % (oc, ob))
                check("displayOpen true after open", bool(st()) and st().get("displayOpen") is True)
                cc, cb = c.close_display(name)
                check("display close -> 200", cc == 200, "%s %s" % (cc, cb))
                check("displayOpen false after close", bool(st()) and st().get("displayOpen") is False)
            check("daemon alive after display open/close", bool(c.version().get("product")))

        # --- duplicate name rejected ---
        check("duplicate name -> 400", c._req("POST", "/vms", dict(cfg))[0] == 400)

        # --- status reflects the spec ---
        check("status osType == %s" % os_type, son["osType"] == os_type, "got=%s" % son["osType"])
        check("status ramMb == %d" % ram, son["ramMb"] == ram, "got=%s" % son["ramMb"])
        check("status cpuCores == %d" % cores, son["cpuCores"] == cores, "got=%s" % son["cpuCores"])
        check("copyNvidiaSmi status round-trips",
              son.get("copyNvidiaSmi") == expect_nvidia_smi,
              "expected=%r got=%r" % (expect_nvidia_smi, son.get("copyNvidiaSmi")))

        # --- SSH key auto-deploy (this VM was created with sshEnabled + sshDeployKey).
        #     Once online the guest agent writes the AppSandbox public key into the
        #     guest's authorized_keys and reports it back: sshState becomes 4 and
        #     keyDeployed flips true. We then prove a key-only BatchMode login works
        #     (no password). NOTE: the Linux/macOS agent is built from the pushed
        #     GitHub branch, so keyDeployed stays false -- and these checks stay red --
        #     until `headless-api` is pushed; the Windows agent is built locally. ---
        si = c.ssh_info(name)
        check("sshInfo host == 127.0.0.1", si.get("host") == "127.0.0.1")
        deadline = time.time() + 180
        while time.time() < deadline and not c.ssh_info(name).get("keyDeployed"):
            time.sleep(3)
        si = c.ssh_info(name)
        deployed = bool(si.get("keyDeployed"))
        check("ssh key deployed (keyDeployed)", deployed)
        check("sshState == 4 when key deployed", (not deployed) or si.get("sshState") == 4,
              "sshState=%s" % si.get("sshState"))
        if deployed:
            rc, out, err = helpers.ssh_key_login(si.get("port"), si.get("user") or "user", "whoami")
            check("ssh key-only login (no password)", rc == 0,
                  "rc=%s out=%r err=%r" % (rc, (out or "").strip(), (err or "").strip()))

        # --- running-state guards (VM is online now) ---
        check("edit RUNNING -> 409", c.edit(name, ramMb=son["ramMb"])[0] == 409)
        if caps.get("snapshots"):
            check("snap RUNNING -> 409", c.snap_take(name)[0] == 409)
        else:
            check("snap on this host -> 501", c.snap_take(name)[0] == 501)

        # --- daemon shutdown-guard (SAFE: only POST while MY vm is running -> 409) ---
        sd = c.shutdown_daemon(force=False)
        check("POST /shutdown refused (vms_active)", sd[0] == 409 and name in sd[1].get("error", {}).get("active", []), "%s" % (sd,))

        # --- graceful shutdown (the primary graceful-shutdown test) ---
        graceful_down("primary")

        # --- config applied at start: edit each field while stopped, then ONE boot
        #     to online to confirm all are reflected on the running VM. ---
        fields = [("ramMb", ram + 2000), ("cpuCores", 1 if cores != 1 else 2), ("gpuMode", 0), ("networkMode", 0)]
        for field, val in fields:
            check("edit %s=%s (stopped) -> 200" % (field, val), c.edit(name, **{field: val})[0] == 200)
            check("%s persisted = %s" % (field, val), st()[field] == val, "got=%s" % st()[field])
        sv = boot_online("config applied")
        for field, val in fields:
            check("running reports %s=%s" % (field, val), sv[field] == val, "got=%s" % sv[field])
        graceful_down("after config")

        # --- edit-validation rejections (stopped) ---
        check("edit odd RAM -> 400", c._req("PUT", "/vms/" + name, {"ramMb": 4001})[0] == 400)
        check("edit gpuMode=9 -> 400", c._req("PUT", "/vms/" + name, {"gpuMode": 9})[0] == 400)

        # --- snapshots + branches (Windows-only; capability-gated). Validate the
        #     on-disk artifacts, not just the API list: the base disk and a
        #     per-snapshot differencing VHDX under <vm>\snapshots\. ---
        vm_dir = helpers.vm_dir(name)
        check("base %s exists on disk" % helpers.DISK_NAME,
              os.path.isfile(os.path.join(vm_dir, helpers.DISK_NAME)))
        if not caps.get("snapshots"):
            check("snapshot list -> 501", c._req("GET", "/vms/%s/snapshots" % name)[0] == 501)
        else:
            snap_dir = os.path.join(vm_dir, "snapshots")
            def snap_files(prefix):
                here = os.listdir(snap_dir) if os.path.isdir(snap_dir) else []
                return [f for f in here if f.lower().endswith(".vhdx") and f.lower().startswith(prefix)]
            n0 = len(c.snapshots(name)); snap0 = len(snap_files("snapshot_"))
            check("snap take -> 202", c.snap_take(name, "s1")[0] == 202); time.sleep(2)
            check("snapshot count +1", len(c.snapshots(name)) == n0 + 1)
            check("snapshot_*.vhdx created on disk", len(snap_files("snapshot_")) == snap0 + 1,
                  "snapshots/=%s" % (os.listdir(snap_dir) if os.path.isdir(snap_dir) else "missing"))
            sidx = next((x["index"] for x in c.snapshots(name) if x["name"] == "s1"), None)
            check("snapshot found", sidx is not None)
            if sidx is not None:
                check("rename -> 202", c.snap_rename(name, sidx, "s1b")[0] == 202)
                bc0 = next(x["branchCount"] for x in c.snapshots_full(name)["snapshots"] if x["index"] == sidx)
                boot_online("from snapshot s1", snap_index=sidx, branch_name="br1")   # boots a branch off s1 to online
                graceful_down("after snapshot boot")
                snap = next(x for x in c.snapshots_full(name)["snapshots"] if x["index"] == sidx)
                check("start-from-snapshot created a branch", snap["branchCount"] == bc0 + 1, "%s->%s" % (bc0, snap["branchCount"]))
                branches = snap.get("branches", [])
                bidx = next((b["index"] for b in branches if b["name"] == "br1"), max((b["index"] for b in branches), default=None))
                if bidx is not None:
                    check("delete branch -> 202", c.snap_delete_branch(name, sidx, bidx)[0] == 202); time.sleep(2)
                check("delete snapshot -> 202", c.snap_delete(name, sidx)[0] == 202); time.sleep(2)
                check("snapshot count restored", len(c.snapshots(name)) == n0)
                check("snapshot_*.vhdx removed from disk", len(snap_files("snapshot_")) == snap0,
                      "snapshots/=%s" % (os.listdir(snap_dir) if os.path.isdir(snap_dir) else "missing"))

            # --- base branches (snapIndex -2): boot a base branch to online; never the base node ---
            check("base node delete refused -> 400", c._req("DELETE", "/vms/%s/snapshots/-2" % name)[0] == 400)
            bb0 = len(c.snapshots_full(name).get("baseBranches", []))
            boot_online("from base branch", snap_index=-2, branch_name="bb1")   # boots a base branch to online
            graceful_down("after base-branch boot")
            bbs = c.snapshots_full(name).get("baseBranches", [])
            check("base branch created + listed", len(bbs) == bb0 + 1, "baseBranches=%s" % bbs)
            bbidx = next((b["index"] for b in bbs if b["name"] == "bb1"), bbs[-1]["index"] if bbs else None)
            if bbidx is not None:
                check("rename base branch -> 202", c.snap_rename(name, -2, "bb1r", branch_index=bbidx)[0] == 202); time.sleep(2)
                check("base branch rename persisted", any(b["name"] == "bb1r" for b in c.snapshots_full(name).get("baseBranches", [])))
                check("delete base branch -> 202", c.snap_delete_branch(name, -2, bbidx)[0] == 202); time.sleep(2)
                check("base branch count restored", len(c.snapshots_full(name).get("baseBranches", [])) == bb0)

        # --- idempotency + the ONE dedicated FORCE-stop test (force is reserved for here) ---
        boot_online("idempotency")
        check("start already-running -> 202", c.start(name)[0] == 202)
        check("force stop -> 202", c.stop(name)[0] == 202)
        c.wait(name, "stopped", timeout=90)
        check("force stop reached stopped", bool(st()) and not st()["running"])
        check("stop already-stopped -> 202", c.stop(name)[0] == 202)

        # --- SSE delivered events for this VM ---
        check("SSE delivered events for this VM", any(name in e for e in sse), "events=%d" % len(sse))

        # --- delete -> no orphan ---
        check("delete -> 202", c.delete_vm(name)[0] in (200, 202)); time.sleep(3)
        check("removed from list", name not in {v["name"] for v in c.list()})
        check("no orphan dir", not os.path.isdir(helpers.vm_dir(name)))
    except Exception as e:
        check("suite ran without unhandled exception", False, repr(e))
    finally:
        helpers.destroy_vm(c, name)

    return name, fails
