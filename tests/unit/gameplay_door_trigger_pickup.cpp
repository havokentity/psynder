// SPDX-License-Identifier: MIT
// Psynder — W10-3 door / trigger / pickup unit tests. Exercises the classic
// indoor-shooter mechanics as DOTS systems over the scene ECS:
//   * a TriggerVolume edge-fires ONCE on entry, not while an actor stays inside,
//     and re-arms after the actor leaves;
//   * a trigger linked to a door requests it open;
//   * a Door blocks a combat raycast while closed and passes it once open,
//     animating its position over the move time (and snapping when instant);
//   * a Pickup grants its effect ONCE on overlap (Health heal / Ammo / Weapon),
//     despawns, and never double-grants;
//   * faction filters gate which actor may fire a trigger / grab a pickup;
//   * the whole tick is deterministic (re-running a scripted sequence on a fresh
//     world reproduces every latch + grant + door position bit-for-bit).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "gameplay/CombatSystems.h"
#include "gameplay/DoorTriggerPickup.h"
#include "gameplay/GameplayComponents.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <array>
#include <span>

using namespace psynder;
using namespace psynder::gameplay;
using psynder::scene::EcsRegistry;
using psynder::scene::HealthComponent;
using psynder::scene::LocalTransform;
using psynder::scene::Scene;
using psynder::scene::WeaponComponent;

namespace {

// EcsRegistry::Get() is a process-global singleton and Catch2 randomizes case
// order; clear it around every case so rows never leak between tests (mirrors
// gameplay_combat.cpp's RegistryReset).
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

// An actor entity: a transform + health (faction) + weapon so pickups can grant.
Entity spawn_actor(Scene& scene, math::Vec3 pos, u32 faction, f32 hp = 100.0f,
                   f32 max_hp = 100.0f, u32 ammo = 10u) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = max_hp;
    health.current_health = hp;
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    WeaponComponent weapon{};
    weapon.ammo = ammo;
    weapon.damage = 20.0f;
    scene.registry().add<WeaponComponent>(e, scene::sanitize_weapon_component(weapon));
    return e;
}

void move_actor(Scene& scene, Entity e, math::Vec3 pos) {
    LocalTransform local{};
    local.translation = pos;
    (void)scene.set_transform(e, local);
}

// A trigger volume entity (sphere by default) at `pos`, optionally linked to a
// door target.
Entity spawn_trigger(Scene& scene, math::Vec3 pos, f32 radius, u32 faction,
                     Entity target = {}) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    TriggerVolumeComponent tv{};
    tv.radius = radius;
    tv.fire_faction = faction;
    tv.target = target;
    scene.registry().add<TriggerVolumeComponent>(e, sanitize_trigger_volume(tv));
    return e;
}

// A door entity with its OWN hitbox (so a raycast respects its open/closed
// state). closed_pos = pos; open_pos slides it aside by `slide`.
Entity spawn_door(Scene& scene, math::Vec3 pos, math::Vec3 slide, f32 move_time,
                  bool instant = false) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    // The door blocks via an AABB hitbox centred on it.
    HitboxComponent hb{};
    hb.radius = 0.0f;  // AABB
    hb.half_extent = {1.0f, 1.0f, 1.0f};
    hb.enabled = 1u;
    scene.registry().add<HitboxComponent>(e, sanitize_hitbox(hb));
    DoorComponent d{};
    d.closed_pos = pos;
    d.open_pos = math::add(pos, slide);
    d.move_time = move_time;
    d.instant = instant ? 1u : 0u;
    d.hitbox_entity = e;
    scene.registry().add<DoorComponent>(e, sanitize_door(d));
    return e;
}

Entity spawn_pickup(Scene& scene, math::Vec3 pos, PickupKind kind, f32 amount,
                    f32 radius, u32 faction = 0u) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    PickupComponent pk{};
    pk.kind = kind;
    pk.amount = amount;
    pk.radius = radius;
    pk.pickup_faction = faction;
    scene.registry().add<PickupComponent>(e, sanitize_pickup(pk));
    return e;
}

}  // namespace

