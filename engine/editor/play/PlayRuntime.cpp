// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime implementation.
//
// See PlayRuntime.h for the lifecycle contract and the DOTS rationale. All
// per-entity sim state lives in pooled ECS components; this TU only owns two
// pooled scratch vectors (entity scan lists) + the authored-transform
// snapshot, all reserved once and reused.
//
// This runtime owns its OWN physics::World (the world_ member), so each Play
// session is isolated from the editor's default world and from any other
// session. begin() resets it to empty; the create_*/destroy_* calls and
// world_.step() all operate on world_ explicitly.
//
// Writeback parallelism: World::get_position / get_rotation are pure const
// reads -- they read this world's body state and interpolate prev/current;
// they NEVER mutate a body or lazily allocate. step() (the only writer)
// completes BEFORE the writeback query runs, and the query writes only its own
// TransformComponent column (per-row safe). So the writeback runs in parallel
// safely. The character writeback is a short serial pass (few characters;
// physics::character::move mutates this world's character store and must not
// run concurrently).

#include "editor/play/PlayRuntime.h"

#include "ai/AiComponents.h"            // AiAgentComponent (gameplay AI phase)
#include "gameplay/GameplayComponents.h"  // WeaponComponent / HealthComponent / ...
#include "math/MathExt.h"               // inverse_affine (parenting writeback)
#include "scene/GameplayComponents.h"   // scene-level gameplay/AI authoring proxies
#include "scene/ScriptComponents.h"     // scene-level ScriptGraphComponent (authored graph link)
#include "script/psygraph/Graph.h"      // psygraph::Graph (deserialize target)
#include "script/psygraph/Serialize.h"  // psygraph::deserialize_graph (blob -> graph)

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string_view>

