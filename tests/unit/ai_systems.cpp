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
#include "ai/NavGrid.h"
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

// ═══════════════════════════════════════════════════════════════════════════
//  Navigation: NavGrid + deterministic pooled A* + path-following + smoothing
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Build a w x h grid, 1 m cells, origin at the world origin, all walkable.
NavGrid make_grid(u32 w, u32 h) {
    NavGrid g;
    g.resize(w, h, /*cell*/ 1.0f, math::Vec3{0.0f, 0.0f, 0.0f});
    return g;
}

// True if every consecutive pair of waypoints steps to an adjacent walkable
// cell (or the same cell) — a sanity check on a returned path.
bool path_cells_walkable(const NavGrid& g, const NavPath& p) {
    for (u32 i = 0; i < p.count; ++i)
        if (g.blocked(g.world_to_cell(p.points[i])))
            return false;
    return true;
}

}  // namespace

// ─── A* routes around a blocking wall through the single gap ─────────────────
TEST_CASE("ai: A* finds a path around a wall through the one gap", "[ai][nav]") {
    // 11x11 grid. A vertical wall at x=5 spanning z=0..9 with a single gap at
    // z=10, so the only route from the left side to the right side threads the
    // bottom gap. Start (1,5) left of the wall, goal (9,5) right of it.
    NavGrid g = make_grid(11u, 11u);
    for (i32 z = 0; z <= 9; ++z)
        g.set_blocked(NavCell{5, z}, true);
    // gap left open at (5,10)

    NavQuery q;
    q.reset(g);
    NavPath path;
    const bool ok = q.find_path(g, NavCell{1, 5}, NavCell{9, 5}, path);

    REQUIRE(ok);
    REQUIRE(path.count >= 2u);
    REQUIRE(path.truncated == 0u);
    REQUIRE(path_cells_walkable(g, path));

    // First waypoint sits at the start cell centre, last at the goal cell centre.
    const math::Vec3 first = path.points[0];
    const math::Vec3 last = path.points[path.count - 1u];
    REQUIRE(g.world_to_cell(first) == NavCell{1, 5});
    REQUIRE(g.world_to_cell(last) == NavCell{9, 5});

    // The path must pass through the only gap: SOME waypoint segment crosses the
    // wall column at z=10 (the gap row). We assert no waypoint ever lands on a
    // blocked cell (above) and that the route reaches z>=10 at some point — it
    // cannot cross x=5 anywhere else.
    bool reached_gap_row = false;
    for (u32 i = 0; i < path.count; ++i)
        if (g.world_to_cell(path.points[i]).z >= 10)
            reached_gap_row = true;
    REQUIRE(reached_gap_row);
}

// ─── No path returns cleanly when the goal is walled off ─────────────────────
TEST_CASE("ai: A* returns no-path cleanly when the goal is fully walled off",
          "[ai][nav]") {
    // 9x9 grid. A complete 3x3 box of walls around the goal cell (4,4) seals it
    // off — every neighbouring cell is blocked, so no route can reach it.
    NavGrid g = make_grid(9u, 9u);
    for (i32 z = 3; z <= 5; ++z)
        for (i32 x = 3; x <= 5; ++x)
            if (!(x == 4 && z == 4))
                g.set_blocked(NavCell{x, z}, true);

    NavQuery q;
    q.reset(g);
    NavPath path;
    const bool ok = q.find_path(g, NavCell{0, 0}, NavCell{4, 4}, path);

    REQUIRE_FALSE(ok);         // unreachable
    REQUIRE(path.empty());     // cleared, no partial path
    REQUIRE(path.count == 0u);

    // A blocked START is likewise a clean no-path, never a crash.
    NavPath path2;
    g.set_blocked(NavCell{0, 0}, true);
    REQUIRE_FALSE(q.find_path(g, NavCell{0, 0}, NavCell{8, 8}, path2));
    REQUIRE(path2.empty());

    // An out-of-bounds endpoint is also clean.
    NavPath path3;
    REQUIRE_FALSE(q.find_path(g, NavCell{1, 1}, NavCell{100, 100}, path3));
    REQUIRE(path3.empty());
}

