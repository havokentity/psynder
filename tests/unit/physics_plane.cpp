// SPDX-License-Identifier: MIT
// Psynder physics unit tests — Plane half-space primitive (Milestone #63).
//
// The Plane is an INFINITE half-space whose surface normal is the body's local
// +Y rotated into world (`rotation * +Y`) and whose offset is
// d = dot(normal, position). The solid side is below the plane; a body
// penetrates when its support point along -normal dips below the surface.
// Because a static plane is infinite, NOTHING can tunnel through it: any body
// on the far (solid) side is by definition penetrating and gets resolved.
//
// Two layers of coverage:
//   * Kernel-level: the closed-form plane-vs-sphere/box/capsule signed-distance
//     kernels report the right separation / penetration depth / contact normal.
//   * World-level: a dynamic body dropped onto a Plane settles ON it (does not
//     fall through, no fake thickness) using the REAL public step path.

#include "physics/internal/Kernels.h"
#include "physics/Physics.h"
#include "physics/FpControl.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

// ─── Kernel-level: signed-distance plane narrowphase ─────────────────────

TEST_CASE("plane-vs-sphere reports penetration depth + up normal for a flat floor",
          "[physics][plane]") {
    // Flat floor: identity rotation -> normal = +Y, offset d = 0 (plane at y=0).
    math::Vec3 pn = plane_normal_world({0, 0, 0, 1});
    f32 pd = plane_offset_world(pn, {0, 0, 0});
    REQUIRE(pn.y == Approx(1.0f).margin(1e-6f));
    REQUIRE(pd == Approx(0.0f).margin(1e-6f));

    // Sphere radius 0.5 centred at y = 0.3 -> its lowest point dips to -0.2,
    // i.e. 0.2 below the surface. Separation -0.2, depth +0.2.
    Contact c{};
    f32 sep = kernels::kernel_plane_sphere(pn, pd, {0, 0.3f, 0}, 0.5f, c);
    REQUIRE(sep == Approx(-0.2f).margin(1e-5f));
    REQUIRE(c.depth == Approx(0.2f).margin(1e-5f));
    REQUIRE(c.normal_world.y == Approx(1.0f).margin(1e-5f));

    // A sphere clearly above the floor is separated (positive gap, no depth).
    f32 sep2 = kernels::kernel_plane_sphere(pn, pd, {0, 2.0f, 0}, 0.5f, c);
    REQUIRE(sep2 == Approx(1.5f).margin(1e-5f));
}

TEST_CASE("plane-vs-box finds the lowest corner of a rotated box", "[physics][plane]") {
    math::Vec3 pn = plane_normal_world({0, 0, 0, 1});
    f32 pd = plane_offset_world(pn, {0, 0, 0});

    // Axis-aligned unit box centred at y = 0.4 -> lowest face at -0.1.
    Contact c{};
    f32 sep = kernels::kernel_plane_box(pn, pd, {0, 0.4f, 0}, {0, 0, 0, 1}, {0.5f, 0.5f, 0.5f}, c);
    REQUIRE(sep == Approx(-0.1f).margin(1e-5f));
    REQUIRE(c.depth == Approx(0.1f).margin(1e-5f));

    // Box rotated 45 deg about Z: half-diagonal in XY is 0.5*sqrt(2) ~ 0.707.
    // Centre at y = 0.6 -> lowest corner at 0.6 - 0.707 = -0.107.
    math::Quat rz = math::quat_from_axis_angle({0, 0, 1}, math::kPi * 0.25f);
    f32 sep_rot = kernels::kernel_plane_box(pn, pd, {0, 0.6f, 0}, rz, {0.5f, 0.5f, 0.5f}, c);
    f32 expected = 0.6f - 0.5f * std::sqrt(2.0f);
    REQUIRE(sep_rot == Approx(expected).margin(1e-4f));
}

TEST_CASE("plane-vs-capsule uses the lower endpoint minus the radius", "[physics][plane]") {
    math::Vec3 pn = plane_normal_world({0, 0, 0, 1});
    f32 pd = plane_offset_world(pn, {0, 0, 0});

    // Upright capsule (axis along local Y), radius 0.3, half-height 0.5, centre
    // at y = 0.6. Lower endpoint at y = 0.1, minus radius -> surface at -0.2.
    Contact c{};
    f32 sep = kernels::kernel_plane_capsule(pn, pd, {0, 0.6f, 0}, {0, 0, 0, 1}, 0.3f, 0.5f, c);
    REQUIRE(sep == Approx(-0.2f).margin(1e-5f));
    REQUIRE(c.normal_world.y == Approx(1.0f).margin(1e-5f));
}

