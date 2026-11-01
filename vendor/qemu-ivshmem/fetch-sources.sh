#!/usr/bin/env bash
# Fetch the LGPL "corresponding source" for the precompiled dylibs we ship in dist/.
# We ship Homebrew's glib binaries, so the corresponding source is the EXACT upstream tarball that
# built them PLUS the patch Homebrew applies (hardcoded-paths.diff) -- that combination is what the
# LGPL requires us to make available. pcre2 is BSD (notice only; source not required, not fetched).
# sha256-verified so the mirrored source provably matches what produced dist/.
#
# Run once per version bump; commit source-mirror/ (these are small). Versions come from the kegs;
# update them here when stage.sh's dist/VERSIONS.txt shows a newer glib.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/source-mirror"
mkdir -p "$OUT"

verify() { echo "$1  $OUT/$2" | shasum -a 256 -c - >/dev/null || { echo "SHA256 MISMATCH: $2"; exit 1; }; }
get() { # url sha256 dest
  [ -f "$OUT/$3" ] || curl -fL --retry 3 -o "$OUT/$3" "$1"
  verify "$2" "$3"; echo "  ok: $3"
}

echo "==> glib 2.90.1 (LGPL v2.1) — REQUIRED corresponding source"
get "https://download.gnome.org/sources/glib/2.90/glib-2.90.1.tar.xz" \
    "93c941aa17d5eb1d53fe838365f29a8b4e539c222a256d974ec8f30fc413e396" \
    "glib-2.90.1.tar.xz"
get "https://raw.githubusercontent.com/Homebrew/homebrew-core/e1a11dc93c6ce0140c983b37cea012199ca82ffc/Patches/glib/hardcoded-paths.diff" \
    "d846efd0bf62918350da94f850db33b0f8727fece9bfaf8164566e3094e80c97" \
    "glib-2.90.1-homebrew-hardcoded-paths.diff"

echo "==> gettext 1.0 (LGPL v2.1) — we ship libintl.8.dylib, so its source is REQUIRED too"
get "https://ftpmirror.gnu.org/gnu/gettext/gettext-1.0.tar.gz" \
    "85d99b79c981a404874c02e0342176cf75c7698e2b51fe41031cf6526d974f1a" \
    "gettext-1.0.tar.gz"

# Keep the exact Homebrew formula that built our glib (the build recipe) for full traceability.
KEG="$(ls /opt/homebrew/Cellar/glib/*/.brew/glib.rb 2>/dev/null | head -1)"
[ -n "$KEG" ] && cp "$KEG" "$OUT/glib.homebrew-formula.rb" && echo "  ok: glib.homebrew-formula.rb"

echo "==> done. glib corresponding source (tarball + Homebrew patch) mirrored + verified in:"
echo "    $OUT"
