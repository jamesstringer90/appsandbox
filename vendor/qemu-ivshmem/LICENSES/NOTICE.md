# Third-party software notices — bundled QEMU + dependencies

AppSandbox bundles the following third-party components under
`AppSandbox.app/Contents/Resources/qemu/`. Full license texts are in this folder.
Exact versions are recorded in `../dist/VERSIONS.txt`.

## QEMU — GPL-2.0  (`GPL-2.0.txt`)
The hardware/firmware virtualizer. We ship a patched build (ivshmem-plain on
macOS/HVF). **Corresponding source:** our fork
`https://github.com/jamesstringer90/asb-qemu`, branch `asb-ivshmem-11.1.3`
(upstream tag `v11.1.3` + four ivshmem changes). The exact source revision is recorded
in the bundled `VERSIONS.txt`. QEMU runs as a separate process on Mac hosts,
so it is aggregated with — not linked into — the AppSandbox application.

## GLib — libglib / libgobject / libgio / libgmodule — LGPL-2.1  (`LGPL-2.1.txt`)
QEMU's core dependency. We ship Homebrew's precompiled GLib **2.90.1** binaries,
**dynamically linked**. **Corresponding source:** the upstream tarball plus the one
patch Homebrew applies, both in `../source-mirror/`
(`glib-2.90.1.tar.xz` + `glib-2.90.1-homebrew-hardcoded-paths.diff`; the exact
build recipe is `glib.homebrew-formula.rb`). Because GLib is dynamically linked,
you may replace the `.dylib`s with a modified GLib (LGPL §6 satisfied). Also
offered per `../WRITTEN_OFFER.txt`.

## gettext (libintl) — `libintl.8.dylib` — LGPL-2.1  (`LGPL-2.1.txt`)
GLib's i18n runtime. We ship Homebrew's precompiled **gettext 1.0** `libintl.8.dylib`,
dynamically linked. We bundle only the LGPL **runtime** library (`libintl`), NOT the
GPL gettext tools (msgfmt etc.). **Corresponding source:** `../source-mirror/gettext-1.0.tar.gz`
(vanilla upstream, no Homebrew patch). Dynamically linked, so replaceable. Offered per
`../WRITTEN_OFFER.txt`.

## PCRE2 — BSD ("PCRE2 Licence")  (`pcre2-LICENCE.txt`)
A GLib dependency. Copyright (c) 1997-2024 University of Cambridge. Permissive;
notice only, no source obligation.

## EDK2 / TianoCore firmware — BSD-2-Clause-Patent  (`EDK2-licenses.txt`)
The guest UEFI firmware (`edk2-*.fd`). Permissive; notice only.

---
None of these licenses are overridden by the AppSandbox EULA. All bundled dylibs in
`dist/lib` are dynamically linked (LGPL §6 relink clause satisfied), and the exact
versions shipped are recorded in `../dist/VERSIONS.txt`.
