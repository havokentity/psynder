#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Psynder — smoke-run a sample binary under the shared Mac smoke lock.
# Acquires /tmp/psynder_smoke.lockdir atomically (mkdir is POSIX-atomic),
# then runs the sample binary. Releases the lock on shell exit via trap.

set -euo pipefail

LOCK=/tmp/psynder_smoke.lockdir
SAMPLE=${1:-build/mac-release/bin/sample_00_clear}
ARGS=("${@:2}")

while ! mkdir "$LOCK" 2>/dev/null; do
    sleep 0.5
done
trap 'rmdir "$LOCK"' EXIT

echo "[smoke] $(date -Iseconds) running $SAMPLE ${ARGS[*]:-}"
"$SAMPLE" "${ARGS[@]}"
echo "[smoke] $(date -Iseconds) done"
