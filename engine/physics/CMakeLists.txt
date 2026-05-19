# SPDX-License-Identifier: MIT
# psynder_physics — Lane 13. Owns: rigid bodies, SAP, GJK/EPA, island solver,
# vehicle + character. DESIGN.md §10.1 + Wave-A bar.

psynder_add_lane(physics LINKS psynder_core psynder_math psynder_simd psynder_jobs psynder_scene)

# ─── Determinism (DESIGN.md §10.1) ────────────────────────────────────────
# Physics must be bit-deterministic across platforms when the same compile
# flags are used:
#   * No fast-math reassociation (-fno-fast-math) — never touch -ffast-math
#     even via project-wide options.
#   * No finite-math-only assumption (-fno-finite-math-only) — NaN/Inf must
#     propagate per IEEE-754.
#   * Round-to-nearest is the IEEE-754 default; the per-step FpGuard in
#     FpControl.h pins it at the start of every step in case host code or
#     a third-party DLL has flipped it.
# We apply these at the target level so EVERY TU in psynder_physics inherits
# them; lane-13 internal headers are header-only and thus picked up too via
# their includers.
if(MSVC)
    # MSVC: /fp:precise is the IEEE-754-conformant mode (no reassoc, no
    # contraction). /fp:strict is even tighter but disables FMA, which we
    # want for performance.
    target_compile_options(psynder_physics PRIVATE /fp:precise)
else()
    target_compile_options(psynder_physics PRIVATE
        -fno-fast-math
        -fno-finite-math-only
        -fno-associative-math
        -fno-reciprocal-math
        -fno-unsafe-math-optimizations
    )
    # -fsignaling-nans is GCC-only; skip on Apple/LLVM where it's no-op
    # and emits a noisy warning that breaks -Werror builds. Quiet-NaN
    # propagation is sufficient for determinism.
endif()

# Mark the property so the orchestrator's PR-checker can grep for it and
# CI can fail loudly if anyone removes the fast-math gate.
set_target_properties(psynder_physics PROPERTIES
    PSY_DETERMINISTIC_FP "ON"
)
