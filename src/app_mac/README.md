# AppSandbox — macOS App

The macOS port of App Sandbox. Uses Apple's Virtualization.framework
for VM lifecycle and `WKWebView` to host the shared `web/` UI.

## Requirements

- macOS 14.0 (Sonoma) or later, Apple Silicon recommended
- Xcode 15.0 or later
- An Apple Development signing identity (free Personal Team is fine for
  local running)

## Building

1. Open `AppSandbox.xcodeproj` (at the repo root) in Xcode.
2. Select the **AppSandbox** scheme and a **My Mac** destination.
3. In the target's **Signing & Capabilities** tab, pick your Team.
4. Build and run (`Cmd-R`).

Every source file, framework, header search path, bundle resource
reference, and entitlement is already configured in the project. The
`web/` folder at the repo root is added as a folder reference and
copies into `Contents/Resources/web/` in the built bundle.

## What the app does on first run

- Shows the App Sandbox window with the shared web UI rendered via
  `WKWebView`.
- Creating a VM starts a macOS guest via Virtualization.framework: on
  first run it fetches the latest supported restore image from Apple,
  downloads it, and runs `VZMacOSInstaller`. Progress and install
  phase strings come back to the web UI via the event callback.
- VMs are persisted under
  `~/Library/Application Support/AppSandbox/VMs/<name>/`.

## Entitlements

- `com.apple.security.virtualization` — required for VZ.
- `com.apple.security.hypervisor` — required by VZ on some configs.
- `com.apple.security.network.{client,server}` — restore image fetch
  and guest NAT networking.
- `com.apple.security.files.user-selected.read-write` — NSOpenPanel
  .ipsw browsing.

`com.apple.security.app-sandbox` is currently `false` because raw
disk image access doesn't play with the sandbox.

## Project layout

```
src/app_mac/          macOS app shell
  main.m              NSApplication bootstrap
  AppDelegate.h/.m    NSApp delegate
  MainWindowController.h/.m   window + WKWebView host
  WebBridge.h/.m      JS <-> native action/event bridge
  Info.plist
  AppSandbox.entitlements

src/backend_mac/      Virtualization.framework engine
  backend_mac.m       BackendVtbl implementation
  host_info.h/.m      host RAM/CPU/free-space probe
  vm_dir.h/.m         on-disk VM layout
  vz_vm.h/.m          VZVirtualMachine build/load/start/stop
  vz_install.h/.m     VZMacOSInstaller orchestration
  vz_disk.h/.m        raw disk image create/copy
  vz_display.h/.m     VZVirtualMachineView window
  vz_network.h/.m     NAT/bridged VZ network configs

src/core/             shared with Windows backend
  backend.h
  vm_types.h
```

## Checking for source drift

After adding or removing files in `src/core/`, run:

```
python3 tools/check-sources.py
```

from the repo root. It reports any files on disk that are missing
from `AppSandboxCore.vcxproj` or `AppSandbox.xcodeproj`, and vice
versa.
