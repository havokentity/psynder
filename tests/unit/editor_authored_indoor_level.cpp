// SPDX-License-Identifier: MIT
// Psynder - LANE W10-1 Definition-of-Done checkpoint: an editor-authored PLAYABLE
// INDOOR-SHOOTER level round-trips through a .psyscene and PLAYS under
// PlayRuntime, with NO per-feature C++. This is the indoor sibling of the Wave-9
// terrain DoD (tests/unit/editor_authored_level.cpp) -- it shares that test's
// author -> save -> reload -> Play -> assert -> strip spine, but swaps the open
// rolling-hills surface for a BOX ROOM whose static geometry BLOCKS line of
// sight, so the discriminating assertion is COVER: a soldier behind a pillar
// has its LOS to the hostile occluded and never engages, while a soldier with a
// clear shot across the open room advances to Attack and its hitscan kills the
// target.
//
// Everything below is authored from the SCENE/EDITOR component model only -- the
// no-code gameplay/AI proxies (Faction / Hitbox / WeaponMode / AiAgent /
// Perception / Patrol), the scene-native Health / Weapon, the scene
// RigidBodyComponent (static box collision), and an editor-authored PsyGraph
// visual script. No engine internals, no bespoke gameplay C++.
//
// The level (authored entirely from the scene/component model):
//   * ROOM: four static RigidBodyComponent (mass 0) box WALLS + a FLOOR slab,
//     forming an enclosed box. Plus a tall central COVER PILLAR (another static
//     box) standing between the two ends of the room. These static bodies
//     synthesise into this Play session's physics world, so the AI LOS raycast
//     (PlayRuntime's world_.raycast) is genuinely occluded by them -- the same
//     mechanism games/shooter_demo/main.cpp exercises, but here driven purely
//     from authored RigidBody proxies through PlayRuntime instead of a bespoke
//     physics::World.
//   * HOSTILE TARGET (faction 1): scene-native Health + a Hitbox proxy so the AI
//     hitscan can resolve a hit, parked at the far +X end of the room.
//   * SOLDIER_CLEAR (faction 2): a no-code ranged AI on the SAME side of the
//     pillar as the target, with an open line across the room -> acquires LOS ->
//     Chase/Attack -> hitscan drains + kills the target.
//   * SOLDIER_SUPPORT (faction 2): a second clear-LOS soldier for a reliable kill.
//   * SOLDIER_BLOCKED (faction 2): a no-code ranged AI on the OPPOSITE side of
//     the pillar from the target, IN range + omni-FOV, so the ONLY thing between
//     it and the hostile is the static pillar body. Its LOS raycast is occluded
//     => its PerceptionComponent never reports can_see, it never reaches Attack,
//     and it never fires. move_speed 0 keeps it planted behind cover for the
//     whole session so the occlusion holds deterministically. THIS is the
//     discriminating indoor assertion versus the open-terrain test.
//   * PsyGraph TRIGGER: an editor-authored OnTick -> ApplyDamage(self) graph
//     stored on a trigger entity via Scene::add_script_graph + a
//     scene::ScriptGraphComponent (exactly the editor IPC author flow). The
//     "on-event -> act" no-code script that must compile + run during Play.
//
// The flow proves the DoD bar end to end:
//   author -> save_scene_file (in-memory .psyscene) -> parse_scene_file ->
//   instantiate_scene_file (FRESH registry/scene) -> PlayRuntime.begin -> tick*N
//   -> ASSERT the level PLAYS (with cover working) -> end (clean strip).
//
// DoD assertions (all on the RELOADED scene, never the authored one):
//   (a) SYNTHESIS: the AI proxies synthesise into live ai::/gameplay::
//       components the combat + AI systems consume.
//   (b) CLEAR LOS ENGAGES + KILLS: a clear-LOS soldier's live FSM advances to
//       Chase/Attack AND the AI fire tally climbs (LOS through the open room
//       works, the physics raycast resolves a clear shot), draining + ultimately
//       killing the hostile target.
//   (c) COVER WORKS (the discriminator): the soldier behind the pillar has its
//       LOS to the target BLOCKED by the static box -- its perception never
//       reports can_see, it NEVER reaches Attack, and it never fires. The static
//       cover body occludes the LOS raycast.
//   (d) PSYGRAPH RUNS: the authored OnTick -> ApplyDamage(self) graph compiled +
//       ran -- the scripted trigger entity's health drained by exactly
//       OnTick*amount over the observed window.
//   (e) ROUND-TRIPS + STRIPS: end() tears down all runtime state -- synthesised
//       gameplay/AI + PsyGraph components stripped back off, leaving the editor
//       scene proxy-only; a 2nd Play re-synthesises cleanly.
//
// Deterministic: fixed-step ticks, omni-FOV acquisition, stationary turrets, no
// RNG, no wall clock.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/AiComponents.h"
#include "editor/play/PlayRuntime.h"
#include "gameplay/GameplayComponents.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/GameplayComponents.h"
#include "scene/PhysicsComponents.h"
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

