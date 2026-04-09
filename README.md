# App Sandbox

Isolated, GPU-accelerated Windows VMs on Windows 11, including Home edition.

<img width="1691" height="562" alt="App Sandbox" src="https://github.com/user-attachments/assets/d77e9d01-0bd9-48c6-9231-f35ff05b340b" />

App Sandbox creates Windows virtual machines that share the host GPU through Windows GPU Paravirtualization (GPU-PV). VMs are created from a standard Windows ISO. The software handles disk creation, driver injection, unattended setup, networking, and display — the VM boots to a usable desktop without manual configuration.

The primary use case is running software that shouldn't have access to your real machine — AI agents, untrusted executables, anything you'd otherwise need a second PC for. The VM is disposable. Snapshot it, let it run, roll back if needed.

The VM does not require an internet connection to function. Display, input, clipboard, and agent communication all use Hyper-V sockets, which are point-to-point between the host and VM and do not traverse your network. Network connectivity is optional and only needed if the software inside the VM requires internet access.

## Background

App Sandbox is the successor to [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV). Easy-GPU-PV was a set of PowerShell scripts that relied on Hyper-V (Windows 11 Pro only) and required the user to connect via RDP or third-party remote desktop tools. App Sandbox replaces all of that with a native application, built-in display and input over Hyper-V sockets, automated VM setup, and runs on the Host Compute System (HCS) — the same backend behind WSL2 and Docker containers. HCS only requires the **Virtual Machine Platform** optional Windows feature, which is available on all Windows 11 editions including Home.

## Requirements

- Windows 11 (any edition)
- **Virtual Machine Platform** enabled in Windows Features
- Administrator privileges
- A Windows 11 ISO

## Setup

1. Enable **Virtual Machine Platform** in Settings > System > Optional Features > More Windows Features. Reboot if prompted.
2. Run `AppSandbox.exe` as Administrator.
3. Create a VM — select your ISO, configure RAM, CPU, and GPU settings.
4. The application builds a VHDX from the ISO, boots the VM, and runs Windows setup automatically.
5. The display window opens when the guest agent comes online.

## Features

- **GPU-PV** — the host GPU is shared with the VM. DirectX and CUDA work inside the guest.
- **Display** — a custom Indirect Display Driver (IDD) in the guest streams the framebuffer to the host over Hyper-V sockets. Only dirty rectangles are transmitted. The host renders with D3D11.
- **Clipboard** — bidirectional clipboard sharing supporting text, files, images, and other formats.
- **Networking** — none, NAT, external, or internal. NAT mode allocates static IPs and configures the guest automatically. External mode connects the VM to a physical adapter on the host — the VM gets a DHCP lease from your router and has access to your local network.
- **Snapshots** — save VM state and create differencing disks. Snapshots support branching — multiple independent working copies from the same point.
- **Templates** — mark a VM as a template at creation time. Windows installs, syspreps, and shuts down automatically. New VMs created from that template skip the image extraction phase and start from OOBE, reducing setup time.
- **Guest agent** — runs inside the VM. Handles heartbeat, graceful shutdown, IP configuration, and GPU driver updates. Launches subprocesses for input injection and clipboard sync.

## Architecture

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

### Source layout

| File | Purpose |
|------|---------|
| `asb_core.c` | VM lifecycle, config persistence, orchestration |
| `hcs_vm.c` | HCS API wrapper |
| `hcn_network.c` | HCN networking |
| `disk_util.c` | VHDX creation, unattend.xml generation |
| `snapshot.c` | Snapshot tree with branching |
| `gpu_enum.c` | GPU-PV device enumeration |
| `vm_display_idd.c` | Host-side IDD frame receiver and D3D11 renderer |
| `tools/vdd/` | Guest-side Indirect Display Driver (IddCx) |
| `tools/agent/` | Guest-side agent |
| `tools/iso-patch/` | ISO to VHDX converter with file injection |

## Building

### Prerequisites

- **Visual Studio 2022** with the **Desktop development with C++** workload
- **Windows SDK** (10.0 or later, included with the C++ workload)
- **Windows Driver Kit (WDK)** — required for building the IDD virtual display driver (`AppSandboxVDD`). Install the WDK matching your SDK version from [Microsoft's WDK download page](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk). Without the WDK, the driver projects will fail to build but everything else will compile.
- WebView2 headers and loader DLL are in `vendor/webview2/`.

### Build steps

1. Clone the repo.
2. Open `AppSandbox.sln` in Visual Studio 2022.
3. Select configuration (Debug or Release) and platform (x64).
4. Build the solution.

Output goes to `bin\Debug\` or `bin\Release\`. The post-build step copies `web/`, `release/resources/`, and `WebView2Loader.dll` into the output directory.

### Projects

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
| p9client | .exe | 9P filesystem client for file sharing |
| AppSandboxVDD | .sys | Indirect Display Driver (IddCx) — requires WDK. Builds with a self-signed certificate. |
| AppSandboxVDD.Package | — | Driver package for AppSandboxVDD |
| test9p | .exe | 9P client test harness |

AppSandbox depends on AppSandboxCore, agent, and p9client. The solution has build dependencies configured.

## How it works

1. `iso-patch.exe` converts a Windows ISO to a VHDX, injecting an unattend.xml, the guest agent, the VDD driver, and setup scripts. The VDD is self-signed, so the setup process installs the certificate into the VM's trusted store and enables test signing mode in the guest BCD.
2. The core library constructs an HCS JSON document describing the VM (CPU, RAM, GPU-PV shares, network endpoint, UEFI firmware, virtual disks) and creates the compute system through HCS.
3. GPU-PV assigns a partition of the host GPU to the VM. Specific GPU driver files are copied from the host into the VM at creation time. On every boot, App Sandbox checks whether the host GPU drivers have changed and, if so, instructs the guest agent to update them automatically via a Plan 9 file share.
4. The IDD in the guest captures frames and sends dirty rectangles to the host over AF_HYPERV sockets. The host uploads the texture and renders through D3D11. Keyboard and mouse input is sent back to the guest over a separate Hyper-V socket. Clipboard data is synchronized bidirectionally over two additional sockets using a delayed-rendering protocol, supporting text, files, images, and other clipboard formats.
5. HCN manages virtual networking. NAT mode allocates a static IP from a pool and the agent configures it inside the guest. External mode bridges to a physical adapter.
6. Snapshots save VM memory state and create a differencing VHDX. Branches fork from any snapshot point.

## License

MIT

## Author

[James Stringer](https://github.com/jamesstringerparsec)
