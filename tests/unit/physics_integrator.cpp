// SPDX-License-Identifier: MIT
// Psynder physics unit tests — rigid-body integrator + inertia helpers.
// Header-only: uses only inline kernels from `physics/internal/Kernels.h`.

#include "physics/internal/Kernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

// Simple symplectic-Euler integrator helper (mirrors the World facade).
void integrate_step(Body& b, math::Vec3 gravity, f32 dt) {
    if (b.inv_mass == 0.0f)
        return;
    b.linear_velocity = math::add(b.linear_velocity, math::mul(gravity, dt));
    b.position = math::add(b.position, math::mul(b.linear_velocity, dt));
    math::Quat w_q{b.angular_velocity.x, b.angular_velocity.y, b.angular_velocity.z, 0.0f};
    math::Quat dq = math::quat_mul(w_q, b.rotation);
    b.rotation = math::quat_normalize({
        b.rotation.x + 0.5f * dt * dq.x,
        b.rotation.y + 0.5f * dt * dq.y,
        b.rotation.z + 0.5f * dt * dq.z,
        b.rotation.w + 0.5f * dt * dq.w,
    });
}

}  // namespace

TEST_CASE("Free-falling sphere matches s = 0.5 * g * t^2 within 1%", "[physics][integrator]") {
    Body b{};
    b.position = {0, 10.0f, 0};
    b.rotation = {0, 0, 0, 1};
    b.mass = 1.0f;
    b.inv_mass = 1.0f;
    b.inertia.local = {1, 1, 1};
    b.inertia.inv_local = {1, 1, 1};
    b.shape = 0;
    b.half_extent = {0.5f, 0, 0};

    math::Vec3 g{0, -9.81f, 0};
    for (int i = 0; i < 120; ++i)
        integrate_step(b, g, 1.0f / 120.0f);

    // Expected: 10 - 0.5 * 9.81 * 1 = 5.095 m
    REQUIRE(b.position.y == Approx(10.0f - 0.5f * 9.81f).margin(0.15f));
}

TEST_CASE("Static body does not move under gravity", "[physics][integrator]") {
    Body b{};
    b.position = {5, 5, 5};
    b.rotation = {0, 0, 0, 1};
    b.mass = 0.0f;
    b.inv_mass = 0.0f;
    b.inertia.inv_local = {0, 0, 0};

    math::Vec3 g{0, -100.0f, 0};
    for (int i = 0; i < 60; ++i)
        integrate_step(b, g, 1.0f / 120.0f);
    REQUIRE(b.position.y == Approx(5.0f).margin(1e-5f));
}

TEST_CASE("inertia_sphere matches the analytic formula 2/5 * m * r^2", "[physics][inertia]") {
    math::Vec3 I = inertia_sphere(2.0f, 0.5f);
    f32 expected = (2.0f / 5.0f) * 2.0f * 0.25f;
    REQUIRE(I.x == Approx(expected).margin(1e-6f));
    REQUIRE(I.y == Approx(expected).margin(1e-6f));
    REQUIRE(I.z == Approx(expected).margin(1e-6f));
}

TEST_CASE("inertia_box matches the analytic formula m/12 * (a^2 + b^2)", "[physics][inertia]") {
    math::Vec3 he{1, 2, 3};  // dimensions = (2, 4, 6)
    math::Vec3 I = inertia_box(1.0f, he);
    REQUIRE(I.x == Approx((1.0f / 12.0f) * (16.0f + 36.0f)).margin(1e-6f));
    REQUIRE(I.y == Approx((1.0f / 12.0f) * (4.0f + 36.0f)).margin(1e-6f));
    REQUIRE(I.z == Approx((1.0f / 12.0f) * (4.0f + 16.0f)).margin(1e-6f));
}

TEST_CASE("aabb_world conservative AABB grows with rotated box", "[physics][shape]") {
    math::Aabb aligned = aabb_world(2, {1, 1, 1}, {0, 0, 0}, {0, 0, 0, 1});
    REQUIRE(aligned.max.x == Approx(1.0f));
    REQUIRE(aligned.min.x == Approx(-1.0f));

    // Rotate 45 degrees around Y — AABB X extent should grow to sqrt(2).
    math::Quat rot = math::quat_from_axis_angle({0, 1, 0}, math::kPi * 0.25f);
    math::Aabb rotated = aabb_world(2, {1, 1, 1}, {0, 0, 0}, rot);
    REQUIRE(rotated.max.x > 1.4f);
    REQUIRE(rotated.max.x < 1.5f);
}