// ─── Trigger: edge-fires once on entry, not on stay ───────────────────────────
TEST_CASE("gameplay: trigger edge-fires once on entry not on stay",
          "[gameplay][trigger]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity actor = spawn_actor(scene, {100, 0, 0}, /*faction*/ 1u);  // far away
    const Entity trig = spawn_trigger(scene, {0, 0, 0}, /*radius*/ 2.0f, /*faction*/ 1u);

    const std::array<Actor, 1> actors{{{actor, 1u}}};
    std::array<TriggerEvent, 4> events{};
    u32 evt = 0u;

    // Frame 0: actor outside => no fire.
    u32 fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 0u);
    REQUIRE(evt == 0u);

    // Frame 1: actor steps inside => exactly one fire.
    move_actor(scene, actor, {0.5f, 0, 0});
    fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 1u);
    REQUIRE(evt == 1u);
    REQUIRE(events[0].trigger == trig);
    REQUIRE(events[0].actor == actor);
    REQUIRE(events[0].fire_count == 1u);

    // Frame 2: actor STAYS inside => no re-fire (edge, not level).
    fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 0u);
    REQUIRE(evt == 0u);
    {
        const auto* tv = scene.registry().get<TriggerVolumeComponent>(trig);
        REQUIRE(tv != nullptr);
        REQUIRE(tv->inside == 1u);
        REQUIRE(tv->fired == 0u);
        REQUIRE(tv->fire_count == 1u);
    }

    // Frame 3: actor leaves => latch clears (re-arm), still no fire.
    move_actor(scene, actor, {100, 0, 0});
    fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 0u);
    REQUIRE(scene.registry().get<TriggerVolumeComponent>(trig)->inside == 0u);

    // Frame 4: actor re-enters => fires AGAIN (count now 2).
    move_actor(scene, actor, {0.0f, 0, 0});
    fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 1u);
    REQUIRE(scene.registry().get<TriggerVolumeComponent>(trig)->fire_count == 2u);
}

TEST_CASE("gameplay: trigger faction filter rejects the wrong faction",
          "[gameplay][trigger][faction]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity player = spawn_actor(scene, {0, 0, 0}, /*faction*/ 1u);
    const Entity enemy = spawn_actor(scene, {0.2f, 0, 0}, /*faction*/ 2u);
    // Trigger only fires for faction 2.
    (void)spawn_trigger(scene, {0, 0, 0}, 2.0f, /*faction*/ 2u);

    std::array<Actor, 2> actors{{{player, 1u}, {enemy, 2u}}};
    std::array<TriggerEvent, 4> events{};
    u32 evt = 0u;
    const u32 fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 1u);
    REQUIRE(events[0].actor == enemy);  // the player (faction 1) did not fire it
}

// ─── Door: blocks a raycast closed, passes open; animates over move_time ───────
TEST_CASE("gameplay: door blocks a raycast closed and passes when open",
          "[gameplay][door]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    // Door slab at x=5; slide it +20 in Y to fully clear the ray when open.
    const Entity door = spawn_door(scene, {5, 0, 0}, {0, 20, 0}, /*move_time*/ 1.0f);

    // A ray down +X must hit the closed door's hitbox at ~t=4 (near face).
    HitResult blocked = raycast_hitboxes(scene.registry(), {0, 0, 0}, {1, 0, 0},
                                         /*max_dist*/ 100.0f, Entity{});
    REQUIRE(blocked.hit);
    REQUIRE(blocked.entity == door);

    // Open it instantly via a direct request, then tick the door once.
    REQUIRE(door_open(scene.registry(), door));
    // move_time 1s; 0.5s gets us halfway (still partly blocking until fully open).
    tick_doors(scene.registry(), 0.5f);
    {
        const auto* d = scene.registry().get<DoorComponent>(door);
        REQUIRE(d != nullptr);
        REQUIRE(d->state == DoorState::Opening);
        REQUIRE(d->t == Catch::Approx(0.5f));
        // Position lerped halfway up.
        REQUIRE(d->position.y == Catch::Approx(10.0f));
        // Still blocking until FULLY open.
        REQUIRE(scene.registry().get<HitboxComponent>(door)->enabled == 1u);
    }

    // Finish opening.
    tick_doors(scene.registry(), 0.6f);  // pushes t past 1 => Open
    {
        const auto* d = scene.registry().get<DoorComponent>(door);
        REQUIRE(d->state == DoorState::Open);
        REQUIRE(d->t == Catch::Approx(1.0f));
        REQUIRE(door_is_open(scene.registry(), door));
        // Now passable: hitbox disabled.
        REQUIRE(scene.registry().get<HitboxComponent>(door)->enabled == 0u);
    }

    // Ray now passes (door hitbox disabled => no hit).
    HitResult open_ray = raycast_hitboxes(scene.registry(), {0, 0, 0}, {1, 0, 0},
                                          100.0f, Entity{});
    REQUIRE_FALSE(open_ray.hit);
}

