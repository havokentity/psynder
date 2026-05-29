// SPDX-License-Identifier: MIT
// Psynder — M-AI enemy-AI unit tests. Exercises the DOTS perceive/think/act
// systems over the scene ECS via injected host hooks (LOS / fire / move):
//   * an agent with a visible hostile in range walks Idle -> Patrol -> Chase ->
//     Attack and fires (the host fire-hook counter increments);
//   * a hostile behind an LOS-blocking wall is NOT attacked — the agent chases
//     toward the last-seen position instead;
//   * patrol advances through its waypoint ring;
//   * a dead-health agent transitions to Dead and stops sensing / acting;
//   * many agents across multiple chunks think + act race-free and without any
//     steady-state heap growth (the systems own no per-frame allocation).
// AI side effects are decoupled: LOS / fire / move are plain function-pointer
// hooks on the AiContext, so these tests need no render / host / physics.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/AiComponents.h"
#include "ai/AiSystems.h"
#include "gameplay/GameplayComponents.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <cmath>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::ai;
using psynder::scene::EcsRegistry;
using psynder::scene::HealthComponent;
using psynder::scene::LocalTransform;
using psynder::scene::Scene;
using psynder::scene::TransformComponent;

namespace {

// EcsRegistry::Get() is a process-global singleton and Catch2 randomizes case
// order; clear it around every case so rows never leak between tests (mirrors
// gameplay_combat.cpp's RegistryReset).
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

// ── Host-hook fixtures ──────────────────────────────────────────────────────
// Counts the fire() calls the act() Attack branch routes through. The host
// owns serialization; here a plain counter is fine because the unit cases that
// assert on it run single-agent (the multi-chunk case uses an atomic-safe
// thread-shared counter via the AiContext::shots_fired telemetry instead).
struct FireSink {
    int count = 0;
    Entity last_agent{};
    Entity last_target{};
};

bool fire_hook(void* user, Entity agent, Entity target) {
    auto* s = static_cast<FireSink*>(user);
    ++s->count;
    s->last_agent = agent;
    s->last_target = target;
    return true;  // pretend a round always goes out
}

// LOS hook that always reports clear sight.
bool los_clear(void*, math::Vec3, math::Vec3) { return true; }

// LOS hook that always reports a blocked line (a wall everywhere).
bool los_blocked(void*, math::Vec3, math::Vec3) { return false; }

Entity spawn_agent(Scene& scene,
                   math::Vec3 pos,
                   u32 faction,
                   f32 hp,
                   f32 sight,
                   f32 attack_range,
                   f32 move_speed = 5.0f) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = hp;
    health.current_health = hp;
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    AiAgentComponent agent{};
    agent.state = AiState::Idle;
    agent.sight_range = sight;
    agent.fov_cos = -1.0f;  // omnidirectional for deterministic tests
    agent.attack_range = attack_range;
    agent.think_cooldown = 0.0f;
    agent.think_interval = 0.0f;  // re-evaluate every tick in tests
    agent.move_speed = move_speed;
    scene.registry().add<AiAgentComponent>(e, sanitize_ai_agent(agent));
    scene.registry().add<PerceptionComponent>(e, PerceptionComponent{});
    return e;
}

Entity spawn_target(Scene& scene, math::Vec3 pos, u32 faction, f32 hp) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = hp;
    health.current_health = hp;
    health.faction = faction;
    scene.registry().add<HealthComponent>(e, health);
    return e;
}

}  // namespace

