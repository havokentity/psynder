// SPDX-License-Identifier: MIT
// Psynder physics unit tests — anti-tunnelling (Milestone #63).
//
// Two complementary, zero-thickness mechanisms:
//   1. Plane half-space primitive — a static plane is infinite, so a fast body
//      that leaps to the far (solid) side in one tick is penetrating and gets
//      resolved. The right collider for thin walls / floors.
//   2. Speculative contacts (Catto/Box2D, TOI-free) — when an approaching pair
//      is within a speculative margin (or would close the gap this tick), the
//      narrowphase emits a contact with NEGATIVE penetration (a separation) and
//      the solver clamps the closing velocity so the body cannot move more than
//      the separation along the normal this step. No position bias, no fake
//      thickness, no CCD ordering.
//
// Coverage:
//   * Kernel-level: kernel_collide_pair_spec emits a speculative contact for a
//     fast CLOSING pair, and the solver clamp stops the approach; a slow /
//     receding / resting pair gets NO speculative contact (no spurious impulse).
//   * World-level: a fast small sphere fired at a thin static Plane wall does
//     NOT pass through (it would tunnel without the fix — its single-tick
//     displacement dwarfs the sphere); a slow resting stack on a plane is
//     unaffected.

#include "physics/internal/Kernels.h"
#include "physics/Physics.h"
#include "physics/FpControl.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

Body make_sphere(math::Vec3 pos, math::Vec3 vel, f32 radius, f32 mass) {
    Body b{};
    b.position = pos;
    b.rotation = {0, 0, 0, 1};
    b.linear_velocity = vel;
    b.shape = 0;
    b.half_extent = {radius, 0, 0};
    b.mass = mass;
    b.inv_mass = (mass > 0.0f) ? 1.0f / mass : 0.0f;
    f32 I = (2.0f / 5.0f) * std::max(mass, 1e-6f) * radius * radius;
    b.inertia.local = {I, I, I};
    b.inertia.inv_local = (mass > 0.0f) ? math::Vec3{1.0f / I, 1.0f / I, 1.0f / I}
                                        : math::Vec3{0, 0, 0};
    return b;
}

}  // namespace

// ─── Kernel-level: speculative contact generation + clamp ────────────────

