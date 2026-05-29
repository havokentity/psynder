// SPDX-License-Identifier: MIT
// Psynder — M-COMBAT gameplay-combat unit tests. Exercises the DOTS combat
// systems over the scene ECS:
//   * hitscan kills an enemy in line of sight, misses behind cover / out of
//     range;
//   * friendly fire respects faction (off + on policies);
//   * a projectile travels, hits, applies damage, and despawns;
//   * health clamps at 0 and a death is flagged + resolved;
//   * multi-entity (multi-chunk) combat applies damage with no races
//     (mutex-guarded accumulation), no per-frame heap growth in the steady
//     state.
// Also direct-tests the ray-vs-sphere / ray-vs-AABB primitives.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "gameplay/CombatSystems.h"
#include "gameplay/GameplayComponents.h"
#include "gameplay/Raycast.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <span>
#include <vector>

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
// scene_query.cpp's RegistryReset).
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

// Spawn a scene entity at `pos` with a sphere hitbox + health in `faction`.
Entity spawn_target(Scene& scene, math::Vec3 pos, u32 faction, f32 hp, f32 radius = 0.5f) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = hp;
    health.current_health = hp;
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    HitboxComponent hb{};
    hb.radius = radius;
    scene.registry().add<HitboxComponent>(e, sanitize_hitbox(hb));
    return e;
}

// Spawn a hitscan shooter in `faction` with a weapon of the given range/damage.
Entity spawn_shooter(Scene& scene,
                     math::Vec3 pos,
                     u32 faction,
                     f32 damage,
                     f32 range,
                     WeaponKind kind = WeaponKind::Hitscan,
                     u32 ammo = 100u) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    WeaponComponent weapon{};
    weapon.damage = damage;
    weapon.range = range;
    weapon.fire_rate = 0.0f;  // 0 => no cooldown gate for single-shot tests
    weapon.ammo = ammo;
    scene.registry().add<WeaponComponent>(e, weapon);
    WeaponRuntimeComponent rt{};
    rt.kind = kind;
    rt.projectile_speed = 20.0f;
    rt.projectile_life = 5.0f;
    scene.registry().add<WeaponRuntimeComponent>(e, sanitize_weapon_runtime(rt));
    return e;
}

}  // namespace

// ─── Ray primitives ──────────────────────────────────────────────────────────
TEST_CASE("gameplay: ray-sphere hits front, misses behind, enters when inside",
          "[gameplay][raycast]") {
    // Ray down +X from origin. Sphere at x=5 r=1 => entry at t=4.
    RayHit h = ray_sphere({0, 0, 0}, {1, 0, 0}, {5, 0, 0}, 1.0f, 100.0f);
    REQUIRE(h.hit);
    REQUIRE(h.t == Catch::Approx(4.0f));

    // Sphere behind the origin => miss.
    RayHit behind = ray_sphere({0, 0, 0}, {1, 0, 0}, {-5, 0, 0}, 1.0f, 100.0f);
    REQUIRE_FALSE(behind.hit);

    // Origin inside the sphere => immediate hit at t=0.
    RayHit inside = ray_sphere({5, 0, 0}, {1, 0, 0}, {5, 0, 0}, 1.0f, 100.0f);
    REQUIRE(inside.hit);
    REQUIRE(inside.t == Catch::Approx(0.0f));

    // Beyond max_t => no hit in window.
    RayHit far = ray_sphere({0, 0, 0}, {1, 0, 0}, {5, 0, 0}, 1.0f, 3.0f);
    REQUIRE_FALSE(far.hit);
}

TEST_CASE("gameplay: ray-aabb slab test hits and misses", "[gameplay][raycast]") {
    // Box centered at x=5, half 1 => near face at t=4.
    RayHit h = ray_aabb({0, 0, 0}, {1, 0, 0}, {5, 0, 0}, {1, 1, 1}, 100.0f);
    REQUIRE(h.hit);
    REQUIRE(h.t == Catch::Approx(4.0f));

    // Ray parallel to a slab and outside it => miss.
    RayHit miss = ray_aabb({0, 5, 0}, {1, 0, 0}, {5, 0, 0}, {1, 1, 1}, 100.0f);
    REQUIRE_FALSE(miss.hit);
}

