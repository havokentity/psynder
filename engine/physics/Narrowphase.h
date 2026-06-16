// SPDX-License-Identifier: MIT
// Psynder physics — GJK + EPA narrowphase + special kernels (DESIGN.md §10.1).
//
// Wave A targets sphere/capsule/box/convex-hull primitives. We split the work:
//   * Specialised closed-form kernels for the common pairs (sphere-sphere,
//     sphere-capsule, capsule-capsule, AABB-AABB) — cheap and exact.
//   * GJK distance + EPA penetration for any other convex pair (box-box,
//     box-capsule, box-sphere when oriented, convex-hull-vs-anything).
//
// Output is a Contact: world-space point on body A's surface, contact normal
// from A→B, separation depth (positive = penetration). The solver consumes
// these directly.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "Body.h"

#include <span>
#include <vector>

namespace psynder::physics::detail {

struct Contact {
    math::Vec3 point_world{0, 0, 0};
    math::Vec3 normal_world{0, 1, 0};  // A → B
    f32 depth = 0.0f;                  // penetration (>0) for touching contacts

    // Speculative contact (Catto/Box2D TOI-free anti-tunnelling). When `true`
    // the bodies are NOT yet overlapping: `separation` is the positive gap
    // along `normal_world` between them THIS tick. The solver clamps the
    // closing velocity so the bodies cannot approach by more than `separation`
    // this step (no position bias is added — see Solver). A regular (touching)
    // contact leaves `speculative == false` and uses `depth` as before, so the
    // resting-contact path for spheres/boxes/capsules is byte-for-byte
    // unchanged.
    bool speculative = false;
    f32 separation = 0.0f;  // >= 0, only meaningful when speculative

    u32 body_a = 0;
    u32 body_b = 0;

    // Solver bookkeeping — populated by Solver.cpp, lives here so the
    // contact buffer is contiguous.
    f32 normal_impulse_acc = 0.0f;
    f32 friction_impulse_acc1 = 0.0f;
    f32 friction_impulse_acc2 = 0.0f;
};

// One-shot collide-pair entry point. Returns true if A and B overlap; on hit,
// `out` is populated with a single (deepest) contact. Lane-13 Wave B can
// upgrade this to a manifold of up to 4 points; one point is enough for the
// PGS solver in Wave A because we re-collide every step.
bool collide_pair(const Body& a, const Body& b, Contact& out) noexcept;

// Speculative-aware collide-pair (anti-tunnelling). Identical to collide_pair
// for penetrating pairs, but additionally returns a SPECULATIVE contact
// (out.speculative == true, out.separation = gap) when the pair is not yet
// touching but is closing fast enough to cross the gap within `dt` (or is
// within `margin`). The solver clamps the closing velocity to the gap so a
// thin/fast body cannot tunnel — no fake thickness, TOI-free. Pairs without a
// closed-form separation here (box-box / capsule / GJK) fall through to
// collide_pair, so their behaviour is unchanged. `margin` is a small constant
// (detail::kernels::kSpeculativeMargin). This is what the world step calls.
bool collide_pair_spec(const Body& a, const Body& b, f32 dt, f32 margin, Contact& out) noexcept;

// ─── Specialised kernels (header-inline for AVX2 vectorisation) ─────────
bool collide_sphere_sphere(math::Vec3 ca, f32 ra, math::Vec3 cb, f32 rb, Contact& out) noexcept;
bool collide_sphere_capsule(
    math::Vec3 cs, f32 rs, math::Vec3 cc, math::Quat qc, f32 rc, f32 hc, Contact& out) noexcept;
bool collide_capsule_capsule(math::Vec3 ca,
                             math::Quat qa,
                             f32 ra,
                             f32 ha,
                             math::Vec3 cb,
                             math::Quat qb,
                             f32 rb,
                             f32 hb,
                             Contact& out) noexcept;
bool collide_aabb_aabb(math::Aabb a, math::Aabb b, Contact& out) noexcept;

// ─── GJK distance + EPA penetration (general convex pair) ───────────────
// Caller passes a support function for each body that returns the farthest
// point in the body's local-shape direction. We rotate into world via the
// body transforms.
struct GjkSupport {
    math::Vec3 position;
    math::Quat rotation;
    u8 shape;
    math::Vec3 half_extent;
};

bool collide_gjk_epa(const GjkSupport& a, const GjkSupport& b, Contact& out) noexcept;

}  // namespace psynder::physics::detail