// Author one static (mass 0) collision box -- a wall / floor / cover pillar. The
// box synthesises into a static physics body in begin(), so the AI LOS raycast
// is occluded by it. Pure scene RigidBodyComponent authoring, no physics C++.
Entity author_static_box(scene::Scene& scene,
                         std::string_view name,
                         math::Vec3 center,
                         math::Vec3 half_extent) {
    const Entity e = scene.create_entity(at(center));
    REQUIRE(scene.set_entity_name(e, name));
    scene::RigidBodyComponent rb{};
    rb.shape = scene::ColliderShape::Box;
    rb.mass = 0.0f;  // static collider
    rb.half_extent = half_extent;
    scene.registry().add<scene::RigidBodyComponent>(e, rb);
    return e;
}

// Author one no-code ranged AI soldier purely from the scene-level proxies. It
// carries a scene-native Health (hostile to the target's faction) + Weapon, plus
// the Faction / Hitbox / WeaponMode / AiAgent / Perception / Patrol authoring
// proxies PlayRuntime maps onto the live gameplay::/ai:: components. fov_cos = -1
// makes acquisition omnidirectional (deterministic), so the ONLY thing that can
// keep a soldier from seeing the target is geometry (cover). move_speed 0 keeps
// it a stationary turret (the behind-cover soldier never drifts out of cover).
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
    agent.move_speed = 0.0f;      // stationary turret (stays in/behind cover)
    reg.add<scene::AiAgentComponent>(e, scene::sanitize_ai_agent_component(agent));

    reg.add<scene::PerceptionComponent>(e, scene::PerceptionComponent{});

    scene::PatrolComponent patrol{};  // proxy -> ai::PatrolComponent (authored route)
    patrol.count = 2u;
    patrol.waypoints[0] = pos;
    patrol.waypoints[1] = {pos.x, pos.y, pos.z + 1.0f};
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

// --- Room layout (axis-aligned box; metres). Forward axis is irrelevant: the
// LOS raycast is purely geometric. The firing line runs along +X. ----------
constexpr f32 kFloorY = 0.0f;
constexpr f32 kCeilY = 3.0f;
constexpr f32 kRoomX0 = -8.0f;
constexpr f32 kRoomX1 = 8.0f;
constexpr f32 kRoomZ0 = -4.0f;
constexpr f32 kRoomZ1 = 4.0f;
constexpr f32 kActorY = 0.9f;   // soldiers + target stand here (eye/centre height)
constexpr f32 kWallT = 0.3f;    // wall half-thickness

