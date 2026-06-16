#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder — launch the release Arcade player from a terminal.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd -- "$SCRIPT_DIR/.." && pwd)
PRESET=${PSYNDER_PRESET:-mac-release}
BIN=${PSYNDER_ARCADE_BIN:-"$ROOT_DIR/build/$PRESET/bin/psynder_arcade"}
LOCK=/tmp/psynder_smoke.lockdir

if [[ ! -x "$BIN" ]]; then
    echo "[start-arcade] missing executable: $BIN" >&2
    echo "[start-arcade] run ./scripts/build_release.sh first" >&2
    exit 1
fi

if [[ -e "$LOCK" ]]; then
    echo "[start-arcade] clearing stale Mac runtime lock: $LOCK"
    rm -rf -- "$LOCK"
fi

while ! mkdir "$LOCK" 2>/dev/null; do
    echo "[start-arcade] waiting for Mac runtime lock: $LOCK"
    sleep 0.5
done
trap 'rmdir "$LOCK"' EXIT

cd "$ROOT_DIR"
echo "[start-arcade] running $BIN $*"
exec "$BIN" "$@"