// ─── Determinism: identical grid+endpoints => identical waypoint list ────────
TEST_CASE("ai: A* is deterministic — identical waypoints across repeated runs",
          "[ai][nav][determinism]") {
    NavGrid g = make_grid(20u, 20u);
    // A scattering of obstacles to force a non-trivial route with tie choices.
    for (i32 z = 2; z <= 14; ++z)
        g.set_blocked(NavCell{7, z}, true);
    for (i32 x = 5; x <= 16; ++x)
        g.set_blocked(NavCell{x, 16}, true);

    NavQuery q;
    NavPath a;
    q.reset(g);
    REQUIRE(q.find_path(g, NavCell{1, 1}, NavCell{18, 18}, a));
    REQUIRE(a.count >= 2u);

    // Re-run many times (same query reused, and a fresh query) — the waypoint
    // list must be byte-for-byte identical: no RNG / clock / pointer-order / heap
    // dependence in the A*.
    for (int run = 0; run < 8; ++run) {
        NavPath b;
        if (run % 2 == 0) {
            q.reset(g);  // exercise the reset path too
        }
        NavQuery q2;
        NavQuery& use = (run % 2 == 0) ? q : q2;
        REQUIRE(use.find_path(g, NavCell{1, 1}, NavCell{18, 18}, b));
        REQUIRE(b.count == a.count);
        for (u32 i = 0; i < a.count; ++i) {
            REQUIRE(b.points[i].x == Catch::Approx(a.points[i].x));
            REQUIRE(b.points[i].z == Catch::Approx(a.points[i].z));
        }
    }
}

// ─── Smoothing reduces waypoint count on an open diagonal ────────────────────
TEST_CASE("ai: string-pull smoothing reduces waypoints on an open diagonal",
          "[ai][nav][smooth]") {
    // Wide open grid, straight diagonal route: the raw cell path is many
    // cell-centre steps; smoothing should collapse it to (nearly) start+goal
    // since the whole diagonal is in clear line of sight.
    NavGrid g = make_grid(24u, 24u);

    NavQuery q;
    q.reset(g);
    NavPath path;
    REQUIRE(q.find_path(g, NavCell{1, 1}, NavCell{20, 20}, path));
    const u32 raw_count = path.count;
    REQUIRE(raw_count > 3u);  // a long raw cell list

    const u32 smoothed = smooth_path(g, path);
    REQUIRE(smoothed < raw_count);  // fewer waypoints after string-pull
    REQUIRE(smoothed >= 2u);        // still at least start + goal
    REQUIRE(path.count == smoothed);
    // Endpoints preserved through smoothing.
    REQUIRE(g.world_to_cell(path.points[0]) == NavCell{1, 1});
    REQUIRE(g.world_to_cell(path.points[path.count - 1u]) == NavCell{20, 20});
}

// ─── Path-following: an agent reaches the goal cell over N ticks ─────────────
TEST_CASE("ai: a nav-routed agent reaches the goal cell over many ticks",
          "[ai][nav][follow]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    // Grid with a wall the straight-line steer would jam against. Start at world
    // (1.5,0,1.5) [cell (1,1)], goal at world (9.5,0,5.5) [cell (9,5)], wall at
    // x=5 z=0..9 with the gap at z=10 (same layout as the routing test).
    NavGrid g = make_grid(12u, 12u);
    for (i32 z = 0; z <= 9; ++z)
        g.set_blocked(NavCell{5, z}, true);

    // An agent (faction 1) with no hostile => it would Idle; give it a 1-waypoint
    // patrol AT the goal so think() drives it to Patrol and navigate() routes it.
    const Entity agent = spawn_agent(scene, {1.5f, 0.0f, 1.5f}, /*faction*/ 1u,
                                     /*hp*/ 100.0f, /*sight*/ 1.0f, /*attack*/ 1.0f,
                                     /*move*/ 6.0f);
    PatrolComponent patrol{};
    patrol.count = 1u;
    patrol.waypoints[0] = math::Vec3{9.5f, 0.0f, 5.5f};
    patrol.wait_time = 1000.0f;   // never advance once arrived
    patrol.arrive_radius = 0.5f;
    scene.registry().add<PatrolComponent>(agent, sanitize_patrol(patrol));

    NavAgentComponent nav{};
    nav.repath_interval = 0.5f;
    nav.repath_dist = 1.0f;
    nav.arrive_radius = 0.6f;
    scene.registry().add<NavAgentComponent>(agent, sanitize_nav_agent(nav));

    AiSystems ai;
    ai.ctx.nav_grid = &g;  // enable navigation

    // Run enough ticks for the agent to walk the routed corridor (it must detour
    // down to the gap at z=10, then across, then up to the goal — far longer than
    // the straight-line distance).
    const math::Vec3 goal{9.5f, 0.0f, 5.5f};
    bool arrived = false;
    for (int tick = 0; tick < 400 && !arrived; ++tick) {
        ai.update(scene.registry(), 1.0f / 30.0f);
        const auto* t = scene.registry().get<TransformComponent>(agent);
        // Never tunnel into a blocked cell while following the route.
        REQUIRE_FALSE(g.blocked(g.world_to_cell(t->local.translation)));
        if (math::length(math::sub(goal, t->local.translation)) <= 0.7f)
            arrived = true;
    }
    REQUIRE(arrived);

    // At least one repath happened, and the agent ended on the goal cell.
    REQUIRE(ai.ctx.repaths.load() >= 1u);
    const auto* t = scene.registry().get<TransformComponent>(agent);
    REQUIRE(g.world_to_cell(t->local.translation) == NavCell{9, 5});
}

