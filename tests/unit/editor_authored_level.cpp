// SPDX-License-Identifier: MIT
// Psynder - LANE W9-4 Definition-of-Done checkpoint: an editor-authored PLAYABLE
// terrain-shooter level round-trips through a .psyscene and PLAYS under
// PlayRuntime, with NO per-feature C++.
//
// This is the capstone that ties together every authoring system that already
// exists in the engine -- scene authoring, the no-code gameplay/AI component
// proxies (Faction / Hitbox / WeaponMode / AiAgent / Perception / Patrol), the
// Wave-8 no-code VehicleComponent (heightfield ground-binding + speed governor),
// the editor-authored PsyGraph visual script, and PlayRuntime synthesis -- using
// ONLY the public scene/editor authoring APIs. No engine internals, no bespoke
// gameplay C++.
//
// The level (authored entirely from the scene/component model):
//   * PLAYER: a Heightfield-grounded drivable VehicleComponent (is_player) sitting
//     above a procedural rolling-hills surface, away from the firefight.
//   * 3 RANGED AI SOLDIERS: each authored from the no-code scene proxies only --
//     scene::FactionComponent + HitboxComponent + AiAgentComponent (omni-FOV so
//     acquisition is deterministic) + PerceptionComponent + PatrolComponent +
//     WeaponModeComponent, plus the scene-native HealthComponent + WeaponComponent.
//   * 1 PLAYER TARGET (the thing the soldiers shoot at): scene-native Health
//     (hostile faction) + a Hitbox proxy so the AI hitscan can resolve a hit.
//   * A PsyGraph TRIGGER: an editor-authored OnTick -> ApplyDamage(self) graph
//     stored on a "trigger" entity via Scene::add_script_graph + a
//     scene::ScriptGraphComponent (exactly the editor IPC author flow). This is
//     the "on-event -> act" no-code script that must compile + run during Play.
//   * 2 STATIC COVER BODIES: scene::RigidBodyComponent (mass 0) boxes the player
//     can drive around / hide behind, placed off the firing line so they never
//     block the AI's shot.
//
// The flow proves the DoD bar end to end:
//   author -> save_scene_file (in-memory .psyscene) -> parse_scene_file ->
//   instantiate_scene_file (FRESH registry/scene) -> PlayRuntime.begin -> tick*N
//   -> ASSERT the level PLAYS -> end (clean strip).
//
// DoD assertions (all on the RELOADED scene, never the authored one):
//   1. VEHICLE PLAYS: begin() synthesised a live physics::vehicle (runtime handles
//      non-zero); under full throttle the chassis drives forward over the terrain
//      (z decreases, stays finite + near the surface).
//   2. AI ENGAGES: the soldier proxies synthesised into live ai::/gameplay::
//      components; the agents perceive + engage the hostile target -- their live
//      AiState advances to Chase/Attack AND the AI fire tally climbs (LOS clear,
//      shots resolved), draining + ultimately killing the target.
//   3. PSYGRAPH RUNS: the authored graph compiled + ran -- the scripted trigger
//      entity's health drained by exactly OnTick*amount over the ticks.
//   4. ROUND-TRIPS + STRIPS: end() tears down all runtime state -- vehicle handles
//      cleared, authored transform restored, synthesised gameplay/AI + PsyGraph
//      components stripped back off, leaving the editor scene proxy-only.
//
// Deterministic: fixed-step ticks, omni-FOV acquisition, no RNG, no wall clock.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/AiComponents.h"
#include "editor/play/PlayRuntime.h"
#include "gameplay/GameplayComponents.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/GameplayComponents.h"
#include "scene/SceneEcs.h"
#include "scene/SceneFile.h"
#include "scene/ScriptComponents.h"
#include "script/psygraph/Graph.h"
#include "script/psygraph/NodeTypes.h"
#include "script/psygraph/Serialize.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;
using Catch::Approx;

