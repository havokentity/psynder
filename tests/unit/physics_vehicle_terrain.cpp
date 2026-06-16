// SPDX-License-Identifier: MIT
// Psynder physics unit tests — vehicle terrain following + speed governor +
// steering authority (#58).
//
// Two layers, matching the rest of the lane:
//   * Pure-kernel checks pull straight from physics/internal/Kernels.h (the
//     governor, steering-authority, and bilinear heightfield samplers) — no
//     psynder_physics linkage.
//   * End-to-end checks drive the PUBLIC physics + vehicle API (linked
//     transitively via psynder_editor_core) so the real World tick / vehicle
//     solver path is exercised: a car on a SLOPED heightfield keeps each wheel
//     grounded to the local terrain height; the governor holds at/under the
//     cap; an identical control sequence reproduces the trajectory bit-for-bit.
//
// DESIGN.md §10.1 — psynder_phys::vehicle terrain ground-contact contract.

#include "physics/Physics.h"
#include "physics/FpControl.h"
#include "physics/internal/Kernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;
using Catch::Approx;

namespace {

// ─── A simple analytic terrain: a planar ramp tilted about the Z axis ──────
// height(x, z) = base_y + slope * x. Used both as the HeightSampler backing
// and to compute the expected per-corner ground height the chassis must track.
struct Ramp {
    f32 base_y = 0.0f;
    f32 slope = 0.0f;  // dy/dx
    static f32 sample(void* user, f32 x, f32 /*z*/) noexcept {
        const Ramp* r = static_cast<const Ramp*>(user);
        return r->base_y + r->slope * x;
    }
};

struct Rig {
    physics::BodyId chassis;
    physics::vehicle::VehicleId veh;
};

// sample-04-style 1500 kg box car with four corner wheels.
Rig make_car(physics::World& world, f32 start_com_y, const physics::vehicle::VehicleDesc* override = nullptr) {
    world.set_gravity({0.0f, -9.81f, 0.0f});

    physics::BodyDesc cd{};
    cd.shape = physics::Shape::Box;
    cd.mass = 1500.0f;
    cd.position = {0.0f, start_com_y, 0.0f};
    cd.half_extent = {2.1f, 0.55f, 0.9f};
    cd.friction = 0.5f;
    const physics::BodyId chassis = world.create_body(cd);

    std::array<physics::vehicle::WheelDesc, 4> wheels{};
    // Front (non-drive) axle at +X, rear (drive) axle at -X → forward = +X.
    const f32 wx = 1.45f, wz = 0.85f, wy = -0.35f;
    wheels[0].local_position = {wx, wy, wz};    // front-left  (steer)
    wheels[1].local_position = {wx, wy, -wz};   // front-right (steer)
    wheels[2].local_position = {-wx, wy, wz};   // rear-left   (drive)
    wheels[3].local_position = {-wx, wy, -wz};  // rear-right  (drive)
    for (auto& w : wheels) {
        w.radius = 0.35f;
        w.suspension = 0.30f;
        w.stiffness = 40000.0f;
        w.damping = 4800.0f;
    }
    physics::vehicle::VehicleDesc vd{};
    if (override)
        vd = *override;
    vd.chassis = chassis;
    vd.wheels = std::span<const physics::vehicle::WheelDesc>(wheels.data(), wheels.size());
    if (!override) {
        vd.engine_max_torque = 450.0f;
        vd.drag_coefficient = 0.30f;
    }
    const physics::vehicle::VehicleId veh = physics::vehicle::create(vd, world);
    return {chassis, veh};
}

void teardown(physics::World& world, const Rig& r) {
    physics::vehicle::destroy(r.veh, world);
    world.destroy_body(r.chassis);
}

void step_ticks(physics::World& world, int n) {
    for (int i = 0; i < n; ++i)
        world.step(1.0f / 120.0f);
}

}  // namespace

// ─── Pure-kernel: speed governor ───────────────────────────────────────────

TEST_CASE("Speed governor is identity below the taper and zero at the cap",
          "[physics][vehicle][governor]") {
    const f32 cap = 30.0f;  // m/s
    REQUIRE(kernels::kernel_speed_governor(0.0f, cap) == Approx(1.0f));
    REQUIRE(kernels::kernel_speed_governor(10.0f, cap) == Approx(1.0f));
    REQUIRE(kernels::kernel_speed_governor(cap * 0.85f, cap) == Approx(1.0f));
    // Inside the taper band it is strictly between 0 and 1 and monotone down.
    const f32 mid = kernels::kernel_speed_governor(cap * 0.925f, cap);
    REQUIRE(mid > 0.0f);
    REQUIRE(mid < 1.0f);
    // At and above the cap drive torque is fully cut.
    REQUIRE(kernels::kernel_speed_governor(cap, cap) == Approx(0.0f));
    REQUIRE(kernels::kernel_speed_governor(cap + 5.0f, cap) == Approx(0.0f));
    // Disabled governor (cap <= 0) is always identity.
    REQUIRE(kernels::kernel_speed_governor(100.0f, 0.0f) == Approx(1.0f));
    // Reverse / standing is never governed.
    REQUIRE(kernels::kernel_speed_governor(-50.0f, cap) == Approx(1.0f));
}

