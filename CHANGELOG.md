# Changelog

## Unreleased — shared core, GUI and API

### Shared control and lifecycle

- The GUI hosts the local API automatically. GUI, API, and tray/menu-bar actions
  use the same core state and viewer ownership. A second core process is rejected.
- Standalone `--headless` and `-headless` launches keep the management UI closed
  and open VM viewers only on explicit requests. Windows headless startup failures
  report through stderr/logging instead of a GUI error dialog.
- VM records have persistent host IDs separate from their names. Existing records
  are migrated automatically; name-based clients and older guest agents remain
  supported. Deleted IDs cannot target a replacement VM with the same name.
- Core events update the shared frontend. Windows records remain stable while
  asynchronous work is active, and viewer-close notifications identify the viewer
  instance so a stale close cannot affect its replacement.

### macOS

- The host minimum is macOS 27. New Mac VMs require a macOS 27+ restore image and
  provision the configured account and automatic desktop login through Apple's API.
  Existing older guests keep their normal boot path. AppSandbox retains its own
  SSH enablement, socket transport, and key deployment.
- Administrator authorization occurs at host startup and covers its lifetime.
  Subsequent VM creation and boot do not request authorization again.
- The menu-bar icon provides running-VM controls. Closing the management window
  leaves the core/API running, and the Dock can reopen the management window.
- Local desktop headless mode also offers the menu-bar icon, a "Headless API"
  indicator, and Quit. Menu creation is best-effort. Remote/non-graphical sessions
  remain independent of AppKit and the menu bar.
- Diagnostic messages use the regular app log rather than a separate log window.
- Windows guests use nested virtualization automatically when the host reports
  support. Nested virtualization is not available for macOS guests, and there is
  no user-facing toggle.

### Templates and networking

- Mac hosts can build templates for both Windows and macOS guests through the GUI
  or API, then create independent instances with the same or a larger usable disk.
  Windows templates use the shared Windows provisioning and Sysprep path. Mac bases
  defer account provisioning until each instance's first boot.
- Template selection and counts follow the selected guest OS. Incomplete Mac-host
  templates are not offered as ready bases.
- New template instances on both hosts continue working after their template is
  deleted. Windows reports copy progress and cancels unfinished copies on shutdown.
  Older Windows VMs remain supported; template deletion is refused while an older
  VM or an active copy requires it, or dependencies cannot be verified.
- Clone virtual hardware identities are distinct. Filesystem identifiers may be
  retained; templates do not promise unique fingerprints to every application.
- Windows and macOS guests on Mac hosts share Apple's NAT/DHCP network.
- New Windows NAT guests on both host platforms configure the relevant adapter as
  Private and enable the existing Windows IPv4 echo-request rule for the local
  subnet. No additional application firewall rules are created.
- On Mac hosts, quitting during disk creation stops owned workers and detaches mounted
  installer media. Unfinished disks are rejected for boot and can be deleted.

### Verification status

Real macOS-host QA covered GUI/API template creation, larger usable clone disks,
concurrent guests, SSH, inter-guest ping, clone restart after base deletion, Mac
viewer synchronization, interrupted disk-build cleanup, and headless guest boots.
The local headless menu icon and menu Quit were confirmed, including listener,
discovery-file, and process cleanup.

Windows-host compilation and runtime QA of the new independent-VHDX path remain
pending, including copy cancellation, template deletion, and legacy snapshot protection.
Full menu-state synchronization, the
Windows guest viewer on the Mac QA host, actual SSH/service host launches, and
hardware-dependent nested virtualization remain unverified. See
[the Windows handoff](WINDOWS_HANDOFF.md) for the remaining Windows work.

These are unreleased branch changes. No app release or deployment-system update
is part of this work.