// ─── Hitscan: LOS / cover / range ─────────────────────────────────────────────
TEST_CASE("gameplay: hitscan kills an enemy in line of sight", "[gameplay][combat]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};  // friendly fire OFF by default

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, /*faction*/ 1u, /*dmg*/ 40.0f,
                                         /*range*/ 100.0f);
    const Entity enemy = spawn_target(scene, {10, 0, 0}, /*faction*/ 2u, /*hp*/ 100.0f);

    CombatContext ctx;
    ctx.begin_tick();
    HitResult hit = fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx,
                                config);
    REQUIRE(hit.hit);
    REQUIRE(hit.entity == enemy);

    flush_damage_events(scene.registry(), ctx, config);
    const auto* hp = scene.registry().get<HealthComponent>(enemy);
    REQUIRE(hp != nullptr);
    REQUIRE(hp->current_health == Catch::Approx(60.0f));
}

TEST_CASE("gameplay: hitscan blocked by nearer cover spares the enemy",
          "[gameplay][combat]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};
    config.friendly_fire = FriendlyFire::On;  // so the wall (any faction) blocks

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, 1u, 40.0f, 100.0f);
    // Cover wall between shooter and enemy (closer along +X).
    const Entity wall = spawn_target(scene, {4, 0, 0}, /*faction*/ 3u, /*hp*/ 100.0f);
    const Entity enemy = spawn_target(scene, {10, 0, 0}, 2u, 100.0f);

    CombatContext ctx;
    ctx.begin_tick();
    HitResult hit = fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx,
                                config);
    REQUIRE(hit.hit);
    REQUIRE(hit.entity == wall);  // nearest hit is the cover, not the enemy
    REQUIRE(hit.entity != enemy);

    flush_damage_events(scene.registry(), ctx, config);
    REQUIRE(scene.registry().get<HealthComponent>(enemy)->current_health ==
            Catch::Approx(100.0f));
    REQUIRE(scene.registry().get<HealthComponent>(wall)->current_health ==
            Catch::Approx(60.0f));
}

TEST_CASE("gameplay: hitscan out of range does not hit", "[gameplay][combat]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, 1u, 40.0f, /*range*/ 5.0f);
    const Entity enemy = spawn_target(scene, {10, 0, 0}, 2u, 100.0f);

    CombatContext ctx;
    ctx.begin_tick();
    HitResult hit = fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx,
                                config);
    REQUIRE_FALSE(hit.hit);
    flush_damage_events(scene.registry(), ctx, config);
    REQUIRE(scene.registry().get<HealthComponent>(enemy)->current_health ==
            Catch::Approx(100.0f));
}

// ─── Friendly fire ────────────────────────────────────────────────────────────
TEST_CASE("gameplay: friendly fire off spares same-faction targets",
          "[gameplay][combat][faction]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};
    config.friendly_fire = FriendlyFire::Off;

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, /*faction*/ 1u, 40.0f, 100.0f);
    const Entity ally = spawn_target(scene, {10, 0, 0}, /*faction*/ 1u, 100.0f);

    CombatContext ctx;
    ctx.begin_tick();
    HitResult hit = fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx,
                                config);
    // The ray geometrically hits the ally, but friendly fire rejects damage =>
    // fire_weapon reports a miss and queues nothing.
    REQUIRE_FALSE(hit.hit);
    flush_damage_events(scene.registry(), ctx, config);
    REQUIRE(scene.registry().get<HealthComponent>(ally)->current_health ==
            Catch::Approx(100.0f));
}

TEST_CASE("gameplay: friendly fire on damages same-faction targets",
          "[gameplay][combat][faction]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};
    config.friendly_fire = FriendlyFire::On;

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, 1u, 40.0f, 100.0f);
    const Entity ally = spawn_target(scene, {10, 0, 0}, 1u, 100.0f);

    CombatContext ctx;
    ctx.begin_tick();
    HitResult hit = fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx,
                                config);
    REQUIRE(hit.hit);
    REQUIRE(hit.entity == ally);
    flush_damage_events(scene.registry(), ctx, config);
    REQUIRE(scene.registry().get<HealthComponent>(ally)->current_health ==
            Catch::Approx(60.0f));
}

// ─── Health clamp + death ─────────────────────────────────────────────────────
TEST_CASE("gameplay: damage clamps health at zero and flags death",
          "[gameplay][combat][death]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const Entity enemy = spawn_target(scene, {0, 0, 0}, /*faction*/ 2u, /*hp*/ 30.0f);

    // Overkill: 100 dmg vs 30 hp clamps to 0, not negative.
    REQUIRE(apply_damage(scene.registry(), enemy, 100.0f, /*src faction*/ 1u, config));
    const auto* hp = scene.registry().get<HealthComponent>(enemy);
    REQUIRE(hp->current_health == Catch::Approx(0.0f));
    REQUIRE(scene.registry().get<DeadComponent>(enemy) != nullptr);

    // A second hit on an already-dead entity applies nothing.
    REQUIRE_FALSE(apply_damage(scene.registry(), enemy, 10.0f, 1u, config));

    // resolve_deaths despawns the corpse.
    const u32 resolved = resolve_deaths(scene, /*despawn*/ true);
    REQUIRE(resolved == 1u);
    REQUIRE_FALSE(scene.registry().alive(enemy));
}