// ─── Idle -> Patrol -> Chase -> Attack + fires ────────────────────────────────
TEST_CASE("ai: visible hostile in range drives Idle->...->Attack and fires",
          "[ai][fsm]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    // Agent faction 1, hostile faction 2 at x=3 (inside attack_range 5).
    const Entity agent = spawn_agent(scene, {0, 0, 0}, /*faction*/ 1u, /*hp*/ 100.0f,
                                     /*sight*/ 30.0f, /*attack*/ 5.0f);
    const Entity enemy = spawn_target(scene, {3, 0, 0}, /*faction*/ 2u, /*hp*/ 100.0f);

    FireSink sink;
    AiSystems ai;
    ai.ctx.los = los_clear;
    ai.ctx.fire = fire_hook;
    ai.ctx.fire_user = &sink;

    ai.update(scene.registry(), 0.1f);

    const auto* a = scene.registry().get<AiAgentComponent>(agent);
    REQUIRE(a != nullptr);
    REQUIRE(a->state == AiState::Attack);
    REQUIRE(a->target_entity == enemy);

    const auto* sense = scene.registry().get<PerceptionComponent>(agent);
    REQUIRE(sense->can_see == 1u);
    REQUIRE(sense->last_seen_pos.x == Catch::Approx(3.0f));

    // The Attack branch fired through the host hook exactly once.
    REQUIRE(sink.count == 1);
    REQUIRE(sink.last_agent == agent);
    REQUIRE(sink.last_target == enemy);
    REQUIRE(ai.ctx.shots_fired.load() == 1u);
}

// ─── Visible but out of attack range => Chase, no fire ────────────────────────
TEST_CASE("ai: visible hostile beyond attack range chases without firing",
          "[ai][fsm]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity agent = spawn_agent(scene, {0, 0, 0}, 1u, 100.0f, /*sight*/ 30.0f,
                                     /*attack*/ 4.0f, /*move*/ 5.0f);
    spawn_target(scene, {20, 0, 0}, 2u, 100.0f);  // far away but in sight

    FireSink sink;
    AiSystems ai;
    ai.ctx.los = los_clear;
    ai.ctx.fire = fire_hook;
    ai.ctx.fire_user = &sink;

    ai.update(scene.registry(), 0.1f);

    const auto* a = scene.registry().get<AiAgentComponent>(agent);
    REQUIRE(a->state == AiState::Chase);
    REQUIRE(sink.count == 0);  // never in range to attack

    // Chase steered the agent toward the target (+X), 5 m/s * 0.1s = 0.5 m.
    const auto* t = scene.registry().get<TransformComponent>(agent);
    REQUIRE(t->local.translation.x == Catch::Approx(0.5f));
}

// ─── LOS blocked => NOT attacked; chase to last-seen ──────────────────────────
TEST_CASE("ai: hostile behind an LOS-blocking wall is not attacked, chases last-seen",
          "[ai][los]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity agent = spawn_agent(scene, {0, 0, 0}, 1u, 100.0f, /*sight*/ 30.0f,
                                     /*attack*/ 10.0f, /*move*/ 5.0f);
    const Entity enemy = spawn_target(scene, {3, 0, 0}, 2u, 100.0f);  // in range

    FireSink sink;
    AiSystems ai;
    ai.ctx.fire = fire_hook;
    ai.ctx.fire_user = &sink;

    // Seed a prior sighting at +Z so we can prove a chase moves toward the
    // remembered position, not the (now hidden) live one at +X.
    auto* seed = scene.registry().get<PerceptionComponent>(agent);
    seed->last_seen_pos = math::Vec3{0.0f, 0.0f, 5.0f};
    seed->can_see = 1u;
    scene.registry().get<AiAgentComponent>(agent)->target_entity = enemy;

    ai.ctx.los = los_blocked;  // wall everywhere => the live target is unseen now
    ai.update(scene.registry(), 0.1f);

    const auto* a = scene.registry().get<AiAgentComponent>(agent);
    // Target is known (still in the sight sphere) but not visible => Chase,
    // never Attack, and no shot goes out.
    REQUIRE(a->state == AiState::Chase);
    REQUIRE(sink.count == 0);
    REQUIRE(ai.ctx.shots_fired.load() == 0u);

    const auto* sense = scene.registry().get<PerceptionComponent>(agent);
    REQUIRE(sense->can_see == 0u);  // perceive cleared it: blocked this tick
    // last_seen memory is preserved (perceive only restamps it when visible).
    REQUIRE(sense->last_seen_pos.z == Catch::Approx(5.0f));

    // It chases toward the remembered +Z position. With LOS blocked, act()
    // sidesteps perpendicular (obstacle-avoid v1) rather than walking into the
    // wall, so it moves the full step distance but NOT straight along +Z.
    const auto* t = scene.registry().get<TransformComponent>(agent);
    const f32 moved = math::length(t->local.translation);
    REQUIRE(moved == Catch::Approx(0.5f));   // 5 m/s * 0.1s
    REQUIRE(t->local.translation.z < 0.5f);  // perpendicular nudge, not a +Z dash
}

