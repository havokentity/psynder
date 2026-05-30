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
    math::Vec3 up = detail::quat_rotate(chassis.rotation, {0, 1, 0});
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
    math::Vec3 forward = detail::quat_rotate(chassis.rotation, fwd_local);
    // right = up x forward, but guard the degenerate case: if the chassis has
    // pitched/rolled so forward is ~parallel to up (e.g. mid-flip) the cross
    // collapses to ~0 and a plain normalize() would yield NaN, poisoning the
    // tire basis for the tick. Fall back to the chassis's own local +X axis.
    const math::Vec3 right_raw = math::cross(up, forward);
    const f32 right_len2 = math::dot(right_raw, right_raw);
    math::Vec3 right = (right_len2 > 1e-8f) ? math::mul(right_raw, 1.0f / std::sqrt(right_len2))
                                            : detail::quat_rotate(chassis.rotation, {1, 0, 0});

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
    // Speed governor (#58): as the chassis FORWARD speed approaches the cap,
    // taper the throttle fed to the drivetrain so engine→wheel torque rolls off
    // smoothly. Scaling the throttle (not just the output torque) is what makes
    // the car CRUISE at the cap instead of running away: less engine torque →
    // less wheel spin → less tire slip → drag pulls the chassis back to the cap.
    // Computed once per tick; gov == 1 when the governor is disabled (legacy).
    const f32 fwd_speed = math::dot(chassis.linear_velocity, forward);
    const f32 gov = kernels::kernel_speed_governor(fwd_speed, v.max_speed_mps);
    const f32 gov_throttle = v.throttle * gov;

    auto dt_out = kernels::kernel_drivetrain_step(v.drivetrain,
                                                  gov_throttle,
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

        // Governor spin clamp: at/above the cap, a drive wheel that is still
        // over-spinning would keep slip-driving the chassis past the cap even
        // with zero throttle (it carries its own angular momentum). Clamp the
        // circumferential speed to the no-slip speed at the cap so the tire
        // longitudinal force cannot push the chassis beyond it. Only ever
        // REDUCES |omega| (never adds energy) and only when governed + the
        // wheel is over-driving forward, so coasting/braking is untouched and
        // determinism holds.
        if (v.max_speed_mps > 0.0f && wh.radius > 1e-4f) {
            const f32 omega_cap = v.max_speed_mps / wh.radius;
            if (wh.angular_velocity > omega_cap)
                wh.angular_velocity = omega_cap;
        }
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
    //
    // GROUND HEIGHT: when a heightfield sampler is attached the per-wheel
    // ground height is sampled under THAT wheel's (x, z) world column, so each
    // corner contacts the local terrain surface and the chassis follows the
    // slope. With no sampler this is the flat fast-path constant `v.ground_y`,
    // bit-for-bit identical to the prior behaviour. The contact normal stays
    // world-up: a per-wheel normal load along +Y over a gently sloped surface
    // is a standard ray-cast-vehicle approximation and keeps the tire basis
    // (which lies in the world XZ plane) consistent across all wheels.
    const math::Vec3 contact_normal{0.0f, 1.0f, 0.0f};  // world up
    for (VehicleWheel& wh : v.wheels) {
        const math::Vec3 attach =
            math::add(chassis.position, detail::quat_rotate(chassis.rotation, wh.local_position));
        const math::Vec3 ra = math::sub(attach, chassis.position);

        // Sampled ground height under this wheel (heightfield) or the flat
        // plane (fast path). The sampler is a borrowed host callback; a null
        // fn keeps the flat ground_y so existing callers are unaffected.
        const f32 ground_h = (v.terrain_fn != nullptr)
                                 ? v.terrain_fn(v.terrain_user, attach.x, attach.z)
                                 : v.ground_y;

        const f32 max_reach = wh.suspension_rest_length + wh.radius;
        const f32 gap = attach.y - ground_h;  // attach height above ground
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
    // Speed-scaled steering authority (#58): full angle at parking speed,
    // tapered at speed to damp twitch. Computed once per tick from the chassis
    // speed magnitude; identity (1.0) when the vehicle opts out (default).
    const f32 speed_mag = math::length(chassis.linear_velocity);
    const f32 steer_auth = kernels::kernel_steer_authority(
        speed_mag, v.steer_full_speed, v.steer_taper_speed, v.steer_min_authority);
    const f32 effective_steer = v.steer * steer_auth;

    for (VehicleWheel& wh : v.wheels) {
        // Propagate the chassis steering command to the steered wheels. Only
        // the front (non-drive) wheels steer; the rear/drive wheels stay fixed.
        // set_steer() only writes v.steer, so without this the per-wheel
        // steer_angle stays 0 and the car cannot turn.
        wh.steer_angle = wh.drive_wheel ? 0.0f : effective_steer;

        if (!wh.on_ground || wh.load_n <= 0.0f)
            continue;

        // World position of the wheel contact patch (approximation: chassis
        // COM + rotated local + downward suspension extension).
        math::Vec3 wheel_world =
            math::add(chassis.position, detail::quat_rotate(chassis.rotation, wh.local_position));
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

namespace {
// Decode a VehicleId to its live slot with a FULL generation check so a stale
// handle never aliases a recycled vehicle slot.
detail::Vehicle* resolve_vehicle(detail::VehicleWorld& w, VehicleId id) noexcept {
    const u32 idx = detail::handle_index(id.raw);
    if (idx >= w.vehicles.size())
        return nullptr;
    detail::Vehicle& v = w.vehicles[idx];
    if (!v.alive || v.gen != detail::handle_gen(id.raw))
        return nullptr;
    return &v;
}
}  // namespace

VehicleId create(const VehicleDesc& d, World& world) {
    auto& w = world.internal().vehicles;
    std::lock_guard<std::mutex> lock(g_mutate);
    u32 idx;
    u32 reuse_gen;
    if (!w.free_slots.empty()) {
        idx = w.free_slots.back();
        w.free_slots.pop_back();
        reuse_gen = detail::handle_next_gen(w.vehicles[idx].gen);
    } else {
        idx = static_cast<u32>(w.vehicles.size());
        w.vehicles.emplace_back();
        reuse_gen = w.vehicles[idx].gen;  // fresh slot starts at gen == 1
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
    // Governor + steering authority (additive; defaults are no-ops so legacy
    // descs that never set these keep the unclamped Wave-B behaviour).
    v.max_speed_mps = d.max_speed;
    v.steer_full_speed = d.steer_full_speed;
    v.steer_taper_speed = d.steer_taper_speed;
    v.steer_min_authority = d.steer_min_authority;
    // A freshly created (or recycled) vehicle starts on the flat ground plane:
    // clear any borrowed heightfield from a prior occupant of this slot.
    v.terrain_fn = nullptr;
    v.terrain_user = nullptr;
    v.ground_y = 0.0f;
    v.throttle = v.brake = v.steer = 0.0f;
    v.clutch = 1.0f;
    v.gear = 1;
    v.engine_rpm = 800.0f;
    detail::vehicle_default_engine_curve(v.drivetrain, d.engine_max_torque);
    v.tire_coeffs = detail::kernels::PacejkaCoeffs{};
    v.tire_friction = 1.0f;
    v.gen = reuse_gen;  // bumped on reuse, fresh slots start at 1
    v.alive = true;
    return VehicleId{detail::handle_encode(v.gen, idx)};
}

void destroy(VehicleId id, World& world) {
    auto& w = world.internal().vehicles;
    std::lock_guard<std::mutex> lock(g_mutate);
    const u32 idx = detail::handle_index(id.raw);
    if (idx >= w.vehicles.size())
        return;
    detail::Vehicle& v = w.vehicles[idx];
    // Reject stale / already-freed handles to guard against a double-destroy
    // pushing the same slot onto the free-list twice.
    if (!v.alive || v.gen != detail::handle_gen(id.raw))
        return;
    v.alive = false;  // KEEP gen for the next reuse to bump
    w.free_slots.push_back(idx);
}

void set_throttle(VehicleId id, f32 t, World& world) {
    auto& w = world.internal().vehicles;
    if (detail::Vehicle* v = resolve_vehicle(w, id))
        v->throttle = t;
}

void set_brake(VehicleId id, f32 b, World& world) {
    auto& w = world.internal().vehicles;
    if (detail::Vehicle* v = resolve_vehicle(w, id))
        v->brake = b;
}

void set_steer(VehicleId id, f32 angle, World& world) {
    auto& w = world.internal().vehicles;
    if (detail::Vehicle* v = resolve_vehicle(w, id))
        v->steer = angle;
}

void set_ground_plane(VehicleId id, f32 ground_y, World& world) {
    auto& w = world.internal().vehicles;
    if (detail::Vehicle* v = resolve_vehicle(w, id)) {
        v->ground_y = ground_y;
        // Selecting the flat fast path DETACHES any borrowed heightfield so a
        // later set_ground_plane call cleanly reverts terrain following.
        v->terrain_fn = nullptr;
        v->terrain_user = nullptr;
    }
}

void set_ground_heightfield(VehicleId id, HeightSampler terrain, World& world) {
    auto& w = world.internal().vehicles;
    if (detail::Vehicle* v = resolve_vehicle(w, id)) {
        // A null fn reverts to the flat ground_y fast path (detach).
        v->terrain_fn = terrain.fn;
        v->terrain_user = terrain.user;
    }
}

}  // namespace psynder::physics::vehicle
