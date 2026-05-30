// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime (DESIGN.md SS10.1, editor-only
// direction 2026-05-26).
//
// PlayRuntime is the engine-side bridge the editor host drives when the user
// enters / leaves Play mode. It is DOTS-correct: ALL per-entity sim linkage
// lives in pooled scene-level ECS components (scene::RigidBodyComponent::
// runtime_body, scene::CharacterControllerComponent::runtime_character, ...)
// processed column-at-a-time via registry().query<reads,writes>(). The handles
// are stored as opaque u32 (the physics handle's .raw); this TU is the single
// place that maps them to/from physics:: types at the physics boundary. There
// is NO Entity->BodyId std::unordered_map side-table.
//
// The only PlayRuntime-owned heap is a small set of pooled std::vectors:
//   * one per-begin scan list per simulated kind -- the entities carrying a
//     RigidBodyComponent, CharacterControllerComponent, VehicleComponent, or
//     HelicopterComponent (gathered in begin(), consumed serially), and
//   * an authored-transform snapshot ({Entity, LocalTransform}) captured in
//     begin() and restored in end().
// All are reserved once and only ever cleared (capacity kept) between
// sessions, so tick() allocates nothing.
//
// Lifecycle (host call sites):
//   begin(scene)        - on "Play": set gravity, scan RigidBody/Character
//                         entities, snapshot authored transforms, create the
//                         physics bodies/characters from the authored pose +
//                         component params, store the returned handles back
//                         into each entity's component. ALSO resets the reused
//                         gameplay contexts (combat scratch, AI clock, PsyGraph
//                         instances) so a new Play session starts clean.
//   tick(scene, dt) * N - per host frame while playing: step the world, write
//                         resolved body poses back into TransformComponent
//                         columns, drive characters, then run the GAMEPLAY phase
//                         (combat -> AI -> PsyGraph), then ONE
//                         graph().update_world_transforms().
//   end(scene)          - on "Stop": destroy all bodies/characters, clear the
//                         handle fields, restore the authored snapshot.
//
// GAMEPLAY PHASE (added; runs only while playing, AFTER the physics step +
// transform writeback, BEFORE the final graph sync / world-transform recompute):
//   per-tick order is
//     input -> physics step -> transform writeback + graph-sync (existing)
//       -> combat tick      (cooldowns, projectile integration + hits, damage
//                            flush, death resolution; gameplay::*)
//       -> AI perceive/think/act (ai::*; LOS via world_.raycast, fire via
//                            gameplay::fire_weapon, movement written into the
//                            agent's TransformComponent)
//       -> psygraph tick    (psygraph::GraphRuntime::tick_psygraphs; action
//                            hooks bound to gameplay::apply_damage / scene spawn
//                            / set-active / log)
//       -> chase cam
//   Everything in this phase is alloc-free in the steady state: the CombatContext
//   scratch, the AiContext, the PsyGraph instance pool, and the per-entity
//   HostContext are all reused PlayRuntime members (reserved/created once, only
//   reset between sessions). Host hooks are plain function pointers / a single
//   reused std::function set, never re-bound per frame, so no per-tick heap.
//   Deterministic: the systems are pure sweeps with no RNG or wall-clock reads.
//
// Parenting: the writeback resolves each simulated body's WORLD pose, then
// stores it into TransformComponent.local correctly for BOTH top-level and
// parented entities. For a parented body the world pose is folded into the
// parent's local space (local = inverse(parent_world) * body_world) using the
// SceneGraph world matrix from the previous tick's update -- exact for a static
// parent, one-frame-lagged for a moving one. Top-level entities take the world
// pose directly. The chase camera still only follows a top-level camera.

#pragma once

