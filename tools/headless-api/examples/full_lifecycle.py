"""
End-to-end tour of the SDK: create a VM, wait for it to install, read and edit
its config while stopped, take/rename/delete a snapshot, branch from a snapshot,
read SSH info, then delete it. Self-contained -- it makes its own VM and removes
it at the end, so it assumes no existing configuration.

    python examples/full_lifecycle.py [linux|windows]

A fresh Linux guest installs in a few minutes; Windows can take ~15-20. Point
imagePath at an ISO you have. Run from an elevated shell (the daemon is too).
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import asb

OS_TYPE = "windows11" if (len(sys.argv) > 1 and sys.argv[1].lower().startswith("win")) else "ubuntu2604lts"
NAME = "demo-vm"
ISO = (r"C:\ISOs\ubuntu-26.04.iso" if OS_TYPE == "ubuntu2604lts"
       else r"C:\ISOs\Win11_24H2.iso")   # <-- edit to a real ISO path

c = asb.connect()
print("daemon:", c.version().get("version"), "| host free:", c.host()["freeGb"], "GB")

if NAME in {v["name"] for v in c.list()}:
    sys.exit("a VM named %r already exists -- delete it first" % NAME)

# --- create: validated exactly like the GUI; async + auto-starts ---
code, body = c.create(name=NAME, osType=OS_TYPE, imagePath=ISO,
                      ramMb=4096, hddGb=40 if OS_TYPE == "ubuntu2604lts" else 64, cpuCores=2,
                      gpuMode=1, networkMode=1,        # 1 = paravirtual GPU, NAT
                      adminUser="user", adminPass="test123",
                      testMode=True, sshEnabled=False)
print("create ->", code, body.get("error") or "ok")
if code not in (200, 202):
    sys.exit("create rejected")

try:
    # --- wait through build -> install -> agent online ---
    print("waiting for the guest to install + boot (this is the slow part)...")
    s = c.wait_online(NAME, timeout=2400)
    print("online: state=%s ram=%dMB cpu=%d gpu=%d net=%d"
          % (s["state"], s["ramMb"], s["cpuCores"], s["gpuMode"], s["networkMode"]))

    # --- ssh info (loopback-forwarded; populated once the agent is up) ---
    ssh = c.ssh_info(NAME)
    print("ssh: %s:%s user=%s sshState=%s" % (ssh.get("host"), ssh.get("port"),
                                              ssh.get("user"), ssh.get("sshState")))

    # --- take a snapshot of the running VM ---
    print("snap take ->", c.snap_take(NAME, "clean-install")[0])
    snaps = c.snapshots(NAME)
    print("snapshots:", [(x["index"], x["name"]) for x in snaps])
    sidx = snaps[-1]["index"]
    print("snap rename ->", c.snap_rename(NAME, sidx, "baseline")[0])

    # --- config edits require the VM stopped; graceful shutdown first ---
    print("graceful shutdown ->", c.shutdown(NAME)[0])
    c.wait(NAME, "stopped", timeout=600)

    print("edit ram=6144,cpu=4 ->", c.edit(NAME, ramMb=6144, cpuCores=4)[0])
    s = c.status(NAME)
    print("now:", s["ramMb"], "MB,", s["cpuCores"], "cores")

    # --- branch: boot from the snapshot onto a new branch (the GUI's only way) ---
    print("start from snapshot %d on branch 'experiment' ->" % sidx,
          c.start(NAME, snap_index=sidx, branch_name="experiment")[0])
    c.wait(NAME, asb.RUNNING_STATES, timeout=300)
    full = c.snapshots_full(NAME)
    print("branches on snap %d:" % sidx,
          next(x["branchCount"] for x in full["snapshots"] if x["index"] == sidx))

    # --- clean up the snapshot tree: drop the branch we made, then the snapshot ---
    c.shutdown(NAME); c.wait(NAME, "stopped", timeout=600)
    snap = next(x for x in c.snapshots_full(NAME)["snapshots"] if x["index"] == sidx)
    branch = next((b for b in snap.get("branches", []) if b["name"] == "experiment"), None)
    if branch:
        print("snap delete branch %d ->" % branch["index"],
              c.snap_delete_branch(NAME, sidx, branch["index"])[0])
    print("snap delete ->", c.snap_delete(NAME, sidx)[0])

finally:
    # --- always delete the VM we made (force-stop if still running) ---
    if c.status(NAME).get("running"):
        c.stop(NAME); c.wait(NAME, "stopped", timeout=120)
    print("delete ->", c.delete_vm(NAME)[0])
