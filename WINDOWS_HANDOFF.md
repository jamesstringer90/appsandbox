# Windows agent handoff

Use the prompt below with the Windows coding agent. Fetch and check out
`mac-headless-improvements`, using the existing Windows build environment and
suitable Windows/Ubuntu installer images. Keep local QA scripts and logs out of
the source tree.

## Prompt

Take over Windows-host verification of AppSandbox's `mac-headless-improvements`
branch against `main`. Read the actual diff, `CHANGELOG.md`, the root README, and
`tools/headless-api/README.md` before forming conclusions. Verify that the checkout
contains the described changes; do not accidentally test the old main build.

The goal is one authoritative core per host. GUI, local HTTP API, and Windows tray
controls must operate on the same VM records and viewers, with live state updates
and no stale targeting. Standalone `--headless` and `-headless` must still run
without the management UI or automatic viewer windows. A GUI host already exposes
the API; a second GUI/headless core must be rejected.

### Boundaries

- Use the primary coding model for code changes. Use GPT-6.1 Terra for real-app QA
  through computer use, the API/CLI, and SSH. Terra must not edit code; independently
  reproduce and inspect every reported defect before accepting it.
- Keep fixes limited to this branch's shared core, targeting, lifecycle, viewers,
  templates, and guest network provisioning. Reuse the existing Windows mechanisms.
  Do not introduce a parallel backend or duplicate working provisioning code.
- Do not change deployment, packaging, release infrastructure, or unrelated drivers.
  Do not publish a release. Do not add or expose credentials or private configuration.
  A previously observed Windows-on-Mac viewer driver issue was explicitly excluded
  from this work; do not turn this into an unrelated driver investigation.
- Add no code comments, debug endpoints, test files, or test harnesses to the branch.
  Existing comments/tests inherited from main are out of scope. Keep temporary QA
  helpers and output outside the checkout. Do not treat helper-only checks as proof
  that the real app works.
- Preserve user VMs, disks, templates, agents, and data. Back up the host registry
  before migration checks. Create uniquely named disposable QA VMs, record their
  persistent IDs, and delete only those fixtures. Keep guest CPU/RAM within the
  host's resources; start with two small guests.
- Changes must support existing VMs with older agent files. Do not require agent
  upgrades or silently recreate a user's VM to make a test pass.

### Implementation to inspect

The shared HTML/JS frontend is in `web/`. Core notifications and persistent IDs are
in `src/core/asb_events.*` and `src/core/asb_vm_id.h`. The ID is host metadata stored
separately from the name in `vms.cfg`; it is not a new guest-protocol requirement.

Inspect `src/app_win/main.c`, `headless.c/.h`, and `ui.c/.h` for startup, API request
dispatch onto the GUI thread, tray actions, state refresh, and shutdown. Inspect
`src/backend_win/asb_core.c`, `hcs_vm.*`, `vm_agent.c`, and `vm_display*.c/.h` for
stable VM record lifetimes, event delivery, asynchronous completion, and viewer
ownership. `AppSandboxCore.vcxproj` must include the shared event implementation.

Windows provisioning is shared through `tools/provision/win_provision.c/.h` and
used by `src/backend_win/disk_util.c`. Regular install, template audit/Sysprep, and
template instance setup are distinct paths. Preserve their existing semantics,
including disk extension; do not invent another grow-disk implementation.
Inspect both the direct ISO-to-VHDX path and existing resource-ISO/boot-from-ISO
paths, and identify which GUI/API inputs actually reach them. Do not invent an
import-VHDX feature or a new provisioning path for QA.

New Windows template instances must survive deleting their template. Existing VM
disks and snapshot chains must remain unchanged. The new copy and deletion paths
have been source-reviewed on Mac, but have not been compiled or run on Windows.

### Build and runtime checks

1. Build the actual Windows x64 application and core with the existing Visual Studio
   solution/projects and established local build procedure. Use the existing resource
   and guest payload preparation. Record the exact source revision and executable
   used. Report missing build prerequisites without modifying release infrastructure.

2. Launch the GUI normally with its expected UAC elevation. Connect using the current
   `tools/headless-api/asb.py` and `asb.connect()` without starting a second daemon.
   Verify queries, SSE events, and mutations work and the GUI stays responsive.
   Launching another core must fail cleanly without altering the active discovery
   file, token, VM registry, or guest networking.

3. Exercise every applicable action through both GUI and API: create, edit while
   stopped, start, graceful shutdown, force stop, delete, SSH, and viewer open/close.
   Compare GUI and tray state with API status after each action. Test invalid-state
   requests and stale confirmation dialogs without allowing them to affect another
   VM. An accepted asynchronous request is not a completed action; observe the result.

4. Run multiple guests concurrently, including Windows and Ubuntu if the host has
   the required resources. While one guest boots, stops, or is deleted, act on another
   by retained ID. Reorder/remove list entries, overlap GUI and API requests, and
   leave a tray menu open during state changes. Check for deadlocks, stale rows,
   wrong-VM operations, missing events, and callbacks from a prior VM run.

