// SPDX-License-Identifier: MIT
// Psynder physics — vehicle module (DESIGN.md §10.1).
//
// Wave A: data shape + driver-input setters.
// Wave B: per-vehicle solver step — Pacejka '94 combined-slip tires at each
// wheel, drivetrain (engine curve → clutch → 6+R gearbox → final-drive
// differential), aero (drag + downforce) applied at COM. Public headers
// stay frozen; new behaviour lives in `vehicle_step()` driven internally
// from the physics tick.

#include "Physics.h"
#include "Vehicle.h"
#include "WorldImpl.h"
#include "internal/Kernels.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace psynder::physics::vehicle {

namespace {
std::mutex g_mutate;
}

}  // namespace psynder::physics::vehicle

namespace psynder::physics::detail {

VehicleWorld& vehicle_world() {
    static VehicleWorld w;
    return w;
}

// ─── Per-vehicle solver step (Wave B) ────────────────────────────────────
//
// Runs once per fixed sub-tick after the integrator has applied gravity but
// before the constraint solver. Per wheel: compute slip ratio + slip angle,
// run Pacejka to get tire forces, sum into chassis force/torque. Drivetrain
// loops the engine RPM to the drive-wheel angular velocity through the
// current gear. Aero drag + downforce act at COM.
//
// We deliberately keep this single-pass / no-virtual: Body is hot, and the
// solver hot loop is the bottleneck — no virtual dispatches, no allocations.
void vehicle_step(Vehicle& v, Body& chassis, f32 dt) noexcept {
    // Chassis basis vectors in world space.
    math::Vec3 forward = quat_rotate(chassis.rotation, {0, 0,  1});
    math::Vec3 right   = quat_rotate(chassis.rotation, {1, 0,  0});
    math::Vec3 up      = quat_rotate(chassis.rotation, {0, 1,  0});

    // Drivetrain: read drive-wheel omegas and step the engine + gearbox.
    f32 omega_l = 0.0f, omega_r = 0.0f;
    u32 drive_count = 0;
    for (const VehicleWheel& wh : v.wheels) {
        if (wh.drive_wheel) {
            if (drive_count == 0) omega_l = wh.angular_velocity;
            else                  omega_r = wh.angular_velocity;
            ++drive_count;
            if (drive_count == 2) break;
        }
    }
    auto dt_out = kernels::kernel_drivetrain_step(
        v.drivetrain, v.throttle, v.brake, v.clutch, v.gear,
        omega_l, omega_r, v.engine_rpm);
    v.engine_rpm = dt_out.engine_rpm;

    // Apply drive torques to the drive wheels (first two flagged drive_wheels).
    u32 assigned = 0;
    for (VehicleWheel& wh : v.wheels) {
        if (!wh.drive_wheel) continue;
        f32 t = (assigned == 0) ? dt_out.wheel_torque_l : dt_out.wheel_torque_r;
        // Wheel inertia is approximated from radius (I ≈ 0.5·m·r^2 with m
        // baked in via the moment); we use a fixed moment per vehicle to keep
        // the kernel deterministic.
        constexpr f32 kWheelMoment = 1.5f;
        wh.angular_velocity += (t / kWheelMoment) * dt;
        ++assigned;
        if (assigned == 2) break;
    }

    // Per-wheel tire forces (Pacejka). For each wheel:
    //   * slip_long = (r·ω − v_long) / max(|v_long|, eps)
    //   * slip_lat  = atan2(v_lat, |v_long|) − steer_angle (radians)
    //   * Fz        = spring·compression + damping (last tick's load)
    math::Vec3 total_force {0, 0, 0};
    math::Vec3 total_torque{0, 0, 0};

    for (VehicleWheel& wh : v.wheels) {
        if (!wh.on_ground || wh.load_n <= 0.0f) continue;

        // World position of the wheel contact patch (approximation: chassis
        // COM + rotated local + downward suspension extension).
        math::Vec3 wheel_world = math::add(chassis.position,
            quat_rotate(chassis.rotation, wh.local_position));
        math::Vec3 ra = math::sub(wheel_world, chassis.position);

        // Velocity of the contact patch in world frame, projected into the
        // tire's longitudinal/lateral basis. The tire's forward axis is the
        // chassis forward rotated by the steering angle around `up`.
        f32 cs = std::cos(wh.steer_angle);
        f32 sn = std::sin(wh.steer_angle);
        math::Vec3 tire_fwd { forward.x * cs + right.x * sn,
                              forward.y * cs + right.y * sn,
                              forward.z * cs + right.z * sn };
        math::Vec3 tire_right{ -forward.x * sn + right.x * cs,
                               -forward.y * sn + right.y * cs,
                               -forward.z * sn + right.z * cs };

        math::Vec3 patch_v = math::add(chassis.linear_velocity,
                                       math::cross(chassis.angular_velocity, ra));
        f32 v_long = math::dot(patch_v, tire_fwd);
        f32 v_lat  = math::dot(patch_v, tire_right);

        // Slip ratio: longitudinal slip between wheel circumferential speed
        // and the ground velocity at the contact patch.
        f32 wheel_speed = wh.angular_velocity * wh.radius;
        f32 denom = std::max(std::fabs(v_long), 1.0f);     // avoid 0-divide
        f32 slip_long = (wheel_speed - v_long) / denom;
        slip_long = std::clamp(slip_long, -2.0f, 2.0f);

        // Slip angle: small-angle lateral velocity / longitudinal velocity.
        // For low speed, atan2 is close to linear and well-behaved.
        f32 slip_lat = std::atan2(v_lat, std::max(std::fabs(v_long), 1.0f));

        auto tf = kernels::kernel_pacejka_combined(
            slip_long, slip_lat, wh.load_n, v.tire_friction, v.tire_coeffs);

        // Project tire forces back into world frame: Fx along tire_fwd, Fy
        // along tire_right. Both lie in the ground plane (perpendicular to
        // suspension up).
        math::Vec3 tire_force = math::add(math::mul(tire_fwd,  tf.Fx),
                                          math::mul(tire_right, tf.Fy));
        total_force  = math::add(total_force,  tire_force);
        total_torque = math::add(total_torque, math::cross(ra, tire_force));

        // Apply the reaction back onto the wheel angular velocity: Fx · r at
        // the contact reduces wheel speed (engine bleed when accelerating).
        constexpr f32 kWheelMoment = 1.5f;
        wh.angular_velocity -= (tf.Fx * wh.radius / kWheelMoment) * dt;
    }

    // Aero. Drag opposes velocity, downforce along world-down (use chassis
    // up inverted so it follows orientation if the car is upside-down). We
    // intentionally use -up so it sucks the chassis toward its own floor.
    math::Vec3 aero = kernels::kernel_aero_force(
        chassis.linear_velocity,
        math::mul(up, -1.0f),
        v.drag_coefficient,      v.frontal_area_m2,
        v.downforce_coefficient, v.downforce_area_m2);
    total_force = math::add(total_force, aero);

    // Accumulate. The integrator picks these up on the next sub-tick.
    chassis.force  = math::add(chassis.force,  total_force);
    chassis.torque = math::add(chassis.torque, total_torque);
}

}  // namespace psynder::physics::detail

