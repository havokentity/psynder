// SPDX-License-Identifier: MIT
// Psynder — M-COMBAT combat systems (DOTS). Free functions + a CombatSystems
// facade that operate over the ECS components in GameplayComponents.h via
// `registry.query<reads<...>, writes<...>>`.
//
// PARALLEL-SAFE / ALLOC-FREE contract (matches scene::gather_* patterns):
//   * query bodies fire once per chunk across worker threads. Any shared
//     accumulation (nearest-hit search, pending-damage list, death list) is
//     serialized with a mutex inside a reusable CombatContext.
//   * No per-frame heap allocation in the steady state: CombatContext owns
//     pre-reserved std::vector scratch buffers that are clear()ed (capacity
//     retained) not freed. Reserve once via CombatContext::reserve().
//   * Structural changes (spawning projectiles, despawning the dead) are
//     deferred — never mutate the archetype mid-query. We collect intents and
//     apply them in a serial pass after the query returns.
//
// This module depends on scene/physics/math only.

#pragma once

#include "core/Types.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Raycast.h"
#include "math/Math.h"
#include "scene/SceneEcs.h"

#include <mutex>
#include <vector>

namespace psynder::gameplay {

using ::psynder::Entity;
using ::psynder::f32;
using ::psynder::u32;
using ::psynder::usize;

// A single resolved hit found during a sweep.
struct HitResult {
    Entity entity{};
    f32 distance = 0.0f;
    u32 faction = 0u;
    bool hit = false;
};

// Reusable per-system scratch + synchronization. Construct ONE of these per
// combat world and reuse it every frame; call reserve() at load to size the
// scratch so the steady state never heap-allocates. Members are mutated under
// `mutex` from parallel query bodies.
struct CombatContext {
    std::mutex mutex;                       // guards every shared field below
    std::vector<DamageEvent> pending;       // damage queued this tick
    std::vector<Entity> deaths;             // entities that hit 0 HP this tick
    std::vector<Entity> despawn;            // projectile entities to remove

    void reserve(usize max_damage, usize max_deaths, usize max_despawn) {
        pending.reserve(max_damage);
        deaths.reserve(max_deaths);
        despawn.reserve(max_despawn);
    }

