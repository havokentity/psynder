// SPDX-License-Identifier: MIT
// Psynder physics — vehicle module skeleton (DESIGN.md §10.1).
//
// Wave A: declare APIs and persist a record per vehicle so gameplay code can
// wire up entities. Full Pacejka-lite + drivetrain + suspension integration
// land in Wave B once the rigid-body integrator is benchmarked.

#include "Physics.h"
#include "Vehicle.h"

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
    for (const WheelDesc& wd : d.wheels) {
        detail::VehicleWheel vw{};
        vw.local_position           = wd.local_position;
        vw.radius                   = wd.radius;
        vw.suspension_rest_length   = wd.suspension;
        vw.spring_k                 = wd.stiffness;
        vw.damping                  = wd.damping;
        v.wheels.push_back(vw);
    }
    v.engine_max_torque     = d.engine_max_torque;
    v.drag_coefficient      = d.drag_coefficient;
    v.downforce_coefficient = d.downforce_coefficient;
    v.throttle = v.brake = v.steer = 0.0f;
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
