// SPDX-License-Identifier: MIT
// Psynder physics unit tests — capsule resting MANIFOLD (ADR-016, Wave 9-3).
//
// The capsule narrowphase used to emit a SINGLE contact per step. A capsule
// lying on its side (or two parallel capsules, or a capsule on a box) then has
// no second point to resist a rocking torque, so it tips / jitters / drifts as
// the single contact migrates tick to tick. CapsuleManifold.h clips the
// capsule's core segment against the other feature and emits TWO contacts (one
// at each end of the overlap) for the near-PARALLEL case, so the body rests
// flat and parallel stacks do not roll off. The non-parallel / end-on / point
// case keeps the legacy single point (no regression).
//
// Two layers:
//   * Kernel-level: the manifold builders return the right point count, point
//     positions, depths, and normals; parallel -> 2, point-like -> 1.
//   * World-level: through the REAL public step path, a side-lying capsule
//     settles flat and stable, stacked parallel capsules rest without rolling
//     off, an end-on hit still gets one contact, and the result is bit-for-bit
//     reproducible.

#include "physics/internal/Kernels.h"
#include "physics/internal/CapsuleManifold.h"
#include "physics/Physics.h"
#include "physics/FpControl.h"
#include "physics/Shape.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

// World axis the capsule's local +Y maps to, given a rotation. For a side-lying
// capsule this is ~horizontal so |y| ~ 0; a tipped capsule has |y| growing.
f32 axis_tilt_y(const physics::World& w, physics::BodyId b) {
    math::Quat q = w.get_rotation(b);
    math::Vec3 ax = physics::detail::quat_rotate(q, {0, 1, 0});
    return std::fabs(ax.y);
}

// A capsule lying on its side: local +Y axis -> world X (90 deg about Z).
math::Quat side_rot() {
    return math::quat_from_axis_angle({0, 0, 1}, math::kHalfPi);
}

}  // namespace

// ─── Kernel-level: plane-capsule manifold ────────────────────────────────────

TEST_CASE("plane-capsule manifold: side-lying capsule emits two end contacts",
          "[physics][capsule][manifold]") {
    // Capsule radius 0.3, half-height 0.8, axis along world X, centre 0.29 above
    // a y=0 floor -> 0.01 penetration along the whole length.
    kernels::CapsuleManifold m = kernels::plane_capsule_manifold(
        {0, 1, 0}, 0.0f, {0, 0.29f, 0}, side_rot(), 0.3f, 0.8f);

    REQUIRE(m.count == 2);
    // Both points sit on the surface (y == 0), at the two endpoints (x = +-0.8),
    // with the +Y plane normal and equal depth.
    f32 xs[2] = {m.pts[0].point.x, m.pts[1].point.x};
    REQUIRE(std::fabs(std::fabs(xs[0]) - 0.8f) < 1e-4f);
    REQUIRE(std::fabs(std::fabs(xs[1]) - 0.8f) < 1e-4f);
    REQUIRE(xs[0] * xs[1] < 0.0f);  // opposite ends
    for (u32 i = 0; i < 2; ++i) {
        REQUIRE(m.pts[i].point.y == Approx(0.0f).margin(1e-4f));
        REQUIRE(m.pts[i].normal.y == Approx(1.0f).margin(1e-5f));
        REQUIRE(m.pts[i].depth == Approx(0.01f).margin(1e-4f));
    }
}

TEST_CASE("plane-capsule manifold: end-on (perpendicular) capsule keeps one point",
          "[physics][capsule][manifold]") {
    // Capsule UPRIGHT (axis along +Y, identity rotation): only the bottom cap
    // touches the floor -> a point-like contact, single point.
    kernels::CapsuleManifold m = kernels::plane_capsule_manifold(
        {0, 1, 0}, 0.0f, {0, 1.09f, 0}, {0, 0, 0, 1}, 0.3f, 0.8f);
    REQUIRE(m.count == 1);
    REQUIRE(m.pts[0].normal.y == Approx(1.0f).margin(1e-5f));
    REQUIRE(m.pts[0].depth == Approx(0.01f).margin(1e-3f));
}

