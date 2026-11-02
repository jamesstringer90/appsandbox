#!/usr/bin/env bash
# Stage the fork-built QEMU + its dylibs + EDK2 firmware into dist/ — the vendored,
# committed, self-contained payload the app embeds. Run after build-qemu.sh.
#
#   dist/bin/qemu-system-aarch64        (install-names -> @loader_path/../lib)
#   dist/lib/*.dylib                    (glib/gettext/pcre2 closure, self-referential)
#   dist/share/qemu/edk2-*.fd.gz        (firmware, gzipped; decompressed at VM-create)
#
# efi-virtio.rom is intentionally dropped (NIC uses romfile=).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU_BIN="$HERE/qemu-build/qemu/bin/qemu-system-aarch64-ivshmem"   # build-qemu.sh output (in-repo)
FW_DIR="$HERE/firmware"                                   # vendored gzipped EDK2 firmware (in-repo)
DIST="$HERE/dist"
REVISION="$(plutil -extract revision raw -o - "$HERE/source-version.json")"
BRANCH="$(plutil -extract branch raw -o - "$HERE/source-version.json")"
UPSTREAM="$(plutil -extract upstreamTag raw -o - "$HERE/source-version.json")"
BUILT_REVISION="$(cat "$HERE/qemu-build/qemu/SOURCE_REVISION" 2>/dev/null || true)"
[ "$BUILT_REVISION" = "$REVISION" ] || { echo "ERROR: rebuild QEMU at the pinned source revision before staging" >&2; exit 1; }

# Refuse to silently bundle different dependencies than the corresponding source/notice pins.
for dependency in glib gettext pcre2; do
  expected="$(plutil -extract "dependencies.$dependency" raw -o - "$HERE/source-version.json")"
  actual="$(python3 -c 'import os,sys; print(os.path.basename(os.path.realpath(sys.argv[1])))' "/opt/homebrew/opt/$dependency")"
  [ "$actual" = "$expected" ] || { echo "ERROR: $dependency version $actual differs from pinned $expected" >&2; exit 1; }
done

[ -x "$QEMU_BIN" ] || { echo "ERROR: build the fork qemu first (./build-qemu.sh)"; exit 1; }
for f in edk2-aarch64-code.fd.gz edk2-arm-vars.fd.gz; do
  [ -f "$FW_DIR/$f" ] || { echo "ERROR: missing vendored firmware $FW_DIR/$f"; exit 1; }
done

rm -rf "$DIST"; mkdir -p "$DIST/bin" "$DIST/lib" "$DIST/share/qemu"
cp "$QEMU_BIN" "$DIST/bin/qemu-system-aarch64"; chmod u+w "$DIST/bin/qemu-system-aarch64"

realpath_py() { python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$1"; }

# Recursively copy every non-system (Homebrew/local) dylib into dist/lib.
collect() {
  local obj="$1" dep base real
  while read -r dep; do
    case "$dep" in
      /opt/homebrew/*|/usr/local/*)
        base="$(basename "$dep")"
        if [ ! -f "$DIST/lib/$base" ]; then
          real="$(realpath_py "$dep")"
          cp "$real" "$DIST/lib/$base"; chmod u+w "$DIST/lib/$base"
          collect "$DIST/lib/$base"
        fi ;;
    esac
  done < <(otool -L "$obj" | tail -n +2 | awk '{print $1}')
}
collect "$DIST/bin/qemu-system-aarch64"

# Rewrite the binary's deps -> @loader_path/../lib/<base>
while read -r dep; do
  case "$dep" in /opt/homebrew/*|/usr/local/*)
    install_name_tool -change "$dep" "@loader_path/../lib/$(basename "$dep")" "$DIST/bin/qemu-system-aarch64" ;;
  esac
done < <(otool -L "$DIST/bin/qemu-system-aarch64" | tail -n +2 | awk '{print $1}')

# Each dylib: id -> @loader_path/<base>; deps -> @loader_path/<base>
for lib in "$DIST"/lib/*.dylib; do
  base="$(basename "$lib")"
  install_name_tool -id "@loader_path/$base" "$lib"
  while read -r dep; do
    case "$dep" in /opt/homebrew/*|/usr/local/*)
      install_name_tool -change "$dep" "@loader_path/$(basename "$dep")" "$lib" ;;
    esac
  done < <(otool -L "$lib" | tail -n +2 | awk '{print $1}')
done

# install_name_tool invalidates code signatures -> re-sign ad-hoc.
# The qemu binary MUST carry com.apple.security.hypervisor or HVF (hv_vm_create) is denied.
codesign --force --sign - "$DIST"/lib/*.dylib
codesign --force --sign - --entitlements "$HERE/qemu.entitlements" "$DIST/bin/qemu-system-aarch64"

# Firmware: copy the in-repo gzipped EDK2 blobs into dist (decompressed at VM-create by the app).
cp "$FW_DIR/edk2-aarch64-code.fd.gz" "$DIST/share/qemu/edk2-aarch64-code.fd.gz"
cp "$FW_DIR/edk2-arm-vars.fd.gz"     "$DIST/share/qemu/edk2-arm-vars.fd.gz"

# Record exact shipped versions for license/source traceability (../source-mirror + ../LICENSES).
GLIB_V="$(plutil -extract dependencies.glib raw -o - "$HERE/source-version.json")"
GETTEXT_V="$(plutil -extract dependencies.gettext raw -o - "$HERE/source-version.json")"
PCRE2_V="$(plutil -extract dependencies.pcre2 raw -o - "$HERE/source-version.json")"
QEMU_V="$("$DIST/bin/qemu-system-aarch64" --version 2>/dev/null | head -1)"
cat > "$DIST/VERSIONS.txt" <<EOF
AppSandbox bundled QEMU + dependencies — exact versions shipped in this dist/.
Corresponding source: ../source-mirror/ (run fetch-sources.sh) and the asb-qemu fork.
Licenses + notices: ../LICENSES/ ; written offer: ../WRITTEN_OFFER.txt

$QEMU_V
  GPL-2.0   source: https://github.com/jamesstringer90/asb-qemu/tree/$REVISION
  branch: $BRANCH; upstream: $UPSTREAM; four ivshmem changes retained
glib     $GLIB_V   LGPL-2.1   source: glib-2.90.1.tar.xz + glib-2.90.1-homebrew-hardcoded-paths.diff
gettext  $GETTEXT_V   LGPL-2.1   source: gettext-1.0.tar.gz  (libintl runtime only; not the GPL tools)
pcre2    $PCRE2_V  BSD        notice only (LICENSES/pcre2-LICENCE.txt)
edk2     AAVMF  BSD-2-Clause-Patent   notice only (LICENSES/EDK2-licenses.txt)

The glib/gettext/pcre2 dylibs are Homebrew-built precompiled binaries, dynamically
linked into qemu; per-keg build recipes are mirrored (e.g. glib.homebrew-formula.rb).
EOF
echo "  wrote VERSIONS.txt"

echo "== dist staged ($(du -sh "$DIST" | awk '{print $1}')) =="
echo "-- bin non-system deps (want @loader_path only) --"
otool -L "$DIST/bin/qemu-system-aarch64" | grep -vE "/System/|/usr/lib/" | grep -E "@loader_path|/opt/|/usr/local/" || true
echo "-- run check (resolves bundled dylibs) --"
"$DIST/bin/qemu-system-aarch64" --version | head -1
echo "-- contents --"; find "$DIST" -type f | sed "s#$DIST/##" | sort
