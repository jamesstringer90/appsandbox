# App Sandbox

Isolated, GPU-accelerated sandboxes (virtual machines) on Windows and macOS.

<img width="1691" height="562" alt="App Sandbox" src="https://github.com/user-attachments/assets/d77e9d01-0bd9-48c6-9231-f35ff05b340b" />

App Sandbox creates isolated app sandboxes with GPU access and a built-in display, agent, clipboard, and SSH tunnel. On Windows, sandboxes share the host GPU through GPU Paravirtualization (GPU-PV) and are created from a standard Windows ISO. On macOS, sandboxes run macOS guests on Apple Silicon using the Virtualization framework and are created from an Apple restore image (.ipsw). On Windows, the entire setup is unattended — the sandbox boots to a usable desktop without manual configuration. On macOS, the install is automated but macOS Setup Assistant still requires a few interactive steps (Apple ID, FileVault, etc.) on first boot before reaching the desktop.

The primary use case is running software that shouldn't have access to your real machine — AI agents, untrusted executables, anything you wouldn't want having access to your personal data. Each sandbox is disposable. Snapshot it, let it run, roll back if needed.

On Windows, the sandbox does not require an internet connection to function. Display, input, clipboard, and agent communication all use Hyper-V sockets, which are point-to-point between the host and sandbox and do not traverse your network. On macOS, agent, clipboard, and SSH communication use virtio-vsock, which is likewise host-to-guest only. Network connectivity is optional on both platforms and only needed if the software inside the sandbox requires internet access.

## Background

App Sandbox is the successor to [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV). Easy-GPU-PV was a set of PowerShell scripts that relied on Hyper-V (Windows 11 Pro only) and required the user to connect via RDP or third-party remote desktop tools. App Sandbox replaces all of that with a native application, built-in display and input over Hyper-V sockets, automated setup, and runs on the Host Compute System (HCS) — the same backend behind WSL2 and Docker containers. HCS only requires the **Virtual Machine Platform** optional Windows feature, which is available on all Windows 11 editions including Home.

The macOS port uses Apple's Virtualization framework to run macOS guests on Apple Silicon Macs. It shares the same web UI as Windows and follows the same architecture: an in-memory sandbox array, INI-style config persistence, and a guest agent for lifecycle management.

## Requirements

### Windows
- Windows 11 (any edition)
- Administrator privileges
- A Windows 11 ISO

### macOS
- Apple Silicon Mac (M1 or later)
- macOS 26 Tahoe or later
- Administrator privileges

## Setup

### Windows

1. Run `AppSandbox.exe` as Administrator. The app checks for the **Virtual Machine Platform** Windows feature and enables it automatically if needed, prompting for a reboot.
2. Create a sandbox — select your ISO, configure RAM, CPU, and GPU settings.
3. The application builds a VHDX from the ISO, boots the sandbox, and runs Windows setup automatically.
4. The display window opens when the guest agent comes online.

### macOS

1. Build and run the Xcode project (`AppSandbox.xcodeproj`).
2. Create a sandbox — the app fetches the latest macOS restore image from Apple (or select a local .ipsw), configure RAM, CPU, and disk settings.
3. The application installs macOS, stages the guest agent into the disk image, and marks the sandbox ready.
4. Start the sandbox. The display window opens and the guest agent connects over virtio-vsock.

## Features

- **Display** — on Windows, a custom Indirect Display Driver (IDD) in the guest streams the framebuffer to the host over Hyper-V sockets; only dirty rectangles are transmitted and the host renders with D3D11. On macOS, the Virtualization framework provides native display via VZVirtualMachineView with automatic resolution tracking.
- **Audio** — on Windows, a virtual speaker device (AppSandboxVAD) streams audio to the host. On macOS, the Virtualization framework provides VirtIO sound with host audio input/output. Audio is muted when the display window is closed or minimized.
- **Clipboard** — bidirectional clipboard sharing supporting text, files, images, and other formats. Uses Hyper-V sockets on Windows and virtio-vsock on macOS with the same delayed-rendering protocol.
- **SSH** — optionally install an SSH server in the guest. The host exposes it on a local TCP port via a socket proxy (Hyper-V sockets on Windows, virtio-vsock on macOS), so `ssh` works without networking configured.
- **Guest agent** — runs inside the sandbox. Handles heartbeat, graceful shutdown, and coordinates subprocesses for clipboard sync. On Windows, also handles IP configuration and GPU driver updates.
- **Networking** — NAT mode is available on both platforms. Windows additionally supports none, external (bridged), and internal modes.
- **GPU acceleration** — the host GPU is shared with the sandbox. On Windows, this uses GPU Paravirtualization (GPU-PV) with DirectX and CUDA support. On macOS, the Virtualization framework provides GPU acceleration automatically.
- **Snapshots** (Windows) — save sandbox state and create differencing disks. Snapshots support branching — multiple independent working copies from the same point.
- **Templates** (Windows) — mark a sandbox as a template at creation time. Windows installs, syspreps, and shuts down automatically. New sandboxes created from that template skip the image extraction phase and start from OOBE, reducing setup time.