TEST_CASE("gameplay: instant door snaps open without animation",
          "[gameplay][door]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    const Entity door = spawn_door(scene, {5, 0, 0}, {0, 20, 0}, 1.0f, /*instant*/ true);

    REQUIRE(door_open(scene.registry(), door));
    tick_doors(scene.registry(), 1.0f / 60.0f);  // one tiny tick
    const auto* d = scene.registry().get<DoorComponent>(door);
    REQUIRE(d->state == DoorState::Open);
    REQUIRE(d->t == Catch::Approx(1.0f));
    REQUIRE(scene.registry().get<HitboxComponent>(door)->enabled == 0u);
}

TEST_CASE("gameplay: trigger opens its linked door", "[gameplay][trigger][door]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity door = spawn_door(scene, {5, 0, 0}, {0, 20, 0}, /*move_time*/ 1.0f,
                                   /*instant*/ true);
    const Entity actor = spawn_actor(scene, {100, 0, 0}, 1u);
    const Entity trig = spawn_trigger(scene, {0, 0, 0}, 2.0f, /*faction*/ 1u, door);
    (void)trig;

    const std::array<Actor, 1> actors{{{actor, 1u}}};
    std::array<TriggerEvent, 2> events{};
    u32 evt = 0u;

    // Door starts closed + blocking.
    REQUIRE(scene.registry().get<HitboxComponent>(door)->enabled == 1u);

    // Actor enters the trigger => the door's open_request is set this tick.
    move_actor(scene, actor, {0, 0, 0});
    const u32 fired = tick_triggers(scene.registry(), actors, events, &evt);
    REQUIRE(fired == 1u);
    REQUIRE(scene.registry().get<DoorComponent>(door)->open_request == 1u);

    // tick_doors consumes the request (instant => snaps open + passable).
    tick_doors(scene.registry(), 1.0f / 60.0f);
    REQUIRE(door_is_open(scene.registry(), door));
    REQUIRE(scene.registry().get<HitboxComponent>(door)->enabled == 0u);
}

// ─── Pickup: grants once, despawns, no double-grant ───────────────────────────
TEST_CASE("gameplay: health pickup grants once, despawns, no double-grant",
          "[gameplay][pickup]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    // Actor at 50/100 HP overlapping a +25 health pickup.
    const Entity actor = spawn_actor(scene, {0, 0, 0}, /*faction*/ 1u, /*hp*/ 50.0f,
                                     /*max*/ 100.0f);
    const Entity pickup =
        spawn_pickup(scene, {0.3f, 0, 0}, PickupKind::Health, /*amount*/ 25.0f,
                     /*radius*/ 1.0f);

    const std::array<Actor, 1> actors{{{actor, 1u}}};
    std::array<Entity, 4> despawn{};
    std::array<PickupEvent, 4> events{};
    u32 desp = 0u;
    u32 evt = 0u;

    u32 grants = tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt);
    REQUIRE(grants == 1u);
    REQUIRE(evt == 1u);
    REQUIRE(events[0].kind == PickupKind::Health);
    REQUIRE(events[0].before == Catch::Approx(50.0f));
    REQUIRE(events[0].after == Catch::Approx(75.0f));
    REQUIRE(scene.registry().get<HealthComponent>(actor)->current_health ==
            Catch::Approx(75.0f));
    // Marked consumed + queued for despawn.
    REQUIRE(scene.registry().get<PickupComponent>(pickup)->consumed == 1u);
    REQUIRE(desp == 1u);
    REQUIRE(despawn[0] == pickup);

    // Host despawns it (as the showcase / smoke does).
    REQUIRE(scene.despawn_entity(pickup));

    // Re-running grants nothing (the pickup is gone) => HP unchanged.
    grants = tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt);
    REQUIRE(grants == 0u);
    REQUIRE(scene.registry().get<HealthComponent>(actor)->current_health ==
            Catch::Approx(75.0f));
}

