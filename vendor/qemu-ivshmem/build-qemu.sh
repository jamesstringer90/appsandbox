#!/usr/bin/env bash
# Build our patched QEMU (ivshmem-plain on macOS/HVF) from the AppSandbox fork.
#
# Source of truth: source-version.json pins the public fork to an exact commit.
# The fork carries the ivshmem changes as commits; no local patches are applied.
# This fork IS our GPL "corresponding source": it builds the exact binary we ship.
#
# Output:     vendor/qemu-ivshmem/qemu-build/qemu/bin/qemu-system-aarch64-ivshmem
# Clone+tree: vendor/qemu-ivshmem/qemu-build/qemu-src (git-ignored; reproducible).
# Everything stays INSIDE appsandbox — nothing is written or read outside the repo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"          # appsandbox/vendor/qemu-ivshmem
BUILD="$HERE/qemu-build"                        # clone + ninja build tree + output binary
SRC="$BUILD/qemu-src"
OUT="$BUILD/qemu"
FORK="$(plutil -extract repository raw -o - "$HERE/source-version.json")"
REVISION="$(plutil -extract revision raw -o - "$HERE/source-version.json")"
JOBS="${ASB_QEMU_BUILD_JOBS:-2}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "ASB_QEMU_BUILD_JOBS must be a positive integer" >&2; exit 1; }
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

# Build-time tools only (NOT linked into the shipped binary). glib is the one real
# runtime dep; pixman/slirp/etc. are configured out below.
for tool in meson ninja pkg-config; do
  command -v "$tool" >/dev/null || { echo "Missing $tool; install meson, ninja, pkg-config and glib before building." >&2; exit 1; }
done
pkg-config --exists glib-2.0 || { echo "Missing glib development files" >&2; exit 1; }

echo "==> source revision $REVISION"
mkdir -p "$SRC"
if [ ! -d "$SRC/asb-qemu/.git" ]; then
  git init "$SRC/asb-qemu"
  git -C "$SRC/asb-qemu" remote add origin "$FORK"
fi
cd "$SRC/asb-qemu"
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "Refusing to overwrite changes in the QEMU source tree" >&2
  exit 1
fi
git cat-file -e "$REVISION^{commit}" 2>/dev/null || git fetch --depth 1 "$FORK" "$REVISION"
if [ "$(git rev-parse HEAD 2>/dev/null || true)" != "$REVISION" ]; then
  git switch --detach "$REVISION"
fi

# Minimal configure: raw hypervisor + vmnet NAT + ivshmem only. No GUI/display
# (the VDD is our display over ivshmem), no VNC/SPICE, no TLS/crypto, no host-USB
# passthrough, no qcow2/compression, no slirp (we use vmnet). This drops every
# Homebrew dylib except glib -- see docs. pixman is disabled (no graphics device).
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/glib/lib/pkgconfig"
export MACOSX_DEPLOYMENT_TARGET=26.0
mkdir -p build; cd build
RECIPE="$(shasum -a 256 "$HERE/build-qemu.sh" | cut -d ' ' -f 1)"
if [ ! -f build.ninja ] || [ "$(cat .asb-build-recipe 2>/dev/null || true)" != "$RECIPE" ]; then
../configure \
  --target-list=aarch64-softmmu \
  --extra-cflags="-ffile-prefix-map=$SRC/asb-qemu=qemu" \
  --extra-objcflags="-ffile-prefix-map=$SRC/asb-qemu=qemu" \
  --enable-hvf --enable-vmnet \
  --disable-pixman --disable-slirp --disable-vnc --disable-gtk --disable-sdl \
  --disable-cocoa --disable-curses --disable-spice --disable-opengl --disable-virglrenderer \
  --disable-gnutls --disable-nettle --disable-gcrypt --disable-capstone \
  --disable-libssh --disable-libusb --disable-curl \
  --disable-zstd --disable-bzip2 --disable-lzo --disable-snappy --disable-png \
  --disable-vde --disable-tpm --disable-brlapi --disable-jack --disable-oss \
  --disable-pa --disable-sndio --disable-coreaudio --disable-docs --disable-tools
printf '%s\n' "$RECIPE" > .asb-build-recipe
fi
ninja -j "$JOBS" qemu-system-aarch64
mkdir -p "$OUT/bin"
# Replace the executable with a fresh inode; overwriting a previously executed
# Mach-O in place can leave macOS using cached validation for the old contents.
BUILT_BINARY="$(mktemp "$OUT/bin/qemu-build.XXXXXX")"
cp qemu-system-aarch64 "$BUILT_BINARY"
chmod 755 "$BUILT_BINARY"
mv -f "$BUILT_BINARY" "$OUT/bin/qemu-system-aarch64-ivshmem"
printf '%s\n' "$REVISION" > "$OUT/SOURCE_REVISION"
echo "==> built: $OUT/bin/qemu-system-aarch64-ivshmem"
"$OUT/bin/qemu-system-aarch64-ivshmem" -device ivshmem-plain,help > "$OUT/ivshmem-properties.txt"
grep -q 'x-device-id' "$OUT/ivshmem-properties.txt" || { echo "Missing ivshmem x-device-id property" >&2; exit 1; }
echo "==> ivshmem-plain and x-device-id present"
echo "==> non-system deps (should be glib only):"
otool -L "$OUT/bin/qemu-system-aarch64-ivshmem" | grep -vE "/System/|/usr/lib/" | grep -E "/opt/|/usr/local/" || true
