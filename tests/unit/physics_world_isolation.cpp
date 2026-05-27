// SPDX-License-Identifier: MIT
// Psynder physics unit tests — multi-instance World isolation.
//
// The `World` is instance-owned (PIMPL over detail::WorldImpl, which holds the
// rigid-body state PLUS the vehicle and character sub-worlds). Two distinct
// `World` instances must therefore share NO state: bodies/vehicles/characters
// created in one must be invisible to the other, and stepping one must not
// perturb the other. This is the whole point of the refactor (multi-scene,
// deterministic test isolation, isolated Play sessions, future networking), so
// it gets its own test.
//
// Drives the *public* physics API (linked transitively into psynder_unit) so
// the real create/step/destroy paths run on each owned world.

#include "physics/Physics.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <cmath>
#include <span>

using namespace psynder;
using Catch::Approx;

namespace {

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

TEST_CASE("two World instances hold independent body state", "[physics][world][isolation]") {
    physics::World a;
    physics::World b;

    // Different gravity per world: A falls, B has none.
    a.set_gravity({0.0f, -9.81f, 0.0f});
    b.set_gravity({0.0f, 0.0f, 0.0f});

    // A body created in A must NOT exist in B and vice versa: the raw handle
    // values may collide (both start at slot 0 / gen 1), but each world only
    // resolves handles against its OWN store. We assert by behaviour — the
    // body in A is subject to A's gravity, the body in B sits still under B's.
    const physics::BodyId ba = make_dynamic_sphere(a, {0.0f, 10.0f, 0.0f}, 1.0f);
    const physics::BodyId bb = make_dynamic_sphere(b, {0.0f, 10.0f, 0.0f}, 1.0f);
    REQUIRE(ba.valid());
    REQUIRE(bb.valid());

    const f32 a_y0 = a.get_position(ba).y;
    const f32 b_y0 = b.get_position(bb).y;
    REQUIRE(a_y0 == Approx(10.0f).margin(1e-4f));
    REQUIRE(b_y0 == Approx(10.0f).margin(1e-4f));

    // Step ONLY world A many ticks. B is never stepped here.
    step_ticks(a, 180);  // ~1.5 s of free fall (≈11 m, clears the 10 m start)

    const f32 a_y1 = a.get_position(ba).y;
    const f32 b_y1 = b.get_position(bb).y;

    // A's body fell under A's gravity (well below its start height).
    REQUIRE(a_y1 < 5.0f);
    // B's body did NOT move at all — stepping A leaked nothing into B.
    REQUIRE(b_y1 == Approx(10.0f).margin(1e-4f));

    // Now step ONLY B. With zero gravity and no contacts it stays put, while
    // A keeps its already-fallen position (we don't step A again).
    const f32 a_y_frozen = a.get_position(ba).y;
    step_ticks(b, 120);
    REQUIRE(b.get_position(bb).y == Approx(10.0f).margin(1e-4f));
    REQUIRE(a.get_position(ba).y == Approx(a_y_frozen).margin(1e-4f));

    // The handle from A resolves to nothing in B: reading ba's slot against B
    // either hits B's own (un-fallen) body or an invalid slot, never A's. Since
    // both worlds happen to use slot 0/gen 1, this reads B's still body — which
    // is at 10, NOT A's fallen value. Concretely: cross-world reads never see
    // A's fall.
    REQUIRE(b.get_position(ba).y != Approx(a_y1).margin(1e-3f));
}

TEST_CASE("two World instances hold independent vehicle + character state",
          "[physics][world][isolation][vehicle][character]") {
    physics::World a;
    physics::World b;
    a.set_gravity({0.0f, -9.81f, 0.0f});
    b.set_gravity({0.0f, -9.81f, 0.0f});

    // ── Vehicles: build a chassis + car in A only ────────────────────────
    physics::BodyDesc cd{};
    cd.shape = physics::Shape::Box;
    cd.mass = 1500.0f;
    cd.position = {0.0f, 1.0f, 0.0f};
    cd.half_extent = {2.1f, 0.55f, 0.9f};
    const physics::BodyId chassis_a = a.create_body(cd);

    std::array<physics::vehicle::WheelDesc, 4> wheels{};
    const f32 wx = 1.45f, wz = 0.85f, wy = -0.35f;
    wheels[0].local_position = {-wx, wy, wz};
    wheels[1].local_position = {-wx, wy, -wz};
    wheels[2].local_position = {wx, wy, wz};
    wheels[3].local_position = {wx, wy, -wz};
    for (auto& w : wheels) {
        w.radius = 0.35f;
        w.suspension = 0.30f;
        w.stiffness = 40000.0f;
        w.damping = 4800.0f;
    }
    physics::vehicle::VehicleDesc vd{};
    vd.chassis = chassis_a;
    vd.wheels = std::span<const physics::vehicle::WheelDesc>(wheels.data(), wheels.size());
    vd.engine_max_torque = 450.0f;
    vd.drag_coefficient = 0.30f;

    // Create the vehicle in A; B has NO vehicle.
    const physics::vehicle::VehicleId veh_a = physics::vehicle::create(vd, a);
    physics::vehicle::set_ground_plane(veh_a, 0.0f, a);
    REQUIRE(veh_a.valid());

    // Step both. A's chassis is held up by its suspension (vehicle solver runs
    // on A's vehicle sub-world). B has no vehicle, so nothing vehicle-related
    // can run in B even though we step it identically.
    step_ticks(a, 200);
    step_ticks(b, 200);

    // A's chassis settled near the suspension rest height (clearly above 0,
    // proving A's vehicle solver ran on A's own sub-world).
    REQUIRE(a.get_position(chassis_a).y > 0.3f);

    // ── Characters: create in B only ─────────────────────────────────────
    physics::character::CharacterDesc chd{};
    chd.position = {5.0f, 0.0f, 0.0f};
    chd.height = 1.8f;
    chd.radius = 0.35f;
    const physics::character::CharacterId chr_b = physics::character::create(chd, b);
    REQUIRE(chr_b.valid());

    // Move the character in B; its position updates in B's character store.
    physics::character::move(chr_b, {1.0f, 0.0f, 0.0f}, 1.0f / 120.0f, b);
    const math::Vec3 pos_in_b = physics::character::get_position(chr_b, b);
    REQUIRE(pos_in_b.x == Approx(6.0f).margin(1e-3f));

    // The SAME handle resolved against world A must NOT find that character —
    // A's character sub-world is empty, so it returns the zero sentinel.
    const math::Vec3 pos_in_a = physics::character::get_position(chr_b, a);
    REQUIRE(pos_in_a.x == Approx(0.0f).margin(1e-4f));
    REQUIRE(pos_in_a.y == Approx(0.0f).margin(1e-4f));
    REQUIRE(pos_in_a.z == Approx(0.0f).margin(1e-4f));

    // Cleanup (also exercises destroy on the owned worlds).
    physics::vehicle::destroy(veh_a, a);
    a.destroy_body(chassis_a);
    physics::character::destroy(chr_b, b);
}
