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

#include <cmath>

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

// ─── TriggerVolume ───────────────────────────────────────────────────────────
// An invisible volume that EDGE-fires once when a tagged actor ENTERS it and
// re-arms only after every actor has left. Shape: sphere when radius > 0, else
// an axis-aligned box (center = entity TransformComponent translation + offset,
// extents = half_extent). `fire_faction` filters which actors count (0 = any).
// On a fresh entry the system bumps `fire_count`, sets `fired = 1` for that
// tick, and (when `target` is valid + `open_target_door != 0`) requests the
// linked Door to open — the data-driven "switch opens a door" wiring with no
// callbacks. `inside` latches occupancy so a body that stays inside does NOT
// re-fire (edge, not level).
PSYNDER_COMPONENT(TriggerVolumeComponent) {
    math::Vec3 offset{0.0f, 0.0f, 0.0f};
    math::Vec3 half_extent{1.0f, 1.0f, 1.0f};
    f32 radius = 0.0f;          // > 0 selects sphere; <= 0 selects the AABB.
    u32 fire_faction = 0u;      // 0 => any faction triggers; else exact match.
    Entity target{};           // optional Door (or any) entity to drive.
    u32 open_target_door = 1u;  // when set, a fresh entry calls open() on target.
    u32 enabled = 1u;
    // ── runtime latch (system-owned) ────────────────────────────────────────
    u32 inside = 0u;     // 1 while >=1 actor is in the volume (occupancy latch).
    u32 fired = 0u;      // 1 only on the tick a fresh entry happened (edge).
    u32 fire_count = 0u; // total distinct entries observed (monotonic).
    Entity last_actor{}; // the actor that caused the most recent entry.
};

// ─── Door ────────────────────────────────────────────────────────────────────
// A sliding slab with two authored endpoints (closed_pos / open_pos) it lerps
// between over `move_time` seconds. The door's BLOCKING is expressed through its
// own HitboxComponent: tick_doors sets HitboxComponent::enabled = 1 while the
// door is anything but fully Open, so a combat raycast (LOS / bullet) is blocked
// when closed and passes when open. A host that also wants a physics slab moved
// reads `position` each tick (the showcase drives a physics body from it). State
// advances Closed -> Opening -> Open -> Closing -> Closed; `open_request` /
// `close_request` are one-shot intents a trigger (or direct open()/close())
// sets, consumed by tick_doors. `instant != 0` snaps with no animation.
enum class DoorState : u8 {
    Closed = 0,
    Opening = 1,
    Open = 2,
    Closing = 3,
};

PSYNDER_COMPONENT(DoorComponent) {
    math::Vec3 closed_pos{0.0f, 0.0f, 0.0f};
    math::Vec3 open_pos{0.0f, 0.0f, 0.0f};
    f32 move_time = 1.0f;     // seconds for a full closed<->open traversal.
    f32 t = 0.0f;             // 0 = fully closed, 1 = fully open (lerp param).
    math::Vec3 position{0.0f, 0.0f, 0.0f};  // current slab centre (system write).
    DoorState state = DoorState::Closed;
    u8 instant = 0u;          // 1 => snap (no animation).
    u8 _pad[2] = {};
    u32 open_request = 0u;    // one-shot: request transition toward Open.
    u32 close_request = 0u;   // one-shot: request transition toward Closed.
    Entity hitbox_entity{};   // entity whose HitboxComponent gates LOS (often self).
};

// ─── Pickup ──────────────────────────────────────────────────────────────────
// A touchable item: on overlap with a tagged actor it grants its effect ONCE
// (edge-triggered via `consumed`) then is collected for despawn. Sphere overlap
// of `radius` about the entity TransformComponent translation (+offset). `kind`
// selects what the grant mutates on the actor:
//   Health  -> actor HealthComponent::current_health += amount (clamped to max)
//   Ammo    -> actor WeaponComponent::ammo += (u32)amount
//   Weapon  -> actor WeaponComponent stats set from {amount=damage, weapon_*}
// `pickup_faction` filters who may grab it (0 = any). `consumed` latches so a
// second overlap never double-grants even before the despawn lands.
enum class PickupKind : u8 {
    Health = 0,
    Ammo = 1,
    Weapon = 2,
};

