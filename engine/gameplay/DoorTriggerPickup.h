// SPDX-License-Identifier: MIT
// Psynder — W10-3 classic indoor-shooter mechanics (Duke3D/Quake): DOORS,
// TRIGGERS, and PICKUPS as DOTS systems over the GameplayComponents.h POD
// components.
//
// DOTS / ALLOC-FREE contract (matches CombatSystems.h + scene::gather_*):
//   * Each tick is one `registry.query<reads<...>, writes<...>>` whose body
//     fires once per chunk. Trigger / pickup overlap tests need the ACTOR
//     positions (the player + AI agents) — those are passed in as a small
//     caller-owned span (no per-frame heap; the host holds the array). The
//     query body reads actor positions via registry.get<TransformComponent>,
//     a pure const lookup, exactly as raycast_hitboxes already does.
//   * Edge semantics are stored IN the component (occupancy / consumed latches),
//     never in a side table — so a trigger fires once per entry and a pickup
//     grants once, deterministically, with no per-frame allocation.
//   * Structural change (despawning a grabbed pickup) is DEFERRED: the system
//     collects spent pickups into a caller list and the host despawns them
//     after the query returns — never mid-iteration.
//
// Depends on scene/math/core only (same layering as the rest of gameplay).

#pragma once

#include "core/Types.h"
#include "gameplay/GameplayComponents.h"
#include "math/Math.h"
#include "scene/SceneEcs.h"

#include <span>

namespace psynder::gameplay {

using ::psynder::Entity;
using ::psynder::f32;
using ::psynder::u32;
using ::psynder::usize;

// ─── Actor view ───────────────────────────────────────────────────────────────
// A trigger / pickup acts on a small, known set of "actors" (the player plus the
// AI agents). The host owns this array; the systems read it without allocating.
// `entity` must carry a TransformComponent (its world-ish position); `faction`
// is used to filter which volumes it can fire / grab (0 matches any filter).
struct Actor {
    Entity entity{};
    u32 faction = 0u;
};

// ─── Trigger event (one entry observed this tick) ─────────────────────────────
struct TriggerEvent {
    Entity trigger{};
    Entity actor{};
    Entity target{};       // the door (or other) the trigger drives, if any.
    u32 fire_count = 0u;   // monotonic entry count after this fire.
};

// ─── Pickup event (one grant resolved this tick) ──────────────────────────────
struct PickupEvent {
    Entity pickup{};
    Entity actor{};
    PickupKind kind = PickupKind::Health;
    f32 amount = 0.0f;
    f32 before = 0.0f;     // actor HP / ammo before the grant (for logging).
    f32 after = 0.0f;      // actor HP / ammo after the grant.
};

// ─── tick_triggers ────────────────────────────────────────────────────────────
// Edge-fire every enabled TriggerVolumeComponent whose volume newly contains a
// matching actor. Occupancy is latched in `inside`: a fresh entry (inside 0->1)
// bumps `fire_count`, sets `fired = 1`, records `last_actor`, appends a
// TriggerEvent to `out_events` (capacity-bounded, no realloc inside the body),
// and — when `open_target_door != 0` and `target` is a valid DoorComponent —
// sets that door's `open_request`. A body that stays inside does NOT re-fire;
// the latch clears only once every actor has left. Returns the number of fresh
// fires this tick. `out_events` may be empty (count still returned).
u32 tick_triggers(scene::EcsRegistry& registry,
                  std::span<const Actor> actors,
                  std::span<TriggerEvent> out_events,
                  u32* out_event_count = nullptr);

// ─── tick_doors ───────────────────────────────────────────────────────────────
// Advance every DoorComponent's state machine by dt: consume one-shot
// open_request / close_request, integrate `t` toward the target over move_time
// (or snap when `instant`), recompute `position` as lerp(closed_pos, open_pos,
// t), and gate the linked HitboxComponent — enabled (blocking) whenever the door
// is not fully Open, disabled (passable) when Open. Pure per-row writes plus a
// const hitbox lookup; no accumulation => parallel-safe across chunks.
void tick_doors(scene::EcsRegistry& registry, f32 dt);

// Direct intents (host / script side, outside a query). open() requests a
// transition toward Open; close() toward Closed. No-op on a stale entity / a
// missing DoorComponent. Returns true if the request was recorded.
bool door_open(scene::EcsRegistry& registry, Entity door);
bool door_close(scene::EcsRegistry& registry, Entity door);

// Convenience predicate: true when the door is fully Open (passable / LOS-clear).
[[nodiscard]] bool door_is_open(scene::EcsRegistry& registry, Entity door);

// ─── tick_pickups ─────────────────────────────────────────────────────────────
// Grant every enabled, un-consumed PickupComponent that overlaps a matching
// actor (sphere of `radius` about the pickup position). The FIRST overlapping
// actor wins; the grant mutates that actor's Health/Weapon per `kind`, the
// pickup latches `consumed = 1` (no double-grant even before despawn), records
// `grabbed_by`, appends a PickupEvent to `out_events`, and the pickup entity is
// appended to `out_despawn` for the host to remove AFTER the query (deferred
// structural change). Returns the number granted this tick.
u32 tick_pickups(scene::EcsRegistry& registry,
                 std::span<const Actor> actors,
                 std::span<Entity> out_despawn,
                 u32* out_despawn_count,
                 std::span<PickupEvent> out_events,
                 u32* out_event_count = nullptr);

}  // namespace psynder::gameplay
