#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder — release Arcade + editor smoke harness for terminal test loops.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
PRESET=${PSYNDER_PRESET:-mac-release}
BIN=${PSYNDER_ARCADE_BIN:-"$ROOT_DIR/build/$PRESET/bin/psynder_arcade"}
LOCK=/tmp/psynder_smoke.lockdir
SMOKE_FRAMES=${PSYNDER_SMOKE_FRAMES:-5}
CTEST_FILTER=${PSYNDER_CTEST_FILTER:-"editor|ipc|scene save|scene file|material library|LightComponent|light component|light gather|material light"}

cd "$ROOT_DIR"

if [[ "${PSYNDER_SKIP_BUILD:-0}" != "1" ]]; then
    "$SCRIPT_DIR/build_release.sh"
fi

if [[ ! -x "$BIN" ]]; then
    echo "[smoke-arcade-editor] missing executable: $BIN" >&2
    exit 1
fi

echo "[smoke-arcade-editor] running focused CTest filter: $CTEST_FILTER"
ctest --test-dir "$ROOT_DIR/build/$PRESET" --output-on-failure -R "$CTEST_FILTER" -j 4

if [[ -e "$LOCK" ]]; then
    echo "[smoke-arcade-editor] clearing stale Mac runtime lock: $LOCK"
    rm -rf -- "$LOCK"
fi

while ! mkdir "$LOCK" 2>/dev/null; do
    echo "[smoke-arcade-editor] waiting for Mac runtime lock: $LOCK"
    sleep 0.5
done
trap 'rmdir "$LOCK"' EXIT

echo "[smoke-arcade-editor] running Arcade smoke for $SMOKE_FRAMES frames"
"$BIN" "--smoke-frames=$SMOKE_FRAMES"

echo "[smoke-arcade-editor] ok"
