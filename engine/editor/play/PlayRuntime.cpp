// SPDX-License-Identifier: MIT
// Psynder - editor PLAY MODE physics runtime implementation.
//
// See PlayRuntime.h for the lifecycle contract and the DOTS rationale. All
// per-entity sim state lives in pooled ECS components; this TU only owns two
// pooled scratch vectors (entity scan lists) + the authored-transform
// snapshot, all reserved once and reused.
//
// Writeback parallelism: World::get_position / get_rotation are pure const
// reads (engine/physics/World.cpp:336/351) -- they read the function-local
// static world_state() and interpolate prev/current; they NEVER mutate a body
// or lazily allocate. step() (the only writer) completes BEFORE the writeback
// query runs, and the query writes only its own TransformComponent column
// (per-row safe). So the writeback runs in parallel safely. The character
// writeback is a short serial pass (few characters; physics::character::move
// mutates the shared character store and must not run concurrently).

#include "editor/play/PlayRuntime.h"

#include "editor/play/CharacterPeek.h"  // peek_character_position (isolated TU)

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace psynder::editor::play {

PlayRuntime::~PlayRuntime() {
    // The handles needed to free bodies live in the ECS components, which we
    // cannot reach without a Scene&. The host contract is to always call end()
    // on Stop / teardown, which frees the bodies and restores transforms; the
    // destructor only marks state. (The shared World is a process singleton and
    // is reclaimed at process exit regardless.)
    playing_ = false;
}

void PlayRuntime::clear_pools() noexcept {
    rigid_entities_.clear();
    character_entities_.clear();
    authored_.clear();
    body_count_ = 0u;
}

void PlayRuntime::begin(scene::Scene& scene) {
    // Idempotent: a stray double-begin tears down the previous session first.
    if (playing_)
        end(scene);

    clear_pools();

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();
    world.set_gravity(config_.gravity);

    // --- Phase 1: scan (parallel, mutex-merged) ---------------------------
    // Gather entities that HAVE a RigidBodyComponent. We read the entity column
    // (via SceneNodeComponent) row-aligned with the rigid body column. The
    // query body fires once per chunk across worker threads, so the shared
    // append is serialized by a mutex and each chunk builds a local buffer
    // first (the gather_scene_render_items pattern).
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, RigidBodyComponent>, scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const RigidBodyComponent> bodies) {
                const usize n = std::min(nodes.size(), bodies.size());
                constexpr usize kMaxChunkRows = 256u;
                std::array<Entity, kMaxChunkRows> chunk_entities{};
                usize chunk_count = 0u;
                for (usize i = 0; i < n; ++i) {
                    if (chunk_count >= kMaxChunkRows)
                        break;  // chunks never exceed this; guard anyway
                    chunk_entities[chunk_count++] = nodes[i].entity;
                }
                if (chunk_count != 0u) {
                    std::scoped_lock lock{append_mutex};
                    rigid_entities_.reserve(rigid_entities_.size() + chunk_count);
                    rigid_entities_.insert(rigid_entities_.end(),
                                           chunk_entities.begin(),
                                           chunk_entities.begin() + chunk_count);
                }
            });
    }

    // Same scan for character entities.
    {
        std::mutex append_mutex;
        reg.query<scene::reads<scene::SceneNodeComponent, CharacterControllerComponent>,
                  scene::writes<>>(
            [&](std::span<const scene::SceneNodeComponent> nodes,
                std::span<const CharacterControllerComponent> chars) {
                const usize n = std::min(nodes.size(), chars.size());
                constexpr usize kMaxChunkRows = 256u;
                std::array<Entity, kMaxChunkRows> chunk_entities{};
                usize chunk_count = 0u;
                for (usize i = 0; i < n; ++i) {
                    if (chunk_count >= kMaxChunkRows)
                        break;
                    chunk_entities[chunk_count++] = nodes[i].entity;
                }
                if (chunk_count != 0u) {
                    std::scoped_lock lock{append_mutex};
                    character_entities_.reserve(character_entities_.size() + chunk_count);
                    character_entities_.insert(character_entities_.end(),
                                               chunk_entities.begin(),
                                               chunk_entities.begin() + chunk_count);
                }
            });
    }

    authored_.reserve(rigid_entities_.size() + character_entities_.size());

    // --- Phase 2: build bodies (serial, main thread) ----------------------
    // create_body mutates the shared World, so this MUST be serial. We read the
    // authored transform per entity, snapshot it, create the body from the
    // authored pose + component params, and write the handle back into the
    // component column.
    for (const Entity e : rigid_entities_) {
        RigidBodyComponent* rb = reg.get<RigidBodyComponent>(e);
        if (rb == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});

        physics::BodyDesc d{};
        d.shape = rb->shape;
        d.mass = rb->mass;  // 0 => static
        d.position = local.translation;
        d.rotation = math::quat_normalize(local.rotation);
        d.half_extent = rb->half_extent;
        d.friction = rb->friction;
        d.restitution = rb->restitution;

        rb->body = world.create_body(d);
        if (rb->body.valid())
            ++body_count_;
    }

    // Characters.
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        const scene::LocalTransform local = scene.transform(e);
        authored_.push_back(AuthoredTransform{e, local});

        physics::character::CharacterDesc d{};
        d.position = local.translation;
        d.height = cc->height;
        d.radius = cc->radius;
        cc->character = physics::character::create(d);
        cc->walk_dir = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    playing_ = true;
}

