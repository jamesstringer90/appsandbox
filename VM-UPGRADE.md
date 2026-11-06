# VM upgrade prompt

Copy this prompt into Claude or Codex to update the guest components in an existing Windows or macOS VM.

```text
The release ZIP copied into the VM must be the same release version as the package
currently used to run App Sandbox on the host, with the appropriate package for
the guest OS and architecture. For best results, download the latest release from
https://github.com/jamesstringer90/appsandbox/releases for both the host application
and the VM agents.

Upgrade this existing Windows or macOS VM using that matching package. Preserve my
apps, files, accounts and settings. Use packaged binaries only; do not build
anything. Confirm the package and host versions match before proceeding.

Detect the guest's native architecture and locate the package:

- Windows x64: AppSandbox-<version>-win-x64.zip.
- Windows ARM64, including Windows VMs on Macs: AppSandbox-<version>-win-arm64.zip.
- macOS ARM64: the guest helpers inside
  AppSandbox.app/Contents/Resources/agent_mac/.

Windows needs the full ZIP, including resources/ and drivers/, not just
AppSandbox.exe. The component lists below are not exhaustive: future releases may
add or change guest files, drivers and services. You are responsible for
identifying and updating every applicable guest component in the supplied release.
Check its OS requirements and package contents; if anything is unclear, carefully
read the deployment code at the matching release tag in
https://github.com/jamesstringer90/appsandbox before proceeding. Ask for missing
files or required access only.

For the best chance of success, perform the upgrade over SSH directly to the VM's
IP address, not App Sandbox's localhost SSH forwarding. Confirm this direct
connection works first and keep it available for recovery if the guest agent or
viewer stops working. It remains independent of those components while the VM,
network and SSH server are running.

Verify signatures and architecture, back up the affected files/configuration and
driver packages, and stage everything first. Run installation independently of
the current terminal/SSH session so it can finish if the connection drops.
Preserve a local log and rollback instructions.

For Windows:

- Stop AppSandboxAgent and its helpers. Replace these resources/ files in
  %SystemRoot%\AppSandbox\: appsandbox-agent.exe, appsandbox-input.exe,
  appsandbox-displays.exe, appsandbox-clipboard.exe,
  appsandbox-clipboard-reader.exe and appsandbox-audio.exe.
- Preserve the automatic LocalSystem service and verify its executable path.
- Use the package's bundled drivers\devcon.exe with
  update <full-INF-path> <exact-hardware-ID> to update AppSandboxVDD.inf for
  ROOT\AppSandboxVDD and AppSandboxVAD.inf for ROOT\AppSandboxVAD. On Mac-hosted
  Windows, update AppSandboxSHM on its existing IVSHMEM PCI device first, matching
  the hardware ID against the packaged INF. Verify the new drivers are active.
  Use devcon install only if a required root device is actually missing;
  repeating it creates duplicate devices.
- Leave GPU/vendor driver provisioning to the updated host and agent. Preserve
  unrelated drivers and working networking.

For macOS:

- Replace /Library/AppSandbox/appsandbox-agent and appsandbox-clipboard with the
  packaged helpers (root:wheel, 0755).
- Update their packaged plist copies in /Library/AppSandbox and the active
  /Library/LaunchDaemons/com.appsandbox.agent.plist and
  /Library/LaunchAgents/com.appsandbox.clipboard.plist (root:wheel, 0644).
- Reload the daemon in launchd's system domain and the clipboard agent in the
  logged-in user's GUI domain.

Do not replay first-boot setup, recreate accounts, enable test signing or disable
signature enforcement. Do not install the host application inside the VM.

Restart/reboot cleanly. Verify the running components, viewer, input, clipboard,
audio and applicable resolution/window-matching features. Report what changed,
anything unverified, and the backup/rollback location.
```
