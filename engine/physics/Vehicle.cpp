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
    //
    // The longitudinal (forward) axis is derived from the wheel layout — the
    // line from the rear (drive) axle to the front (non-drive) axle — rather
    // than assumed to be chassis-local +Z. A vehicle whose wheels are placed
    // along local X must drive along local X, otherwise the drive thrust acts
    // perpendicular to the wheelbase and spins the chassis instead of pushing
    // it (the sample-04 "barely accelerates" bug). For the conventional
    // local-+Z layout this derivation yields exactly +Z, so existing callers
    // are unaffected; it falls back to local +Z when the layout is degenerate
    // (no axle split, or coincident axle centroids).
    math::Vec3 up = quat_rotate(chassis.rotation, {0, 1, 0});
    math::Vec3 fwd_local{0.0f, 0.0f, 1.0f};
    {
        math::Vec3 front_sum{0, 0, 0}, rear_sum{0, 0, 0};
        u32 nf = 0, nr = 0;
        for (const VehicleWheel& wh : v.wheels) {
            if (wh.drive_wheel) {
                rear_sum = math::add(rear_sum, wh.local_position);
                ++nr;
            } else {
                front_sum = math::add(front_sum, wh.local_position);
                ++nf;
            }
        }
        if (nf > 0 && nr > 0) {
            const math::Vec3 d = math::sub(math::mul(front_sum, 1.0f / static_cast<f32>(nf)),
                                           math::mul(rear_sum, 1.0f / static_cast<f32>(nr)));
            if (math::dot(d, d) > 1e-6f)
                fwd_local = math::normalize(d);
        }
    }
    math::Vec3 forward = quat_rotate(chassis.rotation, fwd_local);
    // right = up x forward, but guard the degenerate case: if the chassis has
    // pitched/rolled so forward is ~parallel to up (e.g. mid-flip) the cross
    // collapses to ~0 and a plain normalize() would yield NaN, poisoning the
    // tire basis for the tick. Fall back to the chassis's own local +X axis.
    const math::Vec3 right_raw = math::cross(up, forward);
    const f32 right_len2 = math::dot(right_raw, right_raw);
    math::Vec3 right = (right_len2 > 1e-8f) ? math::mul(right_raw, 1.0f / std::sqrt(right_len2))
                                            : quat_rotate(chassis.rotation, {1, 0, 0});

    // Drivetrain: read drive-wheel omegas and step the engine + gearbox.
    f32 omega_l = 0.0f, omega_r = 0.0f;
    u32 drive_count = 0;
    for (const VehicleWheel& wh : v.wheels) {
        if (wh.drive_wheel) {
            if (drive_count == 0)
                omega_l = wh.angular_velocity;
            else
                omega_r = wh.angular_velocity;
            ++drive_count;
            if (drive_count == 2)
                break;
        }
    }
    auto dt_out = kernels::kernel_drivetrain_step(v.drivetrain,
                                                  v.throttle,
                                                  v.brake,
                                                  v.clutch,
                                                  v.gear,
                                                  omega_l,
                                                  omega_r,
                                                  v.engine_rpm);
    v.engine_rpm = dt_out.engine_rpm;

    // Apply drive torques to the drive wheels (first two flagged drive_wheels).
    u32 assigned = 0;
    for (VehicleWheel& wh : v.wheels) {
        if (!wh.drive_wheel)
            continue;
        f32 t = (assigned == 0) ? dt_out.wheel_torque_l : dt_out.wheel_torque_r;
        // Wheel inertia is approximated from radius (I ≈ 0.5·m·r^2 with m
        // baked in via the moment); we use a fixed moment per vehicle to keep
        // the kernel deterministic.
        constexpr f32 kWheelMoment = 1.5f;
        wh.angular_velocity += (t / kWheelMoment) * dt;
        ++assigned;
        if (assigned == 2)
            break;
    }

    math::Vec3 total_force{0, 0, 0};
    math::Vec3 total_torque{0, 0, 0};

    // ─── Per-wheel suspension ground contact ─────────────────────────────
    //
    // Cast a downward ray from each wheel's attach point on the chassis. The
    // wheel hardware hangs below the attach point by up to the suspension
    // rest travel, and the tire contacts the ground a further `radius` below
    // the bottom of that travel — so the maximum reach from attach point to
    // contact is `rest + radius`. On a flat ground plane the contact normal
    // is world-up and the ground height is the constant `v.ground_y`.
    //
    // compression = (rest + radius) − (attach.y − ground_y), clamped to the
    // physical travel [0, rest]. When > 0 the wheel touches down: we set
    // on_ground, store the spring+damper normal load, and push that load up
    // along the contact normal at the wheel (force + torque about the COM).
    // When the attach point is too high the wheel is airborne (load 0); the
    // Pacejka loop below then skips it.
    const math::Vec3 contact_normal{0.0f, 1.0f, 0.0f};  // flat ground → world up
    for (VehicleWheel& wh : v.wheels) {
        const math::Vec3 attach =
            math::add(chassis.position, quat_rotate(chassis.rotation, wh.local_position));
        const math::Vec3 ra = math::sub(attach, chassis.position);

        const f32 max_reach = wh.suspension_rest_length + wh.radius;
        const f32 gap = attach.y - v.ground_y;  // attach height above ground
        f32 compression = max_reach - gap;

        if (compression <= 0.0f) {
            wh.on_ground = false;
            wh.load_n = 0.0f;
            wh.compression = 0.0f;
            continue;
        }
        // Can't compress past the available suspension travel.
        compression = std::min(compression, wh.suspension_rest_length);

        // Damper rides the chassis velocity at the contact along the normal
        // (its upward speed). Subtracting it from the spring term makes the
        // damper dissipative: when the chassis rises (compression_vel > 0) it
        // bleeds the upward push; when it drops (< 0) it adds resistance.
        // (Using the negated compression *rate* here would invert the sign
        // and pump energy in — the spring would launch and oscillate.)
        const math::Vec3 attach_v =
            math::add(chassis.linear_velocity, math::cross(chassis.angular_velocity, ra));
        const f32 compression_vel = math::dot(attach_v, contact_normal);

        // Spring + damper. Clamp to >= 0 so the suspension can only push the
        // chassis up, never pull it down (no tensile suspension force).
        f32 Fz = wh.spring_k * compression - wh.damping * compression_vel;
        if (Fz < 0.0f)
            Fz = 0.0f;

        wh.on_ground = true;
        wh.load_n = Fz;
        wh.compression = compression;

        const math::Vec3 normal_force = math::mul(contact_normal, Fz);
        total_force = math::add(total_force, normal_force);
        total_torque = math::add(total_torque, math::cross(ra, normal_force));
    }

    // Per-wheel tire forces (Pacejka). For each wheel:
    //   * slip_long = (r·ω − v_long) / max(|v_long|, eps)
    //   * slip_lat  = atan2(v_lat, |v_long|) in the STEERED tire basis. Steering
    //                 is baked into that basis (tire_fwd/tire_right are the
    //                 chassis axes rotated by steer_angle below), so it is NOT
    //                 subtracted again here — doing so would double-count it.
    //   * Fz        = suspension normal load computed above this tick
    for (VehicleWheel& wh : v.wheels) {
        // Propagate the chassis steering command to the steered wheels. Only
        // the front (non-drive) wheels steer; the rear/drive wheels stay fixed.
        // set_steer() only writes v.steer, so without this the per-wheel
        // steer_angle stays 0 and the car cannot turn.
        wh.steer_angle = wh.drive_wheel ? 0.0f : v.steer;

        if (!wh.on_ground || wh.load_n <= 0.0f)
            continue;

        // World position of the wheel contact patch (approximation: chassis
        // COM + rotated local + downward suspension extension).
        math::Vec3 wheel_world =
            math::add(chassis.position, quat_rotate(chassis.rotation, wh.local_position));
        math::Vec3 ra = math::sub(wheel_world, chassis.position);

        // Velocity of the contact patch in world frame, projected into the
        // tire's longitudinal/lateral basis. The tire's forward axis is the
        // chassis forward rotated by the steering angle around `up`.
        f32 cs = std::cos(wh.steer_angle);
        f32 sn = std::sin(wh.steer_angle);
        math::Vec3 tire_fwd{forward.x * cs + right.x * sn,
                            forward.y * cs + right.y * sn,
                            forward.z * cs + right.z * sn};
        math::Vec3 tire_right{-forward.x * sn + right.x * cs,
                              -forward.y * sn + right.y * cs,
                              -forward.z * sn + right.z * cs};

        math::Vec3 patch_v =
            math::add(chassis.linear_velocity, math::cross(chassis.angular_velocity, ra));
        f32 v_long = math::dot(patch_v, tire_fwd);
        f32 v_lat = math::dot(patch_v, tire_right);

        // Slip ratio: longitudinal slip between wheel circumferential speed
        // and the ground velocity at the contact patch.
        f32 wheel_speed = wh.angular_velocity * wh.radius;
        f32 denom = std::max(std::fabs(v_long), 1.0f);  // avoid 0-divide
        f32 slip_long = (wheel_speed - v_long) / denom;
        slip_long = std::clamp(slip_long, -2.0f, 2.0f);

        // Slip angle: small-angle lateral velocity / longitudinal velocity.
        // For low speed, atan2 is close to linear and well-behaved.
        f32 slip_lat = std::atan2(v_lat, std::max(std::fabs(v_long), 1.0f));

        auto tf =
            kernels::kernel_pacejka_combined(slip_long, slip_lat, wh.load_n, v.tire_friction, v.tire_coeffs);

        // Project tire forces back into world frame: Fx along tire_fwd, Fy
        // along tire_right. Both lie in the ground plane (perpendicular to
        // suspension up).
        math::Vec3 tire_force = math::add(math::mul(tire_fwd, tf.Fx), math::mul(tire_right, tf.Fy));
        total_force = math::add(total_force, tire_force);
        total_torque = math::add(total_torque, math::cross(ra, tire_force));

        // Apply the reaction back onto the wheel angular velocity: Fx · r at
        // the contact reduces wheel speed (engine bleed when accelerating).
        constexpr f32 kWheelMoment = 1.5f;
        wh.angular_velocity -= (tf.Fx * wh.radius / kWheelMoment) * dt;
    }

    // Aero. Drag opposes velocity, downforce along world-down (use chassis
    // up inverted so it follows orientation if the car is upside-down). We
    // intentionally use -up so it sucks the chassis toward its own floor.
    math::Vec3 aero = kernels::kernel_aero_force(chassis.linear_velocity,
                                                 math::mul(up, -1.0f),
                                                 v.drag_coefficient,
                                                 v.frontal_area_m2,
                                                 v.downforce_coefficient,
                                                 v.downforce_area_m2);
    total_force = math::add(total_force, aero);

    // Accumulate. The integrator picks these up on the next sub-tick.
    chassis.force = math::add(chassis.force, total_force);
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
        vw.local_position = wd.local_position;
        vw.radius = wd.radius;
        vw.suspension_rest_length = wd.suspension;
        vw.spring_k = wd.stiffness;
        vw.damping = wd.damping;
        // Convention: wheels 2 and 3 are the rear (drive) axle. This matches
        // Wave B's default 4-wheel desc — caller can override later via the
        // internal API once we expose per-wheel flags in Wave C.
        vw.drive_wheel = (wheel_idx >= 2);
        v.wheels.push_back(vw);
        ++wheel_idx;
    }
    v.engine_max_torque = d.engine_max_torque;
    v.drag_coefficient = d.drag_coefficient;
    v.downforce_coefficient = d.downforce_coefficient;
    v.throttle = v.brake = v.steer = 0.0f;
    v.clutch = 1.0f;
    v.gear = 1;
    v.engine_rpm = 800.0f;
    detail::vehicle_default_engine_curve(v.drivetrain, d.engine_max_torque);
    v.tire_coeffs = detail::kernels::PacejkaCoeffs{};
    v.tire_friction = 1.0f;
    if (v.gen == 0)
        v.gen = 1;
    return VehicleId{(v.gen << 24) | (idx & 0x00FFFFFFu)};
}

void destroy(VehicleId id) {
    auto& w = detail::vehicle_world();
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.vehicles.size())
        return;
    w.vehicles[idx].gen = 0;
    w.free_slots.push_back(idx);
}

void set_throttle(VehicleId id, f32 t) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size())
        w.vehicles[idx].throttle = t;
}

void set_brake(VehicleId id, f32 b) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size())
        w.vehicles[idx].brake = b;
}

void set_steer(VehicleId id, f32 angle) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size())
        w.vehicles[idx].steer = angle;
}

void set_ground_plane(VehicleId id, f32 ground_y) {
    auto& w = detail::vehicle_world();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx < w.vehicles.size())
        w.vehicles[idx].ground_y = ground_y;
}

}  // namespace psynder::physics::vehicle
