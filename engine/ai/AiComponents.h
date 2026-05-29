// SPDX-License-Identifier: MIT
// Psynder — M-AI enemy-AI components (DOTS). Pure-POD ECS components for the
// shooter targets (Quake/Duke/Delta Force): a perception sense, a finite-state
// brain, and a patrol route. Trivially-copyable, pooled by the archetype-chunked
// registry, mutated only through queries.
//
// REUSE-vs-NEW (see the M-AI brief):
//   * scene::HealthComponent  — REUSED: an agent goes Dead when current_health
//     hits 0; we never duplicate health here.
//   * scene::TransformComponent — REUSED: agent + target world-ish positions are
//     read from / written to the Transform translation (the same space combat
//     operates in, so LOS + firing line up without a scene-graph flatten).
//   * gameplay::HealthComponent::faction / FactionComponent — REUSED for hostile
//     selection (an agent only chases / attacks a *different* faction).
//   * Everything below is NEW and owned by this module.
//
// This module depends on scene/gameplay/math — never the reverse (no
// scene/gameplay/physics -> ai dependency), and never on render/host/net.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"

namespace psynder::ai {

using ::psynder::Entity;
using ::psynder::f32;
using ::psynder::u8;
using ::psynder::u32;

// ─── Brain state ──────────────────────────────────────────────────────────
// The finite-state machine the `think` system drives. Plain u8 enum so the
// component stays trivially copyable and the state is cheap to compare in the
// hot per-chunk loop.
//   Idle   — no target known; will drop into Patrol if a route exists.
//   Patrol — walking the PatrolComponent waypoint ring.
//   Chase  — a hostile was seen; move toward its last-seen position.
//   Attack — hostile is in range + visible; fire via the host hook.
//   Dead   — HealthComponent hit 0; the agent stops sensing / thinking / acting.
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

// ─── Agent brain ────────────────────────────────────────────────────────────
// One per AI-controlled entity. Holds the current FSM state, the acquired
// target, the sense + combat envelope, and a per-agent think throttle so a big
// crowd does not all re-evaluate every frame (think_cooldown counts down in the
// `think` system; transitions only happen when it reaches 0).
PSYNDER_COMPONENT(AiAgentComponent) {
    AiState state = AiState::Idle;
    u8 _pad[3] = {};
    // Entity currently engaged (invalid => none). Resolved by `perceive`.
    Entity target_entity{};
    // Sight envelope: max distance the agent can see (metres) and the cosine of
    // the half field-of-view angle. A target counts as visible when it is within
    // sight_range AND the dot of (agent forward, dir-to-target) >= fov_cos AND
    // the injected LOS probe is clear. fov_cos = 1 means dead-ahead only; -1
    // means full 360 awareness.
    f32 sight_range = 20.0f;
    f32 fov_cos = 0.5f;  // ~120 deg total FOV
    // Distance (metres) inside which the agent switches Chase -> Attack.
    f32 attack_range = 12.0f;
    // Seconds until the next brain re-evaluation. Decremented every think tick;
    // the FSM only steps when this reaches 0, then it is re-armed to
    // think_interval.
    f32 think_cooldown = 0.0f;
    f32 think_interval = 0.15f;
    // Movement speed (metres/sec) used by `act` when steering toward a target /
    // waypoint.
    f32 move_speed = 3.5f;
};

// ─── Perception sense ─────────────────────────────────────────────────────
// Updated by `perceive`. Decouples sensing from thinking: `think` reads only
// this snapshot, never the raw scene, so the two systems compose without a
// shared scan. `last_seen_pos` is the memory an agent chases toward after it
// loses sight (Chase to last-seen, NOT teleport-to-current).
PSYNDER_COMPONENT(PerceptionComponent) {
    math::Vec3 last_seen_pos{0.0f, 0.0f, 0.0f};
    // Seconds since spawn (host-supplied clock, accumulated by `perceive`) at
    // which the target was last visible. Lets a host expire stale memories.
    f32 last_seen_time = 0.0f;
    u32 can_see = 0u;  // 1 => the target is visible THIS tick.
};

// ─── Patrol route ────────────────────────────────────────────────────────
// A small fixed waypoint ring (no heap; trivially copyable). `act` steers the
// agent toward waypoints[current]; on arrival it waits `wait_time` seconds then
// advances (wrapping) to the next. count <= kMaxWaypoints.
PSYNDER_COMPONENT(PatrolComponent) {
    static constexpr u32 kMaxWaypoints = 8u;
    math::Vec3 waypoints[kMaxWaypoints] = {};
    u32 count = 0u;
    u32 current = 0u;
    // Seconds remaining to dwell at the current waypoint before advancing.
    f32 wait_timer = 0.0f;
    // Seconds to dwell once a waypoint is reached.
    f32 wait_time = 1.0f;
    // Radius (metres) at which a waypoint counts as reached.
    f32 arrive_radius = 0.5f;
    u32 _pad = 0u;
};

// ─── Sanitizers ──────────────────────────────────────────────────────────
[[nodiscard]] inline AiAgentComponent sanitize_ai_agent(AiAgentComponent a) noexcept {
    a.state = sanitize_ai_state(a.state);
    a._pad[0] = a._pad[1] = a._pad[2] = 0u;
    if (!(a.sight_range >= 0.0f)) a.sight_range = 0.0f;
    if (!(a.fov_cos >= -1.0f && a.fov_cos <= 1.0f)) a.fov_cos = 0.5f;
    if (!(a.attack_range >= 0.0f)) a.attack_range = 0.0f;
    if (!(a.think_cooldown >= 0.0f)) a.think_cooldown = 0.0f;
    if (!(a.think_interval >= 0.0f)) a.think_interval = 0.15f;
    if (!(a.move_speed >= 0.0f)) a.move_speed = 0.0f;
    return a;
}

[[nodiscard]] inline PatrolComponent sanitize_patrol(PatrolComponent p) noexcept {
    if (p.count > PatrolComponent::kMaxWaypoints)
        p.count = PatrolComponent::kMaxWaypoints;
    if (p.count != 0u)
        p.current = p.current % p.count;
    else
        p.current = 0u;
    if (!(p.wait_timer >= 0.0f)) p.wait_timer = 0.0f;
    if (!(p.wait_time >= 0.0f)) p.wait_time = 0.0f;
    if (!(p.arrive_radius > 0.0f)) p.arrive_radius = 0.5f;
    p._pad = 0u;
    return p;
}

}  // namespace psynder::ai
