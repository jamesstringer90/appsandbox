#!/bin/sh
set -eu
suite_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$suite_dir/build"
sh "$suite_dir/build-suite.sh"
xcrun clang -fobjc-arc -fblocks -Wall -Wextra -Werror -mmacosx-version-min=13.0 \
    -framework Cocoa -framework ApplicationServices \
    "$suite_dir/ax-probe.m" -o "$suite_dir/build/ax-probe"
codesign --force --sign - "$suite_dir/build/ax-probe"
app="$suite_dir/build/ASBAXConformance.app"
mkdir -p "$app/Contents/MacOS"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.appsandbox.ax-conformance</string>
<key>CFBundleExecutable</key><string>ASBAXConformance</string>
<key>CFBundleName</key><string>AX Conformance</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
fixture_revision=$("$suite_dir/build/ax-suite" fixture-revision)
/usr/libexec/PlistBuddy -c "Add :ASBAXFixtureRevision string $fixture_revision" "$app/Contents/Info.plist"
printf '{"sha256":"%s"}\n' "$fixture_revision" > "$suite_dir/build/fixture-revision.json"
xcrun clang -fobjc-arc -fblocks -Wno-deprecated-declarations -mmacosx-version-min=13.0 \
    -framework Cocoa -framework Accessibility -framework WebKit "$suite_dir/mac-fixture.m" "$suite_dir/fixture-view.m" "$suite_dir/fixture-pages.m" "$suite_dir/fixture-advanced.m" "$suite_dir/fixture-stress.m" \
    -o "$app/Contents/MacOS/ASBAXConformance"
mkdir -p "$app/Contents/Resources"
cp "$suite_dir/macos-sdk.json" "$app/Contents/Resources/"
cp "$suite_dir/web-fixture.html" "$app/Contents/Resources/"
codesign --force --sign - "$app"