namespace {

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

scene::LocalTransform at(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

Entity find_entity_named(scene::Scene& scene, std::string_view name) {
    auto& registry = scene.registry();
    const u32 count = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(count);
    const u32 copied = registry.snapshot_live_entities(entities);
    entities.resize(copied);
    for (const Entity e : entities) {
        if (scene.entity_name(e) == name)
            return e;
    }
    return {};
}

// Author one no-code AI soldier purely from the scene-level proxies. It carries a
// scene-native Health (hostile to the target's faction) + Weapon, plus the
// Faction / Hitbox / WeaponMode / AiAgent / Perception / Patrol authoring proxies
// PlayRuntime maps onto the live gameplay::/ai:: components. fov_cos = -1 makes
// acquisition omnidirectional (and therefore deterministic). Returns the entity.
Entity author_ai_soldier(scene::Scene& scene,
                         std::string_view name,
                         math::Vec3 pos,
                         u32 faction) {
    auto& reg = scene.registry();
    const Entity e = scene.create_entity(at(pos));
    REQUIRE(scene.set_entity_name(e, name));

    scene::HealthComponent health{};  // scene-native
    health.max_health = 100.0f;
    health.current_health = 100.0f;
    health.faction = faction;
    reg.add<scene::HealthComponent>(e, scene::sanitize_health_component(health));

    scene::WeaponComponent weapon{};  // scene-native: hitscan params for the AI
    weapon.damage = 8.0f;
    weapon.range = 100.0f;
    weapon.fire_rate = 10.0f;  // 10 shots/s
    weapon.ammo = 10000u;
    reg.add<scene::WeaponComponent>(e, scene::sanitize_weapon_component(weapon));

    scene::FactionComponent fac{};  // proxy -> gameplay::FactionComponent
    fac.faction = faction;
    reg.add<scene::FactionComponent>(e, fac);

    scene::HitboxComponent hb{};  // proxy -> gameplay::HitboxComponent
    hb.radius = 0.5f;
    reg.add<scene::HitboxComponent>(e, scene::sanitize_hitbox_component(hb));

    scene::WeaponModeComponent wm{};  // proxy -> gameplay::WeaponRuntimeComponent
    wm.kind = scene::WeaponFireKind::Hitscan;
    reg.add<scene::WeaponModeComponent>(e, scene::sanitize_weapon_mode_component(wm));

    scene::AiAgentComponent agent{};  // proxy -> ai::AiAgentComponent
    agent.state = scene::AiState::Patrol;  // starts patrolling, escalates on sight
    agent.sight_range = 60.0f;
    agent.fov_cos = -1.0f;        // omnidirectional => deterministic acquisition
    agent.attack_range = 60.0f;   // always within attack range once seen
    agent.think_interval = 0.0f;  // re-evaluate every tick
    agent.move_speed = 0.0f;      // stationary turret (no drift into the line)
    reg.add<scene::AiAgentComponent>(e, scene::sanitize_ai_agent_component(agent));

    reg.add<scene::PerceptionComponent>(e, scene::PerceptionComponent{});

    scene::PatrolComponent patrol{};  // proxy -> ai::PatrolComponent (authored route)
    patrol.count = 2u;
    patrol.waypoints[0] = pos;
    patrol.waypoints[1] = {pos.x + 2.0f, pos.y, pos.z};
    patrol.wait_time = 1.0f;
    patrol.arrive_radius = 0.5f;
    reg.add<scene::PatrolComponent>(e, scene::sanitize_patrol_component(patrol));
    return e;
}

// Build the editor-authored trigger graph: OnTick -> ApplyDamage(<self>, amount).
// The Target pin is left unconnected (defaults to entity 0) so PlayRuntime's host
// substitutes the running graph's own entity (the documented self fallback). This
// is the same shape the no-code panel emits for an "on event -> act" trigger.
script::psygraph::Graph build_ontick_self_damage_graph(double amount) {
    using namespace script::psygraph;
    Graph g;
    g.variable_count = 0;

    auto add_node = [&](NodeTypeId type, std::vector<u64> params = {}) -> NodeIndex {
        Node n;
        n.type = type;
        n.params = std::move(params);
        g.nodes.push_back(std::move(n));
        return static_cast<NodeIndex>(g.nodes.size() - 1);
    };
    auto exec_edge = [&](NodeIndex from, u16 pin, NodeIndex to) {
        Edge e;
        e.kind = EdgeKind::Exec;
        e.from_node = from;
        e.from_pin = pin;
        e.to_node = to;
        g.edges.push_back(e);
    };
    auto data_edge = [&](NodeIndex from, u16 from_pin, NodeIndex to, u16 to_pin) {
        Edge e;
        e.kind = EdgeKind::Data;
        e.from_node = from;
        e.from_pin = from_pin;
        e.to_node = to;
        e.to_pin = to_pin;
        g.edges.push_back(e);
    };
    auto float_bits = [](double v) -> u64 {
        u64 bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    };

    const NodeIndex on_tick = add_node(NodeTypeId::OnTick);
    const NodeIndex amt = add_node(NodeTypeId::LiteralFloat, {float_bits(amount)});
    const NodeIndex dmg = add_node(NodeTypeId::ApplyDamage);
    // ApplyDamage pins: 0 = Target (Entity, unconnected => self), 1 = Amount.
    data_edge(amt, 0, dmg, 1);
    exec_edge(on_tick, 0, dmg);
    return g;
}

// Author the whole terrain-shooter level into `authored`, returning the cooked
// .psyscene bytes. Uses ONLY the public scene/editor authoring APIs.
void author_level(scene::Scene& authored,
                 scene::detail::AlignedVector<u8>& out_bytes,
                 scene::SceneFileSaveStats& stats) {
    // --- PLAYER: a heightfield-grounded drivable vehicle (Wave-8 no-code). ----
    // Parked away from the firefight (+X) so it never collides with the soldiers
    // or cover. Heightfield ground binding rides a gentle procedural surface.
    const Entity car = authored.create_entity(at({40.0f, 4.0f, 0.0f}));
    REQUIRE(car.valid());
    REQUIRE(authored.set_entity_name(car, "PlayerCar"));
    {
        scene::VehicleComponent vc{};
        vc.is_player = 1u;
        vc.ground_mode = scene::VehicleGroundMode::Heightfield;
        vc.hf_base_y = 1.0f;
        vc.hf_amplitude = 2.0f;
        vc.hf_frequency = 0.05f;
        vc.max_speed = 18.0f;
        vc.steer_full_speed = 4.0f;
        vc.steer_taper_speed = 12.0f;
        vc.steer_min_authority = 0.45f;
        authored.registry().add<scene::VehicleComponent>(car, vc);
    }

    // --- PLAYER TARGET: the hostile the soldiers engage (faction 1). ----------
    // Scene-native Health + a Hitbox proxy so the AI hitscan resolves a hit.
    const Entity target = authored.create_entity(at({6.0f, 0.0f, 0.0f}));
    REQUIRE(target.valid());
    REQUIRE(authored.set_entity_name(target, "PlayerTarget"));
    {
        scene::HealthComponent health{};
        health.max_health = 100.0f;
        health.current_health = 100.0f;
        health.faction = 1u;
        authored.registry().add<scene::HealthComponent>(
            target, scene::sanitize_health_component(health));
        scene::HitboxComponent hb{};
        hb.radius = 0.5f;
        authored.registry().add<scene::HitboxComponent>(
            target, scene::sanitize_hitbox_component(hb));
    }

    // --- 3 RANGED AI SOLDIERS (faction 2), no-code proxies only. --------------
    // Arranged in a loose arc around the origin, all within sight of the target.
    author_ai_soldier(authored, "Soldier_A", {0.0f, 0.0f, 0.0f}, /*faction*/ 2u);
    author_ai_soldier(authored, "Soldier_B", {-3.0f, 0.0f, 2.0f}, /*faction*/ 2u);
    author_ai_soldier(authored, "Soldier_C", {-2.0f, 0.0f, -3.0f}, /*faction*/ 2u);

    // --- PsyGraph TRIGGER: an editor-authored OnTick -> ApplyDamage(self) ------
    // graph stored on a trigger entity (the editor IPC author flow exactly).
    const Entity trigger = authored.create_entity(at({0.0f, 0.0f, 8.0f}));
    REQUIRE(trigger.valid());
    REQUIRE(authored.set_entity_name(trigger, "Trigger"));
    {
        scene::HealthComponent health{};  // the graph drains this each OnTick
        health.max_health = 100.0f;
        health.current_health = 100.0f;
        health.faction = 0u;  // neutral: no AI engages it (no Hitbox either)
        authored.registry().add<scene::HealthComponent>(
            trigger, scene::sanitize_health_component(health));

        const script::psygraph::Graph graph =
            build_ontick_self_damage_graph(/*amount*/ 2.0);
        std::vector<u8> blob;
        script::psygraph::serialize_graph(graph, blob);
        REQUIRE(!blob.empty());
        const u32 slot =
            authored.add_script_graph(std::span<const u8>{blob.data(), blob.size()});
        scene::ScriptGraphComponent comp{};
        comp.graph_slot = slot;
        authored.registry().add<scene::ScriptGraphComponent>(trigger, comp);
    }

    // --- 2 STATIC COVER BODIES: drive-around boxes, off the firing line. -------
    const Entity cover_a = authored.create_entity(at({44.0f, 1.0f, 4.0f}));
    REQUIRE(authored.set_entity_name(cover_a, "Cover_A"));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 0.0f;  // static
        rb.half_extent = math::Vec3{1.0f, 1.0f, 1.0f};
        authored.registry().add<scene::RigidBodyComponent>(cover_a, rb);
    }
    const Entity cover_b = authored.create_entity(at({44.0f, 1.0f, -4.0f}));
    REQUIRE(authored.set_entity_name(cover_b, "Cover_B"));
    {
        scene::RigidBodyComponent rb{};
        rb.shape = scene::ColliderShape::Box;
        rb.mass = 0.0f;  // static
        rb.half_extent = math::Vec3{1.0f, 1.0f, 1.0f};
        authored.registry().add<scene::RigidBodyComponent>(cover_b, rb);
    }

    std::string error;
    REQUIRE(scene::save_scene_file(authored, {}, out_bytes, &stats, &error));
    REQUIRE(error.empty());
}

}  // namespace