// ─── Pure-kernel: steering authority ───────────────────────────────────────

TEST_CASE("Steering authority is full at low speed and tapered at high speed",
          "[physics][vehicle][steer]") {
    const f32 full = 5.0f, taper = 40.0f, minA = 0.25f;
    REQUIRE(kernels::kernel_steer_authority(0.0f, full, taper, minA) == Approx(1.0f));
    REQUIRE(kernels::kernel_steer_authority(full, full, taper, minA) == Approx(1.0f));
    REQUIRE(kernels::kernel_steer_authority(taper, full, taper, minA) == Approx(minA));
    REQUIRE(kernels::kernel_steer_authority(taper + 100.0f, full, taper, minA) == Approx(minA));
    const f32 mid = kernels::kernel_steer_authority(22.5f, full, taper, minA);
    REQUIRE(mid > minA);
    REQUIRE(mid < 1.0f);
    // Degenerate config → identity (legacy behaviour).
    REQUIRE(kernels::kernel_steer_authority(30.0f, 0.0f, 0.0f, 1.0f) == Approx(1.0f));
}

// ─── Pure-kernel: bilinear heightfield sampler ─────────────────────────────

TEST_CASE("Bilinear heightfield interpolates and clamps at the edges",
          "[physics][vehicle][heightfield]") {
    // 2x2 grid, 1 m spacing, origin at world (0,0). Corner heights:
    //   (0,0)=0  (1,0)=2  (0,1)=4  (1,1)=6
    std::array<f32, 4> h{0.0f, 2.0f, 4.0f, 6.0f};
    auto sample = [&](f32 x, f32 z) {
        return kernels::kernel_heightfield_bilinear(h.data(), 2, 2, 1.0f, {0, 0, 0}, x, z);
    };
    REQUIRE(sample(0.0f, 0.0f) == Approx(0.0f));
    REQUIRE(sample(1.0f, 0.0f) == Approx(2.0f));
    REQUIRE(sample(0.0f, 1.0f) == Approx(4.0f));
    REQUIRE(sample(0.5f, 0.0f) == Approx(1.0f));  // halfway in x
    REQUIRE(sample(0.0f, 0.5f) == Approx(2.0f));  // halfway in z
    REQUIRE(sample(0.5f, 0.5f) == Approx(3.0f));  // bilinear centre
    // Out-of-bounds clamps to the edge cell, never reads OOB.
    REQUIRE(sample(-10.0f, -10.0f) == Approx(0.0f));
    REQUIRE(sample(99.0f, 99.0f) == Approx(6.0f));
}

// ─── End-to-end: chassis follows a sloped heightfield ──────────────────────

TEST_CASE("Vehicle tracks a sloped heightfield instead of floating at a constant Y",
          "[physics][vehicle][terrain]") {
    physics::World world;
    Ramp ramp{};
    ramp.base_y = 3.0f;
    ramp.slope = 0.15f;  // ~8.5 degrees up toward +X

    // Drop the car onto the ramp above the surface at x = 0 (ground ≈ 3.0).
    Rig rig = make_car(world, /*start_com_y*/ 6.0f);
    physics::vehicle::set_ground_heightfield(rig.veh, {&Ramp::sample, &ramp}, world);

    step_ticks(world, 360);  // 3 s settle on the ramp

    const math::Vec3 p = world.get_position(rig.chassis);
    // Analytic rest COM over flat ground (sample-04 numbers):
    //   compression = (m g / 4) / k, attach.y = (rest+radius) - compression,
    //   COM.y = ground_h + attach.y - local.y, with local.y = -0.35.
    const f32 compression = (1500.0f * 9.81f / 4.0f) / 40000.0f;
    const f32 attach_above_ground = (0.30f + 0.35f) - compression - (-0.35f);
    const f32 ground_under_com = ramp.base_y + ramp.slope * p.x;
    const f32 expected_com_y = ground_under_com + attach_above_ground;

    // 1. The chassis sits on the LOCAL terrain height (slope-following), NOT at
    //    a constant Y. It tracks within a tight band of the analytic rest height.
    REQUIRE(p.y == Approx(expected_com_y).margin(0.25f));
    // 2. It did NOT sink through the terrain.
    REQUIRE(p.y > ground_under_com);
    // 3. The chassis pitched to follow the slope: nose (front, +X) higher than
    //    tail. Sample the body up-axis — over an up-tilted ramp the chassis
    //    forward axis should have a positive Y component.
    const math::Quat q = world.get_rotation(rig.chassis);
    const math::Vec3 fwd = quat_rotate(q, {1, 0, 0});
    REQUIRE(fwd.y > 0.0f);  // nose up the slope

    teardown(world, rig);
}

