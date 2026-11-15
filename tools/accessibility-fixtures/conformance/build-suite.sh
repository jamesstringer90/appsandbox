#!/bin/sh
set -eu
suite_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$suite_dir/build"
xcrun clang -fobjc-arc -fblocks -Wall -Wextra -Werror -mmacosx-version-min=13.0 \
    -framework Foundation \
    "$suite_dir/suite-main.m" "$suite_dir/suite-common.m" "$suite_dir/suite-runner.m" \
    "$suite_dir/suite-catalog.m" "$suite_dir/suite-coverage.m" "$suite_dir/suite-tools.m" \
    "$suite_dir/../reader/tree-compare.m" -o "$suite_dir/build/ax-suite"
codesign --force --sign - "$suite_dir/build/ax-suite"