TEST_CASE(
    "DoD: an editor-authored terrain-shooter level round-trips through a .psyscene "
    "and PLAYS under PlayRuntime",
    "[play][editor][authoring][level][dod]") {
    // --- 1. AUTHOR the level + SAVE it to an in-memory .psyscene -------------
    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    {
        RegistryReset author_reset;
        auto& author_registry = scene::EcsRegistry::Get();
        author_registry.set_structural_deferred(false);
        scene::Scene authored{author_registry};
        author_level(authored, bytes, stats);
    }

    // The saved file carries every authored piece: the player car + 2 cover bodies
    // (3 physics bodies), and one PsyGraph blob. The SGAI chunk emits a record for
    // EVERY entity carrying any gameplay/AI proxy: the 3 soldiers (full proxy set)
    // PLUS the player target (its Hitbox proxy alone qualifies) => 4 records.
    REQUIRE(stats.physics_bodies == 3u);  // car + 2 cover
    REQUIRE(stats.vehicle_exts == 1u);    // the player car's v6 SVHX record
    REQUIRE(stats.gameplay_ai == 4u);     // 3 soldiers + target's Hitbox proxy
    REQUIRE(stats.script_graphs == 1u);   // the trigger graph

    // Optional: dump the authored level to a checked-in sample .psyscene asset so
    // it can be opened in the editor too. Gated behind an env var so a normal test
    // run never touches the filesystem (keeps the test deterministic + side-effect
    // free); the committed asset is regenerated by running this binary once with
    //   PSYNDER_DUMP_LEVEL_ASSET=<path> ./psynder_unit "[dod]"
    if (const char* dump_path = std::getenv("PSYNDER_DUMP_LEVEL_ASSET");
        dump_path != nullptr && dump_path[0] != '\0') {
        std::ofstream out(dump_path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }

    // --- 2. PARSE + RELOAD into a FRESH registry / scene --------------------
    scene::SceneFileView view{};
    std::string error;
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()},
                                    view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.header->version == scene::kPsySceneVersion);
    REQUIRE(view.physics_bodies.size() == 3u);
    REQUIRE(view.vehicle_exts.size() == 1u);
    REQUIRE(view.gameplay_ai.size() == 4u);  // 3 soldiers + target's Hitbox proxy
    REQUIRE(view.script_graphs.size() == 1u);

    RegistryReset reload_reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);
    scene::Scene scene{registry};
    std::array<scene::SceneMeshBinding, 0> mesh_bindings{};
    std::array<Entity, 0> out_mesh_entities{};
    (void)scene::instantiate_scene_file(scene, view, mesh_bindings, out_mesh_entities);

    // The reloaded scene re-acquired every authored entity + the PsyGraph blob.
    const Entity car = find_entity_named(scene, "PlayerCar");
    const Entity target = find_entity_named(scene, "PlayerTarget");
    const Entity soldier_a = find_entity_named(scene, "Soldier_A");
    const Entity soldier_b = find_entity_named(scene, "Soldier_B");
    const Entity soldier_c = find_entity_named(scene, "Soldier_C");
    const Entity trigger = find_entity_named(scene, "Trigger");
    REQUIRE(car.valid());
    REQUIRE(target.valid());
    REQUIRE(soldier_a.valid());
    REQUIRE(soldier_b.valid());
    REQUIRE(soldier_c.valid());
    REQUIRE(trigger.valid());
    REQUIRE(scene.script_graph_count() == 1u);

    // The reloaded car still carries the heightfield ground-binding authoring (no
    // C++ re-authored it -- it came straight off the .psyscene).
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->is_player == 1u);
        REQUIRE(vc->ground_mode == scene::VehicleGroundMode::Heightfield);
        // Runtime handles are never serialized.
        REQUIRE(vc->runtime_vehicle == 0u);
        REQUIRE(vc->runtime_chassis == 0u);
    }
    // The reloaded soldiers still carry the no-code AI proxies (not live yet).
    for (const Entity s : {soldier_a, soldier_b, soldier_c}) {
        REQUIRE(registry.get<scene::AiAgentComponent>(s) != nullptr);
        REQUIRE(registry.get<scene::FactionComponent>(s) != nullptr);
        REQUIRE(registry.get<scene::HitboxComponent>(s) != nullptr);
        // No live components exist before Play begins.
        REQUIRE(registry.get<ai::AiAgentComponent>(s) == nullptr);
        REQUIRE(registry.get<gameplay::FactionComponent>(s) == nullptr);
    }
    // Capture the authored car pose so we can prove end() restores it.
    const math::Vec3 car_authored = scene.transform(car).translation;
    const f32 trigger_start_hp =
        registry.get<scene::HealthComponent>(trigger)->current_health;

    // --- 3. PLAY: begin synthesises the live sim, tick runs it --------------
    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    // DoD assert (a): the VEHICLE synthesised into a live physics::vehicle.
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->runtime_vehicle != 0u);
        REQUIRE(vc->runtime_chassis != 0u);
    }
    // DoD assert (b-pre): the AI proxies synthesised into LIVE ai::/gameplay::
    // components the combat + AI systems consume.
    for (const Entity s : {soldier_a, soldier_b, soldier_c}) {
        REQUIRE(registry.get<ai::AiAgentComponent>(s) != nullptr);
        REQUIRE(registry.get<ai::PerceptionComponent>(s) != nullptr);
        REQUIRE(registry.get<gameplay::FactionComponent>(s) != nullptr);
        REQUIRE(registry.get<gameplay::HitboxComponent>(s) != nullptr);
        REQUIRE(registry.get<gameplay::WeaponRuntimeComponent>(s) != nullptr);
    }
    REQUIRE(registry.get<gameplay::HitboxComponent>(target) != nullptr);
    // No AI shots before any tick.
    REQUIRE(runtime.ai_shots_fired() == 0u);

    const f32 car_start_z = scene.transform(car).translation.z;
    const f32 dt = 1.0f / 120.0f;

    // --- DoD assert (c): the PsyGraph TRIGGER compiled + RAN ----------------
    // Observe the authored OnTick -> ApplyDamage(self) graph over a SHORT window
    // first, before any other damage source can touch the (neutral, hitbox-less)
    // trigger: each tick must drain exactly the authored amount from its health,
    // proving the editor-authored graph compiled + ran during Play. We capture the
    // evidence while the trigger is still alive (50 ticks would drain it to 0).
    constexpr int kScriptTicks = 10;
    constexpr f32 kScriptDamagePerTick = 2.0f;  // the authored LiteralFloat amount
    for (int step = 0; step < kScriptTicks; ++step)
        runtime.tick(scene, dt);
    {
        REQUIRE(registry.alive(trigger));
        const auto* hp = registry.get<scene::HealthComponent>(trigger);
        REQUIRE(hp != nullptr);
        INFO("trigger health " << trigger_start_hp << " -> " << hp->current_health
                               << " after " << kScriptTicks << " scripted ticks");
        // The graph fired every tick: exact drain = amount * ticks (clamped >= 0).
        REQUIRE(hp->current_health ==
                Approx(trigger_start_hp - kScriptDamagePerTick *
                                              static_cast<f32>(kScriptTicks)));
    }

    // Drive the player car forward (-Z) under full throttle from the first tick,
    // while the AI engages the target. The two stay independent (40 m apart, no
    // shared bodies). We LATCH whether any soldier's live FSM reached Chase/Attack
    // at ANY point during the engagement: the agents revert to Patrol once the
    // target dies, so a single end-of-loop snapshot could miss the window -- the
    // latch records the engagement deterministically regardless of when the kill
    // lands.
    runtime.set_vehicle_input(/*throttle*/ 1.0f, /*brake*/ 0.0f, /*steer*/ 0.0f);
    bool any_engaged = false;
    bool target_died = false;
    // Run a fixed window (no early exit): the car needs enough ticks to settle on
    // the terrain and gain ground speed, which outlasts the AI kill. We latch both
    // the AI engagement and the target's death as they happen.
    for (int step = 0; step < 720; ++step) {
        runtime.tick(scene, dt);
        for (const Entity s : {soldier_a, soldier_b, soldier_c}) {
            const auto* live = registry.get<ai::AiAgentComponent>(s);
            if (live != nullptr &&
                (live->state == ai::AiState::Chase || live->state == ai::AiState::Attack))
                any_engaged = true;
        }
        if (!registry.alive(target))
            target_died = true;
    }

    // DoD assert (b): the AI ENGAGED -- at least one agent's live FSM advanced to
    // Chase or Attack while the hostile lived (omni-FOV + clear LOS).
    INFO("at least one soldier reached Chase/Attack = " << any_engaged);
    REQUIRE(any_engaged);

    // DoD assert (a-cont): the VEHICLE PLAYS -- it responded to throttle and drove
    // forward over the terrain (z decreased) without sinking through or flying off.
    {
        const math::Vec3 end = scene.transform(car).translation;
        INFO("car drove z " << car_start_z << " -> " << end.z << ", y = " << end.y);
        REQUIRE(end.z < car_start_z - 0.5f);  // drove forward along -Z
        REQUIRE(std::isfinite(end.y));
        REQUIRE(end.y > -2.0f);  // rides near the procedural surface
        REQUIRE(end.y < 8.0f);
    }
    // DoD assert (b-cont): the AI fired (LOS clear, hitscan resolved) and the
    // combined fire drained + ultimately killed the hostile target.
    INFO("ai shots fired = " << runtime.ai_shots_fired());
    REQUIRE(runtime.ai_shots_fired() > 0u);
    REQUIRE(target_died);
    REQUIRE_FALSE(registry.alive(target));

    // --- 4. STOP: end() cleanly strips ALL runtime state -------------------
    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());

    // The vehicle handles cleared + the authored transform restored exactly.
    {
        const auto* vc = registry.get<scene::VehicleComponent>(car);
        REQUIRE(vc != nullptr);
        REQUIRE(vc->runtime_vehicle == 0u);
        REQUIRE(vc->runtime_chassis == 0u);
    }
    REQUIRE(scene.transform(car).translation.x == Approx(car_authored.x).margin(1e-3f));
    REQUIRE(scene.transform(car).translation.y == Approx(car_authored.y).margin(1e-3f));
    REQUIRE(scene.transform(car).translation.z == Approx(car_authored.z).margin(1e-3f));

    // The synthesised live gameplay/AI components were stripped back off, leaving
    // the editor scene in its proxy-only authoring state.
    for (const Entity s : {soldier_a, soldier_b, soldier_c}) {
        REQUIRE(registry.get<ai::AiAgentComponent>(s) == nullptr);
        REQUIRE(registry.get<gameplay::FactionComponent>(s) == nullptr);
        REQUIRE(registry.get<gameplay::WeaponRuntimeComponent>(s) == nullptr);
        // The no-code authoring proxies survive the round trip.
        REQUIRE(registry.get<scene::AiAgentComponent>(s) != nullptr);
        REQUIRE(registry.get<scene::FactionComponent>(s) != nullptr);
    }

    // --- 5. RE-PLAY: a second Play session re-synthesises cleanly -----------
    // Proves end() fully tore down the runtime so the authored level is replayable
    // (the borrowed heightfield sampler + handles + script bindings all reset).
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    REQUIRE(registry.get<scene::VehicleComponent>(car)->runtime_vehicle != 0u);
    REQUIRE(registry.get<ai::AiAgentComponent>(soldier_a) != nullptr);
    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
}