TEST_CASE("plane-capsule manifold matches the legacy single-point kernel for the tip case",
          "[physics][capsule][manifold]") {
    // A capsule resting on one endpoint cap (upright) must produce the SAME
    // deepest-point contact the legacy kernel_plane_capsule emits.
    math::Vec3 pn{0, 1, 0};
    f32 pd = 0.0f;
    Contact legacy{};
    kernels::kernel_plane_capsule(pn, pd, {0, 1.05f, 0}, {0, 0, 0, 1}, 0.3f, 0.8f, legacy);

    kernels::CapsuleManifold m =
        kernels::plane_capsule_manifold(pn, pd, {0, 1.05f, 0}, {0, 0, 0, 1}, 0.3f, 0.8f);
    REQUIRE(m.count == 1);
    REQUIRE(m.pts[0].depth == Approx(legacy.depth).margin(1e-5f));
    REQUIRE(m.pts[0].point.y == Approx(legacy.point_world.y).margin(1e-5f));
    REQUIRE(m.pts[0].normal.y == Approx(legacy.normal_world.y).margin(1e-5f));
}

// ─── Kernel-level: capsule-capsule manifold ──────────────────────────────────

TEST_CASE("capsule-capsule manifold: parallel stack emits two end contacts",
          "[physics][capsule][manifold]") {
    math::Quat side = side_rot();
    // Two parallel capsules along X, one above the other, overlapping by 0.01.
    kernels::CapsuleManifold m = kernels::capsule_capsule_manifold(
        {0, 0.3f, 0}, side, 0.3f, 0.8f, {0, 0.89f, 0}, side, 0.3f, 0.8f);
    REQUIRE(m.count == 2);
    for (u32 i = 0; i < 2; ++i) {
        REQUIRE(m.pts[i].normal.y == Approx(1.0f).margin(1e-4f));  // lower -> upper
        REQUIRE(m.pts[i].depth == Approx(0.01f).margin(1e-3f));
        REQUIRE(std::fabs(std::fabs(m.pts[i].point.x) - 0.8f) < 1e-3f);
    }
    REQUIRE(m.pts[0].point.x * m.pts[1].point.x < 0.0f);  // opposite ends
}

TEST_CASE("capsule-capsule manifold: perpendicular (crossing) keeps a single point",
          "[physics][capsule][manifold]") {
    math::Quat side = side_rot();          // axis -> X
    math::Quat id{0, 0, 0, 1};             // axis -> Y
    kernels::CapsuleManifold m = kernels::capsule_capsule_manifold(
        {0, 0, 0}, side, 0.3f, 0.8f, {0, 0.5f, 0}, id, 0.3f, 0.8f);
    REQUIRE(m.count == 1);  // crossing X-axis and Y-axis capsules -> point contact
}

TEST_CASE("capsule-capsule manifold: non-overlapping projection (cap-to-cap) keeps one point",
          "[physics][capsule][manifold]") {
    math::Quat side = side_rot();
    // Two collinear parallel capsules whose cores meet end-to-end along X: the
    // projection overlap is empty, so this is a single cap-cap point contact.
    kernels::CapsuleManifold m = kernels::capsule_capsule_manifold(
        {0, 0, 0}, side, 0.3f, 0.8f, {2.05f, 0, 0}, side, 0.3f, 0.8f);
    REQUIRE(m.count <= 1);
}

// ─── Kernel-level: capsule-box manifold ──────────────────────────────────────

TEST_CASE("capsule-box manifold: capsule flat on a box face emits two end contacts",
          "[physics][capsule][manifold]") {
    math::Quat side = side_rot();  // axis -> X
    kernels::ManifoldPoint fb;
    fb.point = {0, 1.0f, 0};
    fb.normal = {0, -1, 0};  // capsule -> box (downward into box top)
    fb.depth = 0.01f;
    // Capsule lying on the top face (y=1) of a box [he 2,0.5,2] at y=0.5.
    kernels::CapsuleManifold m = kernels::capsule_box_manifold(
        {0, 1.29f, 0}, side, 0.3f, 0.8f,
        {0, 0.5f, 0}, {0, 0, 0, 1}, {2.0f, 0.5f, 2.0f},
        {0, -1, 0}, fb);
    REQUIRE(m.count == 2);
    for (u32 i = 0; i < 2; ++i) {
        REQUIRE(m.pts[i].normal.y == Approx(-1.0f).margin(1e-4f));  // capsule -> box
        REQUIRE(std::fabs(std::fabs(m.pts[i].point.x) - 0.8f) < 1e-3f);
        REQUIRE(m.pts[i].depth == Approx(0.01f).margin(2e-3f));
    }
    REQUIRE(m.pts[0].point.x * m.pts[1].point.x < 0.0f);
}

