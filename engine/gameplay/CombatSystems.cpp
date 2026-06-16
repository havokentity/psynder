// SPDX-License-Identifier: MIT
// Psynder — M-COMBAT combat systems implementation.

#include "gameplay/CombatSystems.h"

#include "scene/EcsRegistry.h"
#include "scene/SceneGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace psynder::gameplay {

using namespace ::psynder::scene;

u32 entity_faction(EcsRegistry& registry, Entity e) {
    if (const auto* health = registry.get<HealthComponent>(e))
        return health->faction;
    if (const auto* faction = registry.get<FactionComponent>(e))
        return faction->faction;
    return 0u;
}

bool entity_position(EcsRegistry& registry, Entity e, math::Vec3& out) {
    if (const auto* transform = registry.get<TransformComponent>(e)) {
        out = transform->local.translation;
        return true;
    }
    return false;
}

bool apply_damage(EcsRegistry& registry,
                  Entity target,
                  f32 amount,
                  u32 source_faction,
                  const CombatConfig& config) {
    if (!registry.alive(target) || amount <= 0.0f)
        return false;
    auto* health = registry.get<HealthComponent>(target);
    if (!health)
        return false;
    if (!config.can_damage(source_faction, health->faction))
        return false;
    if (health->current_health <= 0.0f)
        return false;  // already dead; don't re-flag

    health->current_health -= amount;
    if (health->current_health < 0.0f)
        health->current_health = 0.0f;

    if (health->current_health <= 0.0f) {
        DeadComponent dead{};
        dead.pending = 1u;
        dead.resolved = 0u;
        dead.killer_faction = source_faction;
        registry.add<DeadComponent>(target, dead);
    }
    return true;
}

void flush_damage_events(EcsRegistry& registry,
                         CombatContext& ctx,
                         const CombatConfig& config) {
    // Serial drain of the queued events. No locking needed — the parallel sweep
    // that filled ctx.pending has returned by the time we get here. Record any
    // freshly-killed entity into ctx.deaths for resolve_deaths.
    for (const DamageEvent& ev : ctx.pending) {
        if (apply_damage(registry, ev.target, ev.amount, ev.source_faction, config)) {
            if (auto* health = registry.get<HealthComponent>(ev.target);
                health && health->current_health <= 0.0f) {
                ctx.deaths.push_back(ev.target);
            }
        }
    }
    ctx.pending.clear();
}

HitResult raycast_hitboxes(EcsRegistry& registry,
                           math::Vec3 origin,
                           math::Vec3 dir,
                           f32 max_dist,
                           Entity ignore) {
    // Normalize direction so the ray parameter t is a true world distance.
    const f32 dir_len = math::length(dir);
    if (!(dir_len > 0.0f) || !(max_dist > 0.0f))
        return {};
    const math::Vec3 ndir = math::mul(dir, 1.0f / dir_len);

    // Nearest-hit reduction. The query body fires once per chunk across worker
    // threads, so the shared `best` is serialized with a mutex. The per-chunk
    // work (a handful of ray tests) is cheap; we still keep the body short.
    std::mutex best_mu;
    HitResult best{};
    best.distance = max_dist;

    registry.query<reads<SceneNodeComponent, HitboxComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const HitboxComponent> boxes) {
            const usize n = std::min(nodes.size(), boxes.size());
            // Per-chunk local best, merged once at the end => one lock per chunk.
            HitResult local{};
            local.distance = max_dist;
            for (usize i = 0; i < n; ++i) {
                const HitboxComponent& hb = boxes[i];
                if (hb.enabled == 0u)
                    continue;
                const Entity e = nodes[i].entity;
                if (e == ignore || !registry.alive(e))
                    continue;
                math::Vec3 pos{};
                if (!entity_position(registry, e, pos))
                    continue;
                const math::Vec3 center = math::add(pos, hb.offset);
                RayHit rh{};
                if (hb.radius > 0.0f)
                    rh = ray_sphere(origin, ndir, center, hb.radius, local.distance);
                else
                    rh = ray_aabb(origin, ndir, center, hb.half_extent, local.distance);
                if (rh.hit && rh.t <= local.distance) {
                    local.entity = e;
                    local.distance = rh.t;
                    local.faction = entity_faction(registry, e);
                    local.hit = true;
                }
            }
            if (local.hit) {
                std::scoped_lock lk{best_mu};
                if (local.distance <= best.distance) {
                    best = local;
                }
            }
        });

    if (!best.hit)
        return {};
    return best;
}

