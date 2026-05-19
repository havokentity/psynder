// SPDX-License-Identifier: MIT
// Psynder physics unit tests — narrowphase kernels (DESIGN.md §10.1).
//
// Pulls collision kernels directly from the header-only `internal/Kernels.h`
// so the test compiles without `psynder_physics` linkage. Same pattern the
// audio lane uses for its mixer-core tests (see audio_mixer.cpp).

#include "physics/internal/Kernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

TEST_CASE("sphere-sphere narrowphase detects overlap and reports correct depth",
          "[physics][narrowphase]") {
    Contact c;
    bool hit = kernels::kernel_sphere_sphere(
        {0, 0, 0}, 1.0f, {1.5f, 0, 0}, 1.0f, c);
    REQUIRE(hit);
    REQUIRE(c.depth == Approx(0.5f).margin(1e-5f));
    REQUIRE(c.normal_world.x == Approx(1.0f).margin(1e-5f));

    hit = kernels::kernel_sphere_sphere(
        {0, 0, 0}, 1.0f, {2.0f, 0, 0}, 1.0f, c);
    REQUIRE_FALSE(hit);
}

TEST_CASE("AABB-AABB picks minimum-translation axis", "[physics][narrowphase]") {
    math::Aabb a{{-1, -1, -1}, {1, 1, 1}};
    math::Aabb b{{-0.5f, -0.5f, 0.8f}, {0.5f, 0.5f, 2.0f}};
    Contact c;
    bool hit = kernels::kernel_aabb_aabb(a, b, c);
    REQUIRE(hit);
    REQUIRE(std::fabs(c.normal_world.x) < 1e-5f);
    REQUIRE(std::fabs(c.normal_world.y) < 1e-5f);
    REQUIRE(std::fabs(std::fabs(c.normal_world.z) - 1.0f) < 1e-5f);
    REQUIRE(c.depth == Approx(0.2f).margin(1e-5f));
}

TEST_CASE("capsule-capsule narrowphase handles parallel configurations",
          "[physics][narrowphase]") {
    Contact c;
    math::Quat id{0, 0, 0, 1};
    bool hit = kernels::kernel_capsule_capsule(
        {0, 0, 0}, id, 0.3f, 0.5f,
        {0.5f, 0, 0}, id, 0.3f, 0.5f, c);
    REQUIRE(hit);
    REQUIRE(c.depth == Approx(0.1f).margin(1e-4f));
}

TEST_CASE("sphere-capsule kernel works at any orientation",
          "[physics][narrowphase]") {
    Contact c;
    math::Quat rot = math::quat_from_axis_angle({0, 0, 1}, math::kHalfPi);
    // Capsule rotated 90 deg around Z: its axis lies along X. A sphere offset
    // along Y by less than the capsule's radius+sphere's radius should hit.
    bool hit = kernels::kernel_sphere_capsule(
        {0, 0.4f, 0}, 0.3f,
        {0, 0, 0}, rot, 0.3f, 1.0f,
        c);
    REQUIRE(hit);
    REQUIRE(c.depth > 0.0f);
}

TEST_CASE("GJK+EPA finds penetration for two overlapping axis-aligned boxes",
          "[physics][narrowphase][gjk]") {
    GjkSupport a{{0, 0, 0}, {0, 0, 0, 1}, /*Box*/2, {1, 1, 1}};
    GjkSupport b{{1.5f, 0, 0}, {0, 0, 0, 1}, 2, {1, 1, 1}};
    Contact c;
    bool hit = kernels::kernel_gjk_epa(a, b, c);
    REQUIRE(hit);
    REQUIRE(c.depth == Approx(0.5f).margin(1e-2f));
}

TEST_CASE("GJK+EPA reports no overlap when boxes are clearly apart",
          "[physics][narrowphase][gjk]") {
    GjkSupport a{{0, 0, 0}, {0, 0, 0, 1}, 2, {1, 1, 1}};
    GjkSupport b{{5, 0, 0}, {0, 0, 0, 1}, 2, {1, 1, 1}};
    Contact c;
    REQUIRE_FALSE(kernels::kernel_gjk_epa(a, b, c));
}

TEST_CASE("Sphere-sphere head-on closing speed reverses after solver pass",
          "[physics][solver][energy]") {
    // Two unit-mass spheres, e=1, approaching each other at 2 m/s. After one
    // solver pass with a contact just past the slop tolerance, the closing
    // velocity should flip sign (perfectly elastic collision). We sit the
    // spheres exactly at touching distance so the Baumgarte bias term stays
    // at zero — that bias is the position-correction pseudo-velocity and is
    // not part of the elastic-collision impulse contract.
    std::vector<Body> bodies(2);
    auto& A = bodies[0];
    auto& B = bodies[1];
    A.position = {-0.5f, 0, 0};
    A.linear_velocity = {2.0f, 0, 0};
    A.mass = 1.0f;
    A.inv_mass = 1.0f;
    A.inertia.local = A.inertia.inv_local = {1, 1, 1};
    A.rotation = {0, 0, 0, 1};
    A.restitution = 1.0f;
    A.shape = 0;
    A.half_extent = {0.5f, 0, 0};

    B = A;
    B.position = {0.5f, 0, 0};
    B.linear_velocity = {-2.0f, 0, 0};

    // Manually craft a contact at the touch plane with zero depth so the
    // Baumgarte bias term is zero (we're testing energy, not penetration
    // correction).
    Contact c{};
    c.body_a = 0;
    c.body_b = 1;
    c.normal_world = {1, 0, 0};
    c.depth        = 0.0f;
    c.point_world  = {0, 0, 0};

    std::vector<Contact> contacts{c};
    std::vector<u32> body_indices;
    std::vector<Island> islands;
    kernels::kernel_detect_islands(contacts,
        {bodies.data(), bodies.size()}, body_indices, islands);
    REQUIRE(islands.size() == 1);

    SolverParams params;
    f32 ke_before = 0.5f * (math::dot(A.linear_velocity, A.linear_velocity)
                          + math::dot(B.linear_velocity, B.linear_velocity));

    kernels::kernel_solve_island(islands[0],
        {contacts.data(), contacts.size()},
        {body_indices.data(), body_indices.size()},
        {bodies.data(), bodies.size()},
        params, 1.0f / 120.0f);

    f32 ke_after = 0.5f * (math::dot(A.linear_velocity, A.linear_velocity)
                         + math::dot(B.linear_velocity, B.linear_velocity));

    // Velocities should have reversed sign (perfectly elastic).
    REQUIRE(A.linear_velocity.x < 0.0f);
    REQUIRE(B.linear_velocity.x > 0.0f);
    // Energy must not grow beyond floating-point noise, and must be at
    // least 95% of the original (PGS converges to the analytic answer in
    // 8 iterations for this trivial 1-contact 1-DoF case).
    REQUIRE(ke_after <= ke_before * 1.001f);
    REQUIRE(ke_after >= ke_before * 0.95f);
}
