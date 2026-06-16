// SPDX-License-Identifier: MIT
// Psynder — M-AI squad / flanking coordination unit tests (W13-3). Exercises the
// SquadCoord layer that, when opted in (AiContext::squad.enabled), hands agents
// engaging a COMMON target DISTINCT approach slots around it so the squad
// surrounds / flanks instead of stacking single-file:
//   * 3+ agents on one target get DISTINCT slots / goal cells (not identical),
//     and their flank goals are SPREAD around the target (pairwise separation
//     above a threshold, far beyond the bunched single-target baseline);
//   * each flank goal lands on a FREE nav cell;
//   * the assignment is DETERMINISTIC — same ids => same slots / goals,
//     bit-for-bit, across repeated runs and a fresh context;
//   * with the layer OFF, every agent's goal is the raw target and act() steers
//     exactly as the pre-squad single-target path (no regression);
//   * the suppress-vs-flank role split: the id-lowest member holds head-on while
//     the rest flank.
// Pure DOTS: no render / host / physics — only the AI components + the injected
// LOS / move hooks, mirroring ai_systems.cpp.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/AiComponents.h"
#include "ai/AiSystems.h"
#include "ai/NavGrid.h"
#include "ai/SquadCoord.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

using namespace psynder;
using namespace psynder::ai;
using psynder::scene::EcsRegistry;
using psynder::scene::HealthComponent;
using psynder::scene::LocalTransform;
using psynder::scene::Scene;
using psynder::scene::TransformComponent;

namespace {

// Clear the process-global registry around every case (Catch2 randomizes order)
// — mirrors ai_systems.cpp's RegistryReset.
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

bool los_clear(void*, math::Vec3, math::Vec3) { return true; }

// Spawn a faction-1 agent that will Chase a faction-2 hostile (sight wide, attack
// range tiny so it never flips to Attack and stays in Chase the whole tick).
Entity spawn_agent(Scene& scene, math::Vec3 pos, f32 move_speed = 4.0f) {
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
    agent.sight_range = 200.0f;
    agent.fov_cos = -1.0f;     // omnidirectional
    agent.attack_range = 0.5f;  // tiny => stays Chase, never Attack
    agent.think_cooldown = 0.0f;
    agent.think_interval = 0.0f;  // re-evaluate every tick
    agent.move_speed = move_speed;
    scene.registry().add<AiAgentComponent>(e, sanitize_ai_agent(agent));
    scene.registry().add<PerceptionComponent>(e, PerceptionComponent{});
    return e;
}

Entity spawn_target(Scene& scene, math::Vec3 pos) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e = scene.create_entity(local);
    HealthComponent health{};
    health.max_health = 100.0f;
    health.current_health = 100.0f;
    health.faction = 2u;
    scene.registry().add<HealthComponent>(e, health);
    return e;
}

// Directly seed an agent into Chase on a known target with a known last-seen
// position — bypasses perceive so a test can drive the squad layer in isolation
// with a deterministic target anchor.
void force_chase(Scene& scene, Entity agent, Entity target, math::Vec3 last_seen) {
    auto* a = scene.registry().get<AiAgentComponent>(agent);
    a->state = AiState::Chase;
    a->target_entity = target;
    auto* s = scene.registry().get<PerceptionComponent>(agent);
    s->can_see = 0u;  // remembered but not visible => stays Chase
    s->last_seen_pos = last_seen;
}

