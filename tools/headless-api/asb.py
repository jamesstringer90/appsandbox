"""
asb.py -- Python client for the AppSandbox headless daemon (Windows + macOS).

Start the daemon with:
    Windows:  appsandbox.exe --headless                     (elevated shell)
    macOS:    AppSandbox.app/Contents/MacOS/AppSandbox --headless
Then:

    import asb
    c = asb.connect()                 # reads the per-platform host.json
    c.start("linux")
    c.wait("linux", {"booting", "online"})
    c.stop("linux")

Stdlib only (urllib) -- no third-party dependencies. The daemon exposes a
Docker-style local HTTP/JSON API over loopback; the discovery file gives us the
port + bearer token so connect() takes no arguments. The API is IDENTICAL on
both platforms; feature differences (snapshots/templates are Windows-only) are
advertised in version()["capabilities"] and those routes return 501 on macOS.
"""
import json
import os
import sys
import time
import urllib.request
import urllib.error


def _support_dir():
    """Per-platform AppSandbox data dir (discovery file, ssh keypair, logs)."""
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/AppSandbox")
    base = os.environ.get("ProgramData", r"C:\ProgramData")
    return os.path.join(base, "AppSandbox")


def _discovery_path():
    return os.path.join(_support_dir(), "host.json")


def daemon_launch_hint():
    if sys.platform == "darwin":
        return "AppSandbox.app/Contents/MacOS/AppSandbox --headless"
    return "appsandbox.exe --headless (from an elevated shell)"


def key_path():
    """Path to the AppSandbox SSH private key. VMs created with sshDeployKey=True
    get the matching public key in their authorized_keys, so you can connect with:
        ssh -i <key_path()> -p <sshInfo['port']> <sshInfo['user']>@127.0.0.1
    The daemon generates the keypair (under <support dir>/ssh/) the first time a
    key-deploy VM is created."""
    return os.path.join(_support_dir(), "ssh", "id_appsandbox")


# VM lifecycle states the daemon's derived `state` can report.
RUNNING_STATES = {"booting", "online"}   # powered on (agent maybe not up yet)