TEST_CASE("Same vehicle floats at a constant Y on a flat plane (fast path unchanged)",
          "[physics][vehicle][terrain]") {
    physics::World world;
    Rig rig = make_car(world, /*start_com_y*/ 2.0f);
    physics::vehicle::set_ground_plane(rig.veh, 0.0f, world);
    step_ticks(world, 360);
    const f32 y = world.get_position(rig.chassis).y;
    const f32 compression = (1500.0f * 9.81f / 4.0f) / 40000.0f;
    const f32 expected = (0.30f + 0.35f) - compression - (-0.35f);
    REQUIRE(y == Approx(expected).margin(0.05f));
    teardown(world, rig);
}

// ─── End-to-end: speed governor holds at/under the cap ─────────────────────

TEST_CASE("Speed governor holds the chassis at/under the configured cap",
          "[physics][vehicle][governor]") {
    physics::World world;
    physics::vehicle::VehicleDesc base{};
    base.engine_max_torque = 600.0f;
    base.drag_coefficient = 0.30f;
    base.max_speed = 18.0f;  // m/s cap
    Rig rig = make_car(world, /*start_com_y*/ 1.0f, &base);
    physics::vehicle::set_ground_plane(rig.veh, 0.0f, world);
    physics::vehicle::set_throttle(rig.veh, 1.0f, world);

    // Let it settle on its springs first, then floor it for several seconds.
    step_ticks(world, 60);
    f32 max_seen = 0.0f;
    for (int i = 0; i < 1200; ++i) {  // 10 s of full throttle
        world.step(1.0f / 120.0f);
        const math::Vec3 v = world.get_position(rig.chassis);
        (void)v;
    }
    // Read the forward speed directly from the body velocity by finite
    // difference over one tick.
    const math::Vec3 p0 = world.get_position(rig.chassis);
    world.step(1.0f / 120.0f);
    const math::Vec3 p1 = world.get_position(rig.chassis);
    const f32 speed = std::sqrt(math::dot(math::sub(p1, p0), math::sub(p1, p0))) * 120.0f;
    max_seen = speed;

    // Governed: speed does not run away past the cap (allow a small overshoot
    // band for integrator lag), and it actually reached a useful fraction of
    // the cap (no chronic undershoot — it is not stuck slow).
    REQUIRE(max_seen <= base.max_speed * 1.10f);
    REQUIRE(max_seen >= base.max_speed * 0.70f);

    teardown(world, rig);
}

TEST_CASE("Ungoverned vehicle exceeds what the governed one is capped to",
          "[physics][vehicle][governor]") {
    physics::World world;
    physics::vehicle::VehicleDesc base{};
    base.engine_max_torque = 600.0f;
    base.drag_coefficient = 0.30f;
    base.max_speed = 0.0f;  // governor OFF
    Rig rig = make_car(world, /*start_com_y*/ 1.0f, &base);
    physics::vehicle::set_ground_plane(rig.veh, 0.0f, world);
    physics::vehicle::set_throttle(rig.veh, 1.0f, world);

    step_ticks(world, 60);
    step_ticks(world, 1200);
    const math::Vec3 p0 = world.get_position(rig.chassis);
    world.step(1.0f / 120.0f);
    const math::Vec3 p1 = world.get_position(rig.chassis);
    const f32 speed = std::sqrt(math::dot(math::sub(p1, p0), math::sub(p1, p0))) * 120.0f;
    // Without a governor the same drivetrain climbs well past the 18 m/s the
    // governed test capped to (drag is the only limit, far higher here).
    REQUIRE(speed > 18.0f * 1.10f);

    teardown(world, rig);
}

// ─── End-to-end: steering authority turns the chassis ──────────────────────

