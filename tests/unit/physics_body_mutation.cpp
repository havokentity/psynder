// SPDX-License-Identifier: MIT
// Psynder physics unit tests — public body-mutation writers.
//
// Coverage for the sample-10 physgun support API on physics::World:
//   set_body_position  — teleport + zero velocity (grab / hold a body)
//   set_body_velocity  — overwrite linear velocity (release / fling)
//   apply_impulse      — v += J * inv_mass (mass-scaled throw)
//
// Drives the *public* physics API (linked transitively into psynder_unit via
// psynder_editor_core, same as physics_vehicle_suspension.cpp) so the real
// World::step path runs. Each setter is a plain field write, so a scripted
// call sequence is deterministic and -fno-fast-math friendly.
//
// DESIGN.md §10.8 — physgun grab/throw contract.

#include "physics/Physics.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

using namespace psynder;
using Catch::Approx;

namespace {

// One fixed sub-tick per call leaves the interpolation alpha at 0, so
// get_position reports the body's true state at the last completed tick.
void step_ticks(physics::World& world, int n) {
    for (int i = 0; i < n; ++i)
        world.step(1.0f / 120.0f);
}

physics::BodyId make_dynamic_sphere(physics::World& world, math::Vec3 pos, f32 mass) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Sphere;
    d.mass = mass;
    d.position = pos;
    d.half_extent = {0.5f, 0.5f, 0.5f};
    return world.create_body(d);
}

}  // namespace

TEST_CASE("set_body_position teleports the body and zeroes velocity", "[physics][mutation]") {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, -9.81f, 0.0f});

    // No ground — let it free-fall so it has non-zero velocity before we grab.
    const physics::BodyId id = make_dynamic_sphere(world, {0.0f, 20.0f, 0.0f}, 1.0f);
    step_ticks(world, 30);  // accrue downward velocity

    // Grab: snap to a hold point in front of an imaginary camera.
    const math::Vec3 hold{3.0f, 5.0f, -2.0f};
    world.set_body_position(id, hold);

    const math::Vec3 p0 = world.get_position(id);
    REQUIRE(p0.x == Approx(hold.x).margin(1e-5f));
    REQUIRE(p0.y == Approx(hold.y).margin(1e-5f));
    REQUIRE(p0.z == Approx(hold.z).margin(1e-5f));

    // Velocity was zeroed, so a single tick of gravity only adds g*dt of drop
    // (≈ 0.00068 m), not the accumulated free-fall speed. Confirms the writer
    // cleared the prior momentum.
    step_ticks(world, 1);
    const math::Vec3 p1 = world.get_position(id);
    const f32 dy = hold.y - p1.y;
    REQUIRE(dy < 0.01f);  // only one tick of gravity, not 31 ticks' worth

    world.destroy_body(id);
}

TEST_CASE("set_body_velocity overwrites the linear velocity", "[physics][mutation]") {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, 0.0f, 0.0f});  // isolate the throw from gravity

    const physics::BodyId id = make_dynamic_sphere(world, {0.0f, 10.0f, 0.0f}, 2.0f);

    // Throw straight along +X at 6 m/s.
    world.set_body_velocity(id, {6.0f, 0.0f, 0.0f});

    // Over 60 ticks (0.5 s) it should travel ~3 m in +X with no drift on Y/Z.
    // get_position interpolates by alpha (== 0 after exact-kFixedDt steps), so
    // it reports the body's pose at the *last completed* tick — one sub-tick
    // (6/120 = 0.05 m) behind the integrator. Hence the 0.1 m margin.
    const math::Vec3 start = world.get_position(id);
    step_ticks(world, 60);
    const math::Vec3 end = world.get_position(id);

    REQUIRE((end.x - start.x) == Approx(3.0f).margin(0.1f));
    REQUIRE(end.y == Approx(start.y).margin(1e-4f));
    REQUIRE(end.z == Approx(start.z).margin(1e-4f));

    world.destroy_body(id);
}

TEST_CASE("apply_impulse scales the velocity change by inverse mass", "[physics][mutation]") {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, 0.0f, 0.0f});

    // Same impulse on a light (1 kg) and a heavy (4 kg) body: the light body
    // must end up moving 4x as far, confirming v += J * inv_mass.
    const physics::BodyId light = make_dynamic_sphere(world, {0.0f, 0.0f, 0.0f}, 1.0f);
    const physics::BodyId heavy = make_dynamic_sphere(world, {0.0f, 0.0f, 50.0f}, 4.0f);

    const math::Vec3 J{8.0f, 0.0f, 0.0f};  // kg·m/s
    world.apply_impulse(light, J);
    world.apply_impulse(heavy, J);

    const math::Vec3 ls = world.get_position(light);
    const math::Vec3 hs = world.get_position(heavy);
    step_ticks(world, 60);
    const f32 light_dx = world.get_position(light).x - ls.x;
    const f32 heavy_dx = world.get_position(heavy).x - hs.x;

    REQUIRE(light_dx > 0.0f);
    REQUIRE(heavy_dx > 0.0f);
    REQUIRE(light_dx == Approx(heavy_dx * 4.0f).margin(0.05f));

    world.destroy_body(light);
    world.destroy_body(heavy);
}