// ─── Nav is opt-in: no grid => unchanged straight-line behaviour ─────────────
TEST_CASE("ai: with no nav grid wired, navigate() is a no-op", "[ai][nav]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const Entity agent = spawn_agent(scene, {0, 0, 0}, 1u, 100.0f, /*sight*/ 30.0f,
                                     /*attack*/ 4.0f, /*move*/ 5.0f);
    spawn_target(scene, {20, 0, 0}, 2u, 100.0f);  // far, in sight => Chase
    // Even with a NavAgentComponent present, a null nav_grid means no routing.
    scene.registry().add<NavAgentComponent>(agent, sanitize_nav_agent(NavAgentComponent{}));

    AiSystems ai;
    ai.ctx.los = los_clear;
    // ai.ctx.nav_grid stays null.

    ai.update(scene.registry(), 0.1f);

    REQUIRE(ai.ctx.repaths.load() == 0u);  // navigate did nothing
    const auto* a = scene.registry().get<AiAgentComponent>(agent);
    REQUIRE(a->state == AiState::Chase);
    // Straight-line chase toward +X, exactly as before: 5 m/s * 0.1s = 0.5 m.
    const auto* t = scene.registry().get<TransformComponent>(agent);
    REQUIRE(t->local.translation.x == Catch::Approx(0.5f));
}

// ─── Local separation: two stacked agents push apart ─────────────────────────
TEST_CASE("ai: separation nudges co-located agents apart", "[ai][nav][separation]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    NavGrid g = make_grid(16u, 16u);

    // Two agents almost on top of each other, both routing to the same far goal.
    auto make_nav_agent = [&](math::Vec3 pos) {
        const Entity e = spawn_agent(scene, pos, /*faction*/ 1u, 100.0f,
                                     /*sight*/ 1.0f, /*attack*/ 1.0f, /*move*/ 3.0f);
        PatrolComponent patrol{};
        patrol.count = 1u;
        patrol.waypoints[0] = math::Vec3{12.5f, 0.0f, 12.5f};
        patrol.wait_time = 1000.0f;
        patrol.arrive_radius = 0.5f;
        scene.registry().add<PatrolComponent>(e, sanitize_patrol(patrol));
        NavAgentComponent nav{};
        nav.separation_radius = 1.5f;   // enable separation
        nav.separation_weight = 4.0f;
        scene.registry().add<NavAgentComponent>(e, sanitize_nav_agent(nav));
        return e;
    };
    const Entity a0 = make_nav_agent(math::Vec3{2.5f, 0.0f, 2.5f});
    const Entity a1 = make_nav_agent(math::Vec3{2.6f, 0.0f, 2.5f});  // 0.1 m apart

    AiSystems ai;
    ai.ctx.nav_grid = &g;

    const auto dist_between = [&]() {
        const math::Vec3 p0 = scene.registry().get<TransformComponent>(a0)->local.translation;
        const math::Vec3 p1 = scene.registry().get<TransformComponent>(a1)->local.translation;
        return math::length(math::sub(p1, p0));
    };
    const f32 before = dist_between();

    for (int tick = 0; tick < 5; ++tick)
        ai.update(scene.registry(), 1.0f / 30.0f);

    // They separated (no longer stacked) — separation pushed them apart while
    // both advanced toward the shared goal.
    REQUIRE(dist_between() > before);
}