// Author the whole indoor-shooter level into `authored`, returning the cooked
// .psyscene bytes. Uses ONLY the public scene/editor authoring APIs.
void author_level(scene::Scene& authored,
                  scene::detail::AlignedVector<u8>& out_bytes,
                  scene::SceneFileSaveStats& stats) {
    const f32 cx = 0.5f * (kRoomX0 + kRoomX1);
    const f32 cz = 0.5f * (kRoomZ0 + kRoomZ1);
    const f32 cy = 0.5f * (kFloorY + kCeilY);
    const f32 hx = 0.5f * (kRoomX1 - kRoomX0);
    const f32 hy = 0.5f * (kCeilY - kFloorY);
    const f32 hz = 0.5f * (kRoomZ1 - kRoomZ0);

    // --- ROOM: a floor slab + four static box walls (all mass 0). -----------
    // The walls enclose the firefight; none sits on the +X firing line between a
    // clear soldier and the target, so they never block a CLEAR shot.
    author_static_box(authored, "Floor", {cx, kFloorY - kWallT, cz}, {hx, kWallT, hz});
    author_static_box(authored, "Wall_NegX", {kRoomX0 - kWallT, cy, cz}, {kWallT, hy, hz});
    author_static_box(authored, "Wall_PosX", {kRoomX1 + kWallT, cy, cz}, {kWallT, hy, hz});
    author_static_box(authored, "Wall_NegZ", {cx, cy, kRoomZ0 - kWallT}, {hx, hy, kWallT});
    author_static_box(authored, "Wall_PosZ", {cx, cy, kRoomZ1 + kWallT}, {hx, hy, kWallT});

    // --- COVER PILLAR: a tall static box at the room centre (x=0). It spans the
    // full floor-to-ceiling height and a band around x=0, so any LOS ray that
    // crosses x=0 at actor height is occluded. This is the cover the behind-the-
    // pillar soldier hides behind. ---------------------------------------------
    author_static_box(authored, "Pillar", {0.0f, cy, 0.0f}, {1.0f, hy, 1.0f});

    // --- HOSTILE TARGET (faction 1): far +X end, clear of the pillar. ---------
    // Scene-native Health + a Hitbox proxy so the AI hitscan resolves a hit.
    const Entity target = authored.create_entity(at({6.0f, kActorY, 0.0f}));
    REQUIRE(target.valid());
    REQUIRE(authored.set_entity_name(target, "Hostile"));
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

    // --- SOLDIERS (faction 2), no-code proxies only. -------------------------
    // CLEAR + SUPPORT sit on the SAME (+X) side as the target, x in [3,4]: the
    // ray to the target (x=6) never crosses the pillar band (x in [-1,1]) or any
    // wall, so LOS is clear and they engage + kill.
    author_ai_soldier(authored, "Soldier_Clear", {3.0f, kActorY, 0.0f}, /*faction*/ 2u);
    author_ai_soldier(authored, "Soldier_Support", {4.0f, kActorY, 1.5f}, /*faction*/ 2u);
    // BLOCKED sits on the OPPOSITE (-X) side at x=-6: the straight line to the
    // target at x=+6 passes through x=0 at actor height -- squarely through the
    // pillar box -- so its LOS is occluded. In range + omni-FOV, so cover is the
    // ONLY reason it cannot see. move_speed 0 keeps it planted behind the pillar.
    author_ai_soldier(authored, "Soldier_Blocked", {-6.0f, kActorY, 0.0f}, /*faction*/ 2u);

    // --- PsyGraph TRIGGER: an editor-authored OnTick -> ApplyDamage(self) ------
    // graph stored on a trigger entity (the editor IPC author flow exactly).
    const Entity trigger = authored.create_entity(at({0.0f, kActorY, 3.0f}));
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

    std::string error;
    REQUIRE(scene::save_scene_file(authored, {}, out_bytes, &stats, &error));
    REQUIRE(error.empty());
}

}  // namespace

