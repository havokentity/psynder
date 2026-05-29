// SPDX-License-Identifier: MIT
// Psynder physics unit tests -- public scene raycast (World::raycast).
//
// Coverage for the line-of-sight / bullet-hit query added to physics::World:
//   * nearest-of-two: a ray through two boxes returns the closer one
//   * max_t culling: a target beyond max_t is not hit
//   * ignore: the shooter's own body is skipped
//   * ray-vs-sphere: correct entry point + outward normal
//   * ray-vs-OBB: a ROTATED box reports the face hit point + world normal
//   * miss: a ray pointing away / parallel returns hit == false
//   * gen-safe: a destroyed body's reused slot is not struck through a stale id
//
// Each World here is its OWN instance (instance-owned PIMPL) so the cases are
// isolated from the process-wide default world and from one another. raycast is
// a pure const read -- no step() is needed; it sees the bodies' current pose.
//
// Drives the *public* physics API (linked transitively into psynder_unit via
// psynder_editor_core, same as physics_body_mutation.cpp). The query has no RNG
// and is -fno-fast-math friendly, so these expectations are deterministic.
//
// DESIGN.md 10.1 -- physics scene query contract.

#include "physics/Physics.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using Catch::Approx;

namespace {

physics::BodyId make_box(physics::World& w, math::Vec3 pos, math::Vec3 he,
                         math::Quat rot = {0, 0, 0, 1}, f32 mass = 0.0f) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Box;
    d.mass = mass;  // static by default; raycast hits static + dynamic alike
    d.position = pos;
    d.rotation = rot;
    d.half_extent = he;
    return w.create_body(d);
}

physics::BodyId make_sphere(physics::World& w, math::Vec3 pos, f32 radius,
                            f32 mass = 0.0f) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Sphere;
    d.mass = mass;
    d.position = pos;
    d.half_extent = {radius, radius, radius};
    return w.create_body(d);
}

}  // namespace