TEST_CASE("body mutation writers ignore static bodies and stale handles", "[physics][mutation]") {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, -9.81f, 0.0f});

    // Static body (mass 0) must not move when a writer targets it.
    physics::BodyDesc sd{};
    sd.shape = physics::Shape::Box;
    sd.mass = 0.0f;
    sd.position = {7.0f, 1.0f, 7.0f};
    sd.half_extent = {0.5f, 0.5f, 0.5f};
    const physics::BodyId stat = world.create_body(sd);

    world.set_body_position(stat, {0.0f, 100.0f, 0.0f});
    world.set_body_velocity(stat, {50.0f, 0.0f, 0.0f});
    world.apply_impulse(stat, {50.0f, 0.0f, 0.0f});
    step_ticks(world, 10);
    const math::Vec3 sp = world.get_position(stat);
    REQUIRE(sp.x == Approx(7.0f).margin(1e-5f));
    REQUIRE(sp.y == Approx(1.0f).margin(1e-5f));
    REQUIRE(sp.z == Approx(7.0f).margin(1e-5f));

    // Stale handle: destroy then mutate — must be a silent no-op (no crash,
    // queries return the zero vector for a dead body).
    const physics::BodyId dyn = make_dynamic_sphere(world, {0.0f, 5.0f, 0.0f}, 1.0f);
    world.destroy_body(dyn);
    world.set_body_position(dyn, {1.0f, 1.0f, 1.0f});  // no-op
    world.set_body_velocity(dyn, {1.0f, 1.0f, 1.0f});  // no-op
    world.apply_impulse(dyn, {1.0f, 1.0f, 1.0f});      // no-op
    const math::Vec3 dp = world.get_position(dyn);
    REQUIRE(dp.x == Approx(0.0f).margin(1e-6f));
    REQUIRE(dp.y == Approx(0.0f).margin(1e-6f));
    REQUIRE(dp.z == Approx(0.0f).margin(1e-6f));

    world.destroy_body(stat);
}

// ─── Generation-safe handles (Fix 1) ─────────────────────────────────────

TEST_CASE("stale BodyId is rejected after destroy + recreate (no slot aliasing)",
          "[physics][handles]") {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, 0.0f, 0.0f});  // isolate from gravity

    // Create a body, remember its handle, then destroy it. The freed slot is
    // first on the free-list, so the very next create reuses it — but with a
    // bumped generation. The OLD handle must therefore NOT alias the new body.
    const physics::BodyId old_id = make_dynamic_sphere(world, {11.0f, 0.0f, 0.0f}, 1.0f);
    world.destroy_body(old_id);

    const physics::BodyId new_id = make_dynamic_sphere(world, {99.0f, 0.0f, 0.0f}, 1.0f);

    // The reused slot keeps the same low-24-bit index, but the generation in
    // the high bits differs, so the raw handle values must not be equal.
    REQUIRE(new_id.raw != old_id.raw);

    // The new body is live and reports its own position.
    const math::Vec3 np = world.get_position(new_id);
    REQUIRE(np.x == Approx(99.0f).margin(1e-5f));

    // The stale handle is rejected: query returns the zero sentinel (NOT the
    // new body's position — proving no aliasing).
    const math::Vec3 op = world.get_position(old_id);
    REQUIRE(op.x == Approx(0.0f).margin(1e-6f));
    REQUIRE(op.y == Approx(0.0f).margin(1e-6f));
    REQUIRE(op.z == Approx(0.0f).margin(1e-6f));

    // A mutator on the stale handle must NOT touch the new body either.
    world.set_body_position(old_id, {-50.0f, -50.0f, -50.0f});  // no-op
    world.apply_impulse(old_id, {1000.0f, 0.0f, 0.0f});         // no-op
    const math::Vec3 np2 = world.get_position(new_id);
    REQUIRE(np2.x == Approx(99.0f).margin(1e-5f));  // unchanged

    world.destroy_body(new_id);
}

TEST_CASE("double-destroy of a BodyId is a no-op and does not corrupt the free-list",
          "[physics][handles]") {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, 0.0f, 0.0f});

    const physics::BodyId a = make_dynamic_sphere(world, {1.0f, 0.0f, 0.0f}, 1.0f);
    world.destroy_body(a);
    world.destroy_body(a);  // second destroy must be a silent no-op

    // If the double-destroy had pushed the slot onto the free-list twice, the
    // next TWO creates would hand out the SAME slot index for two live bodies
    // (a UAF-class aliasing bug). Create two bodies and confirm they are
    // independent: moving one must not move the other.
    const physics::BodyId b = make_dynamic_sphere(world, {2.0f, 0.0f, 0.0f}, 1.0f);
    const physics::BodyId c = make_dynamic_sphere(world, {3.0f, 0.0f, 0.0f}, 1.0f);

    REQUIRE(b.raw != c.raw);
    REQUIRE((b.raw & 0x00FFFFFFu) != (c.raw & 0x00FFFFFFu));  // distinct slots

    world.set_body_position(b, {20.0f, 0.0f, 0.0f});
    const math::Vec3 bp = world.get_position(b);
    const math::Vec3 cp = world.get_position(c);
    REQUIRE(bp.x == Approx(20.0f).margin(1e-5f));
    REQUIRE(cp.x == Approx(3.0f).margin(1e-5f));  // c untouched

    world.destroy_body(b);
    world.destroy_body(c);
}
