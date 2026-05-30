// SPDX-License-Identifier: MIT
// Psynder — W10-3 door / trigger / pickup systems implementation.
//
// PARALLEL-SAFE pattern (mirrors CombatSystems::tick_projectiles): a query body
// fires once per chunk and MAY run on a worker thread, so a body never mutates a
// DIFFERENT entity than the row it owns. Cross-entity effects (a trigger opening
// a target door, a pickup granting to an actor + despawning itself) are recorded
// into a per-chunk stack buffer, merged once per chunk under a mutex, then
// APPLIED serially after the query returns. Determinism: the merge order can
// vary with thread scheduling, so the serial apply sorts by (source) entity raw
// id before mutating, giving a chunk-order-independent, reproducible result.

#include "gameplay/DoorTriggerPickup.h"

#include "scene/EcsRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <vector>

namespace psynder::gameplay {

using namespace ::psynder::scene;

namespace {

constexpr usize kChunkScratch = 64u;  // per-chunk stack buffer for collected ops.

// Actor world-ish position from its TransformComponent translation (the same
// space combat + AI operate in). Returns false when the actor is dead / has no
// transform.
bool actor_position(EcsRegistry& registry, Entity e, math::Vec3& out) {
    if (!registry.alive(e))
        return false;
    if (const auto* t = registry.get<TransformComponent>(e)) {
        out = t->local.translation;
        return true;
    }
    return false;
}

// Point-vs-AABB (centre +/- half_extent) inclusive test.
[[nodiscard]] bool point_in_aabb(math::Vec3 p, math::Vec3 center,
                                 math::Vec3 half) noexcept {
    return std::fabs(p.x - center.x) <= half.x &&
           std::fabs(p.y - center.y) <= half.y &&
           std::fabs(p.z - center.z) <= half.z;
}

// Point-vs-sphere (squared, no sqrt) inclusive test.
[[nodiscard]] bool point_in_sphere(math::Vec3 p, math::Vec3 center,
                                   f32 radius) noexcept {
    const math::Vec3 d = math::sub(p, center);
    return math::dot(d, d) <= radius * radius;
}

// First actor (in caller order) inside the volume that passes the faction
// filter. Caller order is stable, so the chosen actor is deterministic.
Entity first_actor_in_volume(EcsRegistry& registry,
                             std::span<const Actor> actors,
                             u32 faction_filter,
                             math::Vec3 center,
                             f32 radius,
                             math::Vec3 half) {
    for (const Actor& a : actors) {
        if (!a.entity.valid())
            continue;
        if (faction_filter != 0u && a.faction != faction_filter)
            continue;
        math::Vec3 p{};
        if (!actor_position(registry, a.entity, p))
            continue;
        const bool in = (radius > 0.0f) ? point_in_sphere(p, center, radius)
                                        : point_in_aabb(p, center, half);
        if (in)
            return a.entity;
    }
    return Entity{};
}

// Apply a pickup's effect to the actor. Returns true if anything was granted and
// fills before/after for the event log. Health clamps at max_health; ammo + and
// weapon overwrite are plain field writes (deterministic, no RNG).
bool grant_pickup(EcsRegistry& registry, const PickupComponent& pk, Entity actor,
                  f32& before, f32& after) {
    before = 0.0f;
    after = 0.0f;
    switch (pk.kind) {
        case PickupKind::Health: {
            auto* h = registry.get<HealthComponent>(actor);
            if (!h)
                return false;
            before = h->current_health;
            f32 hp = h->current_health + pk.amount;
            if (hp > h->max_health)
                hp = h->max_health;
            h->current_health = hp;
            after = hp;
            return true;
        }
        case PickupKind::Ammo: {
            auto* w = registry.get<WeaponComponent>(actor);
            if (!w)
                return false;
            before = static_cast<f32>(w->ammo);
            w->ammo += static_cast<u32>(pk.amount);
            after = static_cast<f32>(w->ammo);
            return true;
        }
        case PickupKind::Weapon: {
            auto* w = registry.get<WeaponComponent>(actor);
            if (!w)
                return false;
            before = w->damage;
            w->damage = pk.amount;
            w->range = pk.weapon_range;
            w->fire_rate = pk.weapon_fire_rate;
            w->ammo = pk.weapon_ammo;
            *w = sanitize_weapon_component(*w);
            after = w->damage;
            return true;
        }
    }
    return false;
}

// A trigger entry collected during the read sweep, applied serially after.
struct TrigOp {
    Entity trigger{};
    Entity actor{};
    Entity target{};
    u32 fire_count = 0u;
    u32 open_door = 0u;
};

// A pickup grab collected during the read sweep, applied serially after.
struct PickOp {
    Entity pickup{};
    Entity actor{};
    PickupKind kind = PickupKind::Health;
    f32 amount = 0.0f;
};

}  // namespace

u32 tick_triggers(EcsRegistry& registry,
                  std::span<const Actor> actors,
                  std::span<TriggerEvent> out_events,
                  u32* out_event_count) {
    // Sweep phase (parallel-safe): update each trigger's OWN occupancy/edge
    // latch in place (a row-local write), and collect the cross-entity "open
    // door" intent for the serial apply below.
    std::mutex mu;
    std::vector<TrigOp> ops;
    ops.reserve(out_events.size());

    registry.query<reads<SceneNodeComponent>, writes<TriggerVolumeComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<TriggerVolumeComponent> trigs) {
            const usize n = std::min(nodes.size(), trigs.size());
            std::array<TrigOp, kChunkScratch> chunk{};
            usize count = 0u;
            for (usize i = 0; i < n; ++i) {
                TriggerVolumeComponent& tv = trigs[i];
                tv.fired = 0u;
                if (tv.enabled == 0u) {
                    tv.inside = 0u;
                    continue;
                }
                const Entity self = nodes[i].entity;
                math::Vec3 origin{};
                if (const auto* t = registry.get<TransformComponent>(self))
                    origin = t->local.translation;
                const math::Vec3 center = math::add(origin, tv.offset);

                const Entity who = first_actor_in_volume(
                    registry, actors, tv.fire_faction, center, tv.radius,
                    tv.half_extent);
                const bool occupied = who.valid();

                if (occupied && tv.inside == 0u) {
                    // Fresh entry => edge fire (row-local latch writes).
                    tv.inside = 1u;
                    tv.fired = 1u;
                    tv.last_actor = who;
                    ++tv.fire_count;
                    if (count < chunk.size()) {
                        TrigOp& op = chunk[count++];
                        op.trigger = self;
                        op.actor = who;
                        op.target = tv.target;
                        op.fire_count = tv.fire_count;
                        op.open_door = (tv.open_target_door != 0u &&
                                        tv.target.valid())
                                           ? 1u
                                           : 0u;
                    }
                } else if (!occupied) {
                    tv.inside = 0u;  // re-arm once everyone has left.
                }
            }
            if (count != 0u) {
                std::scoped_lock lk{mu};
                ops.insert(ops.end(), chunk.begin(), chunk.begin() + count);
            }
        });

    // Serial apply (deterministic): order by the trigger entity id so the door
    // writes + emitted events do not depend on worker scheduling.
    std::sort(ops.begin(), ops.end(), [](const TrigOp& a, const TrigOp& b) {
        return a.trigger.raw < b.trigger.raw;
    });

    u32 evt = 0u;
    for (const TrigOp& op : ops) {
        if (op.open_door != 0u) {
            if (auto* door = registry.get<DoorComponent>(op.target))
                door->open_request = 1u;
        }
        if (evt < out_events.size()) {
            TriggerEvent& e = out_events[evt++];
            e.trigger = op.trigger;
            e.actor = op.actor;
            e.target = op.target;
            e.fire_count = op.fire_count;
        }
    }

    if (out_event_count)
        *out_event_count = evt;
    return static_cast<u32>(ops.size());
}

