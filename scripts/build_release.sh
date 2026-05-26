#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder — configure and build the Mac release Arcade player from a terminal.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
PRESET=${PSYNDER_PRESET:-mac-release}
TARGET=${PSYNDER_TARGET:-psynder_arcade}

cd "$ROOT_DIR"

if [[ "${PSYNDER_SKIP_WEB:-0}" != "1" ]]; then
    echo "[build-release] rebuilding web editor bundle"
    npm --prefix engine/editor/web run build
else
    echo "[build-release] skipping web editor bundle (PSYNDER_SKIP_WEB=1)"
fi

echo "[build-release] configuring CMake preset: $PRESET"
cmake --preset "$PRESET"

echo "[build-release] building target: $TARGET"
cmake --build --preset "$PRESET" --target "$TARGET" --parallel

echo "[build-release] built $ROOT_DIR/build/$PRESET/bin/psynder_arcade"
