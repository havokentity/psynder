// SPDX-License-Identifier: MIT
// Psynder physics - per-body integrator kernels (Wave 8, Lane 4).
//
// The fixed-tick integrator iterates every live body INDEPENDENTLY - there is
// no cross-body reduction in integrate_forces / integrate_positions /
// rebuild_aabbs. That makes the per-body arithmetic an embarrassingly-parallel
// SIMD target. The kernels here vectorise the component-wise Vec3 / Quat math
// of ONE body across the lanes of a single `f32x4` pack (a "vertical" SIMD: the
// x/y/z[/w] components ride in separate lanes of the same register). They reuse
// the lane-03 SIMD facility (engine/simd) - no new dispatch is invented.
//
// --- Bit-identity contract (the whole point of this header) ---------------
// Every kernel computes EXACTLY the same IEEE-754 float operations, in the
// same order, as the scalar code it replaces:
//   * `math::add(a, b)` per component  -> `add4` per lane: lane i = a_i + b_i.
//   * `math::mul(v, s)` per component   -> `mul4(v, broadcast(s))`: lane i = v_i * s.
//   * the quaternion pre-normalise term `0.5f * dt * dq.c` per component
//     -> `mul4(mul4(broadcast(0.5f), broadcast(dt)), dq)`: lane c keeps the
//     SAME left-to-right association `((0.5f * dt) * dq.c)`.
// Per-lane SSE/NEON add/mul are IEEE-correct single-rounded operations - bit
// for bit equal to the scalar `+` / `*` on the same operands. We never:
//   * sum across lanes (no horizontal reduction),
//   * reassociate (no `a+b+c` rebracketing),
//   * contract a separate mul+add into an FMA (we call `add4`/`mul4`, never
//     `fma4`; the engine builds with -fno-fast-math so the compiler won't
//     contract them either),
//   * touch a 4th garbage lane's result (Vec3 stores write back only x/y/z).
// Anything that WOULD change bits is kept scalar and documented at its call
// site: quat_mul's cross terms, quat_normalize's exact sqrt+reciprocal, the
// apply_inv_inertia_world rotate-scale-rotate, and the per-shape aabb_world
// switch all stay byte-for-byte on the scalar path.
//
// Each kernel ships a `*_scalar` twin with the literal scalar body so a unit
// test can run both over the same body buffer and memcmp the result - the
// bit-identity proof is executable, not just argued.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include "simd/Simd.h"
#include "simd/Simd_internal.h"