namespace psynder::editor::play {

namespace {

// Extract a (translation, rotation) world pose from a rigid 4x4 world matrix.
// Reused for the parenting-aware writeback: a simulated body's WORLD pose must
// be re-expressed in its parent's local space before it can be stored into
// TransformComponent.local. Decomposition mirrors the scene save path
// (SceneFile.cpp decompose_world_transform / quat_from_basis): normalize the
// three basis columns, fix handedness, and build a quaternion from the basis.
// Authored (non-uniform) scale on the body is never produced by physics, so
// only translation + rotation are written; the caller preserves lt.scale.
struct WorldPose {
    math::Vec3 translation{};
    math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

[[nodiscard]] math::Vec3 matrix_column(const math::Mat4& m, u32 column) noexcept {
    const u32 base = column * 4u;
    return {m.m[base + 0u], m.m[base + 1u], m.m[base + 2u]};
}

[[nodiscard]] math::Quat quat_from_basis(math::Vec3 x, math::Vec3 y, math::Vec3 z) noexcept {
    const f32 m00 = x.x, m01 = y.x, m02 = z.x;
    const f32 m10 = x.y, m11 = y.y, m12 = z.y;
    const f32 m20 = x.z, m21 = y.z, m22 = z.z;
    const f32 trace = m00 + m11 + m22;

    math::Quat q{};
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return math::quat_normalize(q);
}

// Decompose a node's WORLD matrix into a rigid (translation, rotation) pose,
// dropping any scale. Used to SPAWN the physics body at the entity's true world
// pose (scene.transform() returns the parent-LOCAL transform, which is the
// world pose only for top-level entities). Mirrors the basis normalization in
// world_pose_in_parent below.
[[nodiscard]] WorldPose world_pose_from_matrix(const math::Mat4& world) noexcept {
    WorldPose out{};
    out.translation = {world.m[12], world.m[13], world.m[14]};

    constexpr f32 kMinScale = 1.0e-6f;
    math::Vec3 cx = matrix_column(world, 0u);
    math::Vec3 cy = matrix_column(world, 1u);
    math::Vec3 cz = matrix_column(world, 2u);
    const f32 sx = math::length(cx);
    const f32 sy = math::length(cy);
    const f32 sz = math::length(cz);
    if (sx <= kMinScale || sy <= kMinScale || sz <= kMinScale)
        return out;  // degenerate basis: identity rotation
    cx = math::mul(cx, 1.0f / sx);
    cy = math::mul(cy, 1.0f / sy);
    cz = math::mul(cz, 1.0f / sz);
    if (math::dot(math::cross(cx, cy), cz) < 0.0f)
        cz = math::mul(cz, -1.0f);
    out.rotation = quat_from_basis(cx, cy, cz);
    return out;
}

// Convert a simulated body's WORLD pose (world_pos, world_rot) into the local
// pose under `parent_world`: local = inverse(parent_world) * body_world. Pure,
// alloc-free. The parent world matrix may carry scale; we strip it from the
// rotation basis (normalized columns) so the result is a clean rigid pose.
[[nodiscard]] WorldPose world_pose_in_parent(const math::Mat4& parent_world,
                                             math::Vec3 world_pos,
                                             math::Quat world_rot) noexcept {
    const math::Mat4 inv_parent = math::inverse_affine(parent_world);
    const math::Mat4 body_world = math::mul_affine(math::translate(world_pos),
                                                   math::rotate_quat(world_rot));
    const math::Mat4 local = math::mul_affine(inv_parent, body_world);

    WorldPose out{};
    out.translation = {local.m[12], local.m[13], local.m[14]};

    constexpr f32 kMinScale = 1.0e-6f;
    math::Vec3 cx = matrix_column(local, 0u);
    math::Vec3 cy = matrix_column(local, 1u);
    math::Vec3 cz = matrix_column(local, 2u);
    const f32 sx = math::length(cx);
    const f32 sy = math::length(cy);
    const f32 sz = math::length(cz);
    if (sx <= kMinScale || sy <= kMinScale || sz <= kMinScale) {
        // Degenerate parent basis: keep the body's own world rotation rather
        // than emitting NaNs.
        out.rotation = math::quat_normalize(world_rot);
        return out;
    }
    cx = math::mul(cx, 1.0f / sx);
    cy = math::mul(cy, 1.0f / sy);
    cz = math::mul(cz, 1.0f / sz);
    if (math::dot(math::cross(cx, cy), cz) < 0.0f)
        cz = math::mul(cz, -1.0f);
    out.rotation = quat_from_basis(cx, cy, cz);
    return out;
}

// Collider half-extent in WORLD units = the component's object-local half-extent
// (sized from the mesh's unit bounds) times the entity's transform scale.
// Without this a scaled object -- e.g. a floor authored scale (3, 0.035, 3) on a
// unit-cube mesh -- gets a 1 m cube collider and dynamic bodies miss it / fall
// through. abs() + a small floor so a zero/negative authored scale can't make a
// degenerate (zero/inverted) collider. (Uses the entity's own local scale;
// exact for unparented entities -- the same top-level caveat as the writeback.)
[[nodiscard]] math::Vec3 scaled_half_extent(math::Vec3 half, math::Vec3 scale) noexcept {
    constexpr f32 kMin = 1.0e-4f;
    return {std::max(kMin, half.x * std::fabs(scale.x)),
            std::max(kMin, half.y * std::fabs(scale.y)),
            std::max(kMin, half.z * std::fabs(scale.z))};
}

}  // namespace

PlayRuntime::~PlayRuntime() {
    // The handles needed to clear the ECS component columns + restore authored
    // transforms live in the Scene, which we cannot reach without a Scene&. The
    // host contract is to always call end() on Stop / teardown, which does that
    // restore; the destructor only marks state. The owned `world_` member frees
    // ALL its bodies/vehicles/characters automatically when it is destroyed
    // right after this — no leak even if end() was skipped.
    playing_ = false;
}

void PlayRuntime::clear_pools() noexcept {
    rigid_entities_.clear();
    character_entities_.clear();
    vehicle_entities_.clear();
    helicopter_entities_.clear();
    ai_entities_.clear();
    authored_gameplay_entities_.clear();
    authored_script_entities_.clear();
    authored_.clear();
    body_count_ = 0u;
}

void PlayRuntime::synthesize_authored_gameplay(scene::Scene& scene) {
    scene::EcsRegistry& reg = scene.registry();
    // The host main loop runs with structural-deferred ON, which would QUEUE the
    // adds below instead of applying them immediately — but the AI/physics scans
    // later in begin() must SEE the synthesised live components this same frame.
    // Force immediate structural mutation for this pass, then restore the host's
    // mode (same discipline as the snapshot-restore path in the host).
    const bool restore_deferred = reg.structural_deferred();
    reg.set_structural_deferred(false);

    const u32 total = reg.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = reg.snapshot_live_entities(entities);
    entities.resize(copied);

    for (const Entity e : entities) {
        bool synthesised = false;

        // Faction proxy -> gameplay::FactionComponent.
        if (const auto* fac = reg.get<scene::FactionComponent>(e);
            fac != nullptr && reg.get<gameplay::FactionComponent>(e) == nullptr) {
            gameplay::FactionComponent live{};
            live.faction = fac->faction;
            reg.add<gameplay::FactionComponent>(e, live);
            synthesised = true;
        }

        // Hitbox proxy -> gameplay::HitboxComponent (what combat ray-tests).
        if (const auto* hb = reg.get<scene::HitboxComponent>(e);
            hb != nullptr && reg.get<gameplay::HitboxComponent>(e) == nullptr) {
            gameplay::HitboxComponent live{};
            live.offset = hb->offset;
            live.half_extent = hb->half_extent;
            live.radius = hb->radius;
            live.enabled = hb->enabled;
            reg.add<gameplay::HitboxComponent>(e, gameplay::sanitize_hitbox(live));
            synthesised = true;
        }

        // Weapon-mode proxy -> gameplay::WeaponRuntimeComponent (fire kind +
        // projectile params; the live cooldown starts at 0).
        if (const auto* wm = reg.get<scene::WeaponModeComponent>(e);
            wm != nullptr && reg.get<gameplay::WeaponRuntimeComponent>(e) == nullptr) {
            gameplay::WeaponRuntimeComponent live{};
            live.kind = (wm->kind == scene::WeaponFireKind::Projectile)
                            ? gameplay::WeaponKind::Projectile
                            : gameplay::WeaponKind::Hitscan;
            live.cooldown = 0.0f;
            live.projectile_speed = wm->projectile_speed;
            live.projectile_life = wm->projectile_life;
            reg.add<gameplay::WeaponRuntimeComponent>(
                e, gameplay::sanitize_weapon_runtime(live));
            synthesised = true;
        }

        // AI-agent proxy -> ai::AiAgentComponent (the FSM brain the AI scan +
        // perceive/think/act consume). Runtime target/cooldown start cleared.
        if (const auto* agent = reg.get<scene::AiAgentComponent>(e);
            agent != nullptr && reg.get<ai::AiAgentComponent>(e) == nullptr) {
            ai::AiAgentComponent live{};
            live.state = static_cast<ai::AiState>(agent->state);
            live.sight_range = agent->sight_range;
            live.fov_cos = agent->fov_cos;
            live.attack_range = agent->attack_range;
            live.think_interval = agent->think_interval;
            live.move_speed = agent->move_speed;
            reg.add<ai::AiAgentComponent>(e, ai::sanitize_ai_agent(live));
            synthesised = true;
        }

        // Perception proxy -> ai::PerceptionComponent (sense snapshot, starts
        // at its POD default).
        if (reg.get<scene::PerceptionComponent>(e) != nullptr &&
            reg.get<ai::PerceptionComponent>(e) == nullptr) {
            reg.add<ai::PerceptionComponent>(e, ai::PerceptionComponent{});
            synthesised = true;
        }

        // Patrol proxy -> ai::PatrolComponent (waypoint ring; current/wait_timer
        // start at 0).
        if (const auto* patrol = reg.get<scene::PatrolComponent>(e);
            patrol != nullptr && reg.get<ai::PatrolComponent>(e) == nullptr) {
            ai::PatrolComponent live{};
            const u32 count =
                std::min<u32>(patrol->count, ai::PatrolComponent::kMaxWaypoints);
            live.count = count;
            for (u32 wp = 0u; wp < count; ++wp)
                live.waypoints[wp] = patrol->waypoints[wp];
            live.wait_time = patrol->wait_time;
            live.arrive_radius = patrol->arrive_radius;
            reg.add<ai::PatrolComponent>(e, ai::sanitize_patrol(live));
            synthesised = true;
        }

        if (synthesised)
            authored_gameplay_entities_.push_back(e);
    }

    reg.set_structural_deferred(restore_deferred);
}

void PlayRuntime::clear_authored_gameplay(scene::Scene& scene) {
    scene::EcsRegistry& reg = scene.registry();
    const bool restore_deferred = reg.structural_deferred();
    reg.set_structural_deferred(false);
    // Strip the live gameplay/AI components back off the entities we synthesised
    // onto in begin(). We only ever recorded an entity if WE added at least one
    // live component to it (a demo that added the live component directly is not
    // in this list, so its components survive Stop). Removing a kind that is
    // absent is harmless. Guarding each remove on the matching authoring proxy
    // ensures we never strip a live component that did not come from a proxy.
    for (const Entity e : authored_gameplay_entities_) {
        if (!reg.alive(e))
            continue;
        if (reg.get<scene::FactionComponent>(e) != nullptr &&
            reg.get<gameplay::FactionComponent>(e) != nullptr)
            reg.remove<gameplay::FactionComponent>(e);
        if (reg.get<scene::HitboxComponent>(e) != nullptr &&
            reg.get<gameplay::HitboxComponent>(e) != nullptr)
            reg.remove<gameplay::HitboxComponent>(e);
        if (reg.get<scene::WeaponModeComponent>(e) != nullptr &&
            reg.get<gameplay::WeaponRuntimeComponent>(e) != nullptr)
            reg.remove<gameplay::WeaponRuntimeComponent>(e);
        if (reg.get<scene::AiAgentComponent>(e) != nullptr &&
            reg.get<ai::AiAgentComponent>(e) != nullptr)
            reg.remove<ai::AiAgentComponent>(e);
        if (reg.get<scene::PerceptionComponent>(e) != nullptr &&
            reg.get<ai::PerceptionComponent>(e) != nullptr)
            reg.remove<ai::PerceptionComponent>(e);
        if (reg.get<scene::PatrolComponent>(e) != nullptr &&
            reg.get<ai::PatrolComponent>(e) != nullptr)
            reg.remove<ai::PatrolComponent>(e);
    }

    reg.set_structural_deferred(restore_deferred);
}

void PlayRuntime::synthesize_authored_scripts(scene::Scene& scene) {
    scene::EcsRegistry& reg = scene.registry();
    // Same structural-deferred discipline as synthesize_authored_gameplay: force
    // immediate adds so the PsyGraph tick this session sees the bound component.
    const bool restore_deferred = reg.structural_deferred();
    reg.set_structural_deferred(false);

    const u32 total = reg.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = reg.snapshot_live_entities(entities);
    entities.resize(copied);

    // One reused Graph for the deserialize+compile of each entity's blob. The
    // compile (inside bind_psygraph) is a one-time per-session cost; the per-tick
    // VM run is alloc-free. We never touch the graph after binding.
    script::psygraph::Graph graph;
    std::string err;
    for (const Entity e : entities) {
        const auto* link = reg.get<scene::ScriptGraphComponent>(e);
        if (link == nullptr || link->graph_slot == scene::kInvalidScriptGraphSlot)
            continue;
        // A demo that bound a live psygraph::PsyGraphComponent directly is left
        // untouched (bind_psygraph also rejects an already-bound entity).
        if (reg.get<script::psygraph::PsyGraphComponent>(e) != nullptr)
            continue;
        const std::span<const u8> blob = scene.script_graph(link->graph_slot);
        if (blob.empty())
            continue;
        if (!script::psygraph::deserialize_graph(blob, graph, err))
            continue;  // corrupt blob: skip this entity, keep the session alive
        if (bind_psygraph(scene, e, graph))
            authored_script_entities_.push_back(e);
    }

    reg.set_structural_deferred(restore_deferred);
}

void PlayRuntime::clear_authored_scripts(scene::Scene& scene) {
    scene::EcsRegistry& reg = scene.registry();
    const bool restore_deferred = reg.structural_deferred();
    reg.set_structural_deferred(false);
    // Strip the psygraph::PsyGraphComponent off the entities WE bound a graph
    // onto in begin(), restoring the scene to authoring-only state so a second
    // Play session re-binds cleanly. The compiled programs + VmState pool stay
    // owned by psygraph_runtime_ (re-registered next session); removing a kind
    // that is absent is harmless.
    for (const Entity e : authored_script_entities_) {
        if (!reg.alive(e))
            continue;
        if (reg.get<script::psygraph::PsyGraphComponent>(e) != nullptr)
            reg.remove<script::psygraph::PsyGraphComponent>(e);
    }
    reg.set_structural_deferred(restore_deferred);
}

void PlayRuntime::begin(scene::Scene& scene) {
    // Idempotent: a stray double-begin tears down the previous session first.
    if (playing_)
        end(scene);

    clear_pools();

    // Map editor-authored gameplay/AI proxies into the live gameplay::/ai::
    // components the combat + AI systems tick. MUST run before the scans below so
    // the AI scan finds the synthesised ai::AiAgentComponent. Structural adds
    // here are safe: they run before any query iteration this session.
    synthesize_authored_gameplay(scene);

    // Start this session from a clean, empty world. Move-assigning a freshly
    // constructed World drops every body/vehicle/character from any prior
    // session (the old WorldImpl is destroyed) — isolated sessions, a key win
    // of the instance-owned world. All handles stored on components are about
    // to be overwritten by the create_* calls below.
    world_ = physics::World{};

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = world_;
    world.set_gravity(config_.gravity);

    // --- Phase 1: scan (parallel, mutex-merged) ---------------------------
    // Gather entities that HAVE a RigidBodyComponent. We read the entity column
    // (via SceneNodeComponent) row-aligned with the rigid body column. The
    // query body fires once per chunk across worker threads, so the shared
    // append is serialized by a mutex and each chunk builds a local buffer
    // first (the gather_scene_render_items pattern).
    // Each chunk's `nodes` span is exactly its live-row count, so we drive the
    // append straight off nodes.size() -- there is NO fixed per-chunk cap that
    // could silently drop rows beyond N (the previous std::array<,256> + break
    // would have dropped any row past 256 in a chunk). Each chunk's entities are
    // appended under the mutex; reserve-then-push keeps it amortized alloc-free.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, scene::RigidBodyComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const scene::RigidBodyComponent> bodies) {
                const usize n = std::min(nodes.size(), bodies.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                rigid_entities_.reserve(rigid_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    rigid_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for character entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, scene::CharacterControllerComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const scene::CharacterControllerComponent> chars) {
                const usize n = std::min(nodes.size(), chars.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                character_entities_.reserve(character_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    character_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for vehicle entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, scene::VehicleComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const scene::VehicleComponent> vehicles) {
                const usize n = std::min(nodes.size(), vehicles.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                vehicle_entities_.reserve(vehicle_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    vehicle_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for helicopter entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, scene::HelicopterComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const scene::HelicopterComponent> helis) {
                const usize n = std::min(nodes.size(), helis.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                helicopter_entities_.reserve(helicopter_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    helicopter_entities_.push_back(nodes[i].entity);
            });
    }

    // Same scan for AI agent entities. The AI act() pass steers these via their
    // TransformComponent each tick; we gather them once so the end-of-tick
    // graph-sync can push their moved local into the SceneGraph (the renderer
    // source). AiAgentComponent lives in the ai/ lane; the scan only needs the
    // row-aligned entity column.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, ai::AiAgentComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const ai::AiAgentComponent> agents) {
                const usize n = std::min(nodes.size(), agents.size());
                if (n == 0u)
                    return;
                std::scoped_lock lock{append_mutex};
                ai_entities_.reserve(ai_entities_.size() + n);
                for (usize i = 0; i < n; ++i)
                    ai_entities_.push_back(nodes[i].entity);
            });
    }

    authored_.reserve(rigid_entities_.size() + character_entities_.size() +
                      vehicle_entities_.size() + helicopter_entities_.size());

    // --- Phase 2: build bodies (serial, main thread) ----------------------
    // create_body mutates the shared World, so this MUST be serial. We snapshot
    // the entity's authored (parent-LOCAL) transform for restore, create the
    // body at the entity's WORLD pose, and write the handle back. Physics
    // operates entirely in world space, so a parented body must SPAWN at its
    // world pose (scene.transform() returns the parent-LOCAL transform, which is
    // the world pose only for top-level entities). Refresh world matrices once
    // so a parented entity's world pose is current.
    scene::SceneGraph& graph = scene.graph();
    graph.update_world_transforms();

    // World spawn pose for an entity: its decomposed world matrix when parented,
    // else its authored local (local == world for a top-level entity). Alloc-free.
    const auto world_spawn = [&](const scene::LocalTransform& local,
                                 Entity e) noexcept -> WorldPose {
        const scene::SceneNode node = scene.node(e);
        if (node.valid() && graph.parent(node).valid())
            return world_pose_from_matrix(graph.world_matrix(node));
        return WorldPose{local.translation, math::quat_normalize(local.rotation)};
    };

    for (const Entity e : rigid_entities_) {
        scene::RigidBodyComponent* rb = reg.get<scene::RigidBodyComponent>(e);
        if (rb == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});
        const WorldPose spawn = world_spawn(local, e);

        physics::BodyDesc d{};
        // Map the scene-level collider shape to physics::Shape (the enums share
        // value order, so a single cast is exact — see ColliderShape's contract).
        d.shape = static_cast<physics::Shape>(rb->shape);
        d.mass = rb->mass;  // 0 => static
        d.position = spawn.translation;
        d.rotation = spawn.rotation;
        d.half_extent = scaled_half_extent(rb->half_extent, local.scale);
        d.friction = rb->friction;
        d.restitution = rb->restitution;

        // Store the opaque BodyId.raw back into the component's runtime field.
        const physics::BodyId body = world.create_body(d);
        rb->runtime_body = body.raw;
        if (body.valid())
            ++body_count_;
    }

    // Characters.
    for (const Entity e : character_entities_) {
        scene::CharacterControllerComponent* cc = reg.get<scene::CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});

        physics::character::CharacterDesc d{};
        d.position = world_spawn(local, e).translation;
        d.height = cc->height;
        d.radius = cc->radius;
        cc->runtime_character = physics::character::create(d, world).raw;
        cc->walk_dir = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    // Vehicles (serial; create_body + vehicle::create mutate the shared World).
    // The chassis is a dynamic box body at the authored pose; the four wheels
    // are auto-placed at the corners of half_extent. Front pair (-Z, the car's
    // forward is -Z to match the camera convention) = steer/non-drive, rear pair
    // (+Z) = drive (RWD: the solver treats wheels[2],[3] as the drive axle).
    for (const Entity e : vehicle_entities_) {
        scene::VehicleComponent* vc = reg.get<scene::VehicleComponent>(e);
        if (vc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});
        const WorldPose spawn = world_spawn(local, e);

        physics::BodyDesc cd{};
        cd.shape = physics::Shape::Box;
        cd.mass = vc->mass;
        cd.position = spawn.translation;
        cd.rotation = spawn.rotation;
        cd.half_extent = scaled_half_extent(vc->half_extent, local.scale);
        cd.friction = 0.5f;
        cd.restitution = 0.0f;
        // Store the opaque chassis BodyId.raw back into the component.
        const physics::BodyId chassis = world.create_body(cd);
        vc->runtime_chassis = chassis.raw;
        if (!chassis.valid())
            continue;
        ++body_count_;

        // Four wheels at the half-extent corners, dropped to the chassis bottom.
        const f32 hx = vc->half_extent.x;
        const f32 hy = vc->half_extent.y;
        const f32 hz = vc->half_extent.z;
        std::array<physics::vehicle::WheelDesc, 4> wheels{};
        // Order: [0]=front-left, [1]=front-right (steer); [2]=rear-left,
        // [3]=rear-right (drive). Forward is -Z.
        const std::array<math::Vec3, 4> corners{
            math::Vec3{-hx, -hy, -hz}, math::Vec3{hx, -hy, -hz},
            math::Vec3{-hx, -hy, hz},  math::Vec3{hx, -hy, hz}};
        for (usize w = 0; w < 4; ++w) {
            wheels[w].local_position = corners[w];
            wheels[w].radius = vc->wheel_radius;
            wheels[w].suspension = vc->suspension;
            wheels[w].stiffness = vc->stiffness;
            wheels[w].damping = vc->damping;
        }

        physics::vehicle::VehicleDesc vd{};
        vd.chassis = chassis;
        vd.wheels = std::span<const physics::vehicle::WheelDesc>{wheels.data(), wheels.size()};
        vd.engine_max_torque = vc->engine_max_torque;
        vd.drag_coefficient = vc->drag;
        vd.downforce_coefficient = 0.0f;
        // Store the opaque VehicleId.raw back into the component.
        const physics::vehicle::VehicleId vehicle = physics::vehicle::create(vd, world);
        vc->runtime_vehicle = vehicle.raw;
        // Flat ground plane at y=0 (no terrain yet; KNOWN follow-up).
        physics::vehicle::set_ground_plane(vehicle, 0.0f, world);
    }

