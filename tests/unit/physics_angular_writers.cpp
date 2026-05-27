// SPDX-License-Identifier: MIT
// Psynder — angular dynamics convention behind the public angular writers
// (World::apply_torque / apply_angular_impulse / set_angular_velocity).
//
// Per the physics unit-test convention (see physics_integrator.cpp), this test
// is header-only and operates on raw Body structs + a local mirror of the
// integrator's force step, rather than the World singleton (which carries a
// mutex + global lifetime not meant for the bare test harness). The World
// methods are thin field writes over exactly this machinery:
//   apply_torque(t)            -> b.torque += t           (consumed below)
//   apply_angular_impulse(J)   -> b.angular_velocity += R*(I^-1 (.) (R^T*J))
//   set_angular_velocity(w)    -> b.angular_velocity  = w
// and all three early-return on inv_mass == 0 (static). We verify the math the
// writers rely on matches the integrator's PROPERLY ROTATED inverse-inertia
// convention (R * I_local^-1 * R^T). These cases use identity rotation, where
// that reduces to the local diagonal; the rotated-body behaviour is covered by
// physics_rotated_inertia.cpp.

#include "physics/internal/Kernels.h"
#include "physics/Shape.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

// Mirror of World.cpp::integrate_forces' angular path (properly rotated
// inverse-inertia): w += R*(I_local^-1 (.) (R^T*torque)) * dt, then clear the
// accumulator. Identity rotation here reduces to the local diagonal.
void integrate_angular(Body& b, f32 dt) {
    if (b.inv_mass == 0.0f)
        return;
    const math::Vec3 ang_accel =
        apply_inv_inertia_world(b.rotation, b.inertia.inv_local, b.torque);
    b.angular_velocity = math::add(b.angular_velocity, math::mul(ang_accel, dt));
    b.torque = {0, 0, 0};
}

Body make_box() {
    Body b{};
    b.rotation = {0, 0, 0, 1};
    b.mass = 2.0f;
    b.inv_mass = 0.5f;
    b.inertia.local = {4.0f, 8.0f, 4.0f};
    b.inertia.inv_local = {0.25f, 0.125f, 0.25f};
    return b;
}

}  // namespace

TEST_CASE("apply_torque accumulator integrates to I^-1 * torque * dt", "[physics][angular]") {
    Body b = make_box();
    // apply_torque(t): accumulate.
    b.torque = math::add(b.torque, math::Vec3{2.0f, 4.0f, 0.0f});
    const f32 dt = 1.0f / 120.0f;
    integrate_angular(b, dt);
    // Expect w == I^-1 (.) t * dt.
    REQUIRE(b.angular_velocity.x == Approx(0.25f * 2.0f * dt));
    REQUIRE(b.angular_velocity.y == Approx(0.125f * 4.0f * dt));
    REQUIRE(b.angular_velocity.z == Approx(0.0f));
    // Accumulator cleared after the step (no runaway).
    REQUIRE(b.torque.x == 0.0f);
}

TEST_CASE("apply_angular_impulse adds I^-1 * J instantaneously", "[physics][angular]") {
    Body b = make_box();
    // apply_angular_impulse(J): w += R*(I^-1 (.) (R^T*J)) (no dt). Identity
    // rotation here reduces to the local diagonal.
    const math::Vec3 J{8.0f, 8.0f, 4.0f};
    b.angular_velocity = math::add(
        b.angular_velocity,
        apply_inv_inertia_world(b.rotation, b.inertia.inv_local, J));
    REQUIRE(b.angular_velocity.x == Approx(8.0f * 0.25f));   // 2.0
    REQUIRE(b.angular_velocity.y == Approx(8.0f * 0.125f));  // 1.0
    REQUIRE(b.angular_velocity.z == Approx(4.0f * 0.25f));   // 1.0
}

TEST_CASE("angular writers are inert on a static body (inv_mass == 0)", "[physics][angular]") {
    Body b = make_box();
    b.mass = 0.0f;
    b.inv_mass = 0.0f;  // static
    b.torque = math::add(b.torque, math::Vec3{10.0f, 10.0f, 10.0f});
    integrate_angular(b, 1.0f / 120.0f);  // early-returns for static
    REQUIRE(b.angular_velocity.x == 0.0f);
    REQUIRE(b.angular_velocity.y == 0.0f);
    REQUIRE(b.angular_velocity.z == 0.0f);
}
