# headless-api tests

Self-contained integration tests for the AppSandbox headless daemon. They drive
the daemon over its HTTP/JSON API through the [`asb.py`](../asb.py) client — no
test framework, just stdlib Python. Every test creates the throwaway VMs it needs
(named `brk-*`) and deletes them at the end, so the suite **assumes no existing
configuration and never touches your VMs, templates, or snapshots**. The same
suite runs on Windows and macOS hosts; feature differences (snapshots/templates
are Windows-only) are gated on `version()["capabilities"]`.

## Running

**Windows** — start the daemon (elevated), point the suite at your install
ISOs, then run the harness from this folder's parent:

```
appsandbox.exe --headless
set ASB_ISO_LINUX=C:\path\to\ubuntu-desktop-amd64.iso
set ASB_ISO_WINDOWS=C:\path\to\Win11_x64.iso
python tests\run_all.py
```

**macOS** — start the daemon with sudo (required; it is fully non-interactive
and still operates on YOUR user's VM registry + restore-image cache via
`SUDO_USER`), then run the harness:

```
sudo AppSandbox.app/Contents/MacOS/AppSandbox --headless
python3 tests/run_all.py
```

ISOs default to this `tests/` folder if the env vars are unset. macOS guests
install from a restore IPSW: `ASB_IPSW_MACOS` overrides; by default the suite
**also tests the download path** — it sets any cached `restore.ipsw` aside and
lets the first VM fetch a fresh image from Apple (resumable), while the second
VM installs concurrently from the preserved original. A full run builds real
guests — it is long (a Windows install + sysprep, or a macOS IPSW download +
install, can each take 20–45 min) and uses real disk and bandwidth.

On a Windows host with NVIDIA GPU-PV support, set `ASB_TEST_NVIDIA_SMI=1` to
make the Windows lifecycle select an explicit NVIDIA adapter and exercise the
opt-in `copyNvidiaSmi` status/provisioning path. It remains off by default.

## Files

| File | What it covers |
| --- | --- |
| `run_all.py` | Master harness. Sweeps leftover `brk-*` VMs, runs the static checks once, then one full per-VM lifecycle per spec **concurrently** (Linux + Windows on a Windows host; two macOS guests on a macOS host — one downloading its IPSW, one installing from the cached image), then the Windows template lifecycle (Windows hosts only). Prints a per-thread summary and a coverage table. This is the entry point. |
| `vm_lifecycle.py` | The full per-VM sequence, parametrized by a create-spec. Covers create / status / edit / start / graceful shutdown / force-stop / SSE / delete, the during-build guards, **SSH key auto-deploy** (build with `sshDeployKey`, assert `keyDeployed`/`sshState 4`, then a key-only login), a quick **display open/close** once online (skipped on macOS hosts, on Linux guests until the updated agent is pushed, and in non-interactive daemon sessions), and — where `capabilities.snapshots` — snapshots + branches + base branches (asserts 501 on macOS instead). |
| `test_host_and_validation.py` | No-VM checks: host cores/RAM cross-checked against the exact OS calls the daemon makes (Win32 / sysctl), capability gates, and the create-input-validation rejections matching the GUI's JS guards for the host platform. Builds nothing. |
| `test_windows_template.py` | Windows-only template lifecycle: create a template (`isTemplate`), wait for sysprep finalization, create a VM from it, then delete the template. Never runs on macOS (`RUN_TEMPLATE = not IS_MAC`). |
| `helpers.py` | Shared helpers: per-platform paths (`vm_dir`, disk name), `destroy_vm` (guard-aware teardown), `wait_progress` (stall-based waiting driven by the daemon's live progress reporting — no fixed deadlines on phases that report progress), and `ssh_key_login` (key-only BatchMode SSH login). |

## Notes

- **No arbitrary deadlines on progress-reporting phases.** Builds, installs, and
  IPSW downloads are waited on with `helpers.wait_progress`: any change in the
  daemon-reported `state`/`progress`/`installStatus` resets a stall clock, and
  the wait fails only after 10 minutes of zero observable movement. Fixed
  timeouts remain only for signal-less single transitions (boot→online,
  graceful shutdown).
- **Lifecycle discipline mirrors the GUI.** Every VM start boots all the way to
  *online* (guest agent connected) before the VM is used; the only exceptions are
  the delete/edit-during-build guards. Every shutdown is the graceful,
  agent-mediated one — force-stop is exercised in exactly one dedicated test.
- **Guest agent provenance differs per OS.** The Windows and macOS guest agents
  are built locally and staged into the guest at create time, so their key-deploy
  checks pass with a local build. The **Linux** agent is compiled inside the
  guest from the pushed GitHub branch, so its key-deploy checks stay red until
  the `headless-api` branch is pushed.
- Default guest credentials are `user` / `test123`.