TEST_CASE("capsule-box manifold: capsule standing on a box keeps the single fallback point",
          "[physics][capsule][manifold]") {
    // Capsule UPRIGHT on the box top -> point contact; the box-manifold returns
    // the single fallback unchanged.
    kernels::ManifoldPoint fb;
    fb.point = {0, 1.0f, 0};
    fb.normal = {0, -1, 0};
    fb.depth = 0.02f;
    kernels::CapsuleManifold m = kernels::capsule_box_manifold(
        {0, 2.09f, 0}, {0, 0, 0, 1}, 0.3f, 0.8f,
        {0, 0.5f, 0}, {0, 0, 0, 1}, {2.0f, 0.5f, 2.0f},
        {0, -1, 0}, fb);
    REQUIRE(m.count == 1);
    REQUIRE(m.pts[0].depth == Approx(0.02f).margin(1e-5f));
    REQUIRE(m.pts[0].point.y == Approx(1.0f).margin(1e-5f));
}

// ─── World-level: a side-lying capsule settles FLAT and STABLE ───────────────

TEST_CASE("a side-lying capsule settles flat and stable on a plane (no rocking)",
          "[physics][capsule][manifold][world]") {
    physics::detail::FpGuard fp;
    physics::World world;
    world.set_gravity({0, -9.81f, 0});

    physics::BodyDesc pl{};
    pl.shape = physics::Shape::Plane;
    pl.mass = 0.0f;
    pl.position = {0, 0, 0};
    pl.rotation = {0, 0, 0, 1};
    world.create_body(pl);

    // Capsule lying on its side, dropped from a small height with a slight tilt
    // so the single-point version would migrate the contact to one end and rock.
    physics::BodyDesc cd{};
    cd.shape = physics::Shape::Capsule;
    cd.mass = 1.0f;
    cd.half_extent = {0.3f, 0.8f, 0.3f};
    math::Quat tilt = math::quat_from_axis_angle({1, 0, 0}, 0.10f);  // ~5.7 deg
    cd.rotation = math::quat_mul(tilt, side_rot());
    cd.position = {0, 0.6f, 0};
    physics::BodyId cap = world.create_body(cd);

    // Settle.
    for (int i = 0; i < 360; ++i)
        world.step(1.0f / 120.0f);

    // After settling it must lie FLAT (axis horizontal, |axis.y| ~ 0) and rest
    // at ~ radius above the floor.
    REQUIRE(axis_tilt_y(world, cap) < 0.02f);
    math::Vec3 p = world.get_position(cap);
    REQUIRE(p.y == Approx(0.3f).margin(0.04f));

    // Run MANY more steps: the body must STAY flat (no slow rocking / tipping)
    // and its tilt must not grow. A single-point contact lets it wander; the
    // manifold pins both ends.
    f32 tilt_before = axis_tilt_y(world, cap);
    f32 max_tilt = tilt_before;
    for (int i = 0; i < 1200; ++i) {
        world.step(1.0f / 120.0f);
        max_tilt = std::max(max_tilt, axis_tilt_y(world, cap));
    }
    REQUIRE(max_tilt < 0.02f);                 // stays horizontal over many steps
    REQUIRE(axis_tilt_y(world, cap) < 0.02f);  // still flat at the end
}

// ─── World-level: two stacked parallel capsules rest without rolling off ─────

TEST_CASE("two stacked parallel capsules rest without rolling off",
          "[physics][capsule][manifold][world]") {
    physics::detail::FpGuard fp;
    physics::World world;
    world.set_gravity({0, -9.81f, 0});

    physics::BodyDesc pl{};
    pl.shape = physics::Shape::Plane;
    pl.mass = 0.0f;
    pl.position = {0, 0, 0};
    pl.rotation = {0, 0, 0, 1};
    world.create_body(pl);

    math::Quat side = side_rot();  // axis -> X

    physics::BodyDesc c0{};
    c0.shape = physics::Shape::Capsule;
    c0.mass = 1.0f;
    c0.half_extent = {0.3f, 0.8f, 0.3f};
    c0.rotation = side;
    c0.position = {0, 0.3f, 0};
    physics::BodyId b0 = world.create_body(c0);

    physics::BodyDesc c1{};
    c1.shape = physics::Shape::Capsule;
    c1.mass = 1.0f;
    c1.half_extent = {0.3f, 0.8f, 0.3f};
    c1.rotation = side;
    c1.position = {0, 0.9f, 0};  // resting on top of b0
    physics::BodyId b1 = world.create_body(c1);

    f32 max_zdrift = 0.0f, max_roll = 0.0f;
    for (int i = 0; i < 600; ++i) {
        world.step(1.0f / 120.0f);
        max_zdrift = std::max(max_zdrift, std::fabs(world.get_position(b1).z));
        max_roll = std::max(max_roll, axis_tilt_y(world, b1));
    }

    math::Vec3 p0 = world.get_position(b0);
    math::Vec3 p1 = world.get_position(b1);
    // Neither capsule rolls off: small lateral X drift (the single-point version
    // slid > 0.2 m), no Z drift, and the top stays parallel (no roll).
    REQUIRE(std::fabs(p0.x) < 0.06f);
    REQUIRE(std::fabs(p1.x) < 0.06f);
    REQUIRE(max_zdrift < 0.02f);
    REQUIRE(max_roll < 0.05f);
    // The top capsule stays stacked above the bottom one (did not roll off).
    REQUIRE(p1.y > p0.y + 0.4f);
    REQUIRE(p1.y == Approx(0.9f).margin(0.06f));
}