PSYNDER_COMPONENT(PickupComponent) {
    math::Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    f32 amount = 25.0f;         // heal HP / ammo rounds / weapon damage by kind.
    PickupKind kind = PickupKind::Health;
    u8 _pad[3] = {};
    u32 pickup_faction = 0u;    // 0 => any actor may grab; else exact match.
    u32 enabled = 1u;
    // Weapon-kind authored stats (ignored for Health/Ammo).
    f32 weapon_range = 60.0f;
    f32 weapon_fire_rate = 6.0f;
    u32 weapon_ammo = 60u;
    // ── runtime latch (system-owned) ────────────────────────────────────────
    u32 consumed = 0u;          // 1 once granted (prevents double-grant).
    Entity grabbed_by{};        // actor that consumed it (0 until grabbed).
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

[[nodiscard]] inline TriggerVolumeComponent sanitize_trigger_volume(
    TriggerVolumeComponent t) noexcept {
    if (!(t.half_extent.x > 0.0f)) t.half_extent.x = 1.0f;
    if (!(t.half_extent.y > 0.0f)) t.half_extent.y = 1.0f;
    if (!(t.half_extent.z > 0.0f)) t.half_extent.z = 1.0f;
    if (!(t.radius >= 0.0f)) t.radius = 0.0f;
    t.open_target_door = t.open_target_door != 0u ? 1u : 0u;
    t.enabled = t.enabled != 0u ? 1u : 0u;
    t.inside = t.inside != 0u ? 1u : 0u;
    t.fired = t.fired != 0u ? 1u : 0u;
    return t;
}

[[nodiscard]] inline DoorComponent sanitize_door(DoorComponent d) noexcept {
    d.move_time = (d.move_time > 0.0f) ? d.move_time : 1.0f;
    if (!(d.t >= 0.0f)) d.t = 0.0f;
    if (d.t > 1.0f) d.t = 1.0f;
    switch (d.state) {
        case DoorState::Closed:
        case DoorState::Opening:
        case DoorState::Open:
        case DoorState::Closing:
            break;
        default:
            d.state = DoorState::Closed;
            break;
    }
    d.instant = d.instant != 0u ? 1u : 0u;
    d._pad[0] = d._pad[1] = 0u;
    d.open_request = d.open_request != 0u ? 1u : 0u;
    d.close_request = d.close_request != 0u ? 1u : 0u;
    // Initialise the rendered/queried position to the lerp of the endpoints so a
    // door authored at t!=0 starts visually consistent before the first tick.
    d.position = math::add(d.closed_pos,
                           math::mul(math::sub(d.open_pos, d.closed_pos), d.t));
    return d;
}

[[nodiscard]] inline PickupComponent sanitize_pickup(PickupComponent p) noexcept {
    if (!(p.radius > 0.0f)) p.radius = 0.75f;
    if (!std::isfinite(p.amount) || p.amount < 0.0f) p.amount = 0.0f;
    switch (p.kind) {
        case PickupKind::Health:
        case PickupKind::Ammo:
        case PickupKind::Weapon:
            break;
        default:
            p.kind = PickupKind::Health;
            break;
    }
    p._pad[0] = p._pad[1] = p._pad[2] = 0u;
    p.enabled = p.enabled != 0u ? 1u : 0u;
    p.weapon_range = (p.weapon_range > 0.0f) ? p.weapon_range : 60.0f;
    p.weapon_fire_rate = (p.weapon_fire_rate > 0.0f) ? p.weapon_fire_rate : 6.0f;
    p.consumed = p.consumed != 0u ? 1u : 0u;
    return p;
}

}  // namespace psynder::gameplay