class Client:
    def __init__(self, endpoint, token):
        self.base = endpoint.rstrip("/") + "/v1"
        self.token = token

    def _req(self, method, path, body=None, timeout=60):
        url = self.base + path
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")   # parser tolerates whitespace
        elif method in ("POST", "PUT"):
            data = b"{}"   # http.sys returns 411 on a body-less POST/PUT
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.token)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                raw = r.read().decode("utf-8")
                return r.status, (json.loads(raw) if raw else {})
        except urllib.error.HTTPError as e:
            raw = e.read().decode("utf-8")
            return e.code, (json.loads(raw) if raw else {})

    # ---- queries ----
    def version(self):
        return self._req("GET", "/version")[1]

    def list(self):
        return self._req("GET", "/vms")[1].get("vms", [])

    def status(self, name):
        code, body = self._req("GET", "/vms/" + name)
        if code == 404:
            raise KeyError(name)
        return body

    # ---- lifecycle (return (http_status, body)) ----
    def start(self, name, snap_index=-1, branch_index=-1, branch_name=None):
        """Start the VM. Optionally boot from a snapshot/branch (mirrors the GUI's
        startVm). Passing snap_index + branch_name boots from that snapshot on a new
        branch -- the GUI's only way to create a branch. No args -> current state."""
        body = {}
        if snap_index != -1:   body["snapIndex"] = snap_index
        if branch_index != -1: body["branchIndex"] = branch_index
        if branch_name:        body["branchName"] = branch_name
        return self._req("POST", "/vms/%s/start" % name, body or None)
    def shutdown(self, name): return self._req("POST", "/vms/%s/shutdown" % name)   # graceful
    def stop(self, name):     return self._req("POST", "/vms/%s/stop" % name)       # force
    def delete_vm(self, name):return self._req("POST", "/vms/%s/delete" % name)

    def create(self, **cfg):
        """Create a VM. Config keys: name, osType, imagePath|templateName, ramMb,
        hddGb, cpuCores, gpuMode(0-2), copyNvidiaSmi, networkMode(0-3), netAdapter,
        adminUser, adminPass, testMode, sshEnabled, sshDeployKey, isTemplate.
        copyNvidiaSmi is an optional create-only Windows-host/Windows-guest flag
        and defaults to False. sshDeployKey
        (requires sshEnabled) deploys the AppSandbox public key so you can SSH in
        with key auth (see key_path()); sshInfo reports keyDeployed + sshState 4
        once it lands. The daemon validates these exactly like the GUI; ramMb is
        rounded down to even here (2 MB-aligned, an HCS requirement, like the GUI).
        Async + auto-starts; watch status/events."""
        if isinstance(cfg.get("ramMb"), int):
            cfg["ramMb"] -= cfg["ramMb"] % 2
        return self._req("POST", "/vms", cfg)

    def edit(self, name, **fields):
        """Change config on a STOPPED VM. Honors ramMb/cpuCores/gpuMode/networkMode
        (same ranges as create); name is fixed at create and any other key is ignored.
        Returns (status, body) -- 409 if the VM is running."""
        if isinstance(fields.get("ramMb"), int):
            fields["ramMb"] -= fields["ramMb"] % 2   # 2 MB-aligned, like the GUI
        return self._req("PUT", "/vms/%s" % name, fields)

    # ---- host / templates / ssh / snapshots ----
    def host(self):              return self._req("GET", "/host")[1]
    def templates(self):         return self._req("GET", "/templates")[1].get("templates", [])
    def delete_template(self, name): return self._req("DELETE", "/templates/" + name)
    def ssh_info(self, name):    return self._req("GET", "/vms/%s/sshInfo" % name)[1]
    def open_display(self, name):
        """Open the VM's display window on the daemon's local desktop (the GUI's
        Connect view). The VM must be running, and the daemon must be in an
        interactive session that can show a window (a headless/SSH/service daemon
        returns 409 "no_display"). Returns (status, body); status() reports
        displayOpen, which also goes false if the user closes the window."""
        return self._req("POST", "/vms/%s/display" % name)
    def close_display(self, name): return self._req("DELETE", "/vms/%s/display" % name)
    def display_status(self, name):
        """{'open': bool, 'ready': bool}. 'ready' is the agent's own report that the
        display driver is up (running + agentOnline + idd_status) -- a passive flag,
        NOT a probe: polling it touches no window and no frame channel, so it can't
        disturb the display. Wait for ready, then call open_display()."""
        return self._req("GET", "/vms/%s/display" % name)[1]
    def display_ready(self, name):  return bool(self.display_status(name).get("ready"))
    def snapshots(self, name):   return self._req("GET", "/vms/%s/snapshots" % name)[1].get("snapshots", [])
    def snapshots_full(self, name): return self._req("GET", "/vms/%s/snapshots" % name)[1]  # incl. "current"
    def snap_take(self, name, snapname=None):
        """Take a snapshot -- the VM must be STOPPED (409 vm_running otherwise)."""
        return self._req("POST", "/vms/%s/snapshots" % name, {"name": snapname} if snapname else {})
    def snap_delete(self, name, index):
        return self._req("DELETE", "/vms/%s/snapshots/%d" % (name, index))
    def snap_rename(self, name, index, new_name, branch_index=-1):
        return self._req("PUT", "/vms/%s/snapshots/%d" % (name, index),
                         {"name": new_name, "branchIndex": branch_index})
    def snap_delete_branch(self, name, index, branch_index):
        return self._req("DELETE", "/vms/%s/snapshots/%d/branches/%d" % (name, index, branch_index))

    def shutdown_daemon(self, force=False):
        # Refused (409) while VMs run unless force=True (which terminates them).
        return self._req("POST", "/shutdown", {"force": True} if force else {})

    # ---- events (Server-Sent Events) ----
    def events(self):
        """Stream daemon events as parsed dicts, e.g. {"event":"vmStateChanged",
        "name":..., "running":...}. Blocks -- iterate it in a background thread;
        reconnect by calling again. Events are a convenience: status/wait() are the
        miss-proof source of truth."""
        req = urllib.request.Request(self.base + "/events",
                                     headers={"Authorization": "Bearer " + self.token})
        with urllib.request.urlopen(req) as r:
            for line in r:
                s = line.decode("utf-8", "replace").strip()
                if s.startswith("data:"):
                    payload = s[5:].strip()
                    try:
                        yield json.loads(payload)
                    except ValueError:
                        yield {"raw": payload}

    # ---- waiting (poll-first: level signals, miss-proof) ----
    def wait(self, name, target_states, timeout=300, interval=2):
        if isinstance(target_states, str):
            target_states = {target_states}
        deadline = time.time() + timeout
        while True:
            try:
                s = self.status(name)
            except KeyError:
                raise RuntimeError("%s vanished (404)" % name)
            if s["state"] in target_states:
                return s
            if time.time() > deadline:
                raise TimeoutError(
                    "%s did not reach %s within %ss (last state=%s)"
                    % (name, sorted(target_states), timeout, s["state"]))
            time.sleep(interval)

    def wait_online(self, name, timeout=900):
        return self.wait(name, {"online"}, timeout=timeout)


def connect(endpoint=None, token=None, check=True):
    """Read the discovery file and return a Client.

    By default (check=True) it pings GET /v1/version to confirm a live AppSandbox
    daemon is actually there: the discovery file is removed only on a CLEAN
    shutdown, so after a crash it is stale and may point at a dead pid (or a port
    another process has since taken). The ping raises a clear error instead of
    letting the first real call fail confusingly. Pass check=False to skip it."""
    if endpoint is None or token is None:
        try:
            with open(_discovery_path(), encoding="utf-8") as f:
                info = json.load(f)
        except FileNotFoundError:
            raise RuntimeError(
                "no daemon discovery file at %s -- is the daemon running? "
                "start it with: %s" % (_discovery_path(), daemon_launch_hint()))
        endpoint = endpoint or info["endpoint"]
        token = token or info["token"]
    c = Client(endpoint, token)
    if check:
        try:
            v = c._req("GET", "/version", timeout=3)[1]
        except Exception as e:
            raise RuntimeError(
                "AppSandbox daemon at %s is not responding (stale host.json from an "
                "unclean shutdown?): %s" % (endpoint, e))
        if v.get("product") != "AppSandbox":
            raise RuntimeError(
                "%s is not an AppSandbox daemon -- stale host.json (the port was "
                "taken by another process?)" % endpoint)
    return c