HitResult fire_weapon(EcsRegistry& registry,
                      Scene& scene,
                      Entity shooter,
                      math::Vec3 origin,
                      math::Vec3 dir,
                      CombatContext* ctx,
                      const CombatConfig& config,
                      Entity* out_projectile) {
    if (out_projectile)
        *out_projectile = Entity{};
    if (!registry.alive(shooter))
        return {};
    auto* weapon = registry.get<WeaponComponent>(shooter);
    if (!weapon)
        return {};

    auto* runtime = registry.get<WeaponRuntimeComponent>(shooter);
    WeaponRuntimeComponent rt = runtime ? *runtime : WeaponRuntimeComponent{};

    // Gated by cooldown + ammo.
    if (rt.cooldown > 0.0f)
        return {};
    if (weapon->ammo == 0u)
        return {};

    const u32 shooter_faction = entity_faction(registry, shooter);
    const f32 dir_len = math::length(dir);
    if (!(dir_len > 0.0f))
        return {};
    const math::Vec3 ndir = math::mul(dir, 1.0f / dir_len);

    // Consume one round + arm the cooldown (fire_rate is shots/sec).
    weapon->ammo -= 1u;
    if (weapon->fire_rate > 0.0f)
        rt.cooldown = 1.0f / weapon->fire_rate;
    rt = sanitize_weapon_runtime(rt);
    if (runtime)
        *runtime = rt;
    else
        registry.add<WeaponRuntimeComponent>(shooter, rt);

    if (rt.kind == WeaponKind::Projectile) {
        // Spawn a projectile entity travelling along the aim direction.
        LocalTransform local{};
        local.translation = origin;
        const Entity proj = scene.create_entity(local);
        if (proj.valid()) {
            ProjectileComponent pc{};
            pc.velocity = math::mul(ndir, rt.projectile_speed);
            pc.damage = weapon->damage;
            pc.faction = shooter_faction;
            pc.life = rt.projectile_life;
            pc.source = shooter;
            pc.alive = 1u;
            registry.add<ProjectileComponent>(proj, pc);
            HitboxComponent hb{};
            hb.radius = 0.1f;  // small projectile collision sphere
            registry.add<HitboxComponent>(proj, sanitize_hitbox(hb));
        }
        if (out_projectile)
            *out_projectile = proj;
        return {};  // damage resolved later by tick_projectiles
    }

    // Hitscan: nearest damageable hitbox of a different faction in range.
    HitResult hit = raycast_hitboxes(registry, origin, ndir, weapon->range, shooter);
    if (hit.hit && config.can_damage(shooter_faction, hit.faction)) {
        if (ctx) {
            DamageEvent ev{};
            ev.target = hit.entity;
            ev.amount = weapon->damage;
            ev.source_faction = shooter_faction;
            std::scoped_lock lk{ctx->mutex};
            ctx->pending.push_back(ev);
        }
        return hit;
    }
    // A hit that friendly-fire rules reject is not a kill — report a miss so the
    // caller does not believe it landed damage.
    return {};
}