TEST_CASE("gameplay: resolve_deaths without despawn flags resolved + fires callback",
          "[gameplay][combat][death]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const Entity enemy = spawn_target(scene, {0, 0, 0}, 2u, 10.0f);
    REQUIRE(apply_damage(scene.registry(), enemy, 25.0f, /*killer*/ 7u, config));

    struct Captured {
        Entity entity{};
        u32 killer = 0u;
        int count = 0;
    } captured;
    auto cb = [](void* user, const DeathInfo& info) {
        auto* c = static_cast<Captured*>(user);
        c->entity = info.entity;
        c->killer = info.killer_faction;
        ++c->count;
    };

    const u32 resolved = resolve_deaths(scene, /*despawn*/ false, cb, &captured);
    REQUIRE(resolved == 1u);
    REQUIRE(captured.count == 1);
    REQUIRE(captured.entity == enemy);
    REQUIRE(captured.killer == 7u);
    REQUIRE(scene.registry().alive(enemy));
    REQUIRE(scene.registry().get<DeadComponent>(enemy)->resolved == 1u);

    // Idempotent: a second resolve does nothing (already resolved).
    REQUIRE(resolve_deaths(scene, false, cb, &captured) == 0u);
    REQUIRE(captured.count == 1);
}

// ─── Projectiles ──────────────────────────────────────────────────────────────
TEST_CASE("gameplay: a projectile travels, hits, applies damage, and despawns",
          "[gameplay][combat][projectile]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, /*faction*/ 1u, /*dmg*/ 25.0f,
                                         /*range*/ 100.0f, WeaponKind::Projectile);
    const Entity enemy = spawn_target(scene, {10, 0, 0}, /*faction*/ 2u, /*hp*/ 100.0f);

    CombatContext ctx;
    ctx.begin_tick();
    Entity projectile{};
    HitResult hit = fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx,
                                config, &projectile);
    REQUIRE_FALSE(hit.hit);          // projectile resolves later, not instantly
    REQUIRE(projectile.valid());
    REQUIRE(scene.registry().get<ProjectileComponent>(projectile) != nullptr);

    CombatSystems systems;
    systems.config = config;

    // speed 20 m/s, enemy at x=10 (r=0.5). After ~1s the projectile reaches it.
    // Step a few frames; the swept-segment test catches the crossing.
    bool dead = false;
    for (int frame = 0; frame < 30 && !dead; ++frame) {
        systems.ctx.begin_tick();
        tick_projectiles(scene.registry(), /*dt*/ 0.1f, systems.ctx, config);
        flush_damage_events(scene.registry(), systems.ctx, config);
        cleanup_projectiles(scene, systems.ctx);
        const auto* hp = scene.registry().get<HealthComponent>(enemy);
        if (hp && hp->current_health < 100.0f)
            dead = true;
    }
    REQUIRE(dead);
    REQUIRE(scene.registry().get<HealthComponent>(enemy)->current_health ==
            Catch::Approx(75.0f));
    // Projectile despawned on impact.
    REQUIRE_FALSE(scene.registry().alive(projectile));
}

TEST_CASE("gameplay: a projectile expires by lifetime and despawns without a hit",
          "[gameplay][combat][projectile]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    // Short life, no target in the path.
    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, 1u, 25.0f, 100.0f,
                                         WeaponKind::Projectile);
    auto* rt = scene.registry().get<WeaponRuntimeComponent>(shooter);
    rt->projectile_life = 0.25f;
    rt->projectile_speed = 5.0f;

    CombatContext ctx;
    ctx.begin_tick();
    Entity projectile{};
    fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {0, 1, 0}, &ctx, config,
                &projectile);
    REQUIRE(projectile.valid());

    for (int frame = 0; frame < 10; ++frame) {
        ctx.begin_tick();
        tick_projectiles(scene.registry(), 0.1f, ctx, config);
        flush_damage_events(scene.registry(), ctx, config);
        cleanup_projectiles(scene, ctx);
    }
    REQUIRE_FALSE(scene.registry().alive(projectile));
}