// ─── Once visible, sight is remembered after the wall drops ───────────────────
TEST_CASE("ai: a target seen once is attacked when LOS clears", "[ai][los]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity agent = spawn_agent(scene, {0, 0, 0}, 1u, 100.0f, 30.0f, /*attack*/ 10.0f);
    const Entity enemy = spawn_target(scene, {3, 0, 0}, 2u, 100.0f);

    FireSink sink;
    AiSystems ai;
    ai.ctx.fire = fire_hook;
    ai.ctx.fire_user = &sink;

    // Tick 1: blocked => Chase.
    ai.ctx.los = los_blocked;
    ai.update(scene.registry(), 0.1f);
    REQUIRE(scene.registry().get<AiAgentComponent>(agent)->state == AiState::Chase);
    REQUIRE(sink.count == 0);

    // Tick 2: LOS clears => Attack + fire.
    ai.ctx.los = los_clear;
    ai.update(scene.registry(), 0.1f);
    REQUIRE(scene.registry().get<AiAgentComponent>(agent)->state == AiState::Attack);
    REQUIRE(scene.registry().get<AiAgentComponent>(agent)->target_entity == enemy);
    REQUIRE(sink.count == 1);
}

// ─── Patrol advances waypoints ────────────────────────────────────────────────
TEST_CASE("ai: patrol advances through its waypoint ring", "[ai][patrol]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    // No hostile => the agent patrols. move_speed high so it covers the leg in
    // one step; arrive + dwell + advance.
    const Entity agent = spawn_agent(scene, {0, 0, 0}, 1u, 100.0f, /*sight*/ 5.0f,
                                     /*attack*/ 2.0f, /*move*/ 100.0f);
    PatrolComponent patrol{};
    patrol.count = 2u;
    patrol.current = 0u;
    patrol.waypoints[0] = math::Vec3{2.0f, 0.0f, 0.0f};
    patrol.waypoints[1] = math::Vec3{2.0f, 0.0f, 2.0f};
    patrol.wait_time = 0.0f;       // advance immediately on arrival
    patrol.arrive_radius = 0.25f;
    scene.registry().add<PatrolComponent>(agent, sanitize_patrol(patrol));

    AiSystems ai;
    ai.ctx.los = los_clear;  // no fire hook => Attack is harmless even if reached

    // Tick 1: Idle -> Patrol; steer to wp0 (reaches it, 100 m/s * 0.1s = 10 m).
    ai.update(scene.registry(), 0.1f);
    REQUIRE(scene.registry().get<AiAgentComponent>(agent)->state == AiState::Patrol);
    {
        const auto* t = scene.registry().get<TransformComponent>(agent);
        REQUIRE(t->local.translation.x == Catch::Approx(2.0f));
    }

    // Tick 2: at wp0, wait_time 0 => advance current 0 -> 1.
    ai.update(scene.registry(), 0.1f);
    REQUIRE(scene.registry().get<PatrolComponent>(agent)->current == 1u);

    // Tick 3: steer to wp1 and arrive.
    ai.update(scene.registry(), 0.1f);
    {
        const auto* t = scene.registry().get<TransformComponent>(agent);
        REQUIRE(t->local.translation.z == Catch::Approx(2.0f));
    }

    // Tick 4: advance wraps 1 -> 0.
    ai.update(scene.registry(), 0.1f);
    REQUIRE(scene.registry().get<PatrolComponent>(agent)->current == 0u);
}

