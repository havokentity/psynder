// SPDX-License-Identifier: MIT
// Psynder - SCENE-LEVEL authoring physics components (DESIGN.md SS10.1,
// editor-only direction 2026-05-26).
//
// These are the pooled ECS components a designer attaches in the editor to make
// an entity simulate in Play mode (rigid body / character / vehicle /
// helicopter). They live in engine/scene (NOT editor/play) so two things work:
//   * the scene serializer (engine/scene/SceneFile) can persist the authoring
//     fields with the .psyscene, and
//   * the editor Inspector snapshot (which only knows scene-level components)
//     sees them.
//
// CRITICAL LAYERING RULE: engine/scene must NOT depend on engine/physics. So
// these PODs reference NO physics:: type. The collider shape is a scene-level
// enum (ColliderShape) that MIRRORS physics::Shape's value order, and every
// physics runtime handle (BodyId / CharacterId / VehicleId) is stored as an
// OPAQUE u32 (the handle's .raw). The physics<->u32 mapping happens only at the
// physics boundary inside editor/play/PlayRuntime, which DOES include physics.
//
// Authored vs runtime fields:
//   * shape / mass / half_extent / friction / restitution (RigidBody),
//     height / radius / move_speed (Character), and the vehicle / helicopter
//     authoring params are designer-set in the editor and persist with the
//     scene.
//   * runtime_body / runtime_character / runtime_vehicle / runtime_chassis are
//     RUNTIME-only opaque handle values: 0 when not playing, filled by
//     PlayRuntime::begin() (handle.raw), cleared by PlayRuntime::end(). They are
//     NOT serialized.
//   * walk_dir is per-frame character input the host writes before tick();
//     ang_vel_est is the helicopter's runtime angular-velocity estimate.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"

#include <algorithm>
#include <cmath>

