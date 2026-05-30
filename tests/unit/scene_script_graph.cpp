// SPDX-License-Identifier: MIT
// Psynder — no-code visual-scripting editor lane tests. Covers:
//   * a scene-level authored PsyGraph blob round-trips through the .psyscene
//     SSCG/SCGB chunks (save -> parse -> instantiate identity) and re-attaches a
//     scene::ScriptGraphComponent on load;
//   * an OLDER v4 scene (no SSCG/SCGB chunk) still parses, loading with zero
//     authored graphs (backward compatibility);
//   * an editor-authored graph compiles to bytecode and, run on the VM with stub
//     host hooks, fires the expected action (ApplyDamage) when its condition is
//     true and does NOT when false.

#include <catch2/catch_test_macros.hpp>

#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneFile.h"
#include "scene/ScriptComponents.h"
#include "script/psygraph/Bytecode.h"
#include "script/psygraph/Compiler.h"
#include "script/psygraph/Graph.h"
#include "script/psygraph/Host.h"
#include "script/psygraph/NodeTypes.h"
#include "script/psygraph/Serialize.h"
#include "script/psygraph/Vm.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::script::psygraph;

namespace {

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

u64 float_bits(double v) {
    u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

NodeIndex add_node(Graph& g, NodeTypeId type, std::vector<u64> params = {}) {
    Node n;
    n.type = type;
    n.params = std::move(params);
    g.nodes.push_back(std::move(n));
    return static_cast<NodeIndex>(g.nodes.size() - 1);
}

void exec_edge(Graph& g, NodeIndex from, u16 pin, NodeIndex to) {
    Edge e;
    e.kind = EdgeKind::Exec;
    e.from_node = from;
    e.from_pin = pin;
    e.to_node = to;
    g.edges.push_back(e);
}

void data_edge(Graph& g, NodeIndex from, u16 from_pin, NodeIndex to, u16 to_pin) {
    Edge e;
    e.kind = EdgeKind::Data;
    e.from_node = from;
    e.from_pin = from_pin;
    e.to_node = to;
    e.to_pin = to_pin;
    g.edges.push_back(e);
}

// "On tick, if DeltaTime > 0.5, ApplyDamage(self, 5)". This is exactly the kind
// of graph a designer authors in the web panel: an event entry, a comparison, a
// branch, and an action. Target pin is left unconnected (entity 0) so the host's
// self-substitution applies it to the running entity.
Graph build_fire_on_tick_graph() {
    Graph g;
    g.variable_count = 0;

    const NodeIndex on_tick = add_node(g, NodeTypeId::OnTick);
    const NodeIndex half = add_node(g, NodeTypeId::LiteralFloat, {float_bits(0.5)});
    const NodeIndex cmp = add_node(g, NodeTypeId::Greater);
    const NodeIndex branch = add_node(g, NodeTypeId::Branch);
    const NodeIndex amount = add_node(g, NodeTypeId::LiteralFloat, {float_bits(5.0)});
    const NodeIndex damage = add_node(g, NodeTypeId::ApplyDamage);

    data_edge(g, on_tick, 0, cmp, 0);  // DeltaTime -> A
    data_edge(g, half, 0, cmp, 1);     // 0.5       -> B
    data_edge(g, cmp, 0, branch, 0);   // Greater   -> Condition
    // ApplyDamage.Target (pin 0, Entity) is left UNCONNECTED: the compiler feeds
    // the default entity 0, and the host substitutes the running entity (self).
    data_edge(g, amount, 0, damage, 1);  // 5.0 -> Amount

    exec_edge(g, on_tick, 0, branch);
    exec_edge(g, branch, 0, damage);  // True branch fires ApplyDamage

    return g;
}

}  // namespace

TEST_CASE("scene authored PsyGraph round-trips through the SSCG/SCGB chunks",
          "[scene][scene_file][psygraph][nocode]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    // Author a graph + store its serialized blob on a scene entity, exactly as
    // the editor IPC author flow does (Scene::add_script_graph + the component).
    const Graph authored_graph = build_fire_on_tick_graph();
    REQUIRE(validate_graph(authored_graph).empty());
    std::vector<u8> blob;
    serialize_graph(authored_graph, blob);
    REQUIRE(!blob.empty());

    scene::Scene authored{registry};
    scene::LocalTransform local{};
    local.translation = {1.0f, 2.0f, 3.0f};
    const Entity entity = authored.create_entity(local);
    REQUIRE(entity.valid());
    REQUIRE(authored.set_entity_name(entity, "Scripted Turret"));
    const u32 slot = authored.add_script_graph(std::span<const u8>{blob.data(), blob.size()});
    scene::ScriptGraphComponent comp{};
    comp.graph_slot = slot;
    registry.add<scene::ScriptGraphComponent>(entity, comp);

    const scene::SceneFileSaveHooks hooks{};
    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;
    REQUIRE(scene::save_scene_file(authored, hooks, bytes, &stats, &error));
    REQUIRE(error.empty());
    REQUIRE(stats.script_graphs == 1u);
    REQUIRE(stats.script_graph_blob_bytes == static_cast<u32>(blob.size()));

    // Parse: the view exposes one SSCG record + the SCGB blob pool, version v5.
    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.header != nullptr);
    REQUIRE(view.header->version == scene::kPsySceneVersion);
    REQUIRE(view.header->version == 5u);
    REQUIRE(view.script_graphs.size() == 1u);
    const scene::SceneFileScriptGraph& record = view.script_graphs[0];
    REQUIRE(record.blob_bytes == static_cast<u32>(blob.size()));
    REQUIRE(static_cast<usize>(record.blob_offset) + record.blob_bytes <=
            view.script_graph_blobs.size());