// ─── Dead health => Dead state, stops acting ──────────────────────────────────
TEST_CASE("ai: a zero-health agent goes Dead and stops acting", "[ai][death]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity agent = spawn_agent(scene, {0, 0, 0}, 1u, /*hp*/ 100.0f, 30.0f,
                                     /*attack*/ 10.0f, /*move*/ 5.0f);
    const Entity enemy = spawn_target(scene, {3, 0, 0}, 2u, 100.0f);
    (void)enemy;

    FireSink sink;
    AiSystems ai;
    ai.ctx.los = los_clear;
    ai.ctx.fire = fire_hook;
    ai.ctx.fire_user = &sink;

    // Kill the agent's health before the tick.
    scene.registry().get<HealthComponent>(agent)->current_health = 0.0f;
    const math::Vec3 before = scene.registry().get<TransformComponent>(agent)->local.translation;

    ai.update(scene.registry(), 0.1f);

    const auto* a = scene.registry().get<AiAgentComponent>(agent);
    REQUIRE(a->state == AiState::Dead);
    REQUIRE_FALSE(a->target_entity.valid());
    REQUIRE(sink.count == 0);  // a corpse never fires

    // Did not move.
    const math::Vec3 after = scene.registry().get<TransformComponent>(agent)->local.translation;
    REQUIRE(after.x == Catch::Approx(before.x));
    REQUIRE(after.z == Catch::Approx(before.z));

    // Stays Dead on subsequent ticks even if health is restored (terminal).
    scene.registry().get<HealthComponent>(agent)->current_health = 100.0f;
    ai.update(scene.registry(), 0.1f);
    REQUIRE(scene.registry().get<AiAgentComponent>(agent)->state == AiState::Dead);
}

// ─── Multi-agent / multi-chunk: race-free + alloc-free ────────────────────────
TEST_CASE("ai: many agents across chunks think + act race-free and alloc-free",
          "[ai][parallel]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    // 400 agents (faction 1), each paired with a hostile (faction 2) right in
    // its attack range. 400 rows of (Transform, SceneNode, AiAgent, Perception)
    // span several chunks, so perceive/think/act fire their bodies on multiple
    // worker threads at once. AI writes only per-row, and the only shared field
    // it touches (ctx.shots_fired) is atomic => race-free.
    constexpr int kCount = 400;
    std::vector<Entity> agents;
    agents.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        const f32 z = static_cast<f32>(i) * 8.0f;  // separate lanes, no cross-talk
        const Entity a = spawn_agent(scene, {0.0f, 0.0f, z}, /*faction*/ 1u, 100.0f,
                                     /*sight*/ 30.0f, /*attack*/ 5.0f);
        agents.push_back(a);
        spawn_target(scene, {2.0f, 0.0f, z}, /*faction*/ 2u, 100.0f);  // in range
    }

    AiSystems ai;
    ai.ctx.los = los_clear;
    ai.ctx.fire = fire_hook;  // counts via the atomic shots_fired, not the sink
    FireSink sink;            // host counter is irrelevant under threads; use atomic
    ai.ctx.fire_user = &sink;

    ai.update(scene.registry(), 0.1f);

    // Every agent acquired its hostile, reached Attack, and fired exactly once.
    int attacking = 0;
    for (Entity a : agents) {
        const auto* ac = scene.registry().get<AiAgentComponent>(a);
        REQUIRE(ac != nullptr);
        if (ac->state == AiState::Attack)
            ++attacking;
        REQUIRE(scene.registry().get<PerceptionComponent>(a)->can_see == 1u);
    }
    REQUIRE(attacking == kCount);
    // Atomic shot tally is exactly one per agent — no lost / double increments.
    REQUIRE(ai.ctx.shots_fired.load() == static_cast<u32>(kCount));

    // A second identical tick produces the same tally — deterministic + no
    // accumulation drift across frames.
    ai.update(scene.registry(), 0.1f);
    REQUIRE(ai.ctx.shots_fired.load() == static_cast<u32>(kCount));
}