TEST_CASE("tilted plane derives its normal from the body rotation", "[physics][plane]") {
    // Plane rotated 30 deg about Z: normal tilts in the XY plane.
    math::Quat rz = math::quat_from_axis_angle({0, 0, 1}, math::kPi / 6.0f);
    math::Vec3 pn = plane_normal_world(rz);
    // normal = R * (0,1,0) -> (-sin30, cos30, 0) = (-0.5, 0.866, 0).
    REQUIRE(pn.x == Approx(-0.5f).margin(1e-4f));
    REQUIRE(pn.y == Approx(std::cos(math::kPi / 6.0f)).margin(1e-4f));
    REQUIRE(std::fabs(pn.z) < 1e-5f);
}

TEST_CASE("collide_pair dispatches Plane on either side with consistent A->B normal",
          "[physics][plane]") {
    Body plane{};
    plane.shape = kShapePlane;
    plane.position = {0, 0, 0};
    plane.rotation = {0, 0, 0, 1};
    plane.inv_mass = 0.0f;

    Body sphere{};
    sphere.shape = 0;
    sphere.position = {0, 0.3f, 0};  // dips 0.2 below y=0 surface
    sphere.rotation = {0, 0, 0, 1};
    sphere.half_extent = {0.5f, 0, 0};
    sphere.inv_mass = 1.0f;

    // (plane, sphere): A->B normal points from plane toward sphere == +Y.
    Contact c1{};
    REQUIRE(kernels::kernel_collide_pair(plane, sphere, c1));
    REQUIRE(c1.depth == Approx(0.2f).margin(1e-5f));
    REQUIRE(c1.normal_world.y == Approx(1.0f).margin(1e-5f));

    // (sphere, plane): A->B normal flips to point from sphere toward plane == -Y.
    Contact c2{};
    REQUIRE(kernels::kernel_collide_pair(sphere, plane, c2));
    REQUIRE(c2.depth == Approx(0.2f).margin(1e-5f));
    REQUIRE(c2.normal_world.y == Approx(-1.0f).margin(1e-5f));
}

TEST_CASE("Plane reports a very large world AABB so the broadphase always pairs it",
          "[physics][plane]") {
    math::Aabb box = aabb_world(kShapePlane, {0.5f, 0.5f, 0.5f}, {0, 0, 0}, {0, 0, 0, 1});
    REQUIRE(box.min.x < -1.0e8f);
    REQUIRE(box.max.x > 1.0e8f);
    // Must stay FINITE (NaN/inf in the SAP endpoint sort would be UB).
    REQUIRE(std::isfinite(box.min.x));
    REQUIRE(std::isfinite(box.max.x));
}

// ─── World-level: a body settles on a Plane half-space (no fall-through) ──

TEST_CASE("a dynamic sphere dropped onto a Plane half-space settles on it",
          "[physics][plane]") {
    FpGuard fp;
    physics::World world;
    world.set_gravity({0, -9.81f, 0});

    // Static Plane at y = 0 (identity rotation -> +Y normal, the floor).
    physics::BodyDesc pdesc{};
    pdesc.shape = physics::Shape::Plane;
    pdesc.mass = 0.0f;
    pdesc.position = {0, 0, 0};
    pdesc.rotation = {0, 0, 0, 1};
    world.create_body(pdesc);

    // Sphere radius 0.5 dropped from y = 3.
    physics::BodyDesc sdesc{};
    sdesc.shape = physics::Shape::Sphere;
    sdesc.mass = 1.0f;
    sdesc.position = {0, 3.0f, 0};
    sdesc.half_extent = {0.5f, 0.5f, 0.5f};
    physics::BodyId sphere = world.create_body(sdesc);

    for (int i = 0; i < 600; ++i)  // 5 s
        world.step(1.0f / 120.0f);

    // The sphere centre must rest at ~ +radius above the surface: ABOVE the
    // plane (not fallen through, y far below 0) and NOT floating on fake
    // thickness (y not well above the radius).
    math::Vec3 p = world.get_position(sphere);
    REQUIRE(p.y == Approx(0.5f).margin(0.05f));
}
