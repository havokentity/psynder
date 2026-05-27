// SPDX-License-Identifier: MIT
// Psynder physics — joint solver (DESIGN.md §10.8 six joint kinds).
//
// The contact solver in Solver.cpp / Kernels.h resolves CONTACT constraints
// only. This file adds a STANDALONE post-step pass that resolves the six
// authored joint kinds (weld / axis / slider / ball-socket / rope / elastic)
// as a sequential-impulse / projected-Gauss-Seidel pass over the rigid bodies.
//
// ── How it slots in ──────────────────────────────────────────────────────
// The caller runs it AFTER physics::step(dt) each frame:
//
//     physics::World::Get().step(dt);
//     physics::solve_joints(physics::World::Get(), joints, dt, iterations);
//
// step() has already integrated gravity into the velocities and advanced the
// positions. solve_joints() then corrects velocities (sequential impulse at
// the world-space anchor points, exactly the recipe the contact solver uses)
// and applies a final positional projection so a hanging chain/ragdoll settles
// visibly connected rather than slowly drifting apart. It NEVER touches the
// contact solver's internals.
//
// ── Layout: header-only kernels ──────────────────────────────────────────
// Mirrors physics/internal/Kernels.h: the actual math lives in inline kernels
// here so a unit test can pull it in and run against a locally-staged Body
// array WITHOUT linking psynder_physics. The World-facing overload lives in
// Joints.cpp (the only TU that reaches into the world singleton).

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Body.h"
#include "physics/Physics.h"
#include "physics/Shape.h"

#include <cmath>
#include <span>
#include <vector>

namespace psynder::physics {

// The six kinds, value-compatible in meaning with editor::constraints::Kind
// (we keep an independent enum so this header does not depend on the editor
// lane; the adapter in this file converts between them).
enum class JointKind : u8 {
    Weld = 0,        // rigid attachment: anchors coincident, orientation locked
    Axis = 1,        // 1-DoF hinge about `axis`, anchors coincident
    Slider = 2,      // 1-DoF translation along `axis`, clamped to [min,max]
    BallSocket = 3,  // 3-DoF rotation about a point: anchors coincident
    Rope = 4,        // length-limited distance: |sep| <= rest_length
    Elastic = 5,     // soft spring toward rest_length (stiffness/damping)
};

// One joint. `body_a` / `body_b` are indices into the body span the kernel
// solver runs over (the header-only detail::joints::solve path used by tests
// against a locally-staged Body array). Anchors are body-LOCAL offsets from
// each body's centre of mass.
//
// Generation safety (Fix 1d): a raw array index aliases a destroyed-and-
// recreated body. The World-facing solve_joints() therefore prefers the full
// `body_id_a` / `body_id_b` BodyIds when they are valid: it resolves each to
// the live slot via a gen-checked lookup every solve, skipping the joint if
// either handle is stale. They default invalid so existing index-authored
// joints (and the kernel-level tests) are unaffected — set them via
// joint_with_handles() to opt into the safe path.
struct Joint {
    JointKind kind = JointKind::Weld;
    u32 body_a = 0;            // index into the body span
    u32 body_b = 0;            // index into the body span
    math::Vec3 anchor_a{0, 0, 0};  // local-space anchor on A
    math::Vec3 anchor_b{0, 0, 0};  // local-space anchor on B
    math::Vec3 axis{0, 1, 0};      // hinge / slider direction (local to A)
    f32 rest_length = 0.0f;        // rope max / elastic rest / rigid pin length
    f32 stiffness = 0.0f;          // elastic only (N/m)
    f32 damping = 0.0f;            // elastic only (N*s/m)
    f32 min_limit = 0.0f;          // slider lower bound (m)
    f32 max_limit = 0.0f;          // slider upper bound (m)

    // Optional full handles for the gen-checked World-facing path. Invalid
    // (raw == 0) means "use the raw body_a/body_b indices as-is".
    BodyId body_id_a{};
    BodyId body_id_b{};
};

// A flat list of joints. POD-ish; copy/convert freely.
struct JointSet {
    std::vector<Joint> joints;

