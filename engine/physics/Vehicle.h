// SPDX-License-Identifier: MIT
// Psynder physics — vehicle module internal state (DESIGN.md §10.1).
//
// Wave A: data shape only.
// Wave B: full Pacejka '94 combined-slip tires, drivetrain (engine curve →
// clutch → 6+R gearbox → final-drive differential), aero (drag + downforce).
// The algorithm kernels live in `internal/Kernels.h`; this header is the
// per-vehicle persistent state the dispatcher carries between sub-ticks.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "Body.h"
#include "internal/Kernels.h"

#include <array>
#include <vector>

namespace psynder::physics::detail {

struct VehicleWheel {
    math::Vec3 local_position;
    f32 radius;
    f32 suspension_rest_length;
    f32 spring_k;  // N/m
    f32 damping;   // N/(m/s)
    f32 compression = 0.0f;
    f32 angular_velocity = 0.0f;  // rad/s about wheel spin axis
    f32 steer_angle = 0.0f;       // radians
    bool drive_wheel = false;     // true for rear axle by default
    bool on_ground = false;
    f32 load_n = 0.0f;  // last-tick vertical load Fz (N)
};

struct Vehicle {
    // Full chassis BodyId.raw (index + generation). step_vehicles resolves it
    // with a gen check each tick so a destroyed/recycled chassis is skipped
    // rather than driving a different body.
    u32 chassis_body = 0;
    std::vector<VehicleWheel> wheels;

    // ─── Aero / chassis params ────────────────────────────────────────
    f32 drag_coefficient = 0.30f;
    f32 downforce_coefficient = 0.0f;
    f32 frontal_area_m2 = 2.2f;  // ~midsize sedan
    f32 downforce_area_m2 = 1.0f;
    f32 chassis_mass_kg = 1500.0f;  // mirror for aero

    // ─── Driver inputs (per-tick) ─────────────────────────────────────
    f32 throttle = 0.0f;  // 0..1
    f32 brake = 0.0f;     // 0..1
    f32 steer = 0.0f;     // radians (front-wheel angle)
    f32 clutch = 1.0f;    // 0..1 (1 = engaged)
    i32 gear = 1;         // -1 = reverse, 0 = neutral, 1..6

    // ─── Tire + drivetrain params ─────────────────────────────────────
    kernels::PacejkaCoeffs tire_coeffs{};
    f32 tire_friction = 1.0f;  // overall mu multiplier
    kernels::DrivetrainParams drivetrain{};

    // ─── Ground contact ───────────────────────────────────────────────
    // Flat ground-plane height (world Y) used by the per-wheel suspension
    // raycast. Default 0. Set via vehicle::set_ground_plane(). The sample-04
    // oval track is flat; elevated terrain would swap this scalar for a
    // height-query hook (function pointer + void* user) so the 120 Hz loop
    // stays free of virtual / std::function dispatch.
    f32 ground_y = 0.0f;

    // ─── Persistent drivetrain state ──────────────────────────────────
    f32 engine_rpm = 800.0f;  // idle by default

    // Legacy field — kept so older Wave A callers still see the value via
    // `engine_max_torque`. Wave B reads from the curve.
    f32 engine_max_torque = 400.0f;

    // `gen` is the slot's current generation (1..255, never 0), preserved
    // across destroy and bumped on reuse so a stale VehicleId fails the decode
    // equality check. `alive` marks a live slot vs a hole.
    u32 gen = 1;
    bool alive = false;
};

struct VehicleWorld {
    std::vector<Vehicle> vehicles;
    std::vector<u32> free_slots;
};

VehicleWorld& vehicle_world();

// Per-vehicle solver step (defined in Vehicle.cpp). Runs once per fixed
// sub-tick, accumulating tire / suspension / aero forces into the chassis
// body's force + torque accumulators. Single-pass, no heap, no virtual.
void vehicle_step(Vehicle& v, Body& chassis, f32 dt) noexcept;

// Build a default 6-point passenger-car engine curve scaled to a given peak.
inline void vehicle_default_engine_curve(kernels::DrivetrainParams& d, f32 peak_torque_nm) noexcept {
    // Idle-to-redline curve: torque rises to peak around 4500 rpm, gentle
    // fall-off into redline. Numbers from a midsize NA petrol engine.
    d.curve_count = 8;
    d.curve[0] = {1000.0f, 0.50f * peak_torque_nm};
    d.curve[1] = {2000.0f, 0.72f * peak_torque_nm};
    d.curve[2] = {3000.0f, 0.88f * peak_torque_nm};
    d.curve[3] = {4000.0f, 0.96f * peak_torque_nm};
    d.curve[4] = {4500.0f, 1.00f * peak_torque_nm};
    d.curve[5] = {5500.0f, 0.93f * peak_torque_nm};
    d.curve[6] = {6500.0f, 0.78f * peak_torque_nm};
    d.curve[7] = {7000.0f, 0.55f * peak_torque_nm};
    d.idle_rpm = 800.0f;
    d.redline_rpm = 7000.0f;
}

}  // namespace psynder::physics::detail