    // The persisted blob bytes are byte-identical to what we authored.
    const std::span<const u8> persisted =
        view.script_graph_blobs.subspan(record.blob_offset, record.blob_bytes);
    REQUIRE(std::vector<u8>(persisted.begin(), persisted.end()) == blob);

    // Instantiate into a fresh scene: the entity re-acquires a ScriptGraphComponent
    // and the stored blob round-trips to the same graph topology.
    RegistryReset reload_reset;
    auto& reload_registry = scene::EcsRegistry::Get();
    reload_registry.set_structural_deferred(false);
    scene::Scene loaded{reload_registry};
    const scene::SceneFileInstantiateResult instantiate =
        scene::instantiate_scene_file(loaded, view, {});
    (void)instantiate;

    REQUIRE(loaded.script_graph_count() == 1u);
    const u32 total = reload_registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = reload_registry.snapshot_live_entities(entities);
    entities.resize(copied);
    Entity loaded_entity{};
    for (const Entity e : entities) {
        if (reload_registry.get<scene::ScriptGraphComponent>(e) != nullptr) {
            loaded_entity = e;
            break;
        }
    }
    REQUIRE(loaded_entity.valid());
    const auto* loaded_comp = reload_registry.get<scene::ScriptGraphComponent>(loaded_entity);
    REQUIRE(loaded_comp != nullptr);
    REQUIRE(loaded_comp->graph_slot != scene::kInvalidScriptGraphSlot);

    const std::span<const u8> loaded_blob = loaded.script_graph(loaded_comp->graph_slot);
    REQUIRE(std::vector<u8>(loaded_blob.begin(), loaded_blob.end()) == blob);

    Graph reloaded_graph;
    std::string derr;
    REQUIRE(deserialize_graph(loaded_blob, reloaded_graph, derr));
    REQUIRE(derr.empty());
    REQUIRE(reloaded_graph.nodes.size() == authored_graph.nodes.size());
    REQUIRE(reloaded_graph.edges.size() == authored_graph.edges.size());
    REQUIRE(reloaded_graph.variable_count == authored_graph.variable_count);
    // The reloaded graph still compiles to a runnable program.
    const CompileResult cr = compile_graph(reloaded_graph);
    REQUIRE(cr.ok);
}