    void clear() noexcept { joints.clear(); }
    usize size() const noexcept { return joints.size(); }
    void add(const Joint& j) { joints.push_back(j); }
};

// Tunables for the joint pass.
//
// The solver uses SPLIT IMPULSE (Catto): the velocity pass nulls the relative
// velocity at the anchor WITHOUT a Baumgarte position bias, and a separate
// positional projection pass removes drift by moving positions only. Folding
// position error into the velocity target (plain Baumgarte) pumps energy into a
// closed loop like a pendulum and makes it diverge; split impulse is energy-
// stable. A small per-step velocity damping bleeds off residual swing so a
// hanging chain/ragdoll settles into a static pose rather than swinging forever
// (real engines do the same — frictionless joints are otherwise conservative).
struct JointSolverParams {
    f32 slop = 0.0f;              // distance tolerance before correcting (m)
    f32 position_beta = 0.2f;     // positional projection blend per iteration
    u32 position_iterations = 4;  // Gauss-Seidel position passes
    // Fraction of velocity removed per joint pass from bodies that joints
    // touch. 0 = energy-conserving (swings forever); small positive = settles.
    f32 velocity_damping = 0.04f;
};

namespace detail::joints {

PSY_FORCEINLINE math::Vec3 world_anchor(const Body& b, math::Vec3 local) noexcept {
    return math::add(b.position, detail::quat_rotate(b.rotation, local));
}

PSY_FORCEINLINE math::Vec3 inv_inertia_mul(const Body& b, math::Vec3 v) noexcept {
    return {v.x * b.inertia.inv_local.x, v.y * b.inertia.inv_local.y, v.z * b.inertia.inv_local.z};
}

// Velocity of the material point at world offset r from body b's centre.
PSY_FORCEINLINE math::Vec3 point_velocity(const Body& b, math::Vec3 r) noexcept {
    return math::add(b.linear_velocity, math::cross(b.angular_velocity, r));
}

// Apply a linear impulse P (and its angular component r x P) at world offset
// ra/rb. A receives -P, B receives +P (sign convention matches the contact
// solver: P pushes B away from A along the constraint normal).
PSY_FORCEINLINE void apply_pair_impulse(
    Body& A, Body& B, math::Vec3 ra, math::Vec3 rb, math::Vec3 P) noexcept {
    A.linear_velocity = math::sub(A.linear_velocity, math::mul(P, A.inv_mass));
    B.linear_velocity = math::add(B.linear_velocity, math::mul(P, B.inv_mass));
    A.angular_velocity = math::sub(A.angular_velocity, inv_inertia_mul(A, math::cross(ra, P)));
    B.angular_velocity = math::add(B.angular_velocity, inv_inertia_mul(B, math::cross(rb, P)));
}

// Effective scalar mass along `dir` for the relative point pair (ra on A,
// rb on B): 1 / (n . K n) where K is the combined inverse mass matrix. Returns
// 0 when both bodies are immovable along this direction.
PSY_FORCEINLINE f32 effective_mass(
    const Body& A, const Body& B, math::Vec3 ra, math::Vec3 rb, math::Vec3 dir) noexcept {
    math::Vec3 ra_x = math::cross(ra, dir);
    math::Vec3 rb_x = math::cross(rb, dir);
    f32 ang = math::dot(dir, math::cross(inv_inertia_mul(A, ra_x), ra)) +
              math::dot(dir, math::cross(inv_inertia_mul(B, rb_x), rb));
    f32 k = A.inv_mass + B.inv_mass + ang;
    return (k > 1e-12f) ? 1.0f / k : 0.0f;
}

// Bleed a small fraction of a body's velocity. Joints would otherwise be
// energy-conserving (a frictionless pendulum swings forever); a little damping
// lets a hanging chain/ragdoll settle into a static pose. No-op for statics.
PSY_FORCEINLINE void damp_body(Body& b, f32 frac) noexcept {
    if (b.inv_mass == 0.0f || frac <= 0.0f)
        return;
    f32 s = 1.0f - frac;
    b.linear_velocity = math::mul(b.linear_velocity, s);
    b.angular_velocity = math::mul(b.angular_velocity, s);
}

// ── One velocity iteration for a single joint ─────────────────────────────
// Split impulse: nulls the relative velocity at the anchor with NO Baumgarte
// position bias (drift is fixed by project_joint_position). The lone exception
// is Elastic, whose "bias" is the spring restoring term — that IS the intended
// physics, not a drift correction. `bodies` is the live body array; the joint's
// body_a/body_b index into it.
inline void solve_joint_velocity(const Joint& j,
                                 std::span<Body> bodies,
                                 const JointSolverParams& params,
                                 f32 dt) noexcept {
    Body& A = bodies[j.body_a];
    Body& B = bodies[j.body_b];
    if (A.inv_mass + B.inv_mass <= 0.0f)
        return;  // both pinned/static

    const math::Vec3 pa = world_anchor(A, j.anchor_a);
    const math::Vec3 pb = world_anchor(B, j.anchor_b);
    const math::Vec3 ra = math::sub(pa, A.position);
    const math::Vec3 rb = math::sub(pb, B.position);

    const math::Vec3 sep = math::sub(pb, pa);  // B anchor relative to A anchor
    const f32 dist = math::length(sep);

    const f32 inv_dt = (dt > 1e-8f) ? 1.0f / dt : 0.0f;

    switch (j.kind) {
        case JointKind::Weld:
        case JointKind::Axis:
        case JointKind::BallSocket: {
            // Point-to-point: drive the two anchors coincident along all three
            // axes. For weld/axis/ball-socket the load-bearing constraint for a
            // hanging body is exactly this 3-DoF positional pin; the rotational
            // DoF differences (weld locks orientation, axis allows one spin,
            // ball-socket allows all three) do not affect the anchor pin and
            // are left free here — the pin is what keeps the chain connected.
            for (u32 axis = 0; axis < 3; ++axis) {
                math::Vec3 n{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
                             axis == 2 ? 1.0f : 0.0f};
                f32 m = effective_mass(A, B, ra, rb, n);
                if (m == 0.0f)
                    continue;
                f32 vrel = math::dot(math::sub(point_velocity(B, rb), point_velocity(A, ra)), n);
                f32 lambda = -vrel * m;  // split impulse: no position bias
                apply_pair_impulse(A, B, ra, rb, math::mul(n, lambda));
            }
            break;
        }
        case JointKind::Slider: {
            // Allowed motion is along `axis` (rotated into world by A). Lock the
            // two perpendicular axes (anchors coincident off-axis), and clamp
            // the along-axis separation into [min_limit, max_limit].
            math::Vec3 ax = math::normalize(detail::quat_rotate(A.rotation, j.axis));
            // Perpendicular basis.
            math::Vec3 t1, t2;
            if (std::fabs(ax.x) >= 0.57735f)
                t1 = math::normalize(math::Vec3{ax.y, -ax.x, 0.0f});
            else
                t1 = math::normalize(math::Vec3{0.0f, ax.z, -ax.y});
            t2 = math::cross(ax, t1);

            for (math::Vec3 n : {t1, t2}) {
                f32 m = effective_mass(A, B, ra, rb, n);
                if (m == 0.0f)
                    continue;
                f32 vrel = math::dot(math::sub(point_velocity(B, rb), point_velocity(A, ra)), n);
                f32 lambda = -vrel * m;  // split impulse: no position bias
                apply_pair_impulse(A, B, ra, rb, math::mul(n, lambda));
            }
            // Along-axis limit (unilateral when outside [min,max]).
            f32 along = math::dot(sep, ax);
            if (along >= j.min_limit && along <= j.max_limit)
                break;  // within the slot: free to slide, no impulse
            f32 m = effective_mass(A, B, ra, rb, ax);
            if (m == 0.0f)
                break;
            f32 vrel = math::dot(math::sub(point_velocity(B, rb), point_velocity(A, ra)), ax);
            // Only resist motion that pushes further past the violated bound.
            bool below = along < j.min_limit;
            if ((below && vrel < 0.0f) || (!below && vrel > 0.0f)) {
                f32 lambda = -vrel * m;
                apply_pair_impulse(A, B, ra, rb, math::mul(ax, lambda));
            }
            break;
        }
        case JointKind::Rope: {
            // Length limit: only resist when stretched past rest_length, and
            // only the component pulling them further apart (rope never pushes).
            if (dist <= j.rest_length || dist < 1e-6f)
                break;
            math::Vec3 n = math::mul(sep, 1.0f / dist);
            f32 m = effective_mass(A, B, ra, rb, n);
            if (m == 0.0f)
                break;
            f32 vrel = math::dot(math::sub(point_velocity(B, rb), point_velocity(A, ra)), n);
            if (vrel <= 0.0f)
                break;  // already approaching: rope is going slack, no pull
            f32 lambda = -vrel * m;  // split impulse: no position bias
            apply_pair_impulse(A, B, ra, rb, math::mul(n, lambda));
            break;
        }
        case JointKind::Elastic: {
            // Damped linear spring along the separation axis toward rest_length.
            // Implemented as a soft constraint: gamma/beta CFM-ish blend so the
            // stiffness/damping read as a stable per-step impulse rather than an
            // explicit force (which would explode at large k * dt).
            if (dist < 1e-6f)
                break;
            math::Vec3 n = math::mul(sep, 1.0f / dist);
            f32 m = effective_mass(A, B, ra, rb, n);
            if (m == 0.0f)
                break;
            f32 c = dist - j.rest_length;
            // Soft-constraint coefficients (Catto, "Soft Constraints" GDC 2011):
            //   gamma = 1 / (dt * (d + dt * k)), beta = dt * k / (d + dt * k)
            f32 k = std::max(0.0f, j.stiffness);
            f32 d = std::max(0.0f, j.damping);
            f32 denom = d + dt * k;
            if (denom < 1e-8f)
                break;  // no spring
            f32 gamma = 1.0f / (dt * denom);
            f32 beta = (dt * k) / denom;
            f32 soft_m = 1.0f / ((1.0f / m) + gamma);
            f32 vrel = math::dot(math::sub(point_velocity(B, rb), point_velocity(A, ra)), n);
            f32 bias = beta * inv_dt * c;
            f32 lambda = -(vrel + bias) * soft_m;
            apply_pair_impulse(A, B, ra, rb, math::mul(n, lambda));
            break;
        }
    }
}

// ── Positional projection (one Gauss-Seidel pass) ─────────────────────────
// After the velocity pass, nudge positions so stiff distance constraints
// settle visibly connected. Mass-weighted split exactly like a PBD distance
// projection. Rope projects only the overshoot; elastic is a soft pull;
// slider clamps; weld/axis/ball pin the anchors coincident.
inline void project_joint_position(const Joint& j,
                                   std::span<Body> bodies,
                                   const JointSolverParams& params) noexcept {
    Body& A = bodies[j.body_a];
    Body& B = bodies[j.body_b];
    f32 w_sum = A.inv_mass + B.inv_mass;
    if (w_sum <= 0.0f)
        return;

    const math::Vec3 pa = world_anchor(A, j.anchor_a);
    const math::Vec3 pb = world_anchor(B, j.anchor_b);
    math::Vec3 sep = math::sub(pb, pa);
    f32 dist = math::length(sep);
    if (dist < 1e-6f)
        return;
    math::Vec3 dir = math::mul(sep, 1.0f / dist);

    f32 target = dist;
    f32 strength = params.position_beta;
    switch (j.kind) {
        case JointKind::Weld:
        case JointKind::Axis:
        case JointKind::BallSocket:
            target = j.rest_length;  // pin to the spawn separation
            strength = 1.0f;
            break;
        case JointKind::Slider: {
            f32 lo = j.min_limit, hi = j.max_limit;
            if (dist < lo)
                target = lo;
            else if (dist > hi)
                target = hi;
            else
                return;
            strength = 1.0f;
            break;
        }
        case JointKind::Rope:
            if (dist <= j.rest_length)
                return;
            target = j.rest_length;
            strength = 1.0f;
            break;
        case JointKind::Elastic:
            target = j.rest_length;
            strength = params.position_beta;  // soft: only a fraction per pass
            break;
    }

    f32 err = (dist - target) * strength;
    math::Vec3 corr = math::mul(dir, err / w_sum);
    A.position = math::add(A.position, math::mul(corr, A.inv_mass));
    B.position = math::sub(B.position, math::mul(corr, B.inv_mass));
}

// ── Full pass over a body span ────────────────────────────────────────────
// `iterations` velocity Gauss-Seidel sweeps + params.position_iterations
// positional sweeps. Static bodies (inv_mass == 0) are never written.
inline void solve(std::span<Body> bodies,
                  std::span<const Joint> joints,
                  const JointSolverParams& params,
                  f32 dt,
                  u32 iterations) noexcept {
    const u32 vel_its = iterations == 0 ? 1u : iterations;
    for (u32 it = 0; it < vel_its; ++it) {
        for (const Joint& j : joints) {
            if (j.body_a >= bodies.size() || j.body_b >= bodies.size())
                continue;
            solve_joint_velocity(j, bodies, params, dt);
        }
    }
    // Bleed a little velocity from every body a joint touches so a hanging
    // assembly settles into a static pose (frictionless joints would otherwise
    // conserve energy and swing forever). Each touched body is damped once.
    if (params.velocity_damping > 0.0f) {
        for (const Joint& j : joints) {
            if (j.body_a < bodies.size())
                damp_body(bodies[j.body_a], params.velocity_damping);
            if (j.body_b < bodies.size())
                damp_body(bodies[j.body_b], params.velocity_damping);
        }
    }
    for (u32 it = 0; it < params.position_iterations; ++it) {
        for (const Joint& j : joints) {
            if (j.body_a >= bodies.size() || j.body_b >= bodies.size())
                continue;
            project_joint_position(j, bodies, params);
        }
    }
}

}  // namespace detail::joints

// ── Public, World-facing entry point ──────────────────────────────────────
// Run AFTER World::step(dt). Resolves the joint set against the live bodies.
// `joints` reference body INDICES (Joint::body_a / body_b). Use the
// joints_from_handles() helper below to translate a BodyId-keyed authoring set
// into index space once at setup, or build Joint directly with indices.
//
// Defined in Joints.cpp (the only TU that reaches into the physics world
// singleton). Header-only callers/tests use detail::joints::solve() directly.
void solve_joints(World& world, const JointSet& joints, f32 dt, u32 iterations);
void solve_joints(World& world,
                  const JointSet& joints,
                  const JointSolverParams& params,
                  f32 dt,
                  u32 iterations);

// Convert a BodyId to the body-array index this engine uses internally. Lets a
// caller that authored joints against BodyId fix up Joint::body_a/body_b to the
// index space solve_joints() expects. Returns 0xFFFFFFFF for an invalid id.
//
// NOTE (Fix 1d): this validates the FULL generation of `id` (not just gen != 0),
// so a stale handle whose slot was recycled now returns 0xFFFFFFFF rather than
// silently resolving to whatever body currently occupies that slot. Prefer the
// gen-safe handle path (Joint::body_id_a/b + joint_with_handles) for joints that
// must survive body churn — body_index_of() snapshots the index at call time.
u32 body_index_of(BodyId id) noexcept;

// Build a Joint that carries full BodyIds for the gen-checked World-facing
// path. The raw body_a/body_b indices are seeded from the handles' index bits
// for the kernel/test path and for the visualiser, but solve_joints() re-
// resolves them against the live world (and skips the joint on a stale handle).
inline Joint joint_with_handles(JointKind kind, BodyId a, BodyId b) noexcept {
    Joint j{};
    j.kind = kind;
    j.body_a = detail::handle_index(a.raw);
    j.body_b = detail::handle_index(b.raw);
    j.body_id_a = a;
    j.body_id_b = b;
    return j;
}

}  // namespace psynder::physics
