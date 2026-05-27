// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime (DESIGN.md SS10.1, editor-only
// direction 2026-05-26).
//
// PlayRuntime is the engine-side bridge the editor host drives when the user
// enters / leaves Play mode. It is DOTS-correct: ALL per-entity sim linkage
// lives in pooled ECS components (RigidBodyComponent::body,
// CharacterControllerComponent::character) processed column-at-a-time via
// registry().query<reads,writes>(). There is NO Entity->BodyId
// std::unordered_map side-table.
//
// The only PlayRuntime-owned heap is two pooled std::vectors:
//   * a per-begin scan list of entities that carry a RigidBodyComponent /
//     CharacterControllerComponent, and
//   * an authored-transform snapshot ({Entity, LocalTransform}) captured in
//     begin() and restored in end().
// Both are reserved once and only ever cleared (capacity kept) between
// sessions, so tick() allocates nothing.
//
// Lifecycle (host call sites):
//   begin(scene)        - on "Play": set gravity, scan RigidBody/Character
//                         entities, snapshot authored transforms, create the
//                         physics bodies/characters from the authored pose +
//                         component params, store the returned handles back
//                         into each entity's component.
//   tick(scene, dt) * N - per host frame while playing: step the world, write
//                         resolved body poses back into TransformComponent
//                         columns, drive characters, then run ONE
//                         graph().update_world_transforms().
//   end(scene)          - on "Stop": destroy all bodies/characters, clear the
//                         handle fields, restore the authored snapshot.
//
// Parent/world caveat: the writeback writes a WORLD pose into
// TransformComponent.local. That is exact only for top-level (unparented)
// entities; a parented body would need its parent's inverse world matrix
// folded in. This first pass targets top-level simulated entities.

#pragma once

#include "core/Types.h"
#include "editor/play/PlayComponents.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"

#include <vector>

namespace psynder::editor::play {

// Tuning the host can set before begin(). SI units.
struct PlayConfig {
    math::Vec3 gravity{0.0f, -9.81f, 0.0f};
};

class PlayRuntime {
   public:
    PlayRuntime() = default;
    ~PlayRuntime();

    PlayRuntime(const PlayRuntime&) = delete;
    PlayRuntime& operator=(const PlayRuntime&) = delete;

    void set_config(const PlayConfig& config) noexcept { config_ = config; }
    [[nodiscard]] const PlayConfig& config() const noexcept { return config_; }

    // --- Host entry points -------------------------------------------------
    void begin(scene::Scene& scene);
    void tick(scene::Scene& scene, f32 dt);
    void end(scene::Scene& scene);

    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] usize body_count() const noexcept { return body_count_; }

    // --- Vehicle intent ----------------------------------------------------
    // Per-frame driving intent the host writes before tick(): throttle/brake in
    // 0..1, steer in radians (left positive). Applied to every player vehicle
    // (VehicleComponent::is_player) at the top of tick(). Stored on the runtime
    // (not a component) because it is a single shared player input, not
    // per-entity sim state; it allocates nothing.
    void set_vehicle_input(f32 throttle, f32 brake, f32 steer) noexcept {
        vehicle_throttle_ = throttle;
        vehicle_brake_ = brake;
        vehicle_steer_ = steer;
    }

    // --- Helicopter intent -------------------------------------------------
    // Per-frame flight intent the host writes before tick(): collective in
    // -1..1 (ascend positive, descend negative), pitch/roll/yaw cyclic+pedal in
    // -1..1. Applied to every player helicopter (HelicopterComponent::is_player)
    // at the top of tick(), BEFORE the single World::step(). Stored on the
    // runtime (a single shared player input, not per-entity sim state); it
    // allocates nothing.
    void set_helicopter_input(f32 collective, f32 pitch, f32 roll, f32 yaw) noexcept {
        heli_collective_ = collective;
        heli_pitch_ = pitch;
        heli_roll_ = roll;
        heli_yaw_ = yaw;
    }

    // Position the active scene camera in a chase pose behind/above the player
    // vehicle's chassis (or, if none, the player helicopter's chassis), looking
    // at it. Alloc-free; no-op when not playing or no player craft exists.
    // Called from tick(); also exposed for the host.
    void update_chase_camera(scene::Scene& scene) noexcept;

    // --- Character intent --------------------------------------------------
    // Set the planar walk direction (world XZ; normalized internally, then
    // scaled by the component move_speed * dt) for a character entity. Stored
    // into the entity's CharacterControllerComponent.walk_dir column. No-op if
    // the entity has no CharacterControllerComponent.
    void set_character_input(scene::Scene& scene, Entity entity, math::Vec3 walk_dir) noexcept;

    // True post-move capsule centre for a character entity (read from the
    // engine's internal character store). Returns {0,0,0} if the entity has no
    // live character.
    [[nodiscard]] math::Vec3 character_position(scene::Scene& scene, Entity entity) const noexcept;

   private:
    // Authored-transform snapshot record. Pooled; reserved once.
    struct AuthoredTransform {
        Entity entity{};
        scene::LocalTransform local{};
    };

    void clear_pools() noexcept;

    PlayConfig config_{};
    bool playing_ = false;
    usize body_count_ = 0u;

    // Scan scratch: entities carrying a RigidBodyComponent, gathered (parallel,
    // mutex-merged) in begin() then consumed serially. Reserved once.
    std::vector<Entity> rigid_entities_;
    // Same for character entities.
    std::vector<Entity> character_entities_;
    // Same for vehicle entities.
    std::vector<Entity> vehicle_entities_;
    // Same for helicopter entities.
    std::vector<Entity> helicopter_entities_;
    // Authored transforms captured in begin(), restored in end(). Reserved once.
    std::vector<AuthoredTransform> authored_;

    // Shared player driving intent (host-set per frame; see set_vehicle_input).
    f32 vehicle_throttle_ = 0.0f;
    f32 vehicle_brake_ = 0.0f;
    f32 vehicle_steer_ = 0.0f;

    // Shared player flight intent (host-set per frame; see set_helicopter_input).
    f32 heli_collective_ = 0.0f;
    f32 heli_pitch_ = 0.0f;
    f32 heli_roll_ = 0.0f;
    f32 heli_yaw_ = 0.0f;
};

}  // namespace psynder::editor::play
