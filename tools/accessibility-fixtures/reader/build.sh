#!/bin/sh
set -eu

reader_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$reader_dir/build"
xcrun clang -fobjc-arc -Wall -Wextra -Werror -mmacosx-version-min=13.0 \
    -framework Cocoa -framework ApplicationServices \
    "$reader_dir/ax-tree.m" -o "$reader_dir/build/ax-tree"
codesign --force --sign - "$reader_dir/build/ax-tree"
sh "$reader_dir/../conformance/build-suite.sh"