## Architecture

The codebase is split into platform-specific backends behind a shared web UI (`web/app.js` + `web/index.html`). Both platforms use an in-memory sandbox array persisted to `vms.cfg` in INI format.

### Windows

Written in C. Compiled with Visual Studio 2022. Uses only Windows APIs — no third-party dependencies.

```
AppSandbox.exe              UI (WebView2, display windows, tray)
  |
  +-- appsandbox_core.dll   Core library
        +-- HCS             VM lifecycle (computecore.dll)
        +-- HCN             Networking (computenetwork.dll)
        +-- VirtDisk        VHDX creation (virtdisk.dll)
        +-- SetupAPI        GPU enumeration
```

All system DLLs are loaded dynamically at runtime.

### macOS

Written in Objective-C. Built with Xcode. Uses Apple's Virtualization framework — no third-party dependencies.

```
AppSandbox.app                UI (WKWebView, display windows)
  |
  +-- AppSandboxCore.framework  Core library
        +-- Virtualization   VM lifecycle (VZVirtualMachine)
        +-- virtio-vsock     Agent, clipboard, SSH channels
        +-- iso-patch-mac    Disk build + agent staging (privileged)
```

### Source layout

```
src/
  core/                     Shared types and event definitions
    asb_types.h               Platform-neutral VM types and error codes

  app_win/                  Windows application layer
    ui.c                      WebView2 bridge (JS <-> native)
    main.c                    Entry point, window management

  backend_win/              Windows VM backend
    asb_core.c                VM lifecycle, config persistence, orchestration
    hcs_vm.c                  HCS API wrapper
    hcn_network.c             HCN networking
    disk_util.c               VHDX creation, unattend.xml generation
    snapshot.c                Snapshot tree with branching
    gpu_enum.c                GPU-PV device enumeration
    vm_display_idd.c          Host-side IDD frame receiver and D3D11 renderer
    vm_agent.c                Guest agent connector (Hyper-V sockets)
    vm_ssh_proxy.c            TCP-to-HV-socket SSH relay

  app_mac/                  macOS application layer
    ui.m                      WKWebView bridge (JS <-> native)
    AppDelegate.m             App lifecycle, menu items
    EventLogWindow.m          Developer event log (Cmd+L)

  backend_mac/              macOS VM backend
    asb_core_mac.m            VM lifecycle, config persistence, orchestration
    vz_vm.m                   VZVirtualMachine wrapper (load/start/stop)
    vz_display.m              VZVirtualMachineView display window
    vz_network.m              VZNetworkDeviceConfiguration helpers
    iso_patch_mac.m           Disk build + agent staging (drives iso-patch-mac CLI)
    vm_agent_mac.m            Guest agent connector (virtio-vsock)
    vm_ssh_proxy_mac.m        TCP-to-vsock SSH relay
    vm_clipboard_mac.m        Clipboard sync (virtio-vsock)
    vm_dir.m                  VM directory layout helpers
    host_info.m               Host CPU/RAM/GPU/disk queries

  web/                      Shared web UI
    app.js                    Single-page app driving both platforms
    index.html                UI layout

tools/
  agent/                    Windows guest agent
  agent_mac/                macOS guest agent + clipboard helper + firstboot
  iso-patch/                Windows ISO to VHDX converter
  iso-patch-mac/            macOS disk build + agent staging CLI
  vdd/                      Windows Indirect Display Driver (IddCx)
  vad/                      Windows Virtual Audio Driver
```

## Building

### Windows

#### Prerequisites

