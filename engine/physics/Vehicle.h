// SPDX-License-Identifier: MIT
// Psynder physics — vehicle module internal skeleton (DESIGN.md §10.1).
//
// Wave A declares the data shape; full Pacejka-lite + drivetrain ships in
// Wave B (DESIGN.md milestone M4 follow-up). The skeleton is here so other
// lanes can already wire vehicle entities through the public `vehicle::`
// namespace.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::physics::detail {

struct VehicleWheel {
    math::Vec3 local_position;
    f32        radius;
    f32        suspension_rest_length;
    f32        spring_k;        // N/m
    f32        damping;         // N/(m/s)
    f32        compression = 0.0f;
    f32        angular_velocity = 0.0f;
    f32        steer_angle  = 0.0f;
};

struct Vehicle {
    u32                       chassis_body = 0;
    std::vector<VehicleWheel> wheels;
    f32                       engine_max_torque    = 400.0f;   // N·m
    f32                       drag_coefficient     = 0.30f;
    f32                       downforce_coefficient= 0.0f;

    f32                       throttle    = 0.0f;
    f32                       brake       = 0.0f;
    f32                       steer       = 0.0f;
    u32                       gen         = 1;
};

struct VehicleWorld {
    std::vector<Vehicle> vehicles;
    std::vector<u32>     free_slots;
};

VehicleWorld& vehicle_world();

}  // namespace psynder::physics::detail