void tick_projectiles(EcsRegistry& registry,
                      f32 dt,
                      CombatContext& ctx,
                      const CombatConfig& config) {
    if (dt <= 0.0f)
        return;

    registry.query<reads<SceneNodeComponent>, writes<ProjectileComponent, TransformComponent>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<ProjectileComponent> projs,
            std::span<TransformComponent> transforms) {
            const usize n = std::min({nodes.size(), projs.size(), transforms.size()});
            // Per-chunk scratch merged once under the lock => one lock per chunk
            // for damage + despawn, no per-iteration locking.
            std::array<DamageEvent, 256> chunk_damage{};
            std::array<Entity, 256> chunk_despawn{};
            usize dmg_count = 0u;
            usize desp_count = 0u;

            for (usize i = 0; i < n; ++i) {
                ProjectileComponent& p = projs[i];
                if (p.alive == 0u)
                    continue;
                const math::Vec3 from = transforms[i].local.translation;
                const math::Vec3 step = math::mul(p.velocity, dt);
                const f32 seg_len = math::length(step);
                bool consumed = false;

                if (seg_len > 0.0f) {
                    const math::Vec3 sdir = math::mul(step, 1.0f / seg_len);
                    // Ignore the projectile's own collision sphere; the source
                    // is additionally rejected below so a projectile never hits
                    // the shooter even under friendly-fire-on.
                    const HitResult hit =
                        raycast_hitboxes(registry, from, sdir, seg_len, nodes[i].entity);
                    if (hit.hit && hit.entity != p.source &&
                        config.can_damage(p.faction, hit.faction)) {
                        if (dmg_count < chunk_damage.size()) {
                            DamageEvent ev{};
                            ev.target = hit.entity;
                            ev.amount = p.damage;
                            ev.source_faction = p.faction;
                            chunk_damage[dmg_count++] = ev;
                        }
                        consumed = true;
                    }
                }

                // Advance the projectile to the segment end (or stop at impact).
                transforms[i].local.translation = math::add(from, step);
                p.life -= dt;

                if (consumed || p.life <= 0.0f) {
                    p.alive = 0u;
                    if (desp_count < chunk_despawn.size())
                        chunk_despawn[desp_count++] = nodes[i].entity;
                }
            }

            if (dmg_count != 0u || desp_count != 0u) {
                std::scoped_lock lk{ctx.mutex};
                for (usize k = 0; k < dmg_count; ++k)
                    ctx.pending.push_back(chunk_damage[k]);
                for (usize k = 0; k < desp_count; ++k)
                    ctx.despawn.push_back(chunk_despawn[k]);
            }
        });
}

void cleanup_projectiles(Scene& scene, CombatContext& ctx) {
    for (Entity e : ctx.despawn)
        scene.despawn_entity(e);
    ctx.despawn.clear();
}

void tick_weapon_cooldowns(EcsRegistry& registry, f32 dt) {
    if (dt <= 0.0f)
        return;
    // Pure per-chunk write, no shared state => safe to run unguarded in
    // parallel across chunks.
    registry.query<reads<>, writes<WeaponRuntimeComponent>>(
        [dt](std::span<WeaponRuntimeComponent> weapons) {
            for (WeaponRuntimeComponent& w : weapons) {
                w.cooldown -= dt;
                if (w.cooldown < 0.0f)
                    w.cooldown = 0.0f;
            }
        });
}

u32 resolve_deaths(Scene& scene, bool despawn, DeathCallback cb, void* user) {
    EcsRegistry& registry = scene.registry();

    // Collect the pending dead first (read-only sweep), then mutate serially —
    // never destroy an entity while iterating the archetype it lives in.
    std::mutex collect_mu;
    std::vector<DeathInfo> dead;
    registry.query<reads<SceneNodeComponent, DeadComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const DeadComponent> flags) {
            const usize n = std::min(nodes.size(), flags.size());
            std::array<DeathInfo, 256> chunk{};
            usize count = 0u;
            for (usize i = 0; i < n; ++i) {
                if (flags[i].pending != 0u && flags[i].resolved == 0u) {
                    if (count < chunk.size()) {
                        chunk[count].entity = nodes[i].entity;
                        chunk[count].killer_faction = flags[i].killer_faction;
                        ++count;
                    }
                }
            }
            if (count != 0u) {
                std::scoped_lock lk{collect_mu};
                dead.insert(dead.end(), chunk.begin(), chunk.begin() + count);
            }
        });

    u32 resolved = 0u;
    for (const DeathInfo& info : dead) {
        if (!registry.alive(info.entity))
            continue;
        if (cb)
            cb(user, info);
        if (despawn) {
            scene.despawn_entity(info.entity);
        } else if (auto* flag = registry.get<DeadComponent>(info.entity)) {
            flag->resolved = 1u;
        }
        ++resolved;
    }
    return resolved;
}

u32 CombatSystems::update(Scene& scene,
                          f32 dt,
                          bool despawn_dead,
                          DeathCallback cb,
                          void* user) {
    EcsRegistry& registry = scene.registry();
    ctx.begin_tick();
    tick_weapon_cooldowns(registry, dt);
    tick_projectiles(registry, dt, ctx, config);
    flush_damage_events(registry, ctx, config);
    cleanup_projectiles(scene, ctx);
    return resolve_deaths(scene, despawn_dead, cb, user);
}

}  // namespace psynder::gameplay