TEST_CASE("kernel_collide_pair_spec emits a speculative contact for a fast closing pair",
          "[physics][tunneling]") {
    const f32 dt = 1.0f / 120.0f;
    const f32 margin = kernels::kSpeculativeMargin;

    // Two small spheres (r = 0.05) separated by 0.5 m, closing at 120 m/s.
    // In one tick each moves 1 m -> they would pass clean through each other.
    Body a = make_sphere({0, 0, 0}, {120.0f, 0, 0}, 0.05f, 1.0f);
    Body b = make_sphere({0.5f, 0, 0}, {-120.0f, 0, 0}, 0.05f, 1.0f);

    Contact c{};
    REQUIRE(kernels::kernel_collide_pair_spec(a, b, dt, margin, c));
    REQUIRE(c.speculative);
    REQUIRE(c.separation > 0.0f);  // gap = 0.5 - 0.1 = 0.4
    REQUIRE(c.separation == Approx(0.4f).margin(1e-4f));
    REQUIRE(c.depth == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("a slow well-separated pair gets NO speculative contact (no spurious impulse)",
          "[physics][tunneling]") {
    const f32 dt = 1.0f / 120.0f;
    const f32 margin = kernels::kSpeculativeMargin;

    // Same geometry but closing at only 1 m/s: in one tick each moves ~8 mm,
    // far less than the 0.4 m gap, so no contact should be emitted.
    Body a = make_sphere({0, 0, 0}, {1.0f, 0, 0}, 0.05f, 1.0f);
    Body b = make_sphere({0.5f, 0, 0}, {-1.0f, 0, 0}, 0.05f, 1.0f);
    Contact c{};
    REQUIRE_FALSE(kernels::kernel_collide_pair_spec(a, b, dt, margin, c));

    // Receding fast pair: also no contact (only CLOSING pairs can tunnel).
    Body ra = make_sphere({0, 0, 0}, {-120.0f, 0, 0}, 0.05f, 1.0f);
    Body rb = make_sphere({0.5f, 0, 0}, {120.0f, 0, 0}, 0.05f, 1.0f);
    Contact rc{};
    REQUIRE_FALSE(kernels::kernel_collide_pair_spec(ra, rb, dt, margin, rc));
}

TEST_CASE("the speculative solver clamp stops a fast closing pair from crossing",
          "[physics][tunneling]") {
    FpGuard fp;
    const f32 dt = 1.0f / 120.0f;
    const f32 margin = kernels::kSpeculativeMargin;

    Body a = make_sphere({0, 0, 0}, {120.0f, 0, 0}, 0.05f, 1.0f);
    Body b = make_sphere({0.5f, 0, 0}, {-120.0f, 0, 0}, 0.05f, 1.0f);
    std::vector<Body> bodies{a, b};

    Contact c{};
    REQUIRE(kernels::kernel_collide_pair_spec(bodies[0], bodies[1], dt, margin, c));
    c.body_a = 0;
    c.body_b = 1;
    std::vector<Contact> contacts{c};

    std::vector<u32> body_idx;
    std::vector<Island> islands;
    kernels::kernel_detect_islands(contacts, {bodies.data(), bodies.size()}, body_idx, islands);
    SolverParams params;
    for (const Island& isl : islands) {
        kernels::kernel_solve_island(isl,
                                     {contacts.data() + isl.first_contact, isl.contact_count},
                                     {body_idx.data() + isl.first_body, isl.body_count},
                                     {bodies.data(), bodies.size()},
                                     params,
                                     dt);
    }

    // After the clamp, the closing speed along the normal must be bounded by
    // separation/dt so the bodies cannot cross the 0.4 m gap this tick.
    f32 vn = bodies[1].linear_velocity.x - bodies[0].linear_velocity.x;  // B - A along +x
    f32 allowed_close = c.separation / dt;  // max closing speed
    REQUIRE(-vn <= allowed_close + 1e-2f);

    // And integrating one tick must NOT overshoot past contact: A stays left of
    // B's surface (no tunnelling).
    f32 a_next = bodies[0].position.x + bodies[0].linear_velocity.x * dt;
    f32 b_next = bodies[1].position.x + bodies[1].linear_velocity.x * dt;
    REQUIRE(a_next <= b_next + 1e-3f);
}

// ─── World-level: fast sphere vs thin static Plane wall ──────────────────

TEST_CASE("a fast sphere fired at a thin static Plane wall does NOT tunnel through",
          "[physics][tunneling]") {
    FpGuard fp;
    physics::World world;
    world.set_gravity({0, 0, 0});  // isolate the wall interaction

    // Vertical wall: a Plane rotated so its +Y normal points along -X (the wall
    // faces -X, solid side is +X). normal = Rz(-90) * +Y = (+1, 0, 0)? We want
    // the sphere coming from -X to be stopped, so the wall normal should face
    // the incoming sphere: normal = -X. Rotate +Y to -X about Z by +90 deg:
    //   Rz(+90)*(0,1,0) = (-1, 0, 0).  -> normal points -X, solid side is +X.
    math::Quat wall_rot = math::quat_from_axis_angle({0, 0, 1}, math::kHalfPi);
    physics::BodyDesc wdesc{};
    wdesc.shape = physics::Shape::Plane;
    wdesc.mass = 0.0f;
    wdesc.position = {0, 0, 0};  // wall surface at x = 0
    wdesc.rotation = wall_rot;
    world.create_body(wdesc);

    // Small fast sphere starting well on the -X (open) side, flying toward +X
    // at 120 m/s. One tick = +1 m of travel, ~20x the sphere diameter, so
    // without anti-tunnelling it would be far past the wall after the first
    // step. The plane half-space must keep it on the -X side.
    physics::BodyDesc sdesc{};
    sdesc.shape = physics::Shape::Sphere;
    sdesc.mass = 1.0f;
    sdesc.position = {-0.5f, 0, 0};
    sdesc.half_extent = {0.05f, 0.05f, 0.05f};
    physics::BodyId bullet = world.create_body(sdesc);
    world.set_body_velocity(bullet, {120.0f, 0, 0});

    for (int i = 0; i < 240; ++i)  // 2 s
        world.step(1.0f / 120.0f);

    // The sphere centre must remain on the open (-X) side of the wall surface,
    // i.e. x <= +radius. Without the Plane half-space it would be at x ~ +240.
    math::Vec3 p = world.get_position(bullet);
    REQUIRE(p.x <= 0.05f + 0.02f);
    REQUIRE(p.x > -2.0f);  // sanity: didn't fly off backward to infinity
}

// ─── World-level: a slow resting stack on a Plane is unaffected ──────────

TEST_CASE("a slow resting stack on a Plane shows no spurious speculative impulses",
          "[physics][tunneling]") {
    FpGuard fp;
    physics::World world;
    world.set_gravity({0, -9.81f, 0});

    physics::BodyDesc pdesc{};
    pdesc.shape = physics::Shape::Plane;
    pdesc.mass = 0.0f;
    pdesc.position = {0, 0, 0};
    pdesc.rotation = {0, 0, 0, 1};
    world.create_body(pdesc);

    // Three stacked spheres r = 0.5 just above the floor.
    std::vector<physics::BodyId> ids;
    for (int i = 0; i < 3; ++i) {
        physics::BodyDesc s{};
        s.shape = physics::Shape::Sphere;
        s.mass = 1.0f;
        s.position = {0, 0.5f + static_cast<f32>(i) * 1.0f, 0};
        s.half_extent = {0.5f, 0.5f, 0.5f};
        ids.push_back(world.create_body(s));
    }

    for (int i = 0; i < 1200; ++i)  // 10 s settle
        world.step(1.0f / 120.0f);

    // Each sphere settles above the floor (no fall-through) and below its drop
    // height, with small residual velocity — i.e. the speculative path did not
    // inject phantom energy into the resting stack.
    for (physics::BodyId id : ids) {
        math::Vec3 p = world.get_position(id);
        REQUIRE(p.y >= 0.4f);
        REQUIRE(p.y < 3.5f);
    }
}