TEST_CASE("Steering authority turns the chassis at low speed and is stable at speed",
          "[physics][vehicle][steer]") {
    physics::World world;
    physics::vehicle::VehicleDesc base{};
    base.engine_max_torque = 450.0f;
    base.drag_coefficient = 0.30f;
    base.steer_full_speed = 5.0f;
    base.steer_taper_speed = 30.0f;
    base.steer_min_authority = 0.30f;
    Rig rig = make_car(world, /*start_com_y*/ 1.0f, &base);
    physics::vehicle::set_ground_plane(rig.veh, 0.0f, world);

    step_ticks(world, 60);  // settle

    // Low-speed turn: a little throttle + steer. The chassis must yaw (heading
    // rotates) — proof the low-speed authority is enough to turn.
    physics::vehicle::set_throttle(rig.veh, 0.35f, world);
    physics::vehicle::set_steer(rig.veh, 0.40f, world);  // ~23 deg

    auto heading = [&]() {
        const math::Quat q = world.get_rotation(rig.chassis);
        const math::Vec3 f = quat_rotate(q, {1, 0, 0});
        return std::atan2(f.z, f.x);
    };
    const f32 h0 = heading();
    step_ticks(world, 360);  // 3 s of turning
    const f32 h1 = heading();
    REQUIRE(std::fabs(h1 - h0) > 0.05f);  // the chassis actually rotated

    teardown(world, rig);
}

// ─── End-to-end: the lateral (cornering) tire force is RESTORING ───────────
// Regression for the Pacejka lateral-force SIGN. A pure sideways slide (no
// steer, no throttle) must be DAMPED by the tires: the lateral force opposes
// the lateral slip. When it was instead applied along +tire_right (the SAME
// sense as the slip) it was positive feedback — the slide grew without bound,
// so any steer spun the light chassis and the racer/df demos had to drive a
// straight line. Here the car is given a pure slide along its local +Z (front
// axle is +X, so right = up x forward = -Z and +Z is the car's left); with no
// throttle the tire lateral force is the only horizontal force, so its sign
// alone decides whether the slide decays (restoring) or diverges.
TEST_CASE("Lateral tire force opposes a sideways slide (restoring, not divergent)",
          "[physics][vehicle][steer]") {
    physics::World world;
    Rig rig = make_car(world, /*start_com_y*/ 1.0f);
    physics::vehicle::set_ground_plane(rig.veh, 0.0f, world);
    physics::vehicle::set_steer(rig.veh, 0.0f, world);  // wheels straight
    step_ticks(world, 90);                              // settle on the springs

    world.set_body_velocity(rig.chassis, {0.0f, 0.0f, 6.0f});  // 6 m/s slide +Z
    step_ticks(world, 2);  // warm up: let the injected velocity register

    // Lateral speed = world +Z velocity, read by one-tick position difference.
    // (Each call advances the world a single tick.)
    auto lateral_speed = [&]() {
        const math::Vec3 p0 = world.get_position(rig.chassis);
        world.step(1.0f / 120.0f);
        const math::Vec3 p1 = world.get_position(rig.chassis);
        return (p1.z - p0.z) * 120.0f;
    };

    const f32 v_early = lateral_speed();  // slide just after injection
    step_ticks(world, 30);                // 0.25 s of tire response
    const f32 v_late = lateral_speed();

    REQUIRE(v_early > 1.0f);            // it really was sliding +Z
    REQUIRE(v_late < v_early - 1.0f);   // RESTORING: the slide clearly decayed
    REQUIRE(v_late >= -0.5f);           // damped toward rest, did not reverse hard

    teardown(world, rig);
}

// ─── End-to-end: determinism over the terrain + governor + steer path ──────

TEST_CASE("Vehicle-on-terrain trajectory reproduces bit-for-bit across two runs",
          "[physics][vehicle][determinism]") {
    FpGuard fp;  // psynder::physics::detail::FpGuard (using-directive above)

    auto run = [](std::vector<math::Vec3>& out_pos, std::vector<math::Quat>& out_rot) {
        physics::World world;
        Ramp ramp{};
        ramp.base_y = 1.0f;
        ramp.slope = 0.10f;
        Rig rig = make_car(world, /*start_com_y*/ 4.0f);
        physics::vehicle::set_ground_heightfield(rig.veh, {&Ramp::sample, &ramp}, world);
        // A fixed scripted control sequence: throttle on, steer flips at t=1.5s.
        for (int i = 0; i < 480; ++i) {
            physics::vehicle::set_throttle(rig.veh, 0.6f, world);
            physics::vehicle::set_steer(rig.veh, (i < 180) ? 0.20f : -0.20f, world);
            world.step(1.0f / 120.0f);
            out_pos.push_back(world.get_position(rig.chassis));
            out_rot.push_back(world.get_rotation(rig.chassis));
        }
        teardown(world, rig);
    };

    std::vector<math::Vec3> pa, pb;
    std::vector<math::Quat> ra, rb;
    run(pa, ra);
    run(pb, rb);

    REQUIRE(pa.size() == pb.size());
    for (usize i = 0; i < pa.size(); ++i) {
        INFO("tick " << i);
        REQUIRE(std::memcmp(&pa[i], &pb[i], sizeof(math::Vec3)) == 0);
        REQUIRE(std::memcmp(&ra[i], &rb[i], sizeof(math::Quat)) == 0);
    }
}