f32 dist_xz(math::Vec3 a, math::Vec3 b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

// ─── 3 agents get DISTINCT slots, spread around the target, on free cells ─────
TEST_CASE("ai-squad: agents on one target get distinct spread flank goals on free cells",
          "[ai][squad]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const math::Vec3 tpos{20.0f, 0.0f, 20.0f};
    const Entity target = spawn_target(scene, tpos);

    // Three agents approaching from -Z (so the approach axis is well-defined).
    const Entity a0 = spawn_agent(scene, {18.0f, 0.0f, 6.0f});
    const Entity a1 = spawn_agent(scene, {20.0f, 0.0f, 6.0f});
    const Entity a2 = spawn_agent(scene, {22.0f, 0.0f, 6.0f});
    for (Entity e : {a0, a1, a2})
        force_chase(scene, e, target, tpos);

    SquadConfig cfg = sanitize_squad_config(SquadConfig{});
    cfg.enabled = true;
    cfg.flank_radius = 6.0f;
    cfg.suppressors = 0u;  // every member flanks => all three goals are distinct

    EcsRegistry& reg = scene.registry();
    const auto goal_of = [&](Entity e) {
        const auto* a = reg.get<AiAgentComponent>(e);
        const math::Vec3 self = reg.get<TransformComponent>(e)->local.translation;
        return squad_flank_goal(reg, cfg, e, *a, tpos, self);
    };
    const math::Vec3 g0 = goal_of(a0);
    const math::Vec3 g1 = goal_of(a1);
    const math::Vec3 g2 = goal_of(a2);

    // DISTINCT goals — no two agents aim at the same point.
    REQUIRE(dist_xz(g0, g1) > 0.5f);
    REQUIRE(dist_xz(g0, g2) > 0.5f);
    REQUIRE(dist_xz(g1, g2) > 0.5f);

    // SPREAD around the target: every goal sits ~flank_radius from the target
    // (on the ring) and the mean pairwise separation is well above the bunched
    // baseline (all three at the same target => separation 0).
    REQUIRE(dist_xz(g0, tpos) == Catch::Approx(6.0f).margin(0.01f));
    REQUIRE(dist_xz(g1, tpos) == Catch::Approx(6.0f).margin(0.01f));
    REQUIRE(dist_xz(g2, tpos) == Catch::Approx(6.0f).margin(0.01f));
    const f32 mean_sep = (dist_xz(g0, g1) + dist_xz(g0, g2) + dist_xz(g1, g2)) / 3.0f;
    REQUIRE(mean_sep > 3.0f);  // bunched baseline would be 0

    // Each flank goal lands on a FREE nav cell (an all-walkable grid covering the
    // ring => world_to_cell of each goal is in bounds + walkable).
    NavGrid grid;
    grid.resize(64u, 64u, /*cell*/ 1.0f, math::Vec3{0.0f, 0.0f, 0.0f});
    for (const math::Vec3& g : {g0, g1, g2}) {
        const NavCell c = grid.world_to_cell(g);
        REQUIRE(grid.in_bounds(c));
        REQUIRE(grid.walkable(c));
    }
}

// ─── Distinct SLOTS / ranks from stable ids ───────────────────────────────────
TEST_CASE("ai-squad: engaging agents get distinct id-sorted ranks/slots", "[ai][squad]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const math::Vec3 tpos{10.0f, 0.0f, 10.0f};
    const Entity target = spawn_target(scene, tpos);

    std::vector<Entity> agents;
    for (int i = 0; i < 4; ++i) {
        const Entity e = spawn_agent(scene, {static_cast<f32>(i), 0.0f, 0.0f});
        force_chase(scene, e, target, tpos);
        agents.push_back(e);
    }

    SquadConfig cfg = sanitize_squad_config(SquadConfig{});
    cfg.enabled = true;
    cfg.suppressors = 0u;

    EcsRegistry& reg = scene.registry();
    std::vector<u32> ranks, slots;
    for (Entity e : agents) {
        const auto* a = reg.get<AiAgentComponent>(e);
        const SquadAssignment as = assign_slot(reg, cfg, e, *a);
        REQUIRE(as.group_size == 4u);
        ranks.push_back(as.rank);
        slots.push_back(as.slot);
    }
    // All four ranks distinct and exactly {0,1,2,3}.
    std::vector<u32> sorted = ranks;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(sorted == std::vector<u32>{0u, 1u, 2u, 3u});
    // Slots distinct too (within the cap they equal the ranks).
    std::vector<u32> ss = slots;
    std::sort(ss.begin(), ss.end());
    REQUIRE(ss == std::vector<u32>{0u, 1u, 2u, 3u});
}

// ─── Determinism: same ids => same goals, bit-for-bit ─────────────────────────
TEST_CASE("ai-squad: flank assignment is deterministic across repeated runs",
          "[ai][squad][determinism]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const math::Vec3 tpos{30.0f, 0.0f, 12.0f};
    const Entity target = spawn_target(scene, tpos);
    std::vector<Entity> agents;
    for (int i = 0; i < 5; ++i) {
        const Entity e = spawn_agent(scene, {static_cast<f32>(i) * 2.0f, 0.0f, 0.0f});
        force_chase(scene, e, target, tpos);
        agents.push_back(e);
    }

    SquadConfig cfg = sanitize_squad_config(SquadConfig{});
    cfg.enabled = true;
    cfg.suppressors = 0u;

    EcsRegistry& reg = scene.registry();
    const auto goals = [&]() {
        std::vector<math::Vec3> g;
        for (Entity e : agents) {
            const auto* a = reg.get<AiAgentComponent>(e);
            const math::Vec3 self = reg.get<TransformComponent>(e)->local.translation;
            g.push_back(squad_flank_goal(reg, cfg, e, *a, tpos, self));
        }
        return g;
    };
    const std::vector<math::Vec3> base = goals();
    // Re-run many times — byte-for-byte identical goals (no RNG / clock / order
    // dependence). assign_slot + slot_bearing are pure functions of the id set.
    for (int run = 0; run < 8; ++run) {
        const std::vector<math::Vec3> again = goals();
        REQUIRE(again.size() == base.size());
        for (usize i = 0; i < base.size(); ++i) {
            REQUIRE(again[i].x == Catch::Approx(base[i].x));
            REQUIRE(again[i].y == Catch::Approx(base[i].y));
            REQUIRE(again[i].z == Catch::Approx(base[i].z));
        }
    }
}

// ─── Suppress-vs-flank role split ─────────────────────────────────────────────
TEST_CASE("ai-squad: the id-lowest member holds head-on while the rest flank",
          "[ai][squad][roles]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const math::Vec3 tpos{15.0f, 0.0f, 15.0f};
    const Entity target = spawn_target(scene, tpos);
    std::vector<Entity> agents;
    for (int i = 0; i < 3; ++i) {
        const Entity e = spawn_agent(scene, {static_cast<f32>(i), 0.0f, 2.0f});
        force_chase(scene, e, target, tpos);
        agents.push_back(e);
    }

    SquadConfig cfg = sanitize_squad_config(SquadConfig{});
    cfg.enabled = true;
    cfg.suppressors = 1u;  // exactly one holds

    EcsRegistry& reg = scene.registry();
    int suppressors = 0, flankers = 0;
    for (Entity e : agents) {
        const auto* a = reg.get<AiAgentComponent>(e);
        const SquadAssignment as = assign_slot(reg, cfg, e, *a);
        const math::Vec3 self = reg.get<TransformComponent>(e)->local.translation;
        const math::Vec3 goal = squad_flank_goal(reg, cfg, e, *a, tpos, self);
        if (as.suppressor) {
            ++suppressors;
            // A suppressor holds head-on => its goal IS the raw target.
            REQUIRE(dist_xz(goal, tpos) == Catch::Approx(0.0f).margin(1e-3f));
        } else {
            ++flankers;
            // A flanker rings out to flank_radius from the target.
            REQUIRE(dist_xz(goal, tpos) > 1.0f);
        }
    }
    REQUIRE(suppressors == 1);
    REQUIRE(flankers == 2);
}

// ─── Layer OFF => no regression vs the single-target path ─────────────────────
TEST_CASE("ai-squad: with the layer off, goals are the raw target (no regression)",
          "[ai][squad][off]") {
    RegistryReset reset;
    Scene scene{EcsRegistry::Get()};

    const math::Vec3 tpos{8.0f, 0.0f, 8.0f};
    const Entity target = spawn_target(scene, tpos);
    std::vector<Entity> agents;
    for (int i = 0; i < 3; ++i) {
        const Entity e = spawn_agent(scene, {static_cast<f32>(i), 0.0f, 0.0f});
        force_chase(scene, e, target, tpos);
        agents.push_back(e);
    }

    SquadConfig cfg = sanitize_squad_config(SquadConfig{});
    cfg.enabled = false;  // OFF

    EcsRegistry& reg = scene.registry();
    for (Entity e : agents) {
        const auto* a = reg.get<AiAgentComponent>(e);
        const math::Vec3 self = reg.get<TransformComponent>(e)->local.translation;
        const math::Vec3 goal = squad_flank_goal(reg, cfg, e, *a, tpos, self);
        // Layer off => every agent's goal is the EXACT target (bunched baseline).
        REQUIRE(goal.x == Catch::Approx(tpos.x));
        REQUIRE(goal.y == Catch::Approx(tpos.y));
        REQUIRE(goal.z == Catch::Approx(tpos.z));
    }
}

// ─── End-to-end act(): squad on fans the agents apart vs squad off ────────────
TEST_CASE("ai-squad: act() steers a squad to spread out when the layer is on",
          "[ai][squad][act]") {
    RegistryReset reset;

    // Run the SAME scene twice (layer off, then on) and compare how far the agents
    // spread after several Chase ticks. Off => they steer to the same target and
    // converge; on => they steer to distinct flank slots and fan out.
    const math::Vec3 tpos{40.0f, 0.0f, 40.0f};
    const auto run = [&](bool squad_on) {
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        Scene scene{EcsRegistry::Get()};
        (void)spawn_target(scene, tpos);  // hostile the squad perceives + chases
        std::vector<Entity> agents;
        // Three agents bunched at the same start so the OFF run keeps them bunched.
        for (int i = 0; i < 3; ++i) {
            const Entity e = spawn_agent(scene, {10.0f + static_cast<f32>(i) * 0.2f,
                                                 0.0f, 10.0f}, /*move*/ 6.0f);
            agents.push_back(e);
        }

        AiSystems ai;
        ai.ctx.los = los_clear;  // target stays visible => Chase (attack_range tiny)
        ai.ctx.squad.enabled = squad_on;
        ai.ctx.squad = sanitize_squad_config(ai.ctx.squad);
        ai.ctx.squad.flank_radius = 7.0f;
        ai.ctx.squad.suppressors = 0u;

        for (int tick = 0; tick < 60; ++tick)
            ai.update(scene.registry(), 1.0f / 30.0f);

        // Mean pairwise separation of the three agents after the run.
        std::vector<math::Vec3> p;
        for (Entity e : agents)
            p.push_back(scene.registry().get<TransformComponent>(e)->local.translation);
        const f32 sep = (dist_xz(p[0], p[1]) + dist_xz(p[0], p[2]) + dist_xz(p[1], p[2])) / 3.0f;
        return std::pair<f32, u32>{sep, ai.ctx.flankers.load()};
    };

    const auto [sep_off, flank_off] = run(false);
    const auto [sep_on, flank_on] = run(true);

    // Layer off: no flankers reported; the agents converged on the one target.
    REQUIRE(flank_off == 0u);
    // Layer on: flankers were assigned and the squad ended up MORE spread out than
    // the bunched single-target baseline.
    REQUIRE(flank_on >= 2u);
    REQUIRE(sep_on > sep_off + 2.0f);
}