TEST_CASE("older v4 scene without the SSCG/SCGB chunk still loads (backward compat)",
          "[scene][scene_file][psygraph][nocode][compat]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    // Save a clean scene (no authored graphs => empty SSCG/SCGB chunks, which are
    // the LAST two chunks appended by the writer).
    scene::Scene authored{registry};
    const Entity entity = authored.create_entity();
    REQUIRE(entity.valid());

    const scene::SceneFileSaveHooks hooks{};
    scene::detail::AlignedVector<u8> v5_bytes;
    std::string error;
    REQUIRE(scene::save_scene_file(authored, hooks, v5_bytes, nullptr, &error));
    REQUIRE(error.empty());

    // Build a synthetic v4 file from the v5 bytes by dropping the two trailing
    // script-graph chunks (SSCG + SCGB) and stamping the header back to v4. This
    // produces exactly the byte shape a pre-v5 editor would have written: no
    // script-graph chunks at all. The parser must accept it (version <= current)
    // and treat the missing chunks as empty.
    std::vector<u8> v4(v5_bytes.begin(), v5_bytes.end());
    auto* header = reinterpret_cast<scene::SceneFileHeader*>(v4.data());
    REQUIRE(header->version == 5u);
    const u32 chunk_count = header->chunk_count;
    REQUIRE(chunk_count >= 2u);
    auto* chunks = reinterpret_cast<scene::SceneFileChunk*>(v4.data() + sizeof(scene::SceneFileHeader));

    // Find the smallest offset among the two script chunks; truncate the file
    // there and drop those table entries. (They were appended last, so this
    // safely lops off the tail.)
    u32 truncate_at = header->file_bytes;
    u32 removed = 0u;
    for (u32 i = 0; i < chunk_count; ++i) {
        if (chunks[i].type == scene::SceneFileChunkType::ScriptGraphs ||
            chunks[i].type == scene::SceneFileChunkType::ScriptGraphBlobs) {
            if (chunks[i].offset < truncate_at)
                truncate_at = chunks[i].offset;
            ++removed;
        }
    }
    REQUIRE(removed == 2u);

    // Compact the chunk table: keep only the non-script chunks.
    std::vector<scene::SceneFileChunk> kept;
    for (u32 i = 0; i < chunk_count; ++i) {
        if (chunks[i].type != scene::SceneFileChunkType::ScriptGraphs &&
            chunks[i].type != scene::SceneFileChunkType::ScriptGraphBlobs)
            kept.push_back(chunks[i]);
    }
    REQUIRE(kept.size() == static_cast<usize>(chunk_count) - 2u);

    // Truncate the byte buffer to drop the (trailing) script chunk data.
    v4.resize(truncate_at);
    // Rewrite header + chunk table in place.
    header = reinterpret_cast<scene::SceneFileHeader*>(v4.data());
    header->version = 4u;
    header->chunk_count = static_cast<u32>(kept.size());
    header->file_bytes = static_cast<u32>(v4.size());
    std::memcpy(v4.data() + sizeof(scene::SceneFileHeader),
                kept.data(),
                kept.size() * sizeof(scene::SceneFileChunk));

    // Parse the synthetic v4 file: succeeds, with no authored graphs.
    scene::SceneFileView view{};
    std::string parse_error;
    REQUIRE(scene::parse_scene_file(std::span<const u8>{v4.data(), v4.size()}, view, &parse_error));
    REQUIRE(parse_error.empty());
    REQUIRE(view.header != nullptr);
    REQUIRE(view.header->version == 4u);
    REQUIRE(view.script_graphs.empty());
    REQUIRE(view.script_graph_blobs.empty());

    // Instantiating it attaches no ScriptGraphComponent.
    RegistryReset reload_reset;
    auto& reload_registry = scene::EcsRegistry::Get();
    reload_registry.set_structural_deferred(false);
    scene::Scene loaded{reload_registry};
    (void)scene::instantiate_scene_file(loaded, view, {});
    REQUIRE(loaded.script_graph_count() == 0u);
}

TEST_CASE("authored graph compiles + runs and fires its action when the condition holds",
          "[scene][psygraph][nocode][vm]") {
    // Round-trip the authored graph through serialization first (this is what the
    // scene stores), then compile + run the reloaded form: end-to-end the same
    // path Play takes (deserialize scene blob -> compile -> run on the VM).
    const Graph authored_graph = build_fire_on_tick_graph();
    std::vector<u8> blob;
    serialize_graph(authored_graph, blob);

    Graph graph;
    std::string derr;
    REQUIRE(deserialize_graph(blob, graph, derr));

    REQUIRE(validate_graph(graph).empty());
    const CompileResult cr = compile_graph(graph);
    REQUIRE(cr.ok);
    REQUIRE(cr.diagnostic.empty());
    u32 off = 0;
    REQUIRE(cr.program.entry_for(EventKind::OnTick, off));

    int damage_hits = 0;
    u32 last_target = 0xFFFFFFFFu;
    double last_amount = 0.0;
    HostContext host;
    host.self_entity = 42u;  // the running entity; unconnected Target falls back to self
    // Mirror the PlayRuntime ApplyDamage hook contract (Host.h): an unconnected
    // Target pin arrives as entity 0, and the host substitutes the running
    // graph's own entity (self).
    host.apply_damage = [&](u32 entity, f64 amount) {
        ++damage_hits;
        last_target = entity != 0u ? entity : host.self_entity;
        last_amount = amount;
    };

    VmState state;
    state.reset_for(cr.program);
    Vm vm;

    // 4 ticks with dt = 1.0 (> 0.5): ApplyDamage fires each tick on `self`.
    host.delta_time = 1.0;
    for (int i = 0; i < 4; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(damage_hits == 4);
    REQUIRE(last_target == 42u);
    REQUIRE(last_amount == 5.0);

    // 3 ticks with dt = 0.1 (< 0.5): the branch goes False, no further fires.
    host.delta_time = 0.1;
    for (int i = 0; i < 3; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(damage_hits == 4);  // unchanged
}
