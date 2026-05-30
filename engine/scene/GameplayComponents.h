// SPDX-License-Identifier: MIT
// Psynder - SCENE-LEVEL authoring gameplay + AI components (no-code editor
// authoring lane). Sibling to PhysicsComponents.h: these are the pooled ECS
// components a designer attaches in the editor so an entity fights / senses /
// patrols in Play mode, WITHOUT writing C++.
//
// CRITICAL LAYERING RULE (same as PhysicsComponents.h): engine/scene must NOT
// depend on engine/gameplay or engine/ai. The live combat + AI systems own real
// gameplay::/ai:: components; those headers depend on scene (never the reverse).
// So we cannot store gameplay::FactionComponent / ai::AiAgentComponent directly
// in the scene registry that the serializer + Inspector see. Instead these PODs
// MIRROR the gameplay/ai components field-for-field. The scene<->gameplay/ai
// mapping happens only at the Play boundary inside editor/play/PlayRuntime, which
// DOES include gameplay + ai and copies each authoring proxy into the matching
// live component when a Play session begins.
//
// Why proxies and not a re-export (like scene::HealthComponent /
// scene::WeaponComponent, which gameplay re-exports FROM scene): Faction / Hitbox
// and the AI trio are DEFINED in gameplay/ai, and this lane may not edit those
// modules' internals. A scene-level proxy keeps the authoring + serialization +
// Inspector path entirely inside engine/scene.
//
// All authoring fields here persist with the .psyscene (SGAI chunk, SceneFile
// v4). They carry NO runtime-only handles, so every field round-trips.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"

#include <algorithm>
#include <cmath>

