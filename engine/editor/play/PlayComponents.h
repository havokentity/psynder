// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics components (DESIGN.md SS10.1, editor-only
// direction 2026-05-26).
//
// These are the pooled ECS components that bind a scene entity to the physics
// runtime. They live in editor/play (NOT engine/scene) so the scene library
// stays physics-free: this header is the single place that pulls in both
// physics/Physics.h and scene/EcsRegistry.h to declare the linkage POD.
//
// DOTS rule: the body / character handle is a runtime field carried in a
// pooled component column, processed column-at-a-time by PlayRuntime's
// registry().query<reads,writes>() passes. There is NO Entity->BodyId
// std::unordered_map side-table anywhere in this module.
//
// Authored vs runtime fields:
//   * shape / mass / half_extent / friction / restitution (RigidBody) and
//     height / radius / move_speed (CharacterController) are designer-set in
//     the editor and persist with the scene.
//   * body / character are runtime handles: zero ({}) when not playing, filled
//     by PlayRuntime::begin(), cleared by PlayRuntime::end().
//   * walk_dir is per-frame input the host writes before tick().

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "scene/EcsRegistry.h"

namespace psynder::editor::play {

// A rigid body attached to an entity. mass == 0 => static collider (floors,
// walls). The body handle is filled in begin() and processed column-at-a-time
// in tick()'s writeback query.
PSYNDER_COMPONENT(RigidBodyComponent) {
    physics::Shape shape = physics::Shape::Box;
    u8 _pad[3] = {};
    f32 mass = 1.0f;  // kg; 0 = static
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    physics::BodyId body{};  // runtime handle; 0 when not playing
};

// A drivable vehicle attached to an entity. The chassis is an ordinary rigid
// box body; the vehicle solver runs inside World::step() and writes the chassis
// pose. Authored fields (half_extent / mass / engine torque / drag / wheel
// params / is_player) persist with the scene; the four wheels are auto-placed
// at the corners of half_extent in begin() (front pair = steer/non-drive, rear
// pair = drive, RWD). The vehicle + chassis handles are runtime fields filled
// by PlayRuntime::begin() and cleared by PlayRuntime::end(). is_player marks the
// car the WASD input drives and the chase camera follows.
PSYNDER_COMPONENT(VehicleComponent) {
    math::Vec3 half_extent{1.0f, 0.4f, 2.0f};  // chassis box half-extent (m)
    f32 mass = 1200.0f;                         // kg
    f32 engine_max_torque = 400.0f;             // N.m
    f32 drag = 0.30f;                           // aero drag coefficient
    f32 wheel_radius = 0.34f;                   // m
    f32 suspension = 0.35f;                     // rest length (m)
    f32 stiffness = 35000.0f;                   // N/m
    f32 damping = 4500.0f;                      // N.s/m
    bool is_player = true;                       // WASD-driven + chase-cam target
    u8 _pad[3] = {};
    physics::vehicle::VehicleId vehicle{};  // runtime handle; 0 when not playing
    physics::BodyId chassis{};              // runtime chassis body; 0 when not playing
};

// A flyable arcade helicopter attached to an entity. The chassis is an ordinary
// dynamic box body; PlayRuntime applies a simple body-relative flight model each
// tick BEFORE the single World::step(): collective thrust along the body-up
// axis, cyclic pitch/roll + pedal yaw torque, and angular damping. Authored
// fields (half_extent / mass / thrust / torques / damping / hover_assist /
// is_player) persist with the scene. is_player marks the heli the flight input
// drives and the chase camera follows.
//
// Stability: there is no public angular-velocity reader on the World, so the
// component carries `ang_vel_est` -- an estimate PlayRuntime integrates itself
// from the torques it applies (the heli is the only torque source while
// airborne; gravity adds none at the COM). Each tick it damps the estimate and
// writes it authoritatively via World::set_angular_velocity, which keeps the
// craft controllable and prevents runaway tumbling.
PSYNDER_COMPONENT(HelicopterComponent) {
    math::Vec3 half_extent{1.2f, 0.6f, 2.0f};  // chassis box half-extent (m)
    f32 mass = 900.0f;                          // kg
    f32 max_thrust_n = 14000.0f;                // N (must exceed m*g ~= 8829)
    f32 pitch_torque = 8000.0f;                 // N.m about body-right (cyclic)
    f32 roll_torque = 8000.0f;                  // N.m about body-forward (cyclic)
    f32 yaw_torque = 4000.0f;                   // N.m about body-up (pedals)
    f32 angular_damping = 2.0f;                 // 1/s exponential spin decay
    bool hover_assist = true;                   // neutral collective holds m*g
    bool is_player = true;                       // flight-input + chase-cam target
    u8 _pad[2] = {};
    math::Vec3 ang_vel_est{0.0f, 0.0f, 0.0f};  // runtime body angular vel estimate
    physics::BodyId body{};                     // runtime chassis body; 0 when idle
};

// A kinematic capsule character. move_speed scales walk_dir each tick. The
// character handle is filled in begin() and driven in tick().
PSYNDER_COMPONENT(CharacterControllerComponent) {
    f32 height = 1.8f;
    f32 radius = 0.35f;
    f32 move_speed = 4.5f;
    physics::character::CharacterId character{};  // runtime handle; 0 when not playing
    math::Vec3 walk_dir{0.0f, 0.0f, 0.0f};         // host-set per-frame intent (world XZ)
};

}  // namespace psynder::editor::play