// ─── Cooldown + ammo gating ───────────────────────────────────────────────────
TEST_CASE("gameplay: fire is gated by cooldown and ammo", "[gameplay][combat][weapon]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    const Entity shooter = spawn_shooter(scene, {0, 0, 0}, 1u, 10.0f, 100.0f,
                                         WeaponKind::Hitscan, /*ammo*/ 2u);
    auto* weapon = scene.registry().get<WeaponComponent>(shooter);
    weapon->fire_rate = 2.0f;  // 0.5s cooldown
    const Entity enemy = spawn_target(scene, {10, 0, 0}, 2u, 1000.0f);

    CombatContext ctx;

    // Shot 1 lands.
    ctx.begin_tick();
    REQUIRE(fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx, config).hit);
    flush_damage_events(scene.registry(), ctx, config);

    // Shot 2 immediately => blocked by cooldown.
    ctx.begin_tick();
    REQUIRE_FALSE(fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx, config).hit);

    // Advance past the cooldown, shot 2 lands and consumes the last round.
    tick_weapon_cooldowns(scene.registry(), 0.6f);
    ctx.begin_tick();
    REQUIRE(fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx, config).hit);
    flush_damage_events(scene.registry(), ctx, config);

    // Ammo now 0 => no more fire even after cooldown.
    tick_weapon_cooldowns(scene.registry(), 0.6f);
    ctx.begin_tick();
    REQUIRE_FALSE(fire_weapon(scene.registry(), scene, shooter, {0, 0, 0}, {1, 0, 0}, &ctx, config).hit);

    REQUIRE(scene.registry().get<WeaponComponent>(shooter)->ammo == 0u);
    REQUIRE(scene.registry().get<HealthComponent>(enemy)->current_health ==
            Catch::Approx(980.0f));  // two 10-dmg hits
}

// ─── Multi-entity / multi-chunk: no races ─────────────────────────────────────
TEST_CASE("gameplay: many projectiles hit many enemies across chunks with no races",
          "[gameplay][combat][parallel]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};
    CombatConfig config{};

    // 400 enemies spread along +Z lanes, each with a paired projectile aimed at
    // it. 400 rows of (Transform, SceneNode, Projectile) spans multiple chunks,
    // so tick_projectiles fires its body on several worker threads at once; the
    // mutex-guarded ctx.pending / ctx.despawn accumulation must stay race-free.
    constexpr int kCount = 400;
    std::vector<Entity> enemies;
    enemies.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        const f32 z = static_cast<f32>(i) * 4.0f;
        // Enemy at x=5 in lane z, faction 2.
        const Entity enemy = spawn_target(scene, {5.0f, 0.0f, z}, /*faction*/ 2u, /*hp*/ 50.0f);
        enemies.push_back(enemy);

        // Projectile starting at x=0 lane z, faction 1, heading +X at 100 m/s so
        // a single 0.1s step (10 m) crosses the enemy at x=5.
        LocalTransform local{};
        local.translation = math::Vec3{0.0f, 0.0f, z};
        const Entity proj = scene.create_entity(local);
        ProjectileComponent pc{};
        pc.velocity = math::Vec3{100.0f, 0.0f, 0.0f};
        pc.damage = 50.0f;
        pc.faction = 1u;
        pc.life = 5.0f;
        pc.source = Entity{};
        pc.alive = 1u;
        scene.registry().add<ProjectileComponent>(proj, pc);
        HitboxComponent hb{};
        hb.radius = 0.1f;
        scene.registry().add<HitboxComponent>(proj, sanitize_hitbox(hb));
    }

    CombatSystems systems;
    systems.config = config;
    systems.reserve(/*damage*/ kCount * 2, /*deaths*/ kCount, /*despawn*/ kCount);

    const usize damage_cap_before = systems.ctx.pending.capacity();
    const usize despawn_cap_before = systems.ctx.despawn.capacity();

    // One step crosses every enemy. update() runs cooldowns -> projectiles ->
    // flush -> cleanup -> deaths.
    const u32 deaths = systems.update(scene, /*dt*/ 0.1f, /*despawn_dead*/ true);

    REQUIRE(deaths == static_cast<u32>(kCount));
    for (Entity enemy : enemies)
        REQUIRE_FALSE(scene.registry().alive(enemy));  // all killed + despawned

    // Steady-state alloc-free guarantee: reserved scratch never had to grow.
    REQUIRE(systems.ctx.pending.capacity() == damage_cap_before);
    REQUIRE(systems.ctx.despawn.capacity() == despawn_cap_before);
}
