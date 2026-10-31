#!/bin/sh
set -eu

fixture_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_dir=${1:-"$fixture_dir/build"}
app="$output_dir/ASBNativeControlLab.app"
mkdir -p "$app/Contents/MacOS"
cp "$fixture_dir/Info.plist" "$app/Contents/Info.plist"
xcrun clang -fobjc-arc -mmacosx-version-min=13.0 -framework Cocoa \
    "$fixture_dir/asb-native-control-lab.m" -o "$app/Contents/MacOS/ASBNativeControlLab"
codesign --force --sign - "$app"
printf '%s\n' "$app"
