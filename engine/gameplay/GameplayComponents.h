// SPDX-License-Identifier: MIT
// Psynder — M-COMBAT gameplay-combat components. Pure-POD ECS components for
// the shooter targets (Duke3D/Quake/Delta Force). Trivially-copyable, pooled by
// the archetype-chunked registry, mutated only through queries + the deferred
// structural-change channel.
//
// REUSE-vs-NEW (see the M-COMBAT brief):
//   * scene::HealthComponent {max_health, current_health, faction}  — REUSED.
//   * scene::WeaponComponent  {damage, range, fire_rate, ammo, ...} — REUSED as
//     the authored stat block. It carries no projectile/hitscan discriminator
//     nor a live cooldown, so we add WeaponRuntimeComponent below to hold the
//     fire kind + per-frame cooldown without mutating the scene-owned struct.
//   * Everything else here is NEW and owned by this module.
//
// This module depends on scene/physics/math — never the reverse.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneEcs.h"

namespace psynder::gameplay {

using ::psynder::Entity;
using ::psynder::f32;
using ::psynder::u8;
using ::psynder::u32;

// Re-export the scene-owned stat components under the gameplay namespace so
// callers can write `gameplay::HealthComponent` without caring where they live.
using HealthComponent = ::psynder::scene::HealthComponent;
using WeaponComponent = ::psynder::scene::WeaponComponent;

// ─── Faction friendly-fire policy ─────────────────────────────────────────
// Configurable per call (CombatConfig) rather than baked in, so a deathmatch
// (everyone hostile) and a co-op (same faction never hurts) reuse one code
// path. Factions are plain u32 ids carried by HealthComponent::faction and the
// optional FactionComponent below.
enum class FriendlyFire : u8 {
    Off = 0,   // same faction => no damage (default co-op / squad rules)
    On = 1,    // faction ignored => everyone takes damage (deathmatch)
};

struct CombatConfig {
    FriendlyFire friendly_fire = FriendlyFire::Off;
    // Faction id 0 is treated as "neutral / world" and is always damageable
    // regardless of the friendly-fire policy unless this is set.
    u8 neutral_is_invulnerable = 0u;
    u8 _pad[2] = {};

    [[nodiscard]] constexpr bool can_damage(u32 attacker_faction,
                                            u32 target_faction) const noexcept {
        if (neutral_is_invulnerable != 0u && target_faction == 0u)
            return false;
        if (friendly_fire == FriendlyFire::On)
            return true;
        return attacker_faction != target_faction;
    }
};

// ─── Weapon fire discriminator + live cooldown ─────────────────────────────
// scene::WeaponComponent holds authored stats; this holds the runtime bits the
// combat systems own. Separate component so we never write back into the
// scene-authored stat block from a system.
enum class WeaponKind : u8 {
    Hitscan = 0,
    Projectile = 1,
};

PSYNDER_COMPONENT(WeaponRuntimeComponent) {
    WeaponKind kind = WeaponKind::Hitscan;
    u8 _pad[3] = {};
    // Seconds remaining until the weapon can fire again. fire_weapon refuses to
    // fire while > 0; tick_weapon_cooldowns decrements it. fire_rate on the
    // scene WeaponComponent is shots/sec, so a successful shot sets this to
    // 1/fire_rate.
    f32 cooldown = 0.0f;
    // Projectile spawn speed (m/s) + lifetime (s). Ignored for hitscan.
    f32 projectile_speed = 40.0f;
    f32 projectile_life = 3.0f;
};

// ─── Faction tag (optional) ────────────────────────────────────────────────
// HealthComponent already carries a faction id; this standalone tag lets a
// non-damageable entity (a trigger, a spawner, a projectile source) still
// declare allegiance. Combat reads HealthComponent::faction first and falls
// back to this when no health is present.
PSYNDER_COMPONENT(FactionComponent) {
    u32 faction = 0u;
};

// ─── Hitbox ─────────────────────────────────────────────────────────────────
// A ray/projectile collides against this. We support a sphere (radius > 0) or
// an axis-aligned box (half_extent). When radius > 0 the sphere wins; else the
// box is used. Centered on the entity's TransformComponent translation plus a
// local offset.
PSYNDER_COMPONENT(HitboxComponent) {
    math::Vec3 offset{0.0f, 0.0f, 0.0f};
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 radius = 0.0f;  // > 0 selects sphere; <= 0 selects the AABB.
    u32 enabled = 1u;
};

// ─── Projectile ─────────────────────────────────────────────────────────────
// A live projectile entity. Carries its own velocity/damage/faction so the
// projectile-tick system needs no back-reference to the shooter. `life`
// counts down; <= 0 despawns. The shooter entity is recorded so a projectile
// never hits the thing that fired it on frame zero.
PSYNDER_COMPONENT(ProjectileComponent) {
    math::Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 damage = 0.0f;
    f32 life = 0.0f;
    u32 faction = 0u;
    Entity source{};   // entity that fired it (skipped during hit tests)
    u32 alive = 1u;    // cleared on hit/expiry; resolve_deaths/cleanup despawns
    u32 _pad = 0u;
};

// ─── Death flag ─────────────────────────────────────────────────────────────
// apply_damage NEVER destroys an entity mid-query (that mutates the archetype
// we are iterating). Instead it marks the kill via this flag; resolve_deaths
// runs after the query loop and despawns / fires callbacks. `pending` is set
// the frame the entity dies; `resolved` is set once handled so a double tick
// does not re-process it.
PSYNDER_COMPONENT(DeadComponent) {
    u32 pending = 1u;
    u32 resolved = 0u;
    u32 killer_faction = 0u;
    u32 _pad = 0u;
};

// ─── Damage event (queued) ───────────────────────────────────────────────────
// A pending hit. Combat systems append these to a CombatContext ring (no heap
// alloc) during the read-only spatial sweep, then apply them in a serial pass.
// Exposed as a component too so a host can author scripted damage by adding it
// to an entity; flush_damage_events drains both sources.
struct DamageEvent {
    Entity target{};
    f32 amount = 0.0f;
    u32 source_faction = 0u;
    u32 _pad = 0u;
};

// ─── Sanitizers ──────────────────────────────────────────────────────────────
[[nodiscard]] inline WeaponRuntimeComponent sanitize_weapon_runtime(
    WeaponRuntimeComponent w) noexcept {
    w.kind = (w.kind == WeaponKind::Projectile) ? WeaponKind::Projectile : WeaponKind::Hitscan;
    w._pad[0] = w._pad[1] = w._pad[2] = 0u;
    w.cooldown = (w.cooldown > 0.0f) ? w.cooldown : 0.0f;
    w.projectile_speed = (w.projectile_speed > 0.0f) ? w.projectile_speed : 40.0f;
    w.projectile_life = (w.projectile_life > 0.0f) ? w.projectile_life : 3.0f;
    return w;
}

[[nodiscard]] inline HitboxComponent sanitize_hitbox(HitboxComponent h) noexcept {
    if (!(h.half_extent.x > 0.0f)) h.half_extent.x = 0.5f;
    if (!(h.half_extent.y > 0.0f)) h.half_extent.y = 0.5f;
    if (!(h.half_extent.z > 0.0f)) h.half_extent.z = 0.5f;
    if (!(h.radius >= 0.0f)) h.radius = 0.0f;
    h.enabled = h.enabled != 0u ? 1u : 0u;
    return h;
}

}  // namespace psynder::gameplay