    // Helicopters (serial; create_body mutates the shared World). The chassis is
    // a dynamic box body at the authored pose. The angular-velocity estimate is
    // zeroed so the craft starts at rest.
    for (const Entity e : helicopter_entities_) {
        scene::HelicopterComponent* hc = reg.get<scene::HelicopterComponent>(e);
        if (hc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});
        const WorldPose spawn = world_spawn(local, e);

        physics::BodyDesc hd{};
        hd.shape = physics::Shape::Box;
        hd.mass = hc->mass;
        hd.position = spawn.translation;
        hd.rotation = spawn.rotation;
        hd.half_extent = scaled_half_extent(hc->half_extent, local.scale);
        hd.friction = 0.5f;
        hd.restitution = 0.0f;
        // Store the opaque chassis BodyId.raw back into the component.
        const physics::BodyId body = world.create_body(hd);
        hc->runtime_body = body.raw;
        hc->ang_vel_est = math::Vec3{0.0f, 0.0f, 0.0f};
        if (body.valid())
            ++body_count_;
    }

    vehicle_throttle_ = 0.0f;
    vehicle_brake_ = 0.0f;
    vehicle_steer_ = 0.0f;
    heli_collective_ = 0.0f;
    heli_pitch_ = 0.0f;
    heli_roll_ = 0.0f;
    heli_yaw_ = 0.0f;