TEST_CASE("gameplay: consumed pickup never double-grants before despawn",
          "[gameplay][pickup]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    const Entity actor = spawn_actor(scene, {0, 0, 0}, 1u, /*hp*/ 10.0f, /*max*/ 100.0f);
    (void)spawn_pickup(scene, {0, 0, 0}, PickupKind::Health, 30.0f, 1.0f);

    const std::array<Actor, 1> actors{{{actor, 1u}}};
    std::array<Entity, 4> despawn{};
    std::array<PickupEvent, 4> events{};
    u32 desp = 0u, evt = 0u;

    REQUIRE(tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt) == 1u);
    REQUIRE(scene.registry().get<HealthComponent>(actor)->current_health ==
            Catch::Approx(40.0f));
    // Tick AGAIN without despawning: the consumed latch blocks a second grant.
    REQUIRE(tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt) == 0u);
    REQUIRE(scene.registry().get<HealthComponent>(actor)->current_health ==
            Catch::Approx(40.0f));
}

TEST_CASE("gameplay: ammo + weapon pickups mutate the weapon", "[gameplay][pickup]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    const Entity actor = spawn_actor(scene, {0, 0, 0}, 1u, 100.0f, 100.0f, /*ammo*/ 5u);

    const std::array<Actor, 1> actors{{{actor, 1u}}};
    std::array<Entity, 4> despawn{};
    std::array<PickupEvent, 4> events{};
    u32 desp = 0u, evt = 0u;

    SECTION("ammo pickup adds rounds") {
        (void)spawn_pickup(scene, {0, 0, 0}, PickupKind::Ammo, /*amount*/ 30.0f, 1.0f);
        REQUIRE(tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt) == 1u);
        REQUIRE(scene.registry().get<WeaponComponent>(actor)->ammo == 35u);
    }
    SECTION("weapon pickup overwrites the stat block") {
        Entity wp = spawn_pickup(scene, {0, 0, 0}, PickupKind::Weapon, /*damage*/ 75.0f, 1.0f);
        auto* pk = scene.registry().get<PickupComponent>(wp);
        pk->weapon_ammo = 99u;
        pk->weapon_range = 40.0f;
        pk->weapon_fire_rate = 10.0f;
        REQUIRE(tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt) == 1u);
        const auto* w = scene.registry().get<WeaponComponent>(actor);
        REQUIRE(w->damage == Catch::Approx(75.0f));
        REQUIRE(w->ammo == 99u);
        REQUIRE(w->range == Catch::Approx(40.0f));
    }
}

TEST_CASE("gameplay: pickup faction filter rejects the wrong actor",
          "[gameplay][pickup][faction]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    const Entity enemy = spawn_actor(scene, {0, 0, 0}, /*faction*/ 2u, 50.0f, 100.0f);
    // Health pickup only grabbable by faction 1 (player).
    (void)spawn_pickup(scene, {0, 0, 0}, PickupKind::Health, 25.0f, 1.0f, /*faction*/ 1u);

    const std::array<Actor, 1> actors{{{enemy, 2u}}};
    std::array<Entity, 4> despawn{};
    std::array<PickupEvent, 4> events{};
    u32 desp = 0u, evt = 0u;
    REQUIRE(tick_pickups(scene.registry(), actors, despawn, &desp, events, &evt) == 0u);
    REQUIRE(scene.registry().get<HealthComponent>(enemy)->current_health ==
            Catch::Approx(50.0f));
}