void tick_doors(EcsRegistry& registry, f32 dt) {
    if (!(dt > 0.0f))
        dt = 0.0f;

    // Pure per-row writes (door state + its own linked hitbox enable) => no
    // shared accumulation, safe to run unguarded across chunks.
    registry.query<reads<>, writes<DoorComponent>>(
        [&](std::span<DoorComponent> doors) {
            for (DoorComponent& d : doors) {
                const f32 move_time = (d.move_time > 0.0f) ? d.move_time : 1.0f;

                // Consume one-shot intents. A request always wins over the
                // opposite in-flight motion (a switch re-press re-opens).
                if (d.open_request != 0u) {
                    d.open_request = 0u;
                    if (d.state != DoorState::Open)
                        d.state = d.instant ? DoorState::Open : DoorState::Opening;
                    if (d.instant)
                        d.t = 1.0f;
                }
                if (d.close_request != 0u) {
                    d.close_request = 0u;
                    if (d.state != DoorState::Closed)
                        d.state = d.instant ? DoorState::Closed : DoorState::Closing;
                    if (d.instant)
                        d.t = 0.0f;
                }

                // Integrate the lerp param toward the active state's target.
                const f32 rate = (move_time > 0.0f) ? (dt / move_time) : 1.0f;
                if (d.state == DoorState::Opening) {
                    d.t += rate;
                    if (d.t >= 1.0f) {
                        d.t = 1.0f;
                        d.state = DoorState::Open;
                    }
                } else if (d.state == DoorState::Closing) {
                    d.t -= rate;
                    if (d.t <= 0.0f) {
                        d.t = 0.0f;
                        d.state = DoorState::Closed;
                    }
                }
                d.t = std::clamp(d.t, 0.0f, 1.0f);

                // Current slab centre = lerp(closed, open, t).
                d.position = math::add(
                    d.closed_pos,
                    math::mul(math::sub(d.open_pos, d.closed_pos), d.t));

                // Blocking: the door's hitbox is ENABLED (blocks LOS / bullets)
                // unless the door is fully Open. Drive the linked hitbox so a
                // combat raycast respects the door's openness. The hitbox lives
                // on a distinct entity, but the enable flag is an idempotent
                // set-to-0/1 owned solely by THIS door => race-benign.
                if (d.hitbox_entity.valid()) {
                    if (auto* hb = registry.get<HitboxComponent>(d.hitbox_entity))
                        hb->enabled = (d.state == DoorState::Open) ? 0u : 1u;
                }
            }
        });
}