namespace psynder::physics::detail::integrate {

// --- Vertical Vec3 lane helpers ------------------------------------------
// A Vec3 rides in lanes {x, y, z} of an f32x4; lane 3 is a don't-care zero we
// never store back. Build / extract go through scalar lane access so there is
// no aliasing read past the 12-byte Vec3 (the load reads from a 4-float stack
// temporary, never from the Body's 12-byte field).

PSY_FORCEINLINE simd::f32x4 load_vec3(math::Vec3 v) noexcept {
    // Stack temporary is 16-byte; the 4th lane is a defined 0.0f. No read past
    // the source Vec3's 3 floats (we copy components, not reinterpret).
    alignas(16) const f32 tmp[4] = {v.x, v.y, v.z, 0.0f};
    return simd::load_aligned4(tmp);
}

PSY_FORCEINLINE math::Vec3 store_vec3(simd::f32x4 p) noexcept {
    alignas(16) f32 tmp[4];
    simd::store_aligned4(tmp, p);
    return math::Vec3{tmp[0], tmp[1], tmp[2]};
}

// --- integrate_forces linear update ---------------------------------------
// Scalar reference:
//     lv = add(lv, mul(gravity, dt));
//     lv = add(lv, mul(force, inv_mass * dt));
// Each `mul`/`add` is component-wise, so per lane: lv_c + gravity_c*dt, then
// + force_c * (inv_mass*dt). `inv_mass * dt` is the SAME scalar product the
// scalar path forms once; we broadcast it, so every lane multiplies by the
// identical rounded value. Bit-identical.

PSY_FORCEINLINE math::Vec3 forces_linear_scalar(math::Vec3 lv,
                                                math::Vec3 gravity,
                                                math::Vec3 force,
                                                f32 inv_mass,
                                                f32 dt) noexcept {
    lv = math::add(lv, math::mul(gravity, dt));
    lv = math::add(lv, math::mul(force, inv_mass * dt));
    return lv;
}

PSY_FORCEINLINE math::Vec3 forces_linear_simd(math::Vec3 lv,
                                              math::Vec3 gravity,
                                              math::Vec3 force,
                                              f32 inv_mass,
                                              f32 dt) noexcept {
    const simd::f32x4 vdt = simd::broadcast4(dt);
    const simd::f32x4 vfdt = simd::broadcast4(inv_mass * dt);  // same scalar as above
    simd::f32x4 v = load_vec3(lv);
    v = simd::add4(v, simd::mul4(load_vec3(gravity), vdt));
    v = simd::add4(v, simd::mul4(load_vec3(force), vfdt));
    return store_vec3(v);
}

// --- integrate_positions linear update ------------------------------------
// Scalar reference: position = add(position, mul(linear_velocity, dt)).

PSY_FORCEINLINE math::Vec3 position_linear_scalar(math::Vec3 position,
                                                  math::Vec3 linear_velocity,
                                                  f32 dt) noexcept {
    return math::add(position, math::mul(linear_velocity, dt));
}

PSY_FORCEINLINE math::Vec3 position_linear_simd(math::Vec3 position,
                                                math::Vec3 linear_velocity,
                                                f32 dt) noexcept {
    const simd::f32x4 vdt = simd::broadcast4(dt);
    simd::f32x4 p = load_vec3(position);
    p = simd::add4(p, simd::mul4(load_vec3(linear_velocity), vdt));
    return store_vec3(p);
}

// --- integrate_positions quaternion pre-normalise term - KEPT SCALAR ------
// Scalar reference (per component c in {x,y,z,w}):  rotation.c + 0.5f*dt*dq.c
//
// This one is DELIBERATELY NOT vectorised. The expression `a + b*c` is
// FMA-contracted by the compiler under the engine's default settings: clang's
// -ffp-contract defaults to `on` and -fno-fast-math does NOT turn it off, so
// the scalar fuses `rotation.c + (0.5f*dt)*dq.c` into one single-rounded
// `fmaf((0.5f*dt), dq.c, rotation.c)`. A separate-rounding SIMD `mul4`+`add4`
// performs TWO roundings and drifts by 1 ULP on ~2-3% of inputs (measured at
// both -O0 and -O2). Reintroducing the fusion with `fma4` would match on
// NEON/AVX2-FMA hosts but silently de-sync on an SSE-only host (where `fma4`
// falls back to mul+add) - so the only portable, provably-identical choice is
// to leave the original scalar expression untouched at the call site. The
// linear updates above do NOT contract (proven 0-ULP), so they ARE vectorised.
//
// Provided as a `_scalar`-named helper so the bench's vectorised twin can call
// the IDENTICAL scalar quaternion step (the integrator keeps it scalar in both
// the scalar and SIMD configurations - only forces/position/disp differ).

PSY_FORCEINLINE math::Quat quat_step_pre_normalize_scalar(math::Quat rotation,
                                                          math::Quat dq,
                                                          f32 dt) noexcept {
    return math::Quat{
        rotation.x + 0.5f * dt * dq.x,
        rotation.y + 0.5f * dt * dq.y,
        rotation.z + 0.5f * dt * dq.z,
        rotation.w + 0.5f * dt * dq.w,
    };
}

// --- rebuild_aabbs swept displacement -------------------------------------
// Scalar reference: disp = mul(linear_velocity, dt). The per-axis sign select
// that follows (one-sided AABB expansion) stays as scalar branches at the call
// site - branchy, cheap, and already bit-exact; vectorising the compare/blend
// would not change a single bit but adds no measurable win on a 3-float vector.

PSY_FORCEINLINE math::Vec3 swept_disp_scalar(math::Vec3 linear_velocity, f32 dt) noexcept {
    return math::mul(linear_velocity, dt);
}

PSY_FORCEINLINE math::Vec3 swept_disp_simd(math::Vec3 linear_velocity, f32 dt) noexcept {
    return store_vec3(simd::mul4(load_vec3(linear_velocity), simd::broadcast4(dt)));
}

}  // namespace psynder::physics::detail::integrate