namespace psynder::physics::vehicle {

VehicleId create(const VehicleDesc& d) {
    auto& w = detail::vehicle_world();
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx;
    if (!w.free_slots.empty()) {
        idx = w.free_slots.back();
        w.free_slots.pop_back();
    } else {
        idx = static_cast<u32>(w.vehicles.size());
        w.vehicles.emplace_back();
    }
    detail::Vehicle& v = w.vehicles[idx];
    v.chassis_body = d.chassis.raw;
    v.wheels.clear();
    v.wheels.reserve(d.wheels.size());
    u32 wheel_idx = 0;
    for (const WheelDesc& wd : d.wheels) {
        detail::VehicleWheel vw{};
        vw.local_position           = wd.local_position;
        vw.radius                   = wd.radius;
        vw.suspension_rest_length   = wd.suspension;
        vw.spring_k                 = wd.stiffness;
        vw.damping                  = wd.damping;
        // Convention: wheels 2 and 3 are the rear (drive) axle. This matches
        // Wave B's default 4-wheel desc — caller can override later via the
        // internal API once we expose per-wheel flags in Wave C.
        vw.drive_wheel              = (wheel_idx >= 2);
        v.wheels.push_back(vw);
        ++wheel_idx;
    }
    v.engine_max_torque     = d.engine_max_torque;
    v.drag_coefficient      = d.drag_coefficient;
    v.downforce_coefficient = d.downforce_coefficient;
    v.throttle = v.brake = v.steer = 0.0f;
    v.clutch = 1.0f;
    v.gear   = 1;
    v.engine_rpm = 800.0f;
    detail::vehicle_default_engine_curve(v.drivetrain, d.engine_max_torque);
    v.tire_coeffs = detail::kernels::PacejkaCoeffs{};
    v.tire_friction = 1.0f;
    if (v.gen == 0) v.gen = 1;
    return VehicleId{ (v.gen << 24) | (idx & 0x00FFFFFFu) };
}

void destroy(VehicleId id) {
    auto& w = detail::vehicle_world();
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.vehicles.size()) return;
    w.vehicles[idx].gen = 0;
    w.free_slots.push_back(idx);
}

void set_throttle(VehicleId id, f32 t) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size()) w.vehicles[idx].throttle = t;
}

void set_brake(VehicleId id, f32 b) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size()) w.vehicles[idx].brake = b;
}

void set_steer(VehicleId id, f32 angle) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size()) w.vehicles[idx].steer = angle;
}

}  // namespace psynder::physics::vehicle
