// SPDX-License-Identifier: MIT
// Psynder physics unit tests — vehicle suspension ground contact.
//
// Regression coverage for the "car falls through the ground" bug: before the
// fix, vehicle_step never set wheel.on_ground / wheel.load_n and was never
// even invoked from World::step, so a chassis only saw gravity and sank
// without bound. This test drives the *public* physics + vehicle API (linked
// transitively into psynder_unit via psynder_editor_core) so it exercises the
// real solver path: per-wheel suspension rays against a flat ground plane,
// spring + damper normal load, and the World tick that runs the solver.
//
// DESIGN.md §10.1 — psynder_phys::vehicle ground-contact contract.

#include "physics/Physics.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <cmath>

using namespace psynder;
using Catch::Approx;

namespace {

// Build a sample-04-style 1500 kg box car with four corner wheels resting on
// a flat ground plane at y = 0. Returns the chassis + vehicle handles; the
// caller owns teardown.
struct Rig {
    physics::BodyId chassis;
    physics::vehicle::VehicleId veh;
};

Rig make_car(f32 start_com_y) {
    auto& world = physics::World::Get();
    world.set_gravity({0.0f, -9.81f, 0.0f});

    physics::BodyDesc cd{};
    cd.shape = physics::Shape::Box;
    cd.mass = 1500.0f;
    cd.position = {0.0f, start_com_y, 0.0f};
    cd.half_extent = {2.1f, 0.55f, 0.9f};
    cd.friction = 0.5f;
    const physics::BodyId chassis = world.create_body(cd);

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
    vd.chassis = chassis;
    vd.wheels = std::span<const physics::vehicle::WheelDesc>(wheels.data(), wheels.size());
    vd.engine_max_torque = 450.0f;
    vd.drag_coefficient = 0.30f;
    const physics::vehicle::VehicleId veh = physics::vehicle::create(vd);
    physics::vehicle::set_ground_plane(veh, 0.0f);
    return {chassis, veh};
}

void teardown(const Rig& r) {
    physics::vehicle::destroy(r.veh);
    physics::World::Get().destroy_body(r.chassis);
}

// Analytic rest ride-height of the COM on a flat plane at y = 0:
//   per-wheel load Fz = m*g / 4, compression = Fz / k,
//   attach.y = (rest + radius) - compression, COM.y = attach.y - local.y.
constexpr f32 kRestComY = (0.30f + 0.35f) - (1500.0f * 9.81f / 4.0f) / 40000.0f - (-0.35f);

}  // namespace

// Advance the world by N fixed sub-ticks. We pass exactly kFixedDt per call so
// each call retires one sub-tick and leaves the interpolation alpha at 0; the
// reported position is then the body's true state at the last completed tick.
void step_ticks(physics::World& world, int n) {
    for (int i = 0; i < n; ++i)
        world.step(1.0f / 120.0f);
}

TEST_CASE("vehicle settles on flat ground", "[physics][vehicle][suspension]") {
    auto& world = physics::World::Get();

    // Start the COM above the rest height so the only thing that can keep it
    // off the floor is the suspension catching it — gravity alone (the bug)
    // would sink the chassis straight through the ground plane.
    const Rig rig = make_car(/*start_com_y*/ 1.5f);

    // ~2.5 s at 120 Hz lets the under-damped corner spring ring down.
    step_ticks(world, 300);
    const f32 y_a = world.get_position(rig.chassis).y;
    step_ticks(world, 6);
    const f32 y_b = world.get_position(rig.chassis).y;

    // 1. It did NOT fall through the ground (the bug): the COM stays well
    //    above the plane. Sinking would drive y strongly negative.
    REQUIRE(y_a > 0.5f);

    // 2. It settled near the analytic spring rest height.
    REQUIRE(y_a == Approx(kRestComY).margin(0.05f));

    // 3. It is at rest — the position has stopped changing (no runaway, no
    //    sustained oscillation, which would betray an anti-damped spring).
    REQUIRE(std::fabs(y_b - y_a) < 1e-3f);

    teardown(rig);
}

TEST_CASE("vehicle in the air gets no suspension load", "[physics][vehicle][suspension]") {
    auto& world = physics::World::Get();

    // Start far above the ground so every wheel's suspension ray is out of
    // reach. With no contact the chassis must free-fall under gravity — and
    // accelerate, since nothing is holding it up.
    const Rig rig = make_car(/*start_com_y*/ 50.0f);

    const f32 y0 = world.get_position(rig.chassis).y;
    step_ticks(world, 30);
    const f32 y1 = world.get_position(rig.chassis).y;
    step_ticks(world, 30);
    const f32 y2 = world.get_position(rig.chassis).y;

    const f32 drop1 = y0 - y1;  // distance fallen in the first 30 ticks
    const f32 drop2 = y1 - y2;  // distance fallen in the next 30 ticks
    REQUIRE(drop1 > 0.0f);      // falling
    REQUIRE(drop2 > drop1);     // accelerating (gravity only, no support)
    REQUIRE(y2 < 49.0f);        // clearly descended, not held at the start

    teardown(rig);
}