    // Compile + bind every editor-authored visual-script graph for this session.
    // MUST run before the OnStart re-arm below so a newly bound PsyGraphComponent
    // is included in the latch reset, and before reset_gameplay() is irrelevant
    // (bind just registers the program + reserves a VmState; the host hooks the
    // VM calls are bound in reset_gameplay). Structural adds are forced immediate
    // inside the helper, like synthesize_authored_gameplay.
    synthesize_authored_scripts(scene);

    // Reset the reused gameplay contexts for this session (clears combat scratch,
    // re-binds the alloc-free AI host hooks, zeroes the AI clock + shot tally).
    // Done after the physics scan so the AI fire hook + PsyGraph host can reach
    // this runtime + scene.
    reset_gameplay();

    // Re-arm every PsyGraphComponent's OnStart latch so a fresh Play session
    // (including a replay) re-runs OnStart deterministically. The per-instance
    // VmState is owned by the GraphRuntime; OnStart is expected to (re)initialise
    // any variables it cares about. Cleared serially via a per-row write query.
    reg.query<scene::reads<>, scene::writes<script::psygraph::PsyGraphComponent>>(
        [&](std::span<script::psygraph::PsyGraphComponent> comps) {
            for (auto& c : comps)
                c.started = 0u;
        });

    playing_ = true;
}

// ─── AI host hooks ────────────────────────────────────────────────────────
// Both are bound ONCE into ai_ctx_ (reset_gameplay); `user` is the PlayRuntime.
// They read active_scene_, set at the top of tick(), so AI side effects reach
// the live scene without a per-frame closure. No heap allocation.

bool PlayRuntime::ai_los_hook(void* user, math::Vec3 origin, math::Vec3 target) {
    auto* self = static_cast<PlayRuntime*>(user);
    if (self == nullptr)
        return true;  // no host => treat as clear
    const math::Vec3 delta = math::sub(target, origin);
    const f32 dist2 = math::dot(delta, delta);
    if (dist2 <= 1e-8f)
        return true;  // coincident => trivially visible
    const f32 dist = std::sqrt(dist2);
    const math::Vec3 dir = math::mul(delta, 1.0f / dist);
    // Cast against this session's physics world. A hit strictly nearer than the
    // target (minus a small skin so a body sitting AT the target does not self-
    // occlude) means geometry blocks the line of sight. With no physics bodies
    // in the scene the ray hits nothing => LOS is clear.
    constexpr f32 kSkin = 0.05f;
    const physics::World::RaycastHit hit =
        self->world_.raycast(origin, dir, dist);
    if (hit.hit && hit.t < dist - kSkin)
        return false;  // occluded
    return true;
}

bool PlayRuntime::ai_fire_hook(void* user, Entity agent, Entity target) {
    auto* self = static_cast<PlayRuntime*>(user);
    if (self == nullptr || self->active_scene_ == nullptr)
        return false;
    scene::Scene& scene = *self->active_scene_;
    scene::EcsRegistry& reg = scene.registry();
    // The agent must own a weapon to fire (the combat layer also gates on
    // cooldown / ammo inside fire_weapon).
    if (reg.get<scene::WeaponComponent>(agent) == nullptr)
        return false;
    math::Vec3 origin{};
    math::Vec3 tgt{};
    if (!gameplay::entity_position(reg, agent, origin) ||
        !gameplay::entity_position(reg, target, tgt))
        return false;
    const math::Vec3 delta = math::sub(tgt, origin);
    const f32 dist2 = math::dot(delta, delta);
    if (dist2 <= 1e-8f)
        return false;
    const math::Vec3 dir = math::mul(delta, 1.0f / std::sqrt(dist2));
    // fire_weapon queues damage into combat_ctx_ (flushed later this tick) and
    // honours cooldown/ammo; it returns hit=false on cooldown / out of range /
    // friendly fire. We report "a shot went out" whenever the weapon was free to
    // fire, which fire_weapon signals by consuming the cooldown -- but since we
    // do not see that here, treat a resolved hit OR a cooldown-clear weapon as a
    // shot. Simpler + deterministic: a return of true means the round resolved
    // to a damaging hit; the AI telemetry counts those.
    const gameplay::HitResult res = gameplay::fire_weapon(
        reg, scene, agent, origin, dir, &self->combat_ctx_, self->combat_config_);
    return res.hit;
}