5. Verify IDs persist through app restarts and legacy registry migration. Confirm
   an existing VM with an old agent still boots and supports its previous operations.
   Delete a QA VM, recreate its name, and confirm the old ID returns not-found rather
   than targeting the replacement. Plain SDK strings remain names; `asb.VmId` selects
   explicit IDs. Use a Linux fixture for an ID-shaped name because Windows VM names
   have a 15-character limit. Exercise legacy name-based HTTP routes as well.

6. Verify real viewer behavior, not just API flags: API open followed by GUI/native
   close; GUI/tray open followed by API close; repeated open; close during boot;
   multiple viewers; stop/delete one VM while another viewer remains usable. Closing
   a viewer must not stop its VM. Delayed close notifications must not close a newer
   viewer. Verify display, input, clipboard, and audio still work where supported.

7. Verify Windows snapshots and branches through GUI and API: create while stopped,
   rename, select/start, create a named branch, use base branches, and delete. Confirm
   state and selected disk/branch remain consistent across interfaces. Use disposable
   guest markers to prove the selected disk state is the one actually booted.

8. Build a Windows template through the GUI and another through the API. Observe
   progress through host disk creation, guest audit/Sysprep, shutdown, and readiness.
   Confirm it appears once in the picker/API only when ready. Create at least two
   instances, including one with a larger requested disk. Check usable C: capacity,
   separate identities and disk writes, configured accounts, agent/SSH readiness,
   and concurrent operation. Inspect the actual new VHDXs: they must have distinct
   disk identifiers and no parent dependency. Delete the disposable template, restart
   the host app, and boot both instances through the GUI and API. Check their guest
   markers and snapshots still work. Verify this for new instances made from an
   existing template as well as a newly built template.
   Separately use disposable legacy differencing instances created by main: verify
   they boot unchanged, and GUI/API template deletion is refused while they depend
   on the base. Include inactive branches, a selected snapshot branch, missing
   `tree.dat`, and unreadable/missing disk metadata. Restore any temporarily moved
   fixture metadata before continuing. Do not convert existing disks or chains.
   During a new copy, verify GUI/API responsiveness and progress, multiple concurrent
   copies, refusal to delete the source, and shutdown cancellation. Exercise copy,
   growth, and media-generation failure paths where the real environment permits;
   incomplete disks must not become bootable VMs. Verify successful copies retain
   the resources ISO for mini-setup and for retries after a failed initial boot.
   Cancellation must wait for native I/O completion before deleting output; existing
   synchronous media/download helpers may still delay shutdown once entered.
   Compare template and regular creation paths; do not reuse a normal installed VM
   as a template without the existing Sysprep workflow.

9. On newly provisioned Windows NAT guests, verify the HCN/NetVSC adapter's profile
   is Private and the existing `FPS-ICMP4-ERQ-In` rule is enabled for
   Private/LocalSubnet4. Cover direct ISO-to-VHDX creation, every reachable existing
   resource-ISO path, and template instances through both GUI and API. Verify address
   handling follows the existing HCN agent path and guests can ping each other.
   Do not add ad-hoc firewall rules.
   Verify SSH/key deployment through AppSandbox's socket proxy separately from NAT.

10. With all guests stopped, quit the GUI and launch `appsandbox.exe --headless`
    from an elevated terminal; repeat with `-headless`. Confirm no management window,
    automatic viewer, or startup-error dialog. Drive guest boots and SSH through the
    API; request viewers explicitly only in a local interactive desktop session.
    Check stderr redirection, missing prerequisites, second-instance rejection, and
    a noninteractive/service launch where the test environment permits it. Do not
    assume the Mac-only headless menu-bar feature adds a Windows headless tray icon.

11. Exercise graceful app/API shutdown, forced shutdown, and quitting during a
    disposable disk/template build. Verify workers, viewers, sockets, listeners,
    discovery files, temporary mounts, and VM compute processes clean up. Relaunch
    and verify incomplete images are not offered as usable templates. Check ordinary
    GUI close-to-tray behavior and explicit tray Quit separately.

### Report and completion

For each result, state pass, fail, or blocked and give the actual app/API/guest
evidence. For a defect, provide the minimal reproduction, independently verify its
cause, implement a focused fix, and rerun the affected real workflow. Distinguish a
pre-existing issue from a regression by comparing with main when needed. Do not
infer Windows success from the Mac QA results or from a successful compile.

Before handing back, review the complete diff against main, ensure no new comments
or test/debug scaffolding were introduced, and remove disposable QA fixtures.
Update documentation only for user-visible behavior or API changes. Report what
was built, what was actually tested, any fixes, and any remaining blockers. Do not
merge, publish, or change unrelated deployment behavior.
