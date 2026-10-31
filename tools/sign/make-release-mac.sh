#!/bin/bash
#
# make-release-mac.sh -- Build, sign, notarize, staple, and zip the macOS app.
#
# Mirrors the Windows tools/sign/make-release.ps1 flow. Reads tunable settings
# from mac-release-config.json (committed, no secrets) and private notarization
# details from mac-signing.local.json (git-ignored; matches tools/sign/*.local.*).
#
# Build-time signing identity/team come from src/app_mac/xcconfig/LocalUser.xcconfig
# (also git-ignored). See tools/sign/SIGNING_MAC.md for the full setup.
#
# Usage:
#   tools/sign/make-release-mac.sh [--no-notarize]
#
#   --no-notarize   Build, sign, verify, and zip only (skip Apple notarization).
#                   Useful for a quick local Developer ID build.
#
set -euo pipefail

# --- locate repo root (script lives in tools/sign/) ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CONFIG="$SCRIPT_DIR/mac-release-config.json"
LOCAL="$SCRIPT_DIR/mac-signing.local.json"

NOTARIZE=1
[[ "${1:-}" == "--no-notarize" ]] && NOTARIZE=0

# --- helpers ---
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[36m==> %s\033[0m\n' "$*"; }
# read a key from a json file via plutil (built into macOS); empty string if absent
jget() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || true; }

[[ -f "$CONFIG" ]] || die "missing $CONFIG"
if [[ ! -f "$LOCAL" ]]; then
    cat >&2 <<EOF
error: missing $LOCAL

Create it (git-ignored) with your notarization details, e.g.:

  {
    "notaryKeychainProfile": "appsandbox-notary",
    "appleId": "you@example.com",
    "teamId": "YOURTEAMID",
    "signingIdentity": "Developer ID Application: Your Org (YOURTEAMID)"
  }

The keychain profile is created once with:
  xcrun notarytool store-credentials appsandbox-notary \\
      --apple-id you@example.com --team-id YOURTEAMID --password <app-specific-password>

Alternatively omit notaryKeychainProfile and provide appleId + teamId +
appSpecificPassword directly in this file. See tools/sign/SIGNING_MAC.md.
EOF
    exit 1
fi

PROJECT="$(jget "$CONFIG" project)"
SCHEME="$(jget "$CONFIG" scheme)"
CONFIGURATION="$(jget "$CONFIG" configuration)"
APP_PATH="$(jget "$CONFIG" appPath)"
PACKAGE_DIR="$(jget "$CONFIG" packageDir)"
PRODUCT_NAME="$(jget "$CONFIG" productName)"
OS_TAG="$(jget "$CONFIG" os)"
PLATFORM="$(jget "$CONFIG" platform)"
EXPECTED_IDENTITY="$(jget "$CONFIG" expectedIdentity)"

PROFILE="$(jget "$LOCAL" notaryKeychainProfile)"
APPLE_ID="$(jget "$LOCAL" appleId)"
TEAM_ID="$(jget "$LOCAL" teamId)"
APP_PW="$(jget "$LOCAL" appSpecificPassword)"

[[ -n "$PROJECT" && -n "$SCHEME" && -n "$CONFIGURATION" && -n "$APP_PATH" \
   && -n "$PACKAGE_DIR" && -n "$PRODUCT_NAME" && -n "$OS_TAG" && -n "$PLATFORM" ]] \
    || die "mac-release-config.json missing required keys"

# Working zip used only for notarization submission; never shipped.
SUBMIT_ZIP="$(dirname "$APP_PATH")/.notarize-submit.zip"
# PKG_ZIP is set after the version sync (it embeds the version in the name).
PKG_ZIP=""

# Publish ONLY the final zip into a clean package dir — nothing else (no dSYMs,
# no loose products, no build scratch) ever lands there. This is a production
# release; the folder must contain only what the public should see.
publish_package() {
    rm -rf "$PACKAGE_DIR"
    mkdir -p "$PACKAGE_DIR"
    # Zip the (already signed/notarized/stapled) .app alongside the headless-api SDK
    # so BOTH sit at the zip root -- AppSandbox.app + headless-api/ -- with no tools/
    # parent. ditto copies the bundle verbatim, preserving its signature and staple.
    local stage; stage="$(mktemp -d)"
    ditto "$APP_PATH" "$stage/$(basename "$APP_PATH")"
    local sdk="$ROOT/tools/headless-api"
    local dst="$stage/headless-api"
    mkdir -p "$dst"
    cp "$sdk/asb.py" "$sdk/README.md" "$dst/"
    ditto "$sdk/examples" "$dst/examples"
    ditto "$sdk/tests"    "$dst/tests"
    # __pycache__/*.pyc are local build cruft, never shipped.
    find "$dst" -type d -name __pycache__ -prune -exec rm -rf {} +
    find "$dst" -type f -name '*.pyc' -delete
    ditto -c -k "$stage" "$PKG_ZIP"
    rm -rf "$stage"
    rm -f "$SUBMIT_ZIP"
}

# --- 1. build ---
# The build itself stamps the version into the bundle (the "Stamp version" build
# phase reads Directory.Build.props). Here we only need the version string to
# compose the public zip name, matching the Windows convention:
#   <productName>-<version>-<os>-<platform>.zip   e.g. AppSandbox-0.1.1-mac-arm64.zip
VERSION="$("$SCRIPT_DIR/asb-version.sh" short)"
[[ -n "$VERSION" ]] || die "could not read version from Directory.Build.props"
PKG_ZIP="$PACKAGE_DIR/${PRODUCT_NAME}-${VERSION}-${OS_TAG}-${PLATFORM}.zip"

step "Building $SCHEME ($CONFIGURATION)"
xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIGURATION" clean >/dev/null
xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIGURATION" build >/dev/null
[[ -d "$APP_PATH" ]] || die "build produced no app at $APP_PATH"

# --- 1b. sign the embedded QEMU (Resources/ Mach-O that Xcode does not sign) ---
# qemu + its dylibs live under Contents/Resources/qemu — not an Xcode code location, so the build
# left them ad-hoc. Sign them with Developer ID + hardened runtime + a secure timestamp (all
# required for notarization), then re-seal the .app so its resource envelope covers the re-signed
# qemu. This runs here (not in the Xcode build phase) because the build script-phase sandbox can't
# reach the signing keychain. The qemu binary keeps its HVF entitlement; the dylibs share the team.
step "Signing embedded QEMU + re-sealing the app"
SIGNID="$(jget "$LOCAL" signingIdentity)"
[[ -n "$SIGNID" ]] || SIGNID="$EXPECTED_IDENTITY"
[[ -n "$SIGNID" ]] || die "no signingIdentity in $LOCAL (or expectedIdentity in config)"
QDIR="$APP_PATH/Contents/Resources/qemu"
if [[ -d "$QDIR" ]]; then
    for d in "$QDIR"/lib/*.dylib; do
        [[ -e "$d" ]] && codesign --force --options runtime --timestamp --sign "$SIGNID" "$d"
    done
    codesign --force --options runtime --timestamp \
        --entitlements "$ROOT/vendor/qemu-ivshmem/qemu.entitlements" \
        --sign "$SIGNID" "$QDIR/bin/qemu-system-aarch64"
    # re-seal the app bundle so CodeResources covers the re-signed qemu (preserve the app's own
    # entitlements/identifier/flags; nested frameworks keep their existing Developer ID signatures).
    codesign --force --options runtime --timestamp \
        --preserve-metadata=identifier,entitlements,flags \
        --sign "$SIGNID" "$APP_PATH"
    echo "    signed qemu + $(ls "$QDIR"/lib/*.dylib 2>/dev/null | wc -l | tr -d ' ') dylibs, re-sealed app"
fi

# --- 2. verify signature ---
step "Verifying signature"
codesign --verify --deep --strict --verbose=1 "$APP_PATH"
ACTUAL_AUTH="$(codesign -dvvv "$APP_PATH" 2>&1 | grep '^Authority=' | head -1 | sed 's/^Authority=//')"
echo "    identity: $ACTUAL_AUTH"
if [[ -n "$EXPECTED_IDENTITY" && "$ACTUAL_AUTH" != "$EXPECTED_IDENTITY"* ]]; then
    die "signing identity '$ACTUAL_AUTH' does not match expected '$EXPECTED_IDENTITY' (check LocalUser.xcconfig)"
fi

if [[ "$NOTARIZE" -eq 0 ]]; then
    # Local signed build only — publish the signed (not notarized) zip.
    step "Packaging signed (NOT notarized) app to $PKG_ZIP"
    publish_package
    step "Done (--no-notarize): signed app at $APP_PATH, package at $PKG_ZIP"
    exit 0
fi

# --- 3. zip for notarization submission (working copy, not shipped) ---
step "Zipping for submission"
rm -f "$SUBMIT_ZIP"
ditto -c -k --keepParent "$APP_PATH" "$SUBMIT_ZIP"

# --- 4. notarize (recursive — covers the framework + iso-patch-mac +
#         appsandbox-agent + appsandbox-clipboard nested in the .app) ---
step "Submitting to Apple notary service"
if [[ -n "$PROFILE" ]]; then
    xcrun notarytool submit "$SUBMIT_ZIP" --keychain-profile "$PROFILE" --wait
elif [[ -n "$APPLE_ID" && -n "$TEAM_ID" && -n "$APP_PW" ]]; then
    xcrun notarytool submit "$SUBMIT_ZIP" --apple-id "$APPLE_ID" --team-id "$TEAM_ID" --password "$APP_PW" --wait
else
    rm -f "$SUBMIT_ZIP"
    die "no notarization credentials: set notaryKeychainProfile, or appleId+teamId+appSpecificPassword in $LOCAL"
fi

# --- 5. staple + verify ---
step "Stapling ticket"
xcrun stapler staple "$APP_PATH"

step "Gatekeeper assessment"
spctl -a -t exec -vvv "$APP_PATH"

# --- 6. publish ONLY the final stapled zip into a clean package folder ---
step "Packaging release to $PKG_ZIP"
publish_package

step "Release complete: notarized + stapled. Public package: $PKG_ZIP"
