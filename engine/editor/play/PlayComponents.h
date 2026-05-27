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