// ─── Determinism ──────────────────────────────────────────────────────────────
// Run the same scripted sequence (enter trigger -> open door -> grab pickup)
// twice on independent fresh worlds; every observable (door t/state/position,
// HP, fire/grant counts) must match bit-for-bit.
namespace {
struct SimResult {
    f32 door_t = 0.0f;
    u32 door_state = 0u;
    f32 door_y = 0.0f;
    f32 actor_hp = 0.0f;
    u32 trigger_fires = 0u;
    u32 grants = 0u;
    u32 hitbox_enabled = 1u;
};

SimResult run_sim() {
    Scene scene{EcsRegistry::Get()};
    const Entity door = spawn_door(scene, {5, 0, 0}, {0, 20, 0}, /*move_time*/ 0.5f);
    const Entity actor = spawn_actor(scene, {100, 0, 0}, 1u, /*hp*/ 40.0f, /*max*/ 100.0f);
    const Entity trig = spawn_trigger(scene, {0, 0, 0}, 2.0f, 1u, door);
    (void)trig;
    const Entity pickup = spawn_pickup(scene, {0, 0, 0}, PickupKind::Health, 30.0f, 1.0f);
    (void)pickup;

    std::array<Actor, 1> actors{{{actor, 1u}}};
    std::array<TriggerEvent, 4> tevents{};
    std::array<PickupEvent, 4> pevents{};
    std::array<Entity, 4> despawn{};
    u32 tev = 0u, pev = 0u, desp = 0u;

    SimResult r{};
    constexpr f32 dt = 1.0f / 60.0f;
    for (u32 frame = 0; frame < 60u; ++frame) {
        // Actor walks to the origin at frame 10 and stays.
        if (frame == 10u)
            move_actor(scene, actor, {0, 0, 0});
        r.trigger_fires += tick_triggers(scene.registry(), actors, tevents, &tev);
        tick_doors(scene.registry(), dt);
        const u32 g = tick_pickups(scene.registry(), actors, despawn, &desp, pevents, &pev);
        r.grants += g;
        for (u32 i = 0; i < desp; ++i)
            (void)scene.despawn_entity(despawn[i]);
    }

    const auto* d = scene.registry().get<DoorComponent>(door);
    r.door_t = d->t;
    r.door_state = static_cast<u32>(d->state);
    r.door_y = d->position.y;
    r.actor_hp = scene.registry().get<HealthComponent>(actor)->current_health;
    r.hitbox_enabled = scene.registry().get<HitboxComponent>(door)->enabled;
    return r;
}
}  // namespace

TEST_CASE("gameplay: door/trigger/pickup tick is deterministic",
          "[gameplay][determinism]") {
    SimResult a;
    SimResult b;
    {
        RegistryReset reset;
        a = run_sim();
    }
    {
        RegistryReset reset;
        b = run_sim();
    }
    REQUIRE(a.door_t == b.door_t);
    REQUIRE(a.door_state == b.door_state);
    REQUIRE(a.door_y == b.door_y);
    REQUIRE(a.actor_hp == b.actor_hp);
    REQUIRE(a.trigger_fires == b.trigger_fires);
    REQUIRE(a.grants == b.grants);
    REQUIRE(a.hitbox_enabled == b.hitbox_enabled);

    // Sanity: the scripted run actually exercised everything.
    REQUIRE(a.trigger_fires == 1u);
    REQUIRE(a.grants == 1u);
    REQUIRE(a.actor_hp == Catch::Approx(70.0f));  // 40 + 30 heal
    REQUIRE(a.door_state == static_cast<u32>(DoorState::Open));
    REQUIRE(a.hitbox_enabled == 0u);  // door ended fully open => passable
}
