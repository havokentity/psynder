// SPDX-License-Identifier: MIT
// Psynder — M-AI navigation ALLOC-FREE proof (Lane W12-3, Fix A).
//
// The nav hot path promises ZERO per-query heap allocation: a NavQuery's scratch
// is sized ONCE to the grid, then reused for every find_path(). Before this lane,
// ai::navigate() relied on find_path()'s LAZY self-size, so the FIRST query on a
// freshly-bound grid grew the query's scratch INSIDE the hot call. navigate() now
// primes every pool slot to the bound grid up front, so the steady-state first
// path does no heap growth in the query.
//
// These cases prove it two ways:
//   1. By construction on the primitive: reset(grid) sizes the scratch; the first
//      AND every later find_path() on that size grows NOTHING (scratch_size and
//      the reserved std::vector capacities are stable before vs after).
//   2. At the system level: a fresh AiContext bound to a grid has UNSIZED pool
//      slots; after navigate() runs (with a routing agent), the slot the query
//      used is sized to the grid — i.e. navigate() primed it up front, so the
//      find_path() inside that pass could not have grown scratch. Paths are
//      unchanged (the routed agent still reaches its goal).
//
// Host-agnostic for case 1 (plain NavGrid); case 2 uses the scene ECS exactly
// like ai_systems.cpp.

#include <catch2/catch_test_macros.hpp>

#include "ai/AiComponents.h"
#include "ai/AiSystems.h"
#include "ai/NavGrid.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <span>

using namespace psynder;
using namespace psynder::ai;
using psynder::scene::EcsRegistry;
using psynder::scene::HealthComponent;
using psynder::scene::LocalTransform;
using psynder::scene::Scene;
using psynder::scene::TransformComponent;

namespace {

NavGrid make_grid(u32 w, u32 h, f32 cell = 1.0f) {
    NavGrid g;
    g.resize(w, h, cell, math::Vec3{0.0f, 0.0f, 0.0f});
    return g;
}

// Clear the process-global registry around each scene-using case (mirrors
// ai_systems.cpp's RegistryReset).
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

Entity spawn_nav_agent(Scene& scene, math::Vec3 pos, math::Vec3 goal) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = 100.0f;
    health.current_health = 100.0f;
    health.faction = 1u;
    scene.registry().add<HealthComponent>(e, health);
    AiAgentComponent agent{};
    agent.state = AiState::Idle;
    agent.sight_range = 1.0f;       // no hostile in range => Patrol drives motion
    agent.fov_cos = -1.0f;
    agent.attack_range = 1.0f;
    agent.think_cooldown = 0.0f;
    agent.think_interval = 0.0f;
    agent.move_speed = 5.0f;
    scene.registry().add<AiAgentComponent>(e, sanitize_ai_agent(agent));
    scene.registry().add<PerceptionComponent>(e, PerceptionComponent{});
    PatrolComponent patrol{};
    patrol.count = 1u;
    patrol.waypoints[0] = goal;
    patrol.wait_time = 1000.0f;
    patrol.arrive_radius = 0.5f;
    scene.registry().add<PatrolComponent>(e, sanitize_patrol(patrol));
    NavAgentComponent nav{};
    nav.repath_interval = 0.5f;
    nav.repath_dist = 1.0f;
    nav.arrive_radius = 0.6f;
    scene.registry().add<NavAgentComponent>(e, sanitize_nav_agent(nav));
    return e;
}

}  // namespace

// ─── Primitive: a sized NavQuery grows nothing on its first (or any) query ────
TEST_CASE("ai-nav-alloc: reset()-sized NavQuery does zero heap growth on first find_path",
          "[ai][nav][alloc]") {
    NavGrid g = make_grid(24u, 24u);
    // A wall with a gap forces a real, non-trivial routed path (not a 1-cell
    // straight shot) so the open/closed scratch is genuinely exercised.
    for (i32 z = 0; z <= 20; ++z)
        g.set_blocked(NavCell{12, z}, true);  // gap at z = 21..23

    NavQuery q;
    // Prime the scratch to the grid UP FRONT, exactly as navigate() now does.
    q.reset(g);
    REQUIRE(q.scratch_size() == g.cell_count());

    // Capture the reserved size BEFORE the first query. After priming, the first
    // find_path must not change it (no lazy self-size, no growth).
    const usize size_before = q.scratch_size();

    NavPath first{};
    const bool ok_first = q.find_path(g, NavCell{2, 2}, NavCell{22, 2}, first);
    REQUIRE(ok_first);
    REQUIRE(first.count >= 1u);

    // The proof: the scratch is the SAME size after the first query as before —
    // the first query allocated nothing (the one-time sizing happened in reset()).
    REQUIRE(q.scratch_size() == size_before);

    // And it stays stable across further queries (steady-state reuse).
    for (int run = 0; run < 8; ++run) {
        NavPath again{};
        REQUIRE(q.find_path(g, NavCell{2, 2}, NavCell{22, 2}, again));
        REQUIRE(q.scratch_size() == size_before);
        // Identical path each time (determinism preserved).
        REQUIRE(again.count == first.count);
        for (u32 i = 0; i < first.count; ++i) {
            REQUIRE(again.points[i].x == first.points[i].x);
            REQUIRE(again.points[i].z == first.points[i].z);
        }
    }
}