bool door_open(EcsRegistry& registry, Entity door) {
    if (auto* d = registry.get<DoorComponent>(door)) {
        d->open_request = 1u;
        return true;
    }
    return false;
}

bool door_close(EcsRegistry& registry, Entity door) {
    if (auto* d = registry.get<DoorComponent>(door)) {
        d->close_request = 1u;
        return true;
    }
    return false;
}

bool door_is_open(EcsRegistry& registry, Entity door) {
    if (const auto* d = registry.get<DoorComponent>(door))
        return d->state == DoorState::Open;
    return false;
}

u32 tick_pickups(EcsRegistry& registry,
                 std::span<const Actor> actors,
                 std::span<Entity> out_despawn,
                 u32* out_despawn_count,
                 std::span<PickupEvent> out_events,
                 u32* out_event_count) {
    // Sweep phase (parallel-safe): latch each grabbed pickup's `consumed` /
    // `grabbed_by` in place (row-local) and collect the cross-entity grant for
    // the serial apply. We latch in the sweep so a second pickup chunk cannot
    // re-grab the same item, then apply the actual HP/ammo mutation serially.
    std::mutex mu;
    std::vector<PickOp> ops;
    ops.reserve(out_despawn.size());

    registry.query<reads<SceneNodeComponent>, writes<PickupComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<PickupComponent> pickups) {
            const usize n = std::min(nodes.size(), pickups.size());
            std::array<PickOp, kChunkScratch> chunk{};
            usize count = 0u;
            for (usize i = 0; i < n; ++i) {
                PickupComponent& pk = pickups[i];
                if (pk.enabled == 0u || pk.consumed != 0u)
                    continue;
                const Entity self = nodes[i].entity;
                math::Vec3 origin{};
                if (const auto* t = registry.get<TransformComponent>(self))
                    origin = t->local.translation;
                const math::Vec3 center = math::add(origin, pk.offset);

                const Entity who = first_actor_in_volume(
                    registry, actors, pk.pickup_faction, center, pk.radius,
                    math::Vec3{});  // half unused (pickups are spheres).
                if (!who.valid())
                    continue;

                // Latch immediately so a parallel chunk / re-entry cannot
                // double-grab. The HP/ammo mutation is deferred to the apply.
                pk.consumed = 1u;
                pk.grabbed_by = who;
                if (count < chunk.size()) {
                    PickOp& op = chunk[count++];
                    op.pickup = self;
                    op.actor = who;
                    op.kind = pk.kind;
                    op.amount = pk.amount;
                }
            }
            if (count != 0u) {
                std::scoped_lock lk{mu};
                ops.insert(ops.end(), chunk.begin(), chunk.begin() + count);
            }
        });

    // Serial apply (deterministic): order by pickup id so concurrent grants to
    // the same actor accumulate in a reproducible order. The grant re-reads the
    // live PickupComponent so the weapon-stat block (not copied into PickOp)
    // stays authoritative.
    std::sort(ops.begin(), ops.end(), [](const PickOp& a, const PickOp& b) {
        return a.pickup.raw < b.pickup.raw;
    });

    u32 grants = 0u;
    u32 desp = 0u;
    u32 evt = 0u;
    for (const PickOp& op : ops) {
        const auto* pk = registry.get<PickupComponent>(op.pickup);
        if (!pk)
            continue;
        f32 before = 0.0f;
        f32 after = 0.0f;
        if (!grant_pickup(registry, *pk, op.actor, before, after))
            continue;  // actor lost its target component since the sweep.
        ++grants;
        if (evt < out_events.size()) {
            PickupEvent& e = out_events[evt++];
            e.pickup = op.pickup;
            e.actor = op.actor;
            e.kind = op.kind;
            e.amount = op.amount;
            e.before = before;
            e.after = after;
        }
        if (desp < out_despawn.size())
            out_despawn[desp++] = op.pickup;
    }

    if (out_despawn_count)
        *out_despawn_count = desp;
    if (out_event_count)
        *out_event_count = evt;
    return grants;
}

}  // namespace psynder::gameplay