#include "ai/AiSystems.h"
#include "core/Types.h"
#include "editor/play/PlayComponents.h"
#include "gameplay/CombatSystems.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"
#include "script/psygraph/EcsBinding.h"

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

    // --- Gameplay tuning ---------------------------------------------------
    // The combat friendly-fire / neutral policy applied every tick. Host may set
    // it before begin(); defaults to friendly-fire OFF (same faction never hurts).
    void set_combat_config(const gameplay::CombatConfig& config) noexcept {
        combat_config_ = config;
    }
    [[nodiscard]] const gameplay::CombatConfig& combat_config() const noexcept {
        return combat_config_;
    }

    // --- Gameplay introspection (tests / tools) ----------------------------
    // Shots the AI fire-hook actually let out this Play session (running tally,
    // reset on begin()). Lets a test confirm AI engaged a target.
    [[nodiscard]] u32 ai_shots_fired() const noexcept { return ai_shots_total_; }

    // Bind a compiled PsyGraph asset into the runtime and attach an instance to
    // `entity` (alloc happens here, never during tick). Returns false if the
    // graph failed to compile or the entity already carries the component. The
    // host/editor calls this when authoring a PsyGraphComponent; tests use it to
    // wire a graph onto an entity. Safe to call before or between Play sessions.
    bool bind_psygraph(scene::Scene& scene,
                       Entity entity,
                       const script::psygraph::Graph& graph);

   private:
    // Authored-transform snapshot record. Pooled; reserved once.
    struct AuthoredTransform {
        Entity entity{};
        scene::LocalTransform local{};
    };

    void clear_pools() noexcept;

    // Map every scene-level gameplay/AI authoring proxy onto an entity into the
    // matching live gameplay::/ai:: component the combat + AI systems consume.
    // Runs at the top of begin() BEFORE the physics + AI scans so the AI scan
    // finds the synthesised ai::AiAgentComponent. Idempotent: skips a kind that
    // is already present (a demo that added the live component directly).
    void synthesize_authored_gameplay(scene::Scene& scene);
    // Inverse: strip the live gameplay/AI components off the entities we
    // synthesised onto in begin(), restoring the editor scene to proxy-only state.
    void clear_authored_gameplay(scene::Scene& scene);

    // --- Gameplay phase (called from tick, in this order) ------------------
    // Combat: cooldowns -> projectile integration + hits -> damage flush ->
    // projectile cleanup -> death resolution. Reuses combat_ctx_ scratch.
    void tick_combat(scene::Scene& scene, f32 dt);
    // AI: perceive -> think -> act over every AiAgentComponent entity. LOS,
    // fire, and the shot tally are wired through ai_ctx_'s host hooks (bound
    // once in begin()). Moved agents land in TransformComponent (later folded
    // into the graph by the consolidation sync).
    void tick_ai(scene::Scene& scene, f32 dt);
    // PsyGraph: OnStart (once) + OnTick for every PsyGraphComponent entity, with
    // the reused HostContext's action hooks bound to gameplay/scene.
    void tick_psygraphs(scene::Scene& scene, f32 dt);

    // Reset the reused gameplay contexts for a fresh Play session (begin()).
    void reset_gameplay() noexcept;

    // --- AI host hooks (bound once into ai_ctx_, never per-frame) ----------
    // Plain static functions matching ai::LosFn / ai::FireFn so they can be
    // stored as function pointers (no std::function heap). `user` is `this`.
    // LOS: clear when nothing blocks origin->target. Wired to world_.raycast --
    // occluded if a physics body is struck closer than the target distance.
    static bool ai_los_hook(void* user, math::Vec3 origin, math::Vec3 target);
    // Fire: the agent shoots `target` via gameplay::fire_weapon (origin = agent
    // position, dir = toward the target), queuing damage into combat_ctx_.
    static bool ai_fire_hook(void* user, Entity agent, Entity target);

    PlayConfig config_{};
    bool playing_ = false;
    usize body_count_ = 0u;

    // This Play session's OWN physics world. Instance-owned (not the process
    // global physics::World::Get()), so each Play session is isolated: bodies,
    // vehicles, and characters created here never leak into another session or
    // into editor-side code that still uses the default world. begin() clears
    // it so a new session always starts from an empty world; end() destroys the
    // handles it created. The owner is swappable later (a per-scene sim
    // resource) by changing only who holds this member. Declared after the
    // pools so it tears down before them is irrelevant (independent state).
    physics::World world_;

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
    // Entities carrying an AiAgentComponent, gathered in begin(). The AI act()
    // pass moves these via TransformComponent; the consolidation graph-sync at
    // the end of tick() pushes their (possibly moved) local into the graph so
    // the renderer sees AI motion. Reserved once.
    std::vector<Entity> ai_entities_;
    // Entities that carry a scene-level gameplay/AI AUTHORING proxy
    // (scene::FactionComponent / HitboxComponent / WeaponModeComponent /
    // AiAgentComponent / PerceptionComponent / PatrolComponent). begin() maps the
    // proxy into the matching live gameplay::/ai:: component the combat + AI
    // systems tick; end() strips the live components back off so the editor scene
    // returns to proxy-only state and a second Play session re-synthesises cleanly.
    // Only entities WE synthesised onto are recorded (a demo that added the live
    // component directly is left untouched). Reserved once.
    std::vector<Entity> authored_gameplay_entities_;

    // --- Reused, alloc-free gameplay contexts ------------------------------
    // Combat scratch + policy. ctx scratch is reserve()d in begin() and only
    // ever begin_tick()-cleared (capacity kept) per tick, so combat never heap-
    // allocates in the steady state. Policy is host-set via set_combat_config().
    gameplay::CombatConfig combat_config_{};
    gameplay::CombatContext combat_ctx_{};

    // AI context: host hooks (LOS / fire) are bound ONCE in begin() to this
    // PlayRuntime's raycast + fire wrappers; perceive/think/act own no per-frame
    // heap. apply_move stays null so act() writes the kinematic step straight
    // into the agent's TransformComponent (deterministic, no character store).
    ai::AiContext ai_ctx_{};
    // Running AI shot tally accumulated across the session from ai_ctx_.
    u32 ai_shots_total_ = 0u;

    // PsyGraph runtime owns the compiled programs + the pooled per-instance
    // VmStates (one-time alloc on bind_psygraph). The reused HostContext is
    // refilled (not reallocated) per entity inside tick_psygraphs; its action
    // hooks are bound once in reset_gameplay(). The lambdas capture only
    // active_scene_, so they never re-bind per frame.
    script::psygraph::GraphRuntime psygraph_runtime_{};
    script::psygraph::HostContext psygraph_host_{};
    bool psygraph_hooks_bound_ = false;

    // Transient: the scene currently being ticked. Set at the top of tick()
    // (and bind_psygraph) so the C-pointer AI host hooks + the PsyGraph action
    // lambdas can reach the scene without a per-frame closure allocation. Only
    // ever read while a tick is in flight; null otherwise.
    scene::Scene* active_scene_ = nullptr;

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
