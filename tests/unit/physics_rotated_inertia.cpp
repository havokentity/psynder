// SPDX-License-Identifier: MIT
// Psynder physics — properly ROTATED inverse-inertia tensor (R * I_local^-1 *
// R^T) for angular dynamics.
//
// The integrator stores the diagonal inertia in the body's LOCAL principal
// frame. The old code applied that diagonal directly to WORLD-space torque
// (ang_accel = inv_local (.) torque), which is only correct when the body's
// rotation is identity or its inertia is isotropic. A rotated ASYMMETRIC body
// then responds incorrectly: the angular acceleration is forced parallel (per
// component) to the torque, so the angular-velocity direction never sweeps —
// no cross-axis coupling / precession.
//
// The fix applies the inertia in the local frame and brings the result back to
// world:  ang_accel_world = R * ( I_local^-1 (.) ( R^T * torque_world ) ).
//
// Header-only, per the physics-test convention (physics_integrator.cpp): we
// operate on a raw Body + a local mirror of World.cpp's integrate_forces /
// integrate_positions, exercising the SAME detail::apply_inv_inertia_world the
// facade uses. No linkage to psynder_physics, alloc-free.

#include "physics/internal/Kernels.h"
#include "physics/Shape.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

// Mirror of World.cpp::integrate_forces' angular path: consume an accumulated
// world-space torque through the rotated inverse-inertia tensor, then clear it.
void integrate_forces_angular(Body& b, f32 dt) {
    if (b.inv_mass == 0.0f)
        return;
    const math::Vec3 ang_accel =
        apply_inv_inertia_world(b.rotation, b.inertia.inv_local, b.torque);
    b.angular_velocity = math::add(b.angular_velocity, math::mul(ang_accel, dt));
    b.torque = {0, 0, 0};
}

// Mirror of World.cpp::integrate_positions' rotation path: q' = q + 0.5*dt*w*q,
// renormalised. Position is irrelevant to the angular signature.
void integrate_rotation(Body& b, f32 dt) {
    math::Quat w_q{b.angular_velocity.x, b.angular_velocity.y, b.angular_velocity.z, 0.0f};
    math::Quat dq = math::quat_mul(w_q, b.rotation);
    b.rotation = math::quat_normalize({
        b.rotation.x + 0.5f * dt * dq.x,
        b.rotation.y + 0.5f * dt * dq.y,
        b.rotation.z + 0.5f * dt * dq.z,
        b.rotation.w + 0.5f * dt * dq.w,
    });
}

// The OLD broken convention, kept here purely as a contrast oracle: world-space
// diagonal, ignoring rotation. We assert the fixed path DIVERGES from this.
math::Vec3 old_diagonal_accel(const Body& b, math::Vec3 torque) {
    return {torque.x * b.inertia.inv_local.x,
            torque.y * b.inertia.inv_local.y,
            torque.z * b.inertia.inv_local.z};
}

// A long, distinctly non-uniform box: I_local = {1, 8, 1} so x/z are "easy"
// axes and y is "hard". inv_local is the reciprocal diagonal.
Body make_asym_box() {
    Body b{};
    b.rotation = {0, 0, 0, 1};
    b.mass = 1.0f;
    b.inv_mass = 1.0f;
    b.inertia.local = {1.0f, 8.0f, 1.0f};
    b.inertia.inv_local = {1.0f, 0.125f, 1.0f};
    b.shape = 2;  // box
    b.half_extent = {0.5f, 2.0f, 0.5f};
    return b;
}

math::Vec3 dir_of(math::Vec3 v) {
    return math::normalize(v);
}

}  // namespace

