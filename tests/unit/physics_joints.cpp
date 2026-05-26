// SPDX-License-Identifier: MIT
// Psynder physics unit tests — joint solver (engine/physics/Joints.h).
//
// Header-only: pulls in the inline joint kernels and drives them against a
// locally-staged Body array, so the test compiles WITHOUT linking
// psynder_physics (same pattern as physics_integrator / physics_collision).
//
// Scene under test:
//   * A rope of N point masses hung from a static anchor (Rope joints).
//   * A simple 2-bar pendulum welded to a static hinge point (Weld/BallSocket).
// We integrate gravity + step the joint solver for a few simulated seconds and
// assert the chains stay connected (bounded anchor distances) and settle
// (velocities decay toward zero).

#include "physics/Joints.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::physics;
using Catch::Approx;

namespace {

// Symplectic-Euler integrate of gravity, mirroring World::step's force pass.
// Only translation matters for these point-mass chains; we keep rotation
// inert (joints here pin anchors at body centres, anchor_a/b = origin).
void integrate(std::vector<detail::Body>& bodies, math::Vec3 g, f32 dt) {
    for (auto& b : bodies) {
        if (b.inv_mass == 0.0f)
            continue;
        b.linear_velocity = math::add(b.linear_velocity, math::mul(g, dt));
        b.position = math::add(b.position, math::mul(b.linear_velocity, dt));
    }
}

detail::Body make_body(math::Vec3 p, f32 mass) {
    detail::Body b{};
    b.position = p;
    b.prev_position = p;
    b.rotation = {0, 0, 0, 1};
    b.mass = mass;
    b.inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
    // Point-mass: a small spherical inertia so any angular term stays finite.
    f32 i = (mass > 0.0f) ? (2.0f / 5.0f) * mass * 0.01f : 0.0f;
    b.inertia.local = {i, i, i};
    b.inertia.inv_local = (i > 0.0f) ? math::Vec3{1.0f / i, 1.0f / i, 1.0f / i} : math::Vec3{0, 0, 0};
    b.shape = 0;
    b.half_extent = {0.1f, 0.1f, 0.1f};
    return b;
}

f32 anchor_distance(const detail::Body& a, const detail::Body& b) {
    return math::length(math::sub(b.position, a.position));
}

f32 max_speed(const std::vector<detail::Body>& bodies) {
    f32 m = 0.0f;
    for (const auto& b : bodies)
        if (b.inv_mass > 0.0f)
            m = std::max(m, math::length(b.linear_velocity));
    return m;
}

bool any_nan(const std::vector<detail::Body>& bodies) {
    for (const auto& b : bodies) {
        if (std::isnan(b.position.x) || std::isnan(b.position.y) || std::isnan(b.position.z))
            return true;
    }
    return false;
}

}  // namespace

TEST_CASE("Rope chain hangs from a static anchor, stays connected and settles",
          "[physics][joints][rope]") {
    constexpr u32 kLinks = 6;
    constexpr f32 kSeg = 0.4f;  // rope max segment length (m)
    const math::Vec3 g{0.0f, -9.81f, 0.0f};
    const f32 dt = 1.0f / 120.0f;

    std::vector<detail::Body> bodies;
    bodies.push_back(make_body({0.0f, 5.0f, 0.0f}, 0.0f));  // index 0: static anchor
    math::Vec3 p{0.0f, 5.0f, 0.0f};
    for (u32 i = 0; i < kLinks; ++i) {
        // Spawn slightly off-axis so it has something to swing out of and settle.
        p = math::add(p, math::Vec3{0.05f, -kSeg, 0.0f});
        bodies.push_back(make_body(p, 0.5f));
    }

    std::vector<Joint> joints;
    for (u32 i = 0; i < kLinks; ++i) {
        Joint j{};
        j.kind = JointKind::Rope;
        j.body_a = i;            // anchor (0) then each prior link
        j.body_b = i + 1;
        j.rest_length = kSeg;    // max length
        joints.push_back(j);
    }

    JointSolverParams params{};
    params.position_iterations = 4;

    // 3 simulated seconds.
    const u32 steps = static_cast<u32>(3.0f / dt);
    for (u32 s = 0; s < steps; ++s) {
        integrate(bodies, g, dt);
        detail::joints::solve(bodies, joints, params, dt, /*iterations=*/10);
        REQUIRE_FALSE(any_nan(bodies));
    }

    // Stays connected: every segment within the rope's max length (+ slop).
    for (u32 i = 0; i < kLinks; ++i) {
        f32 d = anchor_distance(bodies[i], bodies[i + 1]);
        REQUIRE(d <= kSeg + 0.02f);
    }

    // Settles: the chain stops swinging. With a static anchor and a fully
    // extended rope, the steady state is a straight vertical hang at rest.
    REQUIRE(max_speed(bodies) < 0.5f);

    // The tip hangs below the anchor (gravity won, no blow-up upward).
    REQUIRE(bodies[kLinks].position.y < bodies[0].position.y);
    // Total drop is bounded by the summed rope length.
    f32 drop = bodies[0].position.y - bodies[kLinks].position.y;
    REQUIRE(drop <= kSeg * kLinks + 0.05f);
}

