"""
Daemon lifecycle frame shared by the daemon-gated test modules.

Owns the fixture environment's daemon lifecycle: backs up the real
%ProgramData%\\AppSandbox\\vms.cfg, installs a fixture config, starts
the daemon, and restores the original config byte-for-byte. Each test
module is the sole owner of its sequence -- the daemon reads vms.cfg
once at startup and holds the machine-global single-instance lock, so
no other AppSandbox instance may be running when a test starts one.
Run the importing test from an elevated shell.
"""
import json
import os
import shutil
import subprocess
import sys
import time

import asb

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DAEMON_EXE = os.path.join(REPO_ROOT, "bin", "Debug", "AppSandbox.exe")
ASB_DIR = asb._support_dir()
CFG_PATH = os.path.join(ASB_DIR, "vms.cfg")
HOST_JSON = asb._discovery_path()
HEADLESS_LOG = os.path.join(ASB_DIR, "headless.log")


def backup_path_for(tag):
    return os.path.join(REPO_ROOT, "preflight", "vms.cfg.bak-" + tag)


def workdir_for(tag):
    return os.path.join(os.environ.get("TEMP", r"C:\Windows\Temp"),
                        "appsandbox-%s-round" % tag)


def die(msg):
    print("FATAL %s" % msg, flush=True)
    sys.exit(1)


def daemon_alive():
    """True when some AppSandbox daemon answers through host.json."""
    if not os.path.isfile(HOST_JSON):
        return False
    try:
        asb.connect()          # pings /v1/version
        return True
    except Exception:
        return False           # stale host.json from an unclean shutdown


def backup_or_die(bak):
    if os.path.isfile(bak):
        die("backup %s already exists -- a previous round did not restore "
            "the original config; restore it first (copy it back over %s, "
            "then delete the backup)" % (bak, CFG_PATH))
    if os.path.isfile(CFG_PATH):
        shutil.copyfile(CFG_PATH, bak)
        print("backed up original config -> %s" % bak, flush=True)
        print("restore command if this round is killed: "
              "copy %s back over %s, then delete the backup"
              % (bak, CFG_PATH), flush=True)
        return True
    print("no original vms.cfg existed (restore = delete the fixture file)",
          flush=True)
    return False


def install_fixture(rows):
    with open(CFG_PATH, "wb") as f:
        f.write(b"\xef\xbb\xbf")                     # UTF-8 BOM, canonical form
        for row in rows:
            f.write(row.encode("utf-8"))


def start_daemon(workdir):
    if os.path.isdir(workdir):
        shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    proc = subprocess.Popen([DAEMON_EXE, "--headless"], cwd=workdir)
    print("started daemon pid %d (cwd %s)" % (proc.pid, workdir), flush=True)
    return proc


def wait_ready(proc, timeout=90):
    """Process alive + host.json parses + pid matches this round's pid +
    a token-authorized read request succeeds (a stale host.json from an
    unclean earlier shutdown passes none of the pid/token checks)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            die("daemon exited early (code %s; an elevated shell is "
                "required -- exit 3 means it was not) -- see %s"
                % (proc.returncode, HEADLESS_LOG))
        if os.path.isfile(HOST_JSON):
            try:
                with open(HOST_JSON, encoding="utf-8") as f:
                    info = json.load(f)
                if int(info.get("pid", 0)) == proc.pid:
                    c = asb.Client(info["endpoint"], info["token"])
                    code, _ = c._req("GET", "/vms", timeout=3)
                    if code == 200:
                        return c
            except Exception:
                pass
        time.sleep(0.25)
    die("daemon never became ready (alive=%s; host.json parse/pid/token "
        "checks failed for %ds; another AppSandbox instance may hold the "
        "single-instance lock) -- see %s"
        % (proc.poll() is None, timeout, HEADLESS_LOG))


def kill_if_alive(proc):
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            die("daemon pid %d did not exit after kill" % proc.pid)


def stop_daemon(proc, c, force=False, workdir=None):
    try:
        c.shutdown_daemon(force=force)
    except Exception:
        pass
    deadline = time.time() + 30
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.25)
    if proc.poll() is None:
        print("daemon still up after bounded wait; forcing pid %d"
              % proc.pid, flush=True)
        kill_if_alive(proc)
    if workdir and os.path.isdir(workdir):
        shutil.rmtree(workdir, ignore_errors=True)


def restore_or_die(bak, had_original):
    if not had_original:
        if os.path.isfile(CFG_PATH):
            os.remove(CFG_PATH)
        print("fixture config removed (no original existed)", flush=True)
        return
    with open(bak, "rb") as f:
        original = f.read()
    with open(CFG_PATH, "wb") as f:
        f.write(original)
    with open(CFG_PATH, "rb") as f:
        restored = f.read()
    if restored != original:
        die("restore mismatch: %s does not byte-match the backup %s -- "
            "the original config is preserved in the backup; do not delete it"
            % (CFG_PATH, bak))
    os.remove(bak)
    print("original config restored byte-for-byte; backup removed", flush=True)


def headless_log_probe_count(token):
    """Count a token's occurrences in the daemon's own log (the file is
    rewritten at each daemon start, so counts are per-round)."""
    if not os.path.isfile(HEADLESS_LOG):
        return 0
    with open(HEADLESS_LOG, "r", encoding="utf-8", errors="replace") as f:
        return f.read().count(token)