TEST_CASE("rotated asymmetric body: torque about a non-principal axis sweeps "
          "the angular-velocity direction (precession signature)",
          "[physics][angular]") {
    Body b = make_asym_box();
    // Rotate 45 deg about Z so the local principal frame is NOT world-aligned:
    // the local x/y axes now sit diagonally in the world XY plane. A torque
    // applied about WORLD x then has a component along local y (the hard axis)
    // and local x (an easy axis), so the inverse tensor couples them.
    b.rotation = math::quat_normalize(
        math::quat_from_axis_angle({0, 0, 1}, 3.14159265f / 4.0f));

    const f32 dt = 1.0f / 120.0f;
    const math::Vec3 torque{0.6f, 0.0f, 0.0f};  // constant, world-space

    // First sub-step's acceleration direction.
    b.torque = torque;
    integrate_forces_angular(b, dt);
    const math::Vec3 dir0 = dir_of(b.angular_velocity);
    integrate_rotation(b, dt);

    // The rotated tensor must produce a coupled response: the angular velocity
    // is NOT parallel to the torque axis (which the old diagonal code would
    // force, since {Tx*ix, 0, 0} is always along x).
    REQUIRE(std::fabs(b.angular_velocity.y) > 1e-4f);

    // Step on under continued torque; the body rotates, so R (hence R*I^-1*R^T)
    // changes and the angular-velocity DIRECTION sweeps over time.
    for (int i = 0; i < 240; ++i) {
        b.torque = torque;
        integrate_forces_angular(b, dt);
        integrate_rotation(b, dt);
    }
    const math::Vec3 dir1 = dir_of(b.angular_velocity);

    // Direction must have changed measurably (cos < 0.999 ~ >2.5 deg). The old
    // world-space-diagonal code keeps the delta fixed along x, so its direction
    // would barely move once spun up — this is the discriminator.
    const f32 cos_swing = math::dot(dir0, dir1);
    REQUIRE(cos_swing < 0.999f);

    // And the fixed path must DIFFER from the broken diagonal oracle on this
    // rotated body (proves we are not silently reducing to the old behaviour).
    const math::Vec3 fixed = apply_inv_inertia_world(b.rotation, b.inertia.inv_local, torque);
    const math::Vec3 broken = old_diagonal_accel(b, torque);
    const f32 delta = math::length(math::sub(fixed, broken));
    REQUIRE(delta > 1e-3f);
}

TEST_CASE("spin purely about a world-aligned principal axis stays stable",
          "[physics][angular]") {
    Body b = make_asym_box();  // identity rotation: local == world axes
    const f32 dt = 1.0f / 120.0f;
    // Spin and torque both about the hard (y) principal axis. With R = identity
    // the tensor is the bare diagonal, so the response is purely about y and
    // the direction must not wander.
    b.angular_velocity = {0, 2.0f, 0};
    const math::Vec3 dir0 = dir_of(b.angular_velocity);
    for (int i = 0; i < 240; ++i) {
        b.torque = {0, 0.25f, 0};  // along the principal spin axis
        integrate_forces_angular(b, dt);
        integrate_rotation(b, dt);
    }
    // No off-axis bleed: x and z stay zero.
    REQUIRE(b.angular_velocity.x == Approx(0.0f).margin(1e-5f));
    REQUIRE(b.angular_velocity.z == Approx(0.0f).margin(1e-5f));
    const math::Vec3 dir1 = dir_of(b.angular_velocity);
    REQUIRE(math::dot(dir0, dir1) == Approx(1.0f).margin(1e-5f));
}

TEST_CASE("apply_angular_impulse on a rotated body yields w = R*(I^-1 (.) (R^T*J))",
          "[physics][angular]") {
    Body b = make_asym_box();
    // Arbitrary off-principal orientation.
    b.rotation = math::quat_normalize(
        math::quat_from_axis_angle(math::normalize(math::Vec3{1, 1, 0}),
                                   3.14159265f / 3.0f));
    const math::Vec3 J{1.5f, -0.5f, 0.8f};

    // World::apply_angular_impulse does exactly this add (no dt).
    const math::Vec3 dw =
        apply_inv_inertia_world(b.rotation, b.inertia.inv_local, J);
    b.angular_velocity = math::add(b.angular_velocity, dw);

    // Independent reconstruction: rotate J into local, scale by the diagonal,
    // rotate back. Must match the helper exactly.
    const math::Vec3 j_local = quat_rotate(quat_conjugate(b.rotation), J);
    const math::Vec3 a_local{j_local.x * b.inertia.inv_local.x,
                             j_local.y * b.inertia.inv_local.y,
                             j_local.z * b.inertia.inv_local.z};
    const math::Vec3 expect = quat_rotate(b.rotation, a_local);

    REQUIRE(b.angular_velocity.x == Approx(expect.x));
    REQUIRE(b.angular_velocity.y == Approx(expect.y));
    REQUIRE(b.angular_velocity.z == Approx(expect.z));

    // Sanity: it must NOT equal the old world-space-diagonal answer on this
    // rotated body (otherwise the test would pass even against the bug).
    const math::Vec3 broken{J.x * b.inertia.inv_local.x,
                            J.y * b.inertia.inv_local.y,
                            J.z * b.inertia.inv_local.z};
    REQUIRE(math::length(math::sub(expect, broken)) > 1e-3f);
}