TEST_CASE("Two-bar pendulum welded to a static hinge stays pinned and settles",
          "[physics][joints][weld]") {
    const math::Vec3 g{0.0f, -9.81f, 0.0f};
    const f32 dt = 1.0f / 120.0f;
    constexpr f32 kBar = 0.6f;

    std::vector<detail::Body> bodies;
    bodies.push_back(make_body({0.0f, 4.0f, 0.0f}, 0.0f));          // 0: static hinge
    bodies.push_back(make_body({0.3f, 4.0f - kBar, 0.0f}, 1.0f));   // 1: first bar mass
    bodies.push_back(make_body({0.6f, 4.0f - 2 * kBar, 0.0f}, 1.0f));  // 2: second bar mass

    std::vector<Joint> joints;
    {
        // Rigid pin hinge -> bar1 (Weld): rest_length baked to spawn distance.
        Joint j{};
        j.kind = JointKind::Weld;
        j.body_a = 0;
        j.body_b = 1;
        j.rest_length = anchor_distance(bodies[0], bodies[1]);
        joints.push_back(j);
    }
    {
        // Ball-socket bar1 -> bar2 (3-DoF rotation about the pin point).
        Joint j{};
        j.kind = JointKind::BallSocket;
        j.body_a = 1;
        j.body_b = 2;
        j.rest_length = anchor_distance(bodies[1], bodies[2]);
        joints.push_back(j);
    }
    const f32 pin0 = joints[0].rest_length;
    const f32 pin1 = joints[1].rest_length;

    JointSolverParams params{};
    params.position_iterations = 4;

    const u32 steps = static_cast<u32>(4.0f / dt);
    for (u32 s = 0; s < steps; ++s) {
        integrate(bodies, g, dt);
        detail::joints::solve(bodies, joints, params, dt, /*iterations=*/12);
        REQUIRE_FALSE(any_nan(bodies));
    }

    // Pins hold: both bar lengths stay near their authored separation.
    REQUIRE(anchor_distance(bodies[0], bodies[1]) == Approx(pin0).margin(0.03f));
    REQUIRE(anchor_distance(bodies[1], bodies[2]) == Approx(pin1).margin(0.03f));

    // Settles: the pendulum stops swinging.
    REQUIRE(max_speed(bodies) < 0.5f);

    // Hangs straight down beneath the hinge at rest (both masses below it,
    // roughly under the hinge x within the combined bar length).
    REQUIRE(bodies[1].position.y < bodies[0].position.y);
    REQUIRE(bodies[2].position.y < bodies[1].position.y);
    REQUIRE(std::fabs(bodies[2].position.x - bodies[0].position.x) < pin0 + pin1);
}

TEST_CASE("Elastic spring pulls a mass toward rest length without exploding",
          "[physics][joints][elastic]") {
    const math::Vec3 g{0.0f, -9.81f, 0.0f};
    const f32 dt = 1.0f / 120.0f;
    constexpr f32 kRest = 0.5f;

    std::vector<detail::Body> bodies;
    bodies.push_back(make_body({0.0f, 3.0f, 0.0f}, 0.0f));  // static
    bodies.push_back(make_body({0.0f, 2.0f, 0.0f}, 0.5f));  // hangs 1.0 below (stretched)

    std::vector<Joint> joints;
    Joint j{};
    j.kind = JointKind::Elastic;
    j.body_a = 0;
    j.body_b = 1;
    j.rest_length = kRest;
    j.stiffness = 300.0f;
    j.damping = 8.0f;
    joints.push_back(j);

    const u32 steps = static_cast<u32>(5.0f / dt);
    for (u32 s = 0; s < steps; ++s) {
        integrate(bodies, g, dt);
        detail::joints::solve(bodies, joints, JointSolverParams{}, dt, /*iterations=*/8);
        REQUIRE_FALSE(any_nan(bodies));
    }

    // A damped spring under gravity settles below rest_length (mg stretch), but
    // stays bounded and at rest. Distance is finite and within a sane band.
    f32 d = anchor_distance(bodies[0], bodies[1]);
    REQUIRE(d > kRest * 0.5f);
    REQUIRE(d < kRest + 1.0f);
    REQUIRE(max_speed(bodies) < 0.5f);
}