void PlayRuntime::tick(scene::Scene& scene, f32 dt) {
    if (!playing_ || dt <= 0.0f)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();

    // --- Drive characters first (serial) ----------------------------------
    // physics::character::move mutates the shared character store, so this is a
    // serial pass. Few characters, so the cost is negligible.
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr || !cc->character.valid())
            continue;
        const math::Vec3 walk = cc->walk_dir;
        const f32 wlen2 = math::dot(walk, walk);
        if (wlen2 > 1e-8f) {
            const f32 wlen = std::sqrt(wlen2);
            const math::Vec3 dir = math::mul(walk, 1.0f / wlen);
            physics::character::move(cc->character, math::mul(dir, cc->move_speed * dt), dt);
        }
    }

    // --- Step the world (serial, single writer) ---------------------------
    world.step(dt);

    // --- Writeback rigid bodies (parallel) --------------------------------
    // get_position / get_rotation are pure const reads (no mutation, no lazy
    // alloc) and step() has completed, so reading the World concurrently is
    // safe. Each row writes only its own TransformComponent column.
    reg.query<scene::reads<RigidBodyComponent>, scene::writes<scene::TransformComponent>>(
        [&](std::span<const RigidBodyComponent> bodies,
            std::span<scene::TransformComponent> transforms) {
            const usize n = std::min(bodies.size(), transforms.size());
            for (usize i = 0; i < n; ++i) {
                const physics::BodyId id = bodies[i].body;
                if (!id.valid())
                    continue;
                scene::LocalTransform& lt = transforms[i].local;
                lt.translation = world.get_position(id);
                lt.rotation = math::quat_normalize(world.get_rotation(id));
                // physics never changes scale; lt.scale is left as authored.
            }
        });

    // --- Writeback characters (serial) ------------------------------------
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr || !cc->character.valid())
            continue;
        auto* tc = reg.get<scene::TransformComponent>(e);
        if (tc == nullptr)
            continue;
        tc->local.translation = peek_character_position(cc->character);
    }

    // --- Recompute world matrices ONCE ------------------------------------
    scene.graph().update_world_transforms();
}

void PlayRuntime::end(scene::Scene& scene) {
    if (!playing_)
        return;

    scene::EcsRegistry& reg = scene.registry();
    physics::World& world = physics::World::Get();

    // Destroy bodies and clear the runtime handle column.
    for (const Entity e : rigid_entities_) {
        RigidBodyComponent* rb = reg.get<RigidBodyComponent>(e);
        if (rb == nullptr)
            continue;
        if (rb->body.valid())
            world.destroy_body(rb->body);
        rb->body = physics::BodyId{};
    }

    // Destroy characters and clear their handle column.
    for (const Entity e : character_entities_) {
        CharacterControllerComponent* cc = reg.get<CharacterControllerComponent>(e);
        if (cc == nullptr)
            continue;
        if (cc->character.valid())
            physics::character::destroy(cc->character);
        cc->character = physics::character::CharacterId{};
        cc->walk_dir = math::Vec3{0.0f, 0.0f, 0.0f};
    }

    // Restore authored transforms (serial).
    for (const AuthoredTransform& a : authored_)
        scene.set_transform(a.entity, a.local);

    scene.graph().update_world_transforms();

    playing_ = false;
    clear_pools();  // keeps vector capacity
}

void PlayRuntime::set_character_input(scene::Scene& scene,
                                      Entity entity,
                                      math::Vec3 walk_dir) noexcept {
    auto* cc = scene.registry().get<CharacterControllerComponent>(entity);
    if (cc != nullptr)
        cc->walk_dir = walk_dir;
}

math::Vec3 PlayRuntime::character_position(scene::Scene& scene, Entity entity) const noexcept {
    auto* cc = scene.registry().get<CharacterControllerComponent>(entity);
    if (cc == nullptr)
        return math::Vec3{0.0f, 0.0f, 0.0f};
    return peek_character_position(cc->character);
}

}  // namespace psynder::editor::play