void PlayRuntime::reset_gameplay() noexcept {
    // Combat scratch: size once for this session's worst case, then only
    // begin_tick()-clear per tick (capacity retained => no steady-state heap).
    // The reserve is a one-time grow; if a prior session already grew it, this
    // is a no-op. Sized off the gathered entity counts as a cheap upper bound.
    const usize agents = ai_entities_.size();
    const usize bodies = rigid_entities_.size();
    // Generous-but-bounded: every agent could queue one shot, every dynamic body
    // could be a projectile/death this tick. Min floors keep tiny scenes sane.
    combat_ctx_.reserve(/*damage*/ std::max<usize>(64u, agents + bodies),
                        /*deaths*/ std::max<usize>(64u, agents + bodies),
                        /*despawn*/ std::max<usize>(64u, bodies));
    combat_ctx_.begin_tick();

    // AI context: bind the alloc-free host hooks ONCE (function pointers, no
    // std::function). apply_move stays null => act() writes Transform directly.
    // Zero the clock + shot telemetry for a fresh session.
    ai_ctx_.los = &PlayRuntime::ai_los_hook;
    ai_ctx_.los_user = this;
    ai_ctx_.fire = &PlayRuntime::ai_fire_hook;
    ai_ctx_.fire_user = this;
    ai_ctx_.apply_move = nullptr;
    ai_ctx_.move_user = nullptr;
    ai_ctx_.time = 0.0f;
    ai_ctx_.begin_tick();
    ai_shots_total_ = 0u;

    // PsyGraph host: bind the action hooks ONCE (the lambdas capture only
    // `this`, so binding is a one-time cost -- they reach the live scene through
    // active_scene_). Re-binding only if not already bound keeps it cheap on
    // replay; the per-entity HostContext is otherwise refilled in tick.
    if (!psygraph_hooks_bound_) {
        // An action's Target pin defaults to entity 0 when unconnected; per the
        // HostContext contract (Host.h) the host then substitutes the running
        // graph's own entity (self). Each hook below applies that fallback inline
        // (entity != 0 ? entity : self) so the std::function captures ONLY `this`
        // (a single pointer => libc++ small-buffer, no per-copy heap alloc).
        // ApplyDamage(entity, amount): route to gameplay::apply_damage with a
        // neutral source faction + friendly-fire ON so a graph can hurt anything
        // regardless of policy (scripted damage is unconditional).
        psygraph_host_.apply_damage = [this](u32 entity, f64 amount) {
            if (active_scene_ == nullptr)
                return;
            const Entity target{entity != 0u ? entity : psygraph_host_.self_entity};
            gameplay::CombatConfig cfg = combat_config_;
            cfg.friendly_fire = gameplay::FriendlyFire::On;
            (void)gameplay::apply_damage(active_scene_->registry(), target,
                                         static_cast<f32>(amount), /*src*/ 0u, cfg);
        };
        // SetHealth(entity, hp): clamp + write the HealthComponent directly.
        psygraph_host_.set_health = [this](u32 entity, f64 health) {
            if (active_scene_ == nullptr)
                return;
            const Entity target{entity != 0u ? entity : psygraph_host_.self_entity};
            if (auto* hc = active_scene_->registry().get<scene::HealthComponent>(target)) {
                f32 hp = static_cast<f32>(health);
                if (hp < 0.0f)
                    hp = 0.0f;
                if (hp > hc->max_health)
                    hp = hc->max_health;
                hc->current_health = hp;
            }
        };
        // SetActive(entity, active): toggle the entity's renderable Visible bit.
        psygraph_host_.set_active = [this](u32 entity, bool active) {
            if (active_scene_ == nullptr)
                return;
            const Entity target{entity != 0u ? entity : psygraph_host_.self_entity};
            auto* r = active_scene_->registry().get<scene::RenderableComponent>(target);
            if (r == nullptr)
                return;
            const u32 bits = scene::renderable_flags_bits(r->flags);
            const u32 vis = scene::renderable_flags_bits(scene::RenderableFlags::Visible);
            r->flags = static_cast<scene::RenderableFlags>(active ? (bits | vis) : (bits & ~vis));
        };
        // SpawnEntity(prefab): no prefab library yet in the play runtime, so
        // spawn a bare entity at the origin and return its raw id (the graph can
        // then act on it). A real prefab resolve is a follow-up.
        psygraph_host_.spawn_entity = [this](std::string_view) -> u32 {
            if (active_scene_ == nullptr)
                return 0u;
            return active_scene_->create_entity().raw;
        };
        // Log: swallowed (the play runtime has no console sink wired here). Bound
        // so the hook is non-null and the VM's Log node is a clean no-op rather
        // than an unset-hook branch.
        psygraph_host_.log = [](std::string_view) {};
        psygraph_hooks_bound_ = true;
    }
}

bool PlayRuntime::bind_psygraph(scene::Scene& scene,
                                Entity entity,
                                const script::psygraph::Graph& graph) {
    if (!entity.valid())
        return false;
    scene::EcsRegistry& reg = scene.registry();
    if (reg.get<script::psygraph::PsyGraphComponent>(entity) != nullptr)
        return false;  // already bound
    std::string err;
    const script::psygraph::GraphId gid = psygraph_runtime_.register_graph(graph, &err);
    if (gid == script::psygraph::kInvalidGraphId)
        return false;
    script::psygraph::PsyGraphComponent comp{};
    comp.graph_id = gid;
    comp.instance = psygraph_runtime_.create_instance(gid);  // one-time alloc here
    comp.started = 0u;
    reg.add<script::psygraph::PsyGraphComponent>(entity, comp);
    return true;
}

// ─── Gameplay phase passes ─────────────────────────────────────────────────

void PlayRuntime::tick_combat(scene::Scene& scene, f32 dt) {
    // NOTE: the orchestration in tick() calls combat_ctx_.begin_tick() ONCE for
    // the whole gameplay phase, runs cooldowns + projectile integration here,
    // then runs AI (which queues more damage into the SAME ctx via fire_weapon),
    // then flushes + resolves deaths AFTER AI so AI hitscan damage lands the same
    // tick. This function does only the pre-AI combat work.
    scene::EcsRegistry& reg = scene.registry();
    gameplay::tick_weapon_cooldowns(reg, dt);
    gameplay::tick_projectiles(reg, dt, combat_ctx_, combat_config_);
}

void PlayRuntime::tick_ai(scene::Scene& scene, f32 dt) {
    if (ai_entities_.empty())
        return;
    scene::EcsRegistry& reg = scene.registry();
    // One full AI step over every AiAgentComponent entity. perceive/think/act
    // are pure DOTS sweeps; the only shared side effects (LOS + fire) go through
    // the function-pointer hooks bound in reset_gameplay(). begin_tick() resets
    // the per-tick shot counter; we fold it into the running session tally.
    ai_ctx_.begin_tick();
    ai::perceive(reg, ai_ctx_, dt);
    ai::think(reg, ai_ctx_, dt);
    ai::act(reg, ai_ctx_, dt);
    ai_shots_total_ += ai_ctx_.shots_fired.load(std::memory_order_relaxed);
}