TEST_CASE(
    "DoD: an editor-authored INDOOR-shooter level round-trips through a .psyscene "
    "and PLAYS under PlayRuntime, with cover blocking line of sight",
    "[play][editor][authoring][level][indoor][dod]") {
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

    // The saved file carries every authored piece. Physics bodies: floor + 4
    // walls + pillar = 6 static boxes. The SGAI chunk emits a record for EVERY
    // entity carrying any gameplay/AI proxy: the 3 soldiers (full proxy set) PLUS
    // the hostile target (its Hitbox proxy alone qualifies) => 4 records. The
    // trigger carries Health only (no proxy, no Hitbox), so it emits NO SGAI
    // record -- only its PsyGraph blob.
    REQUIRE(stats.physics_bodies == 6u);  // floor + 4 walls + pillar
    REQUIRE(stats.vehicle_exts == 0u);    // indoor level: no vehicles
    REQUIRE(stats.gameplay_ai == 4u);     // 3 soldiers + target's Hitbox proxy
    REQUIRE(stats.script_graphs == 1u);   // the trigger graph

    // Optional: dump the authored level to a checked-in sample .psyscene asset so
    // it can be opened in the editor too. Gated behind an env var so a normal test
    // run never touches the filesystem (keeps the test deterministic + side-effect
    // free); the committed asset is regenerated by running this binary once with
    //   PSYNDER_DUMP_INDOOR_ASSET=<path> ./psynder_unit "[indoor][dod]"
    if (const char* dump_path = std::getenv("PSYNDER_DUMP_INDOOR_ASSET");
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
    REQUIRE(view.physics_bodies.size() == 6u);
    REQUIRE(view.vehicle_exts.size() == 0u);
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
    const Entity pillar = find_entity_named(scene, "Pillar");
    const Entity target = find_entity_named(scene, "Hostile");
    const Entity clear = find_entity_named(scene, "Soldier_Clear");
    const Entity support = find_entity_named(scene, "Soldier_Support");
    const Entity blocked = find_entity_named(scene, "Soldier_Blocked");
    const Entity trigger = find_entity_named(scene, "Trigger");
    REQUIRE(pillar.valid());
    REQUIRE(target.valid());
    REQUIRE(clear.valid());
    REQUIRE(support.valid());
    REQUIRE(blocked.valid());
    REQUIRE(trigger.valid());
    REQUIRE(scene.script_graph_count() == 1u);

    // The reloaded pillar still carries the static RigidBody authoring (it came
    // straight off the .psyscene; no C++ re-authored it).
    {
        const auto* rb = registry.get<scene::RigidBodyComponent>(pillar);
        REQUIRE(rb != nullptr);
        REQUIRE(rb->mass == Approx(0.0f));  // static cover
        REQUIRE(rb->shape == scene::ColliderShape::Box);
        REQUIRE(rb->runtime_body == 0u);  // runtime handle is never serialized
    }
    // The reloaded soldiers still carry the no-code AI proxies (not live yet).
    for (const Entity s : {clear, support, blocked}) {
        REQUIRE(registry.get<scene::AiAgentComponent>(s) != nullptr);
        REQUIRE(registry.get<scene::FactionComponent>(s) != nullptr);
        REQUIRE(registry.get<scene::HitboxComponent>(s) != nullptr);
        // No live components exist before Play begins.
        REQUIRE(registry.get<ai::AiAgentComponent>(s) == nullptr);
        REQUIRE(registry.get<gameplay::FactionComponent>(s) == nullptr);
    }
    const f32 trigger_start_hp =
        registry.get<scene::HealthComponent>(trigger)->current_health;

    // --- 3. PLAY: begin synthesises the live sim, tick runs it --------------
    editor::play::PlayRuntime runtime;
    runtime.begin(scene);
    REQUIRE(runtime.playing());

    // DoD assert (a): the AI proxies synthesised into LIVE ai::/gameplay::
    // components the combat + AI systems consume. The static room boxes became
    // live physics bodies (body_count == 6).
    REQUIRE(runtime.body_count() == 6u);
    for (const Entity s : {clear, support, blocked}) {
        REQUIRE(registry.get<ai::AiAgentComponent>(s) != nullptr);
        REQUIRE(registry.get<ai::PerceptionComponent>(s) != nullptr);
        REQUIRE(registry.get<gameplay::FactionComponent>(s) != nullptr);
        REQUIRE(registry.get<gameplay::HitboxComponent>(s) != nullptr);
        REQUIRE(registry.get<gameplay::WeaponRuntimeComponent>(s) != nullptr);
    }
    REQUIRE(registry.get<gameplay::HitboxComponent>(target) != nullptr);
    // No AI shots before any tick.
    REQUIRE(runtime.ai_shots_fired() == 0u);

    const f32 dt = 1.0f / 120.0f;

    // --- DoD assert (d): the PsyGraph TRIGGER compiled + RAN -----------------
    // Observe the authored OnTick -> ApplyDamage(self) graph over a SHORT window
    // first, before any other damage source can touch the (neutral, hitbox-less)
    // trigger: each tick must drain exactly the authored amount from its health,
    // proving the editor-authored graph compiled + ran during Play. We capture the
    // evidence while the trigger is still alive (a long run would drain it to 0).
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
        REQUIRE(hp->current_health ==
                Approx(trigger_start_hp - kScriptDamagePerTick *
                                              static_cast<f32>(kScriptTicks)));
    }

    // --- Run the engagement window, LATCHING per-soldier evidence -----------
    // We latch (across ALL ticks, never a single end-of-loop snapshot) whether
    // each soldier's live FSM ever reached Chase/Attack, whether it ever reported
    // can_see, and whether it ever stood in Attack. The clear soldiers escalate +
    // fire; the behind-cover soldier must NEVER see or attack (cover holds). The
    // agents revert to Patrol once the target dies, so a single snapshot could
    // miss the window -- the latch records the truth regardless of timing.
    bool clear_engaged = false;     // a clear-LOS soldier reached Chase/Attack
    bool clear_attacked = false;    // a clear-LOS soldier reached Attack
    bool blocked_saw = false;       // the behind-cover soldier EVER reported can_see
    bool blocked_attacked = false;  // the behind-cover soldier EVER reached Attack
    bool target_died = false;
    for (int step = 0; step < 600; ++step) {
        runtime.tick(scene, dt);
        for (const Entity s : {clear, support}) {
            const auto* live = registry.get<ai::AiAgentComponent>(s);
            if (live != nullptr &&
                (live->state == ai::AiState::Chase || live->state == ai::AiState::Attack))
                clear_engaged = true;
            if (live != nullptr && live->state == ai::AiState::Attack)
                clear_attacked = true;
        }
        {
            const auto* live = registry.get<ai::AiAgentComponent>(blocked);
            const auto* sense = registry.get<ai::PerceptionComponent>(blocked);
            if (sense != nullptr && sense->can_see != 0u)
                blocked_saw = true;
            if (live != nullptr && live->state == ai::AiState::Attack)
                blocked_attacked = true;
        }
        if (!registry.alive(target))
            target_died = true;
    }

    // DoD assert (b): the CLEAR-LOS soldier ENGAGED -- its live FSM advanced to
    // Chase/Attack (and specifically reached Attack), the AI fired (LOS clear,
    // hitscan resolved through the open room), and the combined fire drained +
    // ultimately KILLED the hostile target.
    INFO("clear soldier reached Chase/Attack = " << clear_engaged
         << ", Attack = " << clear_attacked);
    REQUIRE(clear_engaged);
    REQUIRE(clear_attacked);
    INFO("ai shots fired = " << runtime.ai_shots_fired());
    REQUIRE(runtime.ai_shots_fired() > 0u);
    REQUIRE(target_died);
    REQUIRE_FALSE(registry.alive(target));

    // DoD assert (c) -- THE DISCRIMINATING INDOOR ASSERTION: COVER WORKS. The
    // soldier behind the pillar (in range, omni-FOV, only the static pillar box
    // between it and the hostile) had its LOS raycast OCCLUDED for the entire
    // engagement: its perception NEVER reported can_see and it NEVER reached the
    // Attack state. The static cover body genuinely blocked the line of sight --
    // exactly the indoor-shooter behaviour the open-terrain DoD cannot prove.
    INFO("behind-cover soldier ever saw target = " << blocked_saw
         << ", ever attacked = " << blocked_attacked);
    REQUIRE_FALSE(blocked_saw);
    REQUIRE_FALSE(blocked_attacked);

    // --- 4. STOP: end() cleanly strips ALL runtime state -------------------
    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());

    // The static pillar's runtime body handle is cleared back to 0.
    REQUIRE(registry.get<scene::RigidBodyComponent>(pillar)->runtime_body == 0u);

    // DoD assert (e): the synthesised live gameplay/AI components were stripped
    // back off, leaving the editor scene in its proxy-only authoring state.
    for (const Entity s : {clear, support, blocked}) {
        REQUIRE(registry.get<ai::AiAgentComponent>(s) == nullptr);
        REQUIRE(registry.get<gameplay::FactionComponent>(s) == nullptr);
        REQUIRE(registry.get<gameplay::WeaponRuntimeComponent>(s) == nullptr);
        // The no-code authoring proxies survive the round trip.
        REQUIRE(registry.get<scene::AiAgentComponent>(s) != nullptr);
        REQUIRE(registry.get<scene::FactionComponent>(s) != nullptr);
    }
    // NOTE: the neutral PsyGraph trigger entity was DESPAWNED during the long run
    // -- the authored OnTick -> ApplyDamage(self) graph drained its health to 0
    // and the combat death-resolution despawned it (it has no Hitbox, so only the
    // script could kill it). That the script could kill its own entity is the
    // strongest possible proof DoD (d) -- the graph compiled + ran. We therefore
    // do NOT assert on the trigger after the run; DoD (d) was already captured by
    // the exact-drain check in the short 10-tick window above, while it lived.
    REQUIRE_FALSE(registry.alive(trigger));

    // --- 5. RE-PLAY: a second Play session re-synthesises cleanly -----------
    // Proves end() fully tore down the runtime so the authored level is replayable
    // (the physics bodies, synthesised gameplay/AI, and script bindings all reset).
    runtime.begin(scene);
    REQUIRE(runtime.playing());
    REQUIRE(runtime.body_count() == 6u);
    REQUIRE(registry.get<ai::AiAgentComponent>(clear) != nullptr);
    REQUIRE(registry.get<gameplay::FactionComponent>(clear) != nullptr);
    runtime.end(scene);
    REQUIRE_FALSE(runtime.playing());
}