namespace psynder::scene {

// Scene-level collider shape. The value order MIRRORS physics::Shape so the
// physics boundary (PlayRuntime) can map with a single static_cast. Keep this
// in lock-step with physics::Shape (engine/physics/Physics.h).
enum class ColliderShape : u8 {
    Sphere,
    Capsule,
    Box,
    ConvexHull,
    Compound,
    Heightfield,
    TriangleMesh,
    // Infinite ground half-space (mirrors physics::Shape::Plane). half_extent is
    // ignored: the plane is the y=0 floor at the body's pose. Authoring a static
    // (mass 0) RigidBody with this shape gives a designer an in-editor flat floor
    // that does not tunnel (the #63 plane primitive handles the physics).
    Plane,
};

// A rigid body attached to an entity. mass == 0 => static collider (floors,
// walls). runtime_body is the opaque physics BodyId.raw, filled in begin() and
// processed column-at-a-time in tick()'s writeback query. RUNTIME-only; 0 when
// not playing; NOT serialized.
PSYNDER_COMPONENT(RigidBodyComponent) {
    ColliderShape shape = ColliderShape::Box;
    u8 _pad[3] = {};
    f32 mass = 1.0f;  // kg; 0 = static
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    u32 runtime_body = 0u;  // physics BodyId.raw; 0 when not playing (not saved)
};

// How a drivable vehicle's per-wheel suspension probe finds the ground (Wave 8,
// no-code terrain-on-vehicle authoring). Value order MIRRORS the physics ground
// binding the PlayRuntime selects: Plane => physics::vehicle::set_ground_plane
// (a flat world-Y half-space, the legacy default), Heightfield =>
// physics::vehicle::set_ground_heightfield (each wheel samples a procedural
// terrain surface so the chassis rides the relief instead of floating at a
// constant Y). Stored as a u8 in the component + the cooked SVHX chunk; 0 stays
// Plane so every pre-Wave-8 vehicle keeps its bit-for-bit flat-ground behaviour.
enum class VehicleGroundMode : u8 {
    Plane = 0,
    Heightfield = 1,
};

[[nodiscard]] constexpr VehicleGroundMode sanitize_vehicle_ground_mode(
    VehicleGroundMode m) noexcept {
    return (m == VehicleGroundMode::Heightfield) ? VehicleGroundMode::Heightfield
                                                 : VehicleGroundMode::Plane;
}

// A drivable vehicle attached to an entity. The chassis is an ordinary rigid
// box body; the vehicle solver runs inside World::step() and writes the chassis
// pose. Authored fields (half_extent / mass / engine torque / drag / wheel
// params / governor + steer authority / ground binding / is_player) persist with
// the scene; the four wheels are auto-placed at the corners of half_extent in
// begin() (front pair = steer/non-drive, rear pair = drive, RWD). runtime_vehicle
// (VehicleId.raw) + runtime_chassis (BodyId.raw) are runtime-only handle values
// filled by PlayRuntime::begin() and cleared by PlayRuntime::end(). is_player
// marks the car the WASD input drives and the chase camera follows.
//
// Ground binding (Wave 8): ground_mode selects how PlayRuntime grounds the four
// wheels. Plane (default) binds a flat plane at plane_y, identical to the legacy
// behaviour. Heightfield binds a procedural terrain surface the wheels probe per
// tick, so a designer can author a car that climbs hills with NO C++: the
// surface is fully described by the compact hf_* fields below (amplitude +
// spatial frequency + base height), evaluated by the pure, alloc-free
// vehicle_terrain_height() helper. Keeping the surface ON the component means the
// height source round-trips with the scene and needs no terrain subsystem.
PSYNDER_COMPONENT(VehicleComponent) {
    math::Vec3 half_extent{1.0f, 0.4f, 2.0f};  // chassis box half-extent (m)
    f32 mass = 1200.0f;                         // kg
    f32 engine_max_torque = 400.0f;             // N.m
    f32 drag = 0.30f;                           // aero drag coefficient
    f32 wheel_radius = 0.34f;                   // m
    f32 suspension = 0.35f;                     // rest length (m)
    f32 stiffness = 35000.0f;                   // N/m
    f32 damping = 4500.0f;                      // N.s/m
    // --- Speed governor + speed-scaled steering authority (Wave 8) --------
    // Mirror physics::vehicle::VehicleDesc. max_speed == 0 disables the
    // governor (legacy uncapped). The steer-authority trio defaults to a no-op
    // identity (full authority everywhere) so an un-authored car is unchanged.
    f32 max_speed = 0.0f;            // m/s governed forward cap; 0 = no governor
    f32 steer_full_speed = 0.0f;     // m/s; <= this -> full steering authority
    f32 steer_taper_speed = 0.0f;    // m/s; >= this -> steer_min_authority
    f32 steer_min_authority = 1.0f;  // 0..1 authority at/above the taper speed
    // --- Ground binding (Wave 8 terrain-on-vehicle) -----------------------
    VehicleGroundMode ground_mode = VehicleGroundMode::Plane;
    u8 is_player = 1u;  // WASD-driven + chase-cam target
    u8 _pad[2] = {};
    f32 plane_y = 0.0f;  // flat-ground world Y (Plane mode; also the hf fallback)
    // Procedural Heightfield surface (used only when ground_mode==Heightfield).
    // height(x,z) = hf_base_y + hf_amplitude * 0.5 * (sin(x*hf_frequency) +
    // sin(z*hf_frequency)). A compact, deterministic, alloc-free rolling-hills
    // field a designer tunes with two sliders; PlayRuntime feeds it to the
    // suspension probe via a borrowed HeightSampler.
    f32 hf_base_y = 0.0f;      // world Y the rolling surface oscillates about (m)
    f32 hf_amplitude = 4.0f;   // peak-to-trough half-height of the hills (m)
    f32 hf_frequency = 0.05f;  // spatial frequency (rad/m); larger = tighter hills
    u32 runtime_vehicle = 0u;  // physics VehicleId.raw; 0 when not playing (not saved)
    u32 runtime_chassis = 0u;  // physics BodyId.raw;    0 when not playing (not saved)
};

// Pure, deterministic, alloc-free procedural terrain height for a Heightfield-
// mode vehicle. Returns the world-Y of the authored rolling-hills surface under
// world column (x, z). Lives here (not in physics, which scene must not depend
// on) so PlayRuntime can wrap it in a physics::vehicle::HeightSampler. No RNG, no
// wall-clock, -fno-fast-math friendly, so a scripted drive reproduces bit-for-bit.
[[nodiscard]] inline f32 vehicle_terrain_height(const VehicleComponent& vc,
                                                f32 x,
                                                f32 z) noexcept {
    return vc.hf_base_y +
           vc.hf_amplitude * 0.5f *
               (std::sin(x * vc.hf_frequency) + std::sin(z * vc.hf_frequency));
}

// Clamp an authored VehicleComponent to sane ranges. Mirrors the gameplay/AI
// proxy sanitizers: idempotent, leaves the runtime handles untouched, zeroes pad.
[[nodiscard]] inline VehicleComponent sanitize_vehicle_component(VehicleComponent vc) noexcept {
    if (!(vc.half_extent.x > 0.0f)) vc.half_extent.x = 1.0f;
    if (!(vc.half_extent.y > 0.0f)) vc.half_extent.y = 0.4f;
    if (!(vc.half_extent.z > 0.0f)) vc.half_extent.z = 2.0f;
    if (!(vc.mass > 0.0f)) vc.mass = 1200.0f;
    if (!(vc.engine_max_torque >= 0.0f)) vc.engine_max_torque = 0.0f;
    if (!(vc.drag >= 0.0f)) vc.drag = 0.0f;
    if (!(vc.wheel_radius > 0.0f)) vc.wheel_radius = 0.34f;
    if (!(vc.suspension >= 0.0f)) vc.suspension = 0.0f;
    if (!(vc.stiffness >= 0.0f)) vc.stiffness = 0.0f;
    if (!(vc.damping >= 0.0f)) vc.damping = 0.0f;
    if (!(vc.max_speed >= 0.0f)) vc.max_speed = 0.0f;
    if (!(vc.steer_full_speed >= 0.0f)) vc.steer_full_speed = 0.0f;
    if (!(vc.steer_taper_speed >= 0.0f)) vc.steer_taper_speed = 0.0f;
    vc.steer_min_authority = std::clamp(vc.steer_min_authority, 0.0f, 1.0f);
    vc.ground_mode = sanitize_vehicle_ground_mode(vc.ground_mode);
    vc.is_player = vc.is_player != 0u ? 1u : 0u;
    vc._pad[0] = vc._pad[1] = 0u;
    if (!std::isfinite(vc.plane_y)) vc.plane_y = 0.0f;
    if (!std::isfinite(vc.hf_base_y)) vc.hf_base_y = 0.0f;
    if (!(vc.hf_amplitude >= 0.0f)) vc.hf_amplitude = 0.0f;
    if (!(vc.hf_frequency >= 0.0f)) vc.hf_frequency = 0.0f;
    return vc;
}

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
// from the torques it applies. runtime_body is the opaque chassis BodyId.raw,
// 0 when idle (not saved).
PSYNDER_COMPONENT(HelicopterComponent) {
    math::Vec3 half_extent{1.2f, 0.6f, 2.0f};  // chassis box half-extent (m)
    f32 mass = 900.0f;                          // kg
    f32 max_thrust_n = 14000.0f;                // N (must exceed m*g ~= 8829)
    f32 pitch_torque = 8000.0f;                 // N.m about body-right (cyclic)
    f32 roll_torque = 8000.0f;                  // N.m about body-forward (cyclic)
    f32 yaw_torque = 4000.0f;                   // N.m about body-up (pedals)
    f32 angular_damping = 2.0f;                 // 1/s exponential spin decay
    u8 hover_assist = 1u;                        // neutral collective holds m*g
    u8 is_player = 1u;                           // flight-input + chase-cam target
    u8 _pad[2] = {};
    u32 runtime_body = 0u;  // physics BodyId.raw; 0 when idle (not saved)
    math::Vec3 ang_vel_est{0.0f, 0.0f, 0.0f};  // runtime body angular vel estimate
};

// A kinematic capsule character. move_speed scales walk_dir each tick.
// runtime_character is the opaque physics CharacterId.raw, filled in begin() and
// driven in tick(). RUNTIME-only; 0 when not playing; NOT serialized.
PSYNDER_COMPONENT(CharacterControllerComponent) {
    f32 height = 1.8f;
    f32 radius = 0.35f;
    f32 move_speed = 4.5f;
    u32 runtime_character = 0u;  // physics CharacterId.raw; 0 when not playing (not saved)
    math::Vec3 walk_dir{0.0f, 0.0f, 0.0f};  // host-set per-frame intent (world XZ)
};

}  // namespace psynder::scene