    // Retains capacity (no free) — call at the top of a combat tick.
    void begin_tick() noexcept {
        pending.clear();
        deaths.clear();
        despawn.clear();
    }
};

// ─── Faction lookup ──────────────────────────────────────────────────────────
// HealthComponent::faction is the primary source; FactionComponent is the
// fallback for entities without health. 0 => neutral/world.
[[nodiscard]] u32 entity_faction(scene::EcsRegistry& registry, Entity e);

// Read an entity's world-ish position from its TransformComponent translation.
// Combat operates in this space directly (the projectile + hitbox positions are
// authored there); no scene-graph world-matrix flatten is required, which keeps
// the systems pure and test-friendly.
[[nodiscard]] bool entity_position(scene::EcsRegistry& registry,
                                   Entity e,
                                   math::Vec3& out);

// ─── apply_damage ────────────────────────────────────────────────────────────
// Decrement target health, clamp at 0, and on death add a DeadComponent flag
// (deferred destruction — resolve_deaths handles it). Respects friendly fire.
// Returns true if any damage was applied. Safe to call OUTSIDE a query (it
// mutates a single entity's HealthComponent in place + may add DeadComponent,
// which is a structural change routed through the registry's deferred channel
// when enabled). Do NOT call from inside a parallel query body — queue a
// DamageEvent into CombatContext instead and flush after.
bool apply_damage(scene::EcsRegistry& registry,
                  Entity target,
                  f32 amount,
                  u32 source_faction,
                  const CombatConfig& config);

// Drain a CombatContext's pending DamageEvents (serial). Also drains any
// DamageEvent components a host scripted onto entities. Populates ctx.deaths.
void flush_damage_events(scene::EcsRegistry& registry,
                         CombatContext& ctx,
                         const CombatConfig& config);

// ─── fire_weapon ─────────────────────────────────────────────────────────────
// Hitscan: sweep the ray (origin,dir) against every enabled HitboxComponent of
// a damageable, different-faction entity within `weapon.range`; the NEAREST hit
// takes damage (queued into ctx). Returns the resolved hit (hit=false on miss /
// on cooldown / out of ammo).
//
// Projectile: spawns a projectile entity travelling along dir; no immediate
// damage (tick_projectiles resolves it). The returned HitResult.hit is false
// but the projectile Entity is returned via out_projectile when non-null.
//
// Honors WeaponRuntimeComponent::cooldown + WeaponComponent::ammo (decrements
// ammo, sets cooldown = 1/fire_rate). Pass nullptr ctx to skip damage queueing
// (e.g. a pure line-of-sight probe).
HitResult fire_weapon(scene::EcsRegistry& registry,
                      scene::Scene& scene,
                      Entity shooter,
                      math::Vec3 origin,
                      math::Vec3 dir,
                      CombatContext* ctx,
                      const CombatConfig& config,
                      Entity* out_projectile = nullptr);

// Line-of-sight / nearest-hitbox query with NO side effects. Sweeps every
// enabled hitbox (optionally excluding `ignore`) and returns the nearest hit
// within max_dist. Parallel-safe: uses a mutex-guarded nearest reduction.
HitResult raycast_hitboxes(scene::EcsRegistry& registry,
                           math::Vec3 origin,
                           math::Vec3 dir,
                           f32 max_dist,
                           Entity ignore);

// ─── tick_projectiles ────────────────────────────────────────────────────────
// Integrate every live projectile by dt, sweep the swept segment against
// hitboxes (different faction, not the source), queue damage on the first hit,
// mark the projectile dead, and decrement life. Expired/Hit projectiles are
// collected into ctx.despawn. Call cleanup_projectiles after to despawn them.
// Pooled + alloc-free (writes ProjectileComponent + TransformComponent in
// place; queues into ctx).
void tick_projectiles(scene::EcsRegistry& registry,
                      f32 dt,
                      CombatContext& ctx,
                      const CombatConfig& config);

// Despawn the projectile entities collected in ctx.despawn (serial, deferred-
// safe). Clears ctx.despawn.
void cleanup_projectiles(scene::Scene& scene, CombatContext& ctx);

// ─── tick_weapon_cooldowns ───────────────────────────────────────────────────
// Decrement every WeaponRuntimeComponent::cooldown by dt (clamped at 0). Pure
// per-chunk write, no accumulation => trivially parallel-safe.
void tick_weapon_cooldowns(scene::EcsRegistry& registry, f32 dt);

// ─── resolve_deaths ──────────────────────────────────────────────────────────
// Handle entities flagged DeadComponent{pending=1,resolved=0}: invoke the
// optional callback, mark resolved, and (when despawn=true) destroy them via
// the scene. Deferred — never called mid-query. Returns the number resolved.
struct DeathInfo {
    Entity entity{};
    u32 killer_faction = 0u;
};
using DeathCallback = void (*)(void* user, const DeathInfo& info);

u32 resolve_deaths(scene::Scene& scene,
                   bool despawn,
                   DeathCallback cb = nullptr,
                   void* user = nullptr);

// ─── CombatSystems facade ────────────────────────────────────────────────────
// Thin struct wrapper so a host can hold one object that drives a per-frame
// combat update (cooldowns -> projectiles -> flush -> deaths). Owns its
// CombatContext + CombatConfig. Stateless beyond that scratch.
struct CombatSystems {
    CombatConfig config{};
    CombatContext ctx{};

    void reserve(usize max_damage, usize max_deaths, usize max_despawn) {
        ctx.reserve(max_damage, max_deaths, max_despawn);
    }

    // One full combat step: cooldowns, projectile integration + hits, damage
    // application, projectile cleanup, death resolution. Returns deaths handled.
    u32 update(scene::Scene& scene,
               f32 dt,
               bool despawn_dead = true,
               DeathCallback cb = nullptr,
               void* user = nullptr);

    HitResult fire(scene::Scene& scene,
                   Entity shooter,
                   math::Vec3 origin,
                   math::Vec3 dir,
                   Entity* out_projectile = nullptr) {
        return fire_weapon(scene.registry(), scene, shooter, origin, dir, &ctx, config,
                           out_projectile);
    }
};

}  // namespace psynder::gameplay