TEST_CASE("raycast returns the nearer of two boxes along the ray", "[physics][raycast]") {
    physics::World w;

    // Two unit boxes straight ahead on +X at 5 m and 10 m. A ray from the
    // origin along +X must report the near one, with t ~ (5 - 0.5) = 4.5.
    const physics::BodyId near_box = make_box(w, {5.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    const physics::BodyId far_box = make_box(w, {10.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});

    const auto hit = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(hit.hit);
    REQUIRE(hit.body.raw == near_box.raw);
    REQUIRE(hit.body.raw != far_box.raw);
    REQUIRE(hit.t == Approx(4.5f).margin(1e-4f));
    REQUIRE(hit.point.x == Approx(4.5f).margin(1e-4f));
    REQUIRE(hit.point.y == Approx(0.0f).margin(1e-4f));
    REQUIRE(hit.point.z == Approx(0.0f).margin(1e-4f));
    // The struck face is the -X face, so its outward normal points back at us.
    REQUIRE(hit.normal.x == Approx(-1.0f).margin(1e-4f));
    REQUIRE(hit.normal.y == Approx(0.0f).margin(1e-4f));
    REQUIRE(hit.normal.z == Approx(0.0f).margin(1e-4f));

    // A non-unit direction is normalised internally, so t stays a true metre
    // distance regardless of the input length.
    const auto hit2 = w.raycast({0.0f, 0.0f, 0.0f}, {7.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(hit2.hit);
    REQUIRE(hit2.t == Approx(4.5f).margin(1e-4f));
}

TEST_CASE("raycast culls targets beyond max_t", "[physics][raycast]") {
    physics::World w;
    make_box(w, {20.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});  // box front face at x=19.5

    // max_t shorter than the box: no hit.
    const auto miss = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f);
    REQUIRE_FALSE(miss.hit);

    // max_t past the front face: hit.
    const auto got = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 25.0f);
    REQUIRE(got.hit);
    REQUIRE(got.t == Approx(19.5f).margin(1e-4f));
}

TEST_CASE("raycast skips the ignore (shooter) body", "[physics][raycast]") {
    physics::World w;

    // Shooter's own body sits AT the origin; a real target is further along.
    const physics::BodyId shooter = make_box(w, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    const physics::BodyId target = make_box(w, {6.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});

    // Without ignore, the ray starting inside the shooter would hit it at t=0.
    const auto self = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(self.hit);
    REQUIRE(self.body.raw == shooter.raw);

    // With ignore == shooter, the query skips it and finds the target instead.
    const auto downrange = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, shooter);
    REQUIRE(downrange.hit);
    REQUIRE(downrange.body.raw == target.raw);
    REQUIRE(downrange.t == Approx(5.5f).margin(1e-4f));
}

TEST_CASE("raycast against a sphere reports the entry point and outward normal",
          "[physics][raycast]") {
    physics::World w;

    // Radius-2 sphere centred at (0, 0, -8); fire along -Z from the origin.
    const physics::BodyId s = make_sphere(w, {0.0f, 0.0f, -8.0f}, 2.0f);

    const auto hit = w.raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 100.0f);
    REQUIRE(hit.hit);
    REQUIRE(hit.body.raw == s.raw);
    // Entry at z = -8 + 2 = -6, i.e. t = 6.
    REQUIRE(hit.t == Approx(6.0f).margin(1e-4f));
    REQUIRE(hit.point.z == Approx(-6.0f).margin(1e-4f));
    // Outward normal on the near cap points back at the shooter (+Z).
    REQUIRE(hit.normal.x == Approx(0.0f).margin(1e-4f));
    REQUIRE(hit.normal.y == Approx(0.0f).margin(1e-4f));
    REQUIRE(hit.normal.z == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("raycast against a rotated box uses the oriented OBB", "[physics][raycast]") {
    physics::World w;

    // Box rotated 45 deg about Y. Its half-extents are 1 along each axis, so the
    // rotated corner reaches sqrt(2) ~ 1.4142 along +X. A ray along +X aimed at
    // the centre height must strike the rotated face, NOT the axis-aligned 1.0.
    const f32 ang = math::kPi * 0.25f;  // 45 degrees
    const math::Quat rot = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, ang);
    const physics::BodyId box = make_box(w, {6.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, rot);

    const auto hit = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(hit.hit);
    REQUIRE(hit.body.raw == box.raw);
    // Nearest face is at centre_x - sqrt(2) = 6 - 1.41421 = 4.58579.
    const f32 expect_t = 6.0f - std::sqrt(2.0f);
    REQUIRE(hit.t == Approx(expect_t).margin(1e-3f));
    REQUIRE(hit.point.x == Approx(expect_t).margin(1e-3f));
    // The struck face normal is the box's local -X axis rotated 45 deg about Y:
    // (-cos45, 0, +sin45) ~ (-0.7071, 0, 0.7071); it points back toward +X-ish.
    const f32 c = std::cos(ang);
    REQUIRE(hit.normal.x == Approx(-c).margin(1e-3f));
    REQUIRE(hit.normal.y == Approx(0.0f).margin(1e-3f));
    REQUIRE(std::fabs(hit.normal.z) == Approx(c).margin(1e-3f));
    REQUIRE(hit.normal.x < 0.0f);  // faces back toward the shooter
}

TEST_CASE("raycast returns no hit when the ray points away or runs parallel",
          "[physics][raycast]") {
    physics::World w;
    make_box(w, {5.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});

    // Pointing the opposite way (-X): the box is entirely behind the ray.
    const auto away = w.raycast({0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE_FALSE(away.hit);

    // Parallel along +Z, offset on Y above the box: the slab test rejects it.
    const auto parallel = w.raycast({0.0f, 10.0f, -5.0f}, {0.0f, 0.0f, 1.0f}, 100.0f);
    REQUIRE_FALSE(parallel.hit);

    // Degenerate zero-length direction can never hit.
    const auto degenerate = w.raycast({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE_FALSE(degenerate.hit);
}

TEST_CASE("raycast does not strike a destroyed body's slot (gen-safe)",
          "[physics][raycast]") {
    physics::World w;

    // One box directly ahead. Destroy it: the ray must now miss entirely (the
    // slot is a hole, skipped by the alive check).
    const physics::BodyId victim = make_box(w, {5.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    {
        const auto before = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
        REQUIRE(before.hit);
        REQUIRE(before.body.raw == victim.raw);
    }
    w.destroy_body(victim);
    {
        const auto after = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
        REQUIRE_FALSE(after.hit);
    }

    // Recreate a body: it reuses the freed slot with a BUMPED generation. The
    // returned hit handle must equal the NEW id (not the stale victim handle),
    // proving the encoded gen tracks the live slot.
    const physics::BodyId reused = make_box(w, {5.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f});
    REQUIRE(reused.raw != victim.raw);  // same index, bumped gen
    const auto reborn = w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(reborn.hit);
    REQUIRE(reborn.body.raw == reused.raw);
    REQUIRE(reborn.body.raw != victim.raw);

    // A stale ignore handle (the old victim id) must NOT blanket-skip the reused
    // slot: the live body is still found.
    const auto with_stale_ignore =
        w.raycast({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f, victim);
    REQUIRE(with_stale_ignore.hit);
    REQUIRE(with_stale_ignore.body.raw == reused.raw);
}