// ─── World-level: an end-on hit still gets a single contact (no regression) ──

TEST_CASE("an upright capsule dropped end-on a plane still settles on its cap",
          "[physics][capsule][manifold][world]") {
    physics::detail::FpGuard fp;
    physics::World world;
    world.set_gravity({0, -9.81f, 0});

    physics::BodyDesc pl{};
    pl.shape = physics::Shape::Plane;
    pl.mass = 0.0f;
    pl.position = {0, 0, 0};
    pl.rotation = {0, 0, 0, 1};
    world.create_body(pl);

    // Upright capsule (axis +Y) dropped straight down -> end-on, point contact.
    physics::BodyDesc cd{};
    cd.shape = physics::Shape::Capsule;
    cd.mass = 1.0f;
    cd.half_extent = {0.3f, 0.8f, 0.3f};
    cd.rotation = {0, 0, 0, 1};
    cd.position = {0, 2.0f, 0};
    physics::BodyId cap = world.create_body(cd);

    for (int i = 0; i < 600; ++i)
        world.step(1.0f / 120.0f);

    // The capsule centre rests at ~ (half_height + radius) = 1.1 above the floor
    // and stays upright (does not fall through, does not float).
    math::Vec3 p = world.get_position(cap);
    REQUIRE(p.y == Approx(1.1f).margin(0.06f));
    REQUIRE(p.y > 0.5f);  // not fallen through
}

// ─── Determinism: the manifold reproduces bit-for-bit run-to-run ─────────────

TEST_CASE("capsule manifold simulation is bit-for-bit deterministic",
          "[physics][capsule][manifold][world][determinism]") {
    auto run = []() {
        physics::detail::FpGuard fp;
        physics::World world;
        world.set_gravity({0, -9.81f, 0});

        physics::BodyDesc pl{};
        pl.shape = physics::Shape::Plane;
        pl.mass = 0.0f;
        pl.position = {0, 0, 0};
        pl.rotation = {0, 0, 0, 1};
        world.create_body(pl);

        math::Quat side = side_rot();

        physics::BodyDesc c0{};
        c0.shape = physics::Shape::Capsule;
        c0.mass = 1.0f;
        c0.half_extent = {0.3f, 0.8f, 0.3f};
        c0.rotation = side;
        c0.position = {0, 0.3f, 0};
        physics::BodyId b0 = world.create_body(c0);

        physics::BodyDesc c1{};
        c1.shape = physics::Shape::Capsule;
        c1.mass = 1.0f;
        c1.half_extent = {0.3f, 0.8f, 0.3f};
        c1.rotation = side;
        c1.position = {0.05f, 0.95f, 0.0f};  // slightly offset -> exercises clip
        physics::BodyId b1 = world.create_body(c1);

        for (int i = 0; i < 400; ++i)
            world.step(1.0f / 120.0f);

        math::Vec3 p0 = world.get_position(b0);
        math::Vec3 p1 = world.get_position(b1);
        math::Quat q1 = world.get_rotation(b1);
        return std::vector<f32>{p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, q1.x, q1.y, q1.z, q1.w};
    };

    std::vector<f32> a = run();
    std::vector<f32> b = run();
    std::vector<f32> c = run();
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        // Bit-for-bit: identical float patterns, not just approximately equal.
        REQUIRE(a[i] == b[i]);
        REQUIRE(a[i] == c[i]);
    }
}