namespace psynder::scene {

// ─── Faction tag (proxy of gameplay::FactionComponent) ──────────────────────
// A team id. Combat reads HealthComponent::faction first and falls back to this
// when an entity has no health (a trigger / spawner / projectile source).
PSYNDER_COMPONENT(FactionComponent) {
    u32 faction = 0u;
};

// ─── Hitbox (proxy of gameplay::HitboxComponent) ────────────────────────────
// A ray/projectile collides against this. radius > 0 selects a sphere; else the
// axis-aligned box (half_extent) is used. Centred on the entity's Transform
// translation plus a local offset. Field order MIRRORS gameplay::HitboxComponent.
PSYNDER_COMPONENT(HitboxComponent) {
    math::Vec3 offset{0.0f, 0.0f, 0.0f};
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 radius = 0.0f;   // > 0 selects sphere; <= 0 selects the AABB.
    u32 enabled = 1u;
};

// ─── Weapon fire kind (proxy of gameplay::WeaponRuntimeComponent authoring) ──
// scene::WeaponComponent already holds damage/range/fire_rate/ammo. This proxy
// carries the projectile-vs-hitscan discriminator + the projectile spawn params
// the combat layer's WeaponRuntimeComponent needs. The live cooldown is RUNTIME
// state owned by combat and is NOT authored here. kind: 0 = Hitscan, 1 =
// Projectile (mirrors gameplay::WeaponKind value order).
enum class WeaponFireKind : u8 {
    Hitscan = 0,
    Projectile = 1,
};

[[nodiscard]] constexpr WeaponFireKind sanitize_weapon_fire_kind(WeaponFireKind k) noexcept {
    return (k == WeaponFireKind::Projectile) ? WeaponFireKind::Projectile
                                             : WeaponFireKind::Hitscan;
}

PSYNDER_COMPONENT(WeaponModeComponent) {
    WeaponFireKind kind = WeaponFireKind::Hitscan;
    u8 _pad[3] = {};
    f32 projectile_speed = 40.0f;  // m/s; ignored for hitscan
    f32 projectile_life = 3.0f;    // s;   ignored for hitscan
};

// ─── AI brain state (proxy of ai::AiState) ──────────────────────────────────
// Mirrors ai::AiState value order so PlayRuntime maps with a single cast.
enum class AiState : u8 {
    Idle = 0,
    Patrol = 1,
    Chase = 2,
    Attack = 3,
    Dead = 4,
};

[[nodiscard]] constexpr AiState sanitize_ai_state(AiState s) noexcept {
    switch (s) {
        case AiState::Idle:
        case AiState::Patrol:
        case AiState::Chase:
        case AiState::Attack:
        case AiState::Dead:
            return s;
    }
    return AiState::Idle;
}

// ─── AI agent brain (proxy of ai::AiAgentComponent authoring) ───────────────
// The acquired-target Entity + per-frame think cooldown are RUNTIME state filled
// by the AI systems, so they are NOT authored here. The designer sets the FSM
// start state + the sense / combat envelope + the think interval + move speed.
PSYNDER_COMPONENT(AiAgentComponent) {
    AiState state = AiState::Idle;
    u8 _pad[3] = {};
    f32 sight_range = 20.0f;     // metres the agent can see
    f32 fov_cos = 0.5f;          // cos(half-FOV); 1 = dead-ahead, -1 = 360 deg
    f32 attack_range = 12.0f;    // metres inside which Chase -> Attack
    f32 think_interval = 0.15f;  // seconds between brain re-evaluations
    f32 move_speed = 3.5f;       // metres/sec steering speed
};

// ─── Perception sense (proxy of ai::PerceptionComponent) ────────────────────
// Authoring this (even empty) marks an agent as wanting a perception snapshot.
// last_seen_pos / last_seen_time / can_see are RUNTIME and start at their POD
// defaults each Play session, so the proxy carries no authored fields beyond the
// tag itself; an empty struct keeps the component trivially copyable + present.
PSYNDER_COMPONENT(PerceptionComponent) {
    u32 _reserved = 0u;
};

// ─── Patrol route (proxy of ai::PatrolComponent authoring) ──────────────────
// A small fixed waypoint ring (no heap). The agent walks waypoints[current];
// on arrival it dwells wait_time seconds then advances (wrapping). `current` /
// `wait_timer` are RUNTIME and start at 0 each session, so only the waypoint
// list + count + dwell + arrive radius are authored.
PSYNDER_COMPONENT(PatrolComponent) {
    static constexpr u32 kMaxWaypoints = 8u;
    math::Vec3 waypoints[kMaxWaypoints] = {};
    u32 count = 0u;
    f32 wait_time = 1.0f;       // seconds to dwell at a reached waypoint
    f32 arrive_radius = 0.5f;   // metres at which a waypoint counts as reached
    u32 _pad = 0u;
};

// ─── Sanitizers ──────────────────────────────────────────────────────────────
[[nodiscard]] inline HitboxComponent sanitize_hitbox_component(HitboxComponent h) noexcept {
    if (!(h.half_extent.x > 0.0f)) h.half_extent.x = 0.5f;
    if (!(h.half_extent.y > 0.0f)) h.half_extent.y = 0.5f;
    if (!(h.half_extent.z > 0.0f)) h.half_extent.z = 0.5f;
    if (!(h.radius >= 0.0f)) h.radius = 0.0f;
    h.enabled = h.enabled != 0u ? 1u : 0u;
    return h;
}

[[nodiscard]] inline WeaponModeComponent sanitize_weapon_mode_component(
    WeaponModeComponent w) noexcept {
    w.kind = sanitize_weapon_fire_kind(w.kind);
    w._pad[0] = w._pad[1] = w._pad[2] = 0u;
    w.projectile_speed = (w.projectile_speed > 0.0f) ? w.projectile_speed : 40.0f;
    w.projectile_life = (w.projectile_life > 0.0f) ? w.projectile_life : 3.0f;
    return w;
}

[[nodiscard]] inline AiAgentComponent sanitize_ai_agent_component(AiAgentComponent a) noexcept {
    a.state = sanitize_ai_state(a.state);
    a._pad[0] = a._pad[1] = a._pad[2] = 0u;
    if (!(a.sight_range >= 0.0f)) a.sight_range = 0.0f;
    if (!(a.fov_cos >= -1.0f && a.fov_cos <= 1.0f)) a.fov_cos = 0.5f;
    if (!(a.attack_range >= 0.0f)) a.attack_range = 0.0f;
    if (!(a.think_interval >= 0.0f)) a.think_interval = 0.15f;
    if (!(a.move_speed >= 0.0f)) a.move_speed = 0.0f;
    return a;
}

[[nodiscard]] inline PatrolComponent sanitize_patrol_component(PatrolComponent p) noexcept {
    if (p.count > PatrolComponent::kMaxWaypoints)
        p.count = PatrolComponent::kMaxWaypoints;
    if (!(p.wait_time >= 0.0f)) p.wait_time = 0.0f;
    if (!(p.arrive_radius > 0.0f)) p.arrive_radius = 0.5f;
    p._pad = 0u;
    return p;
}

}  // namespace psynder::scene