- **Visual Studio 2022** with the **Desktop development with C++** workload
- **Windows SDK** (10.0 or later, included with the C++ workload)
- **Windows Driver Kit (WDK)** — required for building the IDD virtual display driver (`AppSandboxVDD`). Install the WDK matching your SDK version from [Microsoft's WDK download page](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).
- WebView2 headers and loader DLL are in `vendor/webview2/`.

#### Build steps

1. Clone the repo.
2. Open `AppSandbox.sln` in Visual Studio 2022.
3. Select configuration (Debug or Release) and platform (x64).
4. Build the solution.

Output goes to `bin\Debug\` or `bin\Release\`. The post-build step copies `web/`, `release/resources/`, and `WebView2Loader.dll` into the output directory.

#### Projects

| Project | Type | Description |
|---------|------|-------------|
| AppSandbox | .exe | Main application — WebView2 UI, display windows, tray icon |
| AppSandboxCore | .dll | Core library — VM orchestration, HCS/HCN/VirtDisk, persistence |
| iso-patch | .exe | Converts a Windows ISO to a VHDX with file injection |
| agent | .exe | Guest-side agent — heartbeat, shutdown, IP config |
| appsandbox-input | .exe | Guest-side input receiver (keyboard/mouse over Hyper-V sockets) |
| appsandbox-displays | .exe | Guest-side IDD frame sender |
| appsandbox-clipboard | .exe | Guest-side clipboard writer (host to guest) |
| appsandbox-clipboard-reader | .exe | Guest-side clipboard reader (guest to host) |
| AppSandboxVDD | .sys | Indirect Display Driver (IddCx) — requires WDK. Builds with a self-signed certificate. |
| AppSandboxVDD.Package | — | Driver package for AppSandboxVDD |
| AppSandboxVAD | .sys | Virtual Audio Driver (WDM audio miniport) — speaker device in the guest. Requires WDK. |
| VADPackage | — | Driver package for AppSandboxVAD |

AppSandbox depends on AppSandboxCore and agent. The solution has build dependencies configured.

### macOS

#### Prerequisites

- **Xcode 15** or later
- macOS 26 Tahoe or later
- Apple Silicon Mac

#### Build steps

1. Clone the repo.
2. Open `AppSandbox.xcodeproj` in Xcode.
3. Select the **AppSandbox** scheme and build.

The Xcode project builds the main app (which embeds the core framework) and the `iso-patch-mac` CLI tool. The guest agent binaries in `tools/agent_mac/` are compiled separately and bundled into the app's Resources.

## How it works

### Windows

1. `iso-patch.exe` converts a Windows ISO to a VHDX, injecting an unattend.xml, the guest agent, the VDD driver, and setup scripts. The VDD is self-signed, so the setup process installs the certificate into the VM's trusted store and enables test signing mode in the guest BCD.
2. The core library constructs an HCS JSON document describing the VM (CPU, RAM, GPU-PV shares, network endpoint, UEFI firmware, virtual disks) and creates the compute system through HCS.
3. GPU-PV assigns a partition of the host GPU to the VM. Specific GPU driver files are copied from the host into the VM at creation time. On every boot, App Sandbox checks whether the host GPU drivers have changed and, if so, instructs the guest agent to update them automatically via a Plan 9 file share.
4. The IDD in the guest captures frames and sends dirty rectangles to the host over AF_HYPERV sockets. The host uploads the texture and renders through D3D11. Keyboard and mouse input is sent back to the guest over a separate Hyper-V socket. Clipboard data is synchronized bidirectionally over two additional sockets using a delayed-rendering protocol, supporting text, files, images, and other clipboard formats.
5. HCN manages virtual networking. NAT mode allocates a static IP from a pool and the agent configures it inside the guest. External mode bridges to a physical adapter.
6. Snapshots save VM memory state and create a differencing VHDX. Branches fork from any snapshot point.

### macOS

1. The app fetches the latest supported macOS restore image from Apple (or uses a cached/user-provided .ipsw). `VZMacOSInstaller` installs macOS onto a raw disk image.
2. After install, `iso-patch-mac` mounts the disk image (with admin privileges), creates a pre-configured admin user, stages the guest agent and clipboard helper as LaunchDaemons/LaunchAgents, optionally enables SSH, and sets the computer name. A first-boot script finalizes the setup.
3. When the VM starts, the Virtualization framework boots the guest with the configured CPU, RAM, and disk. The display uses `VZVirtualMachineView`. Audio uses VirtIO sound devices.
4. The guest agent connects to the host over virtio-vsock. It handles heartbeat, graceful shutdown (via `/sbin/shutdown`), audio mute/unmute, and SSH state reporting. Clipboard sync runs over a separate vsock channel, gated on display window focus to prevent background leakage.
5. SSH is proxied from a host loopback port through virtio-vsock to the guest's sshd, so `ssh user@127.0.0.1 -p <port>` works without guest networking.

## License

MIT

## Acknowledgements

Beyond my own experience building [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV), I found [NanaBox](https://github.com/M2Team/NanaBox) to be a really helpful resource for understanding HCS.

## Author

[James Stringer](https://github.com/jamesstringerparsec)
