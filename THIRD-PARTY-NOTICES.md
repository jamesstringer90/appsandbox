# Third-Party Notices

AppSandbox is licensed under the MIT License (see [LICENSE](LICENSE)). That MIT
grant covers **only AppSandbox's own source code**. Bundled and downloaded
third-party components retain their own licenses and copyrights; AppSandbox's
MIT License does not replace those terms. Per-file license notices govern the
corresponding source files.

---

## asb-dxgkrnl — Linux guest GPU driver

- **License:** GNU General Public License, version 2 (`SPDX: GPL-2.0`).
  A verbatim copy of the license text is provided at
  [`tools/linux/dxgkrnl/COPYING`](tools/linux/dxgkrnl/COPYING).
- **Copyright:** Copyright (c) Microsoft Corporation.
- **Source:** [jamesstringer90/asb-dxgkrnl](https://github.com/jamesstringer90/asb-dxgkrnl),
  based on Microsoft's WSL2 paravirtualized GPU kernel driver. Linux guest
  provisioning downloads the source version selected by
  [`src/core/linux_deps.h`](src/core/linux_deps.h) and builds it in the guest.
  Source files carry their own GPL-2.0 notices; this driver is not relicensed
  under AppSandbox's MIT License.

## tools/iso-patch/engine/xz/ — XZ Embedded decoder

- **License:** BSD Zero Clause License (`SPDX: 0BSD`).
- **Copyright:** Lasse Collin and the XZ Embedded contributors.
- An embedded XZ/LZMA decompressor. Every file carries its own
  `SPDX-License-Identifier: 0BSD` header. 0BSD is a public-domain-equivalent,
  MIT-compatible permissive license.

## vendor/mesa-d3d12/ — AppSandbox Mesa D3D12 build

- **License:** MIT License (the Mesa project's overall license; some bundled
  components are under other compatible permissive licenses, e.g. the SGI Free
  Software License B). The headline MIT license text is shipped at
  [`vendor/mesa-d3d12/MESA-LICENSE.txt`](vendor/mesa-d3d12/MESA-LICENSE.txt);
  the full per-component listing is at <https://docs.mesa3d.org/license.html>.
- **Copyright:** Copyright (c) the Mesa contributors.
- **Source:** [jamesstringer90/asb-mesa](https://github.com/jamesstringer90/asb-mesa),
  based on Mesa 26.2.6 with AppSandbox D3D12 display integration. The source
  revision and archive checksum are recorded in
  [`vendor/mesa-d3d12/source-version.json`](vendor/mesa-d3d12/source-version.json).
  The prebuilt archive is cached under `vendor/mesa-d3d12/prebuilt/` or downloaded
  from the release selected by [`src/core/linux_deps.h`](src/core/linux_deps.h).
  LLVM is an external guest package dependency, not bundled in that archive.

## tools/vdd/ThirdParty/wdf/ — Microsoft Windows Driver Framework (WDF)

- **License:** Microsoft Software License Terms for the Windows Driver Kit
  (redistributable SDK headers). See
  <https://learn.microsoft.com/en-us/legal/windows-sdk/>.
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/webview2/ — Microsoft Edge WebView2 SDK

- **License:** Microsoft Software License Terms — Microsoft Edge WebView2 SDK
  (redistributable). See
  <https://learn.microsoft.com/en-us/microsoft-edge/webview2/>.
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/devcon/devcon.exe — Microsoft DevCon utility

- **License:** Microsoft Software License Terms for the Windows Driver Kit
  (redistributable tool).
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/qemu-ivshmem/ — QEMU + its runtime dependencies (bundled macOS VMM)

- **License:** QEMU — GNU General Public License, version 2 (`SPDX: GPL-2.0`);
  GLib and gettext/libintl — GNU Lesser General Public License, version 2.1
  (`SPDX: LGPL-2.1`); PCRE2 — BSD ("PCRE2 Licence"); EDK2/TianoCore guest UEFI
  firmware — BSD-2-Clause-Patent (`SPDX: BSD-2-Clause-Patent`).
- **Copyright:** the QEMU, GLib, GNU gettext, PCRE2 (University of Cambridge),
  and TianoCore/EDK2 contributors.
- The macOS host runs Windows guests on a vendored, patched build of **QEMU
  11.1.3** (ivshmem-plain on HVF) together with its GLib / gettext / PCRE2
  runtime and the EDK2 UEFI firmware, embedded under
  `AppSandbox.app/Contents/Resources/qemu/`. QEMU runs as a separate process
  (aggregated with, not linked into, the app); the LGPL dylibs are dynamically
  linked (LGPL §6 relink clause satisfied). Per-component notices, the GPL/LGPL
  written offer, and corresponding-source pointers are in
  [`vendor/qemu-ivshmem/LICENSES/NOTICE.md`](vendor/qemu-ivshmem/LICENSES/NOTICE.md)
  and [`vendor/qemu-ivshmem/WRITTEN_OFFER.txt`](vendor/qemu-ivshmem/WRITTEN_OFFER.txt)
  (both also shipped in the app bundle); `vendor/qemu-ivshmem/fetch-sources.sh`
  retrieves the corresponding source tarballs.

## vendor/virtio-win/ — NetKVM and VirtIO SCSI guest drivers

- **License:** BSD 3-Clause (`SPDX: BSD-3-Clause`). License text:
  [`vendor/virtio-win/virtio-win_license.txt`](vendor/virtio-win/virtio-win_license.txt).
- **Copyright:** Copyright (c) Red Hat, Inc. and the virtio-win contributors.
- `vendor/virtio-win/netkvm-arm64.zip` is the unmodified, WHQL-signed ARM64
  NetKVM virtio-net guest driver, JIT-staged into a Mac-hosted Windows guest for
  NAT networking. Redistributed unmodified; the license text is staged alongside
  the driver in the guest.
- `vendor/virtio-win/vioscsi-arm64-0.1.302.zip` contains the unmodified,
  Microsoft-signed ARM64 VirtIO SCSI guest driver from Fedora's
  `virtio-win-0.1.302-1` release. It is JIT-staged into Mac-hosted Windows guests
  configured for VirtIO SCSI storage. The archive includes the upstream license
  text and a `source.json` provenance record; the license text is staged alongside
  the driver in the guest.

---

## AppSandbox's own Linux kernel module (not third-party)

`tools/linux/asb_drm/` is **AppSandbox's own** code, intentionally dual-licensed
under **MIT OR GPL-2.0** (`SPDX-License-Identifier: (GPL-2.0 OR MIT)`) and
declaring `MODULE_LICENSE("Dual MIT/GPL")`. The dual license lets the module
resolve GPL-only kernel symbols (`EXPORT_SYMBOL_GPL` DRM/KMS helpers) when it is
built into or loaded by the Linux kernel, while keeping the source available
under MIT for everyone else. It is listed here only for clarity — it is covered
by AppSandbox's own copyright, not a third party's.