void PlayRuntime::tick_psygraphs(scene::Scene& scene, f32 dt) {
    scene::EcsRegistry& reg = scene.registry();
    // tick_psygraphs builds a per-entity HostContext via the make_host callback
    // (it copies the returned value into a local, then sets self + delta_time on
    // that LOCAL copy). Our action hooks read self_entity off the reused
    // psygraph_host_ MEMBER (the hooks capture `this`, not the copy), so we must
    // stamp the member with the running entity here, BEFORE the binding copies +
    // runs it. The sweep is serial (one entity at a time), so the member always
    // reflects the entity currently executing. The hooks each capture only
    // `this` (a single pointer) => the per-entity copy stays inside libc++'s
    // small-buffer optimisation and allocates nothing. They reach the live scene
    // through active_scene_, set at the top of tick().
    const f64 dt64 = static_cast<f64>(dt);
    psygraph_runtime_.tick_psygraphs(
        reg,
        [this](Entity self, f64 step) -> const script::psygraph::HostContext& {
            psygraph_host_.self_entity = self.raw;
            psygraph_host_.delta_time = step;
            return psygraph_host_;
        },
        dt64);
}

void PlayRuntime::tick(scene::Scene& scene, f32 dt) {
    if (!playing_ || dt <= 0.0f)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = world_;

    // The gameplay phase host hooks (AI fire/LOS, PsyGraph actions) reach the
    // scene through this transient pointer; valid only while this tick runs.
    active_scene_ = &scene;

    // --- Drive characters first (serial) ----------------------------------
    // physics::character::move mutates the shared character store, so this is a
    // serial pass. Few characters, so the cost is negligible.
    for (const Entity e : character_entities_) {
        scene::CharacterControllerComponent* cc = reg.get<scene::CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        const physics::character::CharacterId character{cc->runtime_character};
        if (!character.valid())
            continue;
        const math::Vec3 walk = cc->walk_dir;
        const f32 wlen2 = math::dot(walk, walk);
        if (wlen2 > 1e-8f) {
            const f32 wlen = std::sqrt(wlen2);
            const math::Vec3 dir = math::mul(walk, 1.0f / wlen);
            physics::character::move(character, math::mul(dir, cc->move_speed * dt), dt, world);
        }
    }

    // --- Apply vehicle driving intent (serial) ----------------------------
    // The shared player input drives every is_player vehicle. set_* only stash
    // controller values on the engine vehicle; the solver consumes them inside
    // world.step() below, so no per-vehicle stepping happens here.
    for (const Entity e : vehicle_entities_) {
        scene::VehicleComponent* vc = reg.get<scene::VehicleComponent>(e);
        if (vc == nullptr || vc->is_player == 0u)
            continue;
        const physics::vehicle::VehicleId vehicle{vc->runtime_vehicle};
        if (!vehicle.valid())
            continue;
        physics::vehicle::set_throttle(vehicle, vehicle_throttle_, world);
        physics::vehicle::set_brake(vehicle, vehicle_brake_, world);
        physics::vehicle::set_steer(vehicle, vehicle_steer_, world);
    }

    // --- Apply helicopter flight intent (serial) --------------------------
    // Body-relative arcade flight model, applied BEFORE the single step:
    //   * Collective -> thrust magnitude in [0, max_thrust] along the body-up
    //     axis (hover_assist centres neutral collective at m*g so it hovers).
    //     Continuous thrust is modelled as a per-tick linear impulse J=force*dt
    //     (there is no public linear apply_force).
    //   * Cyclic pitch/roll + pedal yaw -> body-axis torques, integrated into a
    //     component-carried angular-velocity estimate (the engine has no public
    //     angular reader). The estimate is exponentially damped each tick and
    //     written authoritatively via set_angular_velocity, so the craft stays
    //     controllable and never tumbles out of control. We are the only torque
    //     source while airborne (gravity adds no torque at the COM), so the
    //     estimate tracks the body. An inertia proxy I = m * (hx^2+hz^2)/3 maps
    //     torque -> angular acceleration (solid-box about a horizontal axis).
    for (const Entity e : helicopter_entities_) {
        scene::HelicopterComponent* hc = reg.get<scene::HelicopterComponent>(e);
        if (hc == nullptr || hc->is_player == 0u)
            continue;
        const physics::BodyId body{hc->runtime_body};
        if (!body.valid())
            continue;

        const math::Quat rot = math::quat_normalize(world.get_rotation(body));
        const math::Vec3 up = math::quat_rotate(rot, math::Vec3{0.0f, 1.0f, 0.0f});
        const math::Vec3 right = math::quat_rotate(rot, math::Vec3{1.0f, 0.0f, 0.0f});
        const math::Vec3 fwd = math::quat_rotate(rot, math::Vec3{0.0f, 0.0f, -1.0f});

        // Collective -> thrust along body-up.
        const f32 hover = hc->hover_assist ? hc->mass * (-config_.gravity.y) : 0.0f;
        f32 thrust = hover + heli_collective_ * hc->max_thrust_n;
        if (thrust < 0.0f)
            thrust = 0.0f;
        if (thrust > hc->max_thrust_n)
            thrust = hc->max_thrust_n;
        world.apply_impulse(body, math::mul(up, thrust * dt));

        // Cyclic + pedals -> torque about body axes. Pitch nose-down (positive
        // input) tilts the nose forward (negative torque about body-right);
        // roll-right (positive) rolls right; yaw-left/right about body-up.
        const math::Vec3 torque =
            math::add(math::add(math::mul(right, -hc->pitch_torque * heli_pitch_),
                                math::mul(fwd, hc->roll_torque * heli_roll_)),
                      math::mul(up, hc->yaw_torque * heli_yaw_));

        // Inertia proxy (solid box about a horizontal axis) maps torque to an
        // angular acceleration. A single scalar keeps the estimate stable.
        const f32 hx = hc->half_extent.x;
        const f32 hz = hc->half_extent.z;
        const f32 inertia = std::max(1e-3f, hc->mass * (hx * hx + hz * hz) / 3.0f);
        const f32 inv_inertia = 1.0f / inertia;

        math::Vec3 w = hc->ang_vel_est;
        w = math::add(w, math::mul(torque, inv_inertia * dt));  // integrate torque
        // Exponential angular damping so it doesn't spin forever.
        const f32 decay = std::max(0.0f, 1.0f - hc->angular_damping * dt);
        w = math::mul(w, decay);
        hc->ang_vel_est = w;
        world.set_angular_velocity(body, w);
    }

    // --- Step the world ONCE (serial, single writer) ----------------------
    // The single step advances rigid bodies, characters' resolution, AND the
    // vehicle solver (which writes each chassis body's pose). Both the rigid
    // body and vehicle writebacks below read from this one step -- the world is
    // never stepped twice per tick.
    world.step(dt);

    // --- Writeback rigid bodies (parallel) --------------------------------
    // get_position / get_rotation are pure const reads (no mutation, no lazy
    // alloc) and step() has completed, so reading the World concurrently is
    // safe. Each row writes only its own TransformComponent column.
    // SceneNodeComponent is read row-aligned (read-only, parallel-safe) so each
    // row can find its parent: a parented body's WORLD pose must be folded into
    // the parent's local space (local = inverse(parent_world) * body_world)
    // before storing into TransformComponent.local. graph().parent() and
    // graph().world_matrix() are pure const reads. The parent's world matrix is
    // from the previous tick's update_world_transforms() (this tick's runs
    // after every writeback) -- exact for a static parent, one-frame-lagged for
    // a moving one, which is the best achievable in a single graph pass.
    const scene::SceneGraph& graph = scene.graph();
    reg.query<scene::reads<scene::SceneNodeComponent, scene::RigidBodyComponent>,
              scene::writes<scene::TransformComponent>>(
        [&](std::span<const scene::SceneNodeComponent> nodes,
            std::span<const scene::RigidBodyComponent> bodies,
            std::span<scene::TransformComponent> transforms) {
            const usize n =
                std::min(std::min(nodes.size(), bodies.size()), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id{bodies[i].runtime_body};
                if (!id.valid())
                    continue;
                const math::Vec3 wp = world.get_position(id);
                const math::Quat wr = math::quat_normalize(world.get_rotation(id));
                scene::LocalTransform& lt = transforms[i].local;
                const scene::SceneNode parent = graph.parent(nodes[i].node);
                if (parent.valid()) {
                    const WorldPose lp =
                        world_pose_in_parent(graph.world_matrix(parent), wp, wr);
                    lt.translation = lp.translation;
                    lt.rotation = lp.rotation;
                } else {
                    lt.translation = wp;
                    lt.rotation = wr;
                }
                // physics never changes scale; lt.scale is left as authored.
            }
        });

    // --- Writeback vehicle chassis poses (parallel) -----------------------
    // Same safety as the rigid body writeback: get_position/get_rotation are
    // pure const reads and step() has completed. Each row writes only its own
    // TransformComponent column.
    reg.query<scene::reads<scene::SceneNodeComponent, scene::VehicleComponent>,
              scene::writes<scene::TransformComponent>>(
        [&](std::span<const scene::SceneNodeComponent> nodes,
            std::span<const scene::VehicleComponent> vehicles,
            std::span<scene::TransformComponent> transforms) {
            const usize n =
                std::min(std::min(nodes.size(), vehicles.size()), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id{vehicles[i].runtime_chassis};
                if (!id.valid())
                    continue;
                const math::Vec3 wp = world.get_position(id);
                const math::Quat wr = math::quat_normalize(world.get_rotation(id));
                scene::LocalTransform& lt = transforms[i].local;
                const scene::SceneNode parent = graph.parent(nodes[i].node);
                if (parent.valid()) {
                    const WorldPose lp =
                        world_pose_in_parent(graph.world_matrix(parent), wp, wr);
                    lt.translation = lp.translation;
                    lt.rotation = lp.rotation;
                } else {
                    lt.translation = wp;
                    lt.rotation = wr;
                }
            }
        });

    // --- Writeback helicopter chassis poses (parallel) --------------------
    // Same safety as the rigid body / vehicle writebacks: get_position /
    // get_rotation are pure const reads and step() has completed. Each row
    // writes only its own TransformComponent column.
    reg.query<scene::reads<scene::SceneNodeComponent, scene::HelicopterComponent>,
              scene::writes<scene::TransformComponent>>(
        [&](std::span<const scene::SceneNodeComponent> nodes,
            std::span<const scene::HelicopterComponent> helis,
            std::span<scene::TransformComponent> transforms) {
            const usize n =
                std::min(std::min(nodes.size(), helis.size()), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id{helis[i].runtime_body};
                if (!id.valid())
                    continue;
                const math::Vec3 wp = world.get_position(id);
                const math::Quat wr = math::quat_normalize(world.get_rotation(id));
                scene::LocalTransform& lt = transforms[i].local;
                const scene::SceneNode parent = graph.parent(nodes[i].node);
                if (parent.valid()) {
                    const WorldPose lp =
                        world_pose_in_parent(graph.world_matrix(parent), wp, wr);
                    lt.translation = lp.translation;
                    lt.rotation = lp.rotation;
                } else {
                    lt.translation = wp;
                    lt.rotation = wr;
                }
            }
        });

    // --- Writeback characters (serial) ------------------------------------
    // The character has no body rotation; only its capsule-centre WORLD
    // position is resolved. Parented characters fold that world point into the
    // parent's local space (rotation kept as authored).
    for (const Entity e : character_entities_) {
        scene::CharacterControllerComponent* cc = reg.get<scene::CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        const physics::character::CharacterId character{cc->runtime_character};
        if (!character.valid())
            continue;
        auto* tc = reg.get<scene::TransformComponent>(e);
        if (tc == nullptr)
            continue;
        const math::Vec3 wp = physics::character::get_position(character, world);
        const scene::SceneNode parent = graph.parent(scene.node(e));
        if (parent.valid()) {
            const WorldPose lp =
                world_pose_in_parent(graph.world_matrix(parent), wp, tc->local.rotation);
            tc->local.translation = lp.translation;
        } else {
            tc->local.translation = wp;
        }
    }

    // --- GAMEPLAY PHASE (serial, alloc-free) ------------------------------
    // Runs only while playing, AFTER the physics step + transform writeback and
    // BEFORE the chase camera + final graph sync. Order (see PlayRuntime.h):
    //   combat (pre) -> AI -> combat (flush + deaths) -> psygraph.
    // All scratch is reused (combat_ctx_, ai_ctx_, the PsyGraph pools + host),
    // so this phase never heap-allocates in the steady state, and the systems
    // are deterministic pure sweeps. The damage flush + death resolution run
    // AFTER the AI pass so an AI hitscan fired this tick lands the same tick
    // (AI's fire hook queues into the SAME combat_ctx_).
    {
        // One begin_tick() for the whole phase: clears the combat scratch so the
        // projectile pass and the AI fire hook queue into a fresh ctx.
        combat_ctx_.begin_tick();
        // Combat (pre): cooldowns + projectile integration / hits.
        tick_combat(scene, dt);
        // AI: perceive/think/act; the Attack branch fires via ai_fire_hook ->
        // gameplay::fire_weapon, queuing damage into combat_ctx_.
        tick_ai(scene, dt);
        // Combat (post): apply ALL queued damage (projectiles + AI), despawn
        // spent projectiles, then resolve deaths (despawn the dead). Done here so
        // a target the AI killed this tick is gone before the renderer samples.
        gameplay::flush_damage_events(reg, combat_ctx_, combat_config_);
        gameplay::cleanup_projectiles(scene, combat_ctx_);
        (void)gameplay::resolve_deaths(scene, /*despawn*/ true);
        // PsyGraph: OnStart (once) + OnTick for every PsyGraphComponent entity.
        tick_psygraphs(scene, dt);
    }

    // --- Chase camera (serial) --------------------------------------------
    // Position the active scene camera behind/above the player chassis. Done
    // before update_world_transforms so the camera's world matrix refreshes in
    // the single pass below.
    update_chase_camera(scene);

    // --- Sync the simulated poses into the SceneGraph (serial) ------------
    // The writebacks above store into TransformComponent.local, but the
    // renderer reads SceneGraph world matrices, and update_world_transforms()
    // recomputes those from the graph's OWN local store (local_translation_/
    // rotation_/scale_) -- NOT from TransformComponent. Without this push the
    // graph never sees the simulated motion and bodies render at their authored
    // pose. set_local_transform() mutates shared dirty bookkeeping (dirty_roots_)
    // so it CANNOT run inside the parallel writeback queries above; we apply it
    // serially over the (pooled) simulated-entity lists. Reads the local just
    // written by the writeback, so the parenting conversion is preserved.
    scene::SceneGraph& graph_mut = scene.graph();
    auto sync_graph_locals = [&](const std::vector<Entity>& ents) {
        for (const Entity e : ents) {
            if (const auto* tc = reg.get<scene::TransformComponent>(e))
                graph_mut.set_local_transform(scene.node(e), tc->local);
        }
    };
    sync_graph_locals(rigid_entities_);
    sync_graph_locals(vehicle_entities_);
    sync_graph_locals(helicopter_entities_);
    sync_graph_locals(character_entities_);
    // AI agents are kinematic (no physics body): act() moved them directly in
    // their TransformComponent during the gameplay phase, so push those locals
    // into the graph too or the renderer would never see AI motion. A despawned
    // agent's node() is invalid; set_local_transform tolerates that.
    sync_graph_locals(ai_entities_);

    // --- Recompute world matrices ONCE ------------------------------------
    scene.graph().update_world_transforms();

    // The transient scene pointer is only valid during a tick; clear it so a
    // stray hook call between ticks can't dereference a stale scene.
    active_scene_ = nullptr;
}