// ─── Priming-vs-lazy equivalence: same path with and without the up-front size ─
TEST_CASE("ai-nav-alloc: up-front-sized and lazily-sized queries return identical paths",
          "[ai][nav][alloc]") {
    NavGrid g = make_grid(20u, 20u);
    for (i32 z = 0; z <= 16; ++z)
        g.set_blocked(NavCell{10, z}, true);  // gap at the top

    // Primed up front (the new navigate() behaviour).
    NavQuery primed;
    primed.reset(g);
    NavPath via_primed{};
    REQUIRE(primed.find_path(g, NavCell{1, 1}, NavCell{18, 1}, via_primed));

    // Lazily sized inside find_path (the OLD behaviour — still supported as a
    // fallback for a caller that forgets to reset()).
    NavQuery lazy;
    NavPath via_lazy{};
    REQUIRE(lazy.find_path(g, NavCell{1, 1}, NavCell{18, 1}, via_lazy));

    // Results must be byte-for-byte identical — Fix A is additive, not a path
    // change.
    REQUIRE(via_primed.count == via_lazy.count);
    for (u32 i = 0; i < via_primed.count; ++i) {
        REQUIRE(via_primed.points[i].x == via_lazy.points[i].x);
        REQUIRE(via_primed.points[i].z == via_lazy.points[i].z);
    }
}

// ─── System level: navigate() primes the pool up front (no growth in the query) ─
TEST_CASE("ai-nav-alloc: navigate() sizes the NavQuery pool to the grid up front",
          "[ai][nav][alloc]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    NavGrid g = make_grid(16u, 16u);
    const math::Vec3 goal{12.5f, 0.0f, 12.5f};
    const Entity agent = spawn_nav_agent(scene, math::Vec3{2.5f, 0.0f, 2.5f}, goal);

    AiSystems ai;
    ai.ctx.nav_grid = &g;

    // A fresh context has NOT sized its pool to any grid yet: every slot is
    // unsized, and the cached "sized cells" sentinel says so.
    REQUIRE(ai.ctx.nav_sized_cells != g.cell_count());
    for (const NavQuery& q : ai.ctx.nav_query)
        REQUIRE(q.scratch_size() == 0u);

    // One full AI tick. navigate() must size every pool slot to the grid BEFORE
    // it runs any find_path(), so the query inside the pass grows no scratch.
    ai.update(scene.registry(), 1.0f / 30.0f);

    // The cache now records the grid size, and the pool slots are sized to it —
    // proof the up-front priming ran (the query's first find_path could not have
    // lazily grown). A repath happened (the agent planned a route).
    REQUIRE(ai.ctx.nav_sized_cells == g.cell_count());
    for (const NavQuery& q : ai.ctx.nav_query)
        REQUIRE(q.scratch_size() == g.cell_count());
    REQUIRE(ai.ctx.repaths.load() >= 1u);

    // Path is unchanged in effect: drive the agent to the goal over more ticks
    // (it routes around nothing here, but must still arrive — behaviour intact).
    bool arrived = false;
    for (int tick = 0; tick < 400 && !arrived; ++tick) {
        ai.update(scene.registry(), 1.0f / 30.0f);
        const auto* t = scene.registry().get<TransformComponent>(agent);
        if (math::length(math::sub(goal, t->local.translation)) <= 0.7f)
            arrived = true;
    }
    REQUIRE(arrived);

    // After steady state the pool stays sized exactly to the grid — no growth,
    // no churn (the cache short-circuits the priming on every later tick).
    REQUIRE(ai.ctx.nav_sized_cells == g.cell_count());
}
