#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
MESA_SRC="${MESA_SRC:-$HOME/asb-mesa}"
SOURCE_TAG="$(sed -n 's/^[[:space:]]*"tag": "\([^"]*\)".*/\1/p' "$ROOT/vendor/mesa-d3d12/source-version.json")"
[[ -n "$SOURCE_TAG" ]]

if [[ ! -d "$MESA_SRC/.git" ]]; then
    git clone --depth 1 --branch "$SOURCE_TAG" \
        https://github.com/jamesstringer90/asb-mesa.git "$MESA_SRC"
fi
[[ -z "$(git -C "$MESA_SRC" status --porcelain --untracked-files=no)" ]] || {
    echo 'The Mesa source tree has local changes.' >&2
    exit 1
}
git -C "$MESA_SRC" rev-parse --verify "refs/tags/$SOURCE_TAG" >/dev/null 2>&1 || \
    git -C "$MESA_SRC" fetch --depth 1 origin "refs/tags/$SOURCE_TAG:refs/tags/$SOURCE_TAG"
git -C "$MESA_SRC" switch --detach "refs/tags/$SOURCE_TAG"
exec bash "$MESA_SRC/build-appsandbox.sh"