void PlayRuntime::end(scene::Scene& scene) {
    if (!playing_)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = world_;

    // Destroy bodies and clear the runtime handle column.
    for (const Entity e : rigid_entities_) {
        scene::RigidBodyComponent* rb = reg.get<scene::RigidBodyComponent>(e);
        if (rb == nullptr)
            continue;
        const physics::BodyId body{rb->runtime_body};
        if (body.valid())
            world.destroy_body(body);
        rb->runtime_body = 0u;
    }

    // Destroy characters and clear their handle column.
    for (const Entity e : character_entities_) {
        scene::CharacterControllerComponent* cc = reg.get<scene::CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        const physics::character::CharacterId character{cc->runtime_character};
        if (character.valid())
            physics::character::destroy(character, world);
        cc->runtime_character = 0u;
        cc->walk_dir = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    // Destroy vehicles + their chassis bodies and clear the handle columns.
    for (const Entity e : vehicle_entities_) {
        scene::VehicleComponent* vc = reg.get<scene::VehicleComponent>(e);
        if (vc == nullptr)
            continue;
        const physics::vehicle::VehicleId vehicle{vc->runtime_vehicle};
        const physics::BodyId chassis{vc->runtime_chassis};
        if (vehicle.valid())
            physics::vehicle::destroy(vehicle, world);
        if (chassis.valid())
            world.destroy_body(chassis);
        vc->runtime_vehicle = 0u;
        vc->runtime_chassis = 0u;
    }

    // Destroy helicopter chassis bodies and clear the handle column.
    for (const Entity e : helicopter_entities_) {
        scene::HelicopterComponent* hc = reg.get<scene::HelicopterComponent>(e);
        if (hc == nullptr)
            continue;
        const physics::BodyId body{hc->runtime_body};
        if (body.valid())
            world.destroy_body(body);
        hc->runtime_body = 0u;
        hc->ang_vel_est = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    vehicle_throttle_ = 0.0f;
    vehicle_brake_ = 0.0f;
    vehicle_steer_ = 0.0f;
    heli_collective_ = 0.0f;
    heli_pitch_ = 0.0f;
    heli_roll_ = 0.0f;
    heli_yaw_ = 0.0f;

    // Strip the psygraph::PsyGraphComponent we bound from each entity's authored
    // graph, restoring the scene to authoring-only state (the scene-level
    // ScriptGraphComponent + the graph blob stay; we re-bind next session).
    clear_authored_scripts(scene);

    // Strip the live gameplay/AI components we synthesised from the editor
    // authoring proxies, restoring the scene to proxy-only state.
    clear_authored_gameplay(scene);

    // Restore authored transforms (serial).
    for (const AuthoredTransform& a : authored_)
        scene.set_transform(a.entity, a.local);

    scene.graph().update_world_transforms();

    playing_ = false;
    clear_pools();  // keeps vector capacity
}

void PlayRuntime::update_chase_camera(scene::Scene& scene) noexcept {
    if (!playing_)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = world_;

    // Find the first live player vehicle's chassis (serial; few vehicles). If
    // none, fall back to the first live player helicopter's chassis so the
    // chase camera follows the craft the flight input drives.
    physics::BodyId chassis{};
    for (const Entity e : vehicle_entities_) {
        const scene::VehicleComponent* vc = reg.get<scene::VehicleComponent>(e);
        if (vc != nullptr && vc->is_player != 0u && vc->runtime_chassis != 0u) {
            chassis = physics::BodyId{vc->runtime_chassis};
            break;
        }
    }
    if (!chassis.valid()) {
        for (const Entity e : helicopter_entities_) {
            const scene::HelicopterComponent* hc = reg.get<scene::HelicopterComponent>(e);
            if (hc != nullptr && hc->is_player != 0u && hc->runtime_body != 0u) {
                chassis = physics::BodyId{hc->runtime_body};
                break;
            }
        }
    }
    if (!chassis.valid())
        return;

    const Entity camera = scene.active_camera_entity();
    if (!camera.valid() || !reg.alive(camera) || reg.get<scene::CameraComponent>(camera) == nullptr)
        return;
    // Chase only a TOP-LEVEL camera: the writeback puts a WORLD pose into
    // TransformComponent.local, which is only exact for an unparented entity.
    const scene::SceneNode cam_node = scene.node(camera);
    if (cam_node.valid() && scene.graph().parent(cam_node).valid())
        return;

    const math::Vec3 target = world.get_position(chassis);
    const math::Quat chassis_rot = math::quat_normalize(world.get_rotation(chassis));
    // Chassis forward is -Z (matches the editor/FPS camera convention). The
    // camera sits BEHIND (-forward) and ABOVE the chassis, looking at it.
    const math::Vec3 forward =
        math::normalize(math::quat_rotate(chassis_rot, math::Vec3{0.0f, 0.0f, -1.0f}));
    constexpr f32 kBack = 6.0f;
    constexpr f32 kUp = 2.5f;
    const math::Vec3 eye{target.x - forward.x * kBack,
                         target.y - forward.y * kBack + kUp,
                         target.z - forward.z * kBack};

    // Build a look-at rotation in the editor camera convention:
    // quat_from_euler(pitch, yaw, 0) with view direction = rotate * (0,0,-1).
    const math::Vec3 dir = math::normalize(math::Vec3{target.x - eye.x,
                                                      target.y - eye.y,
                                                      target.z - eye.z});
    const f32 yaw = std::atan2(dir.x, -dir.z);
    const f32 horiz = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    const f32 pitch = std::atan2(dir.y, horiz);
    const math::Quat rot = math::quat_normalize(math::quat_from_euler(pitch, yaw, 0.0f));

    scene::LocalTransform cam_local = scene.transform(camera);
    cam_local.translation = eye;
    cam_local.rotation = rot;
    (void)scene.set_transform(camera, cam_local);
}

void PlayRuntime::set_character_input(scene::Scene& scene,
                                      Entity entity,
                                      math::Vec3 walk_dir) noexcept {
    auto* cc = scene.registry().get<scene::CharacterControllerComponent>(entity);
    if (cc != nullptr)
        cc->walk_dir = walk_dir;
}

math::Vec3 PlayRuntime::character_position(scene::Scene& scene, Entity entity) const noexcept {
    auto* cc = scene.registry().get<scene::CharacterControllerComponent>(entity);
    if (cc == nullptr)
        return math::Vec3{0.0f, 0.0f, 0.0f};
    // get_position reads this session's world; the read is logically const but
    // the free function takes World& (uniform with the mutating character ops),
    // so cast away const on our own member to call it. No state is mutated.
    return physics::character::get_position(
        physics::character::CharacterId{cc->runtime_character},
        const_cast<physics::World&>(world_));
}

}  // namespace psynder::editor::play
