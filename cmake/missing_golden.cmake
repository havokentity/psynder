# SPDX-License-Identifier: MIT
# Psynder — missing-golden ctest driver. Adapted from dmonte.
#
# Fires when a host has no committed reference PNG for the cell. Prints
# a regen hint and exits non-zero so the cell's WILL_FAIL TRUE property
# inverts to PASS. Maintainers see the hint in CI logs without a broken
# build.
#
# Variables (passed via -D...):
#   GOLDEN  -- absolute path of the missing baseline
#   CELL    -- ctest cell name (printed in the hint)
#   ACTUAL  -- absolute path of the just-rendered output (the copy source)

if(NOT DEFINED GOLDEN OR NOT DEFINED CELL OR NOT DEFINED ACTUAL)
    message(FATAL_ERROR
        "missing_golden.cmake: requires -DGOLDEN=... -DCELL=... -DACTUAL=...")
endif()

get_filename_component(_golden_dir "${GOLDEN}" DIRECTORY)

message("")
message("  ── golden missing for cell '${CELL}' ─────────────────────────")
message("  expected: ${GOLDEN}")
message("  actual:   ${ACTUAL}")
message("")
message("  regenerate via:")
message("    cmake -E make_directory \"${_golden_dir}\"")
message("    cmake -E copy \"${ACTUAL}\" \"${GOLDEN}\"")
message("  ─────────────────────────────────────────────────────────────")
message("")

# Exit non-zero so the ctest WILL_FAIL TRUE on the diff test inverts to
# PASS, surfacing the missing state to the maintainer without breaking
# CI as a whole.
message(FATAL_ERROR "golden missing")
