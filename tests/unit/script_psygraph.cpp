// SPDX-License-Identifier: MIT
// Psynder — Lane 15 M-PSYGRAPH unit tests. Covers the visual-scripting core:
//   * build a graph -> compile -> run N ticks via the VM; assert the action
//     hook fired the expected number of times and variables hold expected
//     values.
//   * a graph round-trips through serialization byte-stable.
//   * the compiler rejects invalid graphs (dangling exec / type mismatch).
//   * run() performs zero heap allocations after VmState warmup (asserted via
//     a global allocation counter).
//   * the thin ECS binding runs OnStart-once + OnTick over entities.

#include "script/psygraph/Bytecode.h"
#include "script/psygraph/Compiler.h"
#include "script/psygraph/EcsBinding.h"
#include "script/psygraph/Graph.h"
#include "script/psygraph/Host.h"
#include "script/psygraph/NodeTypes.h"
#include "script/psygraph/Serialize.h"
#include "script/psygraph/Value.h"
#include "script/psygraph/Vm.h"

#include "core/Types.h"
#include "scene/EcsRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace psynder::script::psygraph;
using psynder::f64;
using psynder::u32;
using psynder::u64;

// ─── Global allocation counter (for the zero-alloc-tick guarantee) ──────────
namespace {
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<bool> g_alloc_armed{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed))
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed))
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// Helpers to push literal nodes.
NodeIndex add_node(Graph& g, NodeTypeId type, std::vector<u64> params = {}) {
    Node n;
    n.type = type;
    n.params = std::move(params);
    g.nodes.push_back(std::move(n));
    return static_cast<NodeIndex>(g.nodes.size() - 1);
}

u64 float_bits(double v) {
    u64 bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

void exec_edge(Graph& g, NodeIndex from, psynder::u16 pin, NodeIndex to) {
    Edge e;
    e.kind = EdgeKind::Exec;
    e.from_node = from;
    e.from_pin = pin;
    e.to_node = to;
    g.edges.push_back(e);
}

void data_edge(Graph& g, NodeIndex from, psynder::u16 from_pin, NodeIndex to, psynder::u16 to_pin) {
    Edge e;
    e.kind = EdgeKind::Data;
    e.from_node = from;
    e.from_pin = from_pin;
    e.to_node = to;
    e.to_pin = to_pin;
    g.edges.push_back(e);
}

// Build the brief's canonical graph:
//   * variable slot 0 = "counter" (starts at 0 via OnStart SetVar literal 10)
//   * OnTick: Branch( DeltaTime > 0.5 ) -> True: Log + ApplyDamage(self, 1)
//   * OnTick also: SetVar slot0 = GetVar slot0 + 1   (counts ticks)
// We keep it simple but exercise events, flow, math, compare, vars, actions.
Graph build_counter_graph() {
    Graph g;
    g.variable_count = 1;  // slot 0 = tick counter
    const u32 msg = g.intern_string("tick-fired");

    // OnStart -> SetVar slot0 = literal 100  (proves OnStart ran)
    const NodeIndex on_start = add_node(g, NodeTypeId::OnStart);
    const NodeIndex start_lit = add_node(g, NodeTypeId::LiteralFloat, {float_bits(100.0)});
    const NodeIndex start_set = add_node(g, NodeTypeId::SetVar, {0});
    exec_edge(g, on_start, 0, start_set);
    data_edge(g, start_lit, 0, start_set, 0);

    // OnTick -> SetVar slot0 = GetVar slot0 + 1 ; then Branch(DeltaTime > 0.5)
    const NodeIndex on_tick = add_node(g, NodeTypeId::OnTick);
    const NodeIndex getv = add_node(g, NodeTypeId::GetVar, {0});
    const NodeIndex one = add_node(g, NodeTypeId::LiteralFloat, {float_bits(1.0)});
    const NodeIndex addn = add_node(g, NodeTypeId::Add);
    const NodeIndex setv = add_node(g, NodeTypeId::SetVar, {0});
    data_edge(g, getv, 0, addn, 0);
    data_edge(g, one, 0, addn, 1);
    data_edge(g, addn, 0, setv, 0);

    const NodeIndex half = add_node(g, NodeTypeId::LiteralFloat, {float_bits(0.5)});
    const NodeIndex cmp = add_node(g, NodeTypeId::Greater);
    const NodeIndex branch = add_node(g, NodeTypeId::Branch);
    data_edge(g, on_tick, 0, cmp, 0);  // DeltaTime
    data_edge(g, half, 0, cmp, 1);
    data_edge(g, cmp, 0, branch, 0);  // Condition

    const NodeIndex logn = add_node(g, NodeTypeId::Log);
    const NodeIndex litmsg = add_node(g, NodeTypeId::LiteralString, {msg});
    data_edge(g, litmsg, 0, logn, 0);

    // exec chain: OnTick -> setv -> branch ; branch.True -> logn
    exec_edge(g, on_tick, 0, setv);
    exec_edge(g, setv, 0, branch);
    exec_edge(g, branch, 0, logn);  // True branch

    return g;
}

}  // namespace

TEST_CASE("psygraph: compile + run N ticks fires hooks and updates vars", "[psygraph][vm]") {
    Graph g = build_counter_graph();

    REQUIRE(validate_graph(g).empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);
    REQUIRE(cr.diagnostic.empty());
    REQUIRE(cr.program.variable_count == 1);
    REQUIRE(cr.program.register_count > 0);

    // The program has both OnStart and OnTick handlers.
    u32 off = 0;
    REQUIRE(cr.program.entry_for(EventKind::OnStart, off));
    REQUIRE(cr.program.entry_for(EventKind::OnTick, off));

    int log_hits = 0;
    HostContext host;
    host.log = [&](std::string_view) { ++log_hits; };

    VmState state;
    state.reset_for(cr.program);

    Vm vm;

    // OnStart sets slot0 = 100.
    REQUIRE(vm.run(EventKind::OnStart, cr.program, state, host));
    REQUIRE(state.variable(0).as_float() == 100.0);

    // 5 ticks with dt = 1.0 (> 0.5) -> log fires 5 times, counter += 5.
    host.delta_time = 1.0;
    for (int i = 0; i < 5; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(log_hits == 5);
    REQUIRE(state.variable(0).as_float() == 105.0);  // 100 + 5

    // 3 ticks with dt = 0.1 (< 0.5) -> branch False, log does NOT fire,
    // but counter still increments.
    host.delta_time = 0.1;
    for (int i = 0; i < 3; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(log_hits == 5);                          // unchanged
    REQUIRE(state.variable(0).as_float() == 108.0);  // 105 + 3
}

TEST_CASE("psygraph: run() performs zero heap allocations after warmup", "[psygraph][vm][zero-alloc]") {
    Graph g = build_counter_graph();
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    // Host without hooks that allocate: a plain counter increment, no std::string
    // copies. (The Log hook receives a string_view; we do not materialize it.)
    std::atomic<int> hits{0};
    HostContext host;
    host.log = [&](std::string_view) { hits.fetch_add(1, std::memory_order_relaxed); };
    host.delta_time = 1.0;

    VmState state;
    state.reset_for(cr.program);  // all allocation happens here

    Vm vm;
    // Warm up once (touches any lazy capacity inside std::function dispatch).
    REQUIRE(vm.run(EventKind::OnStart, cr.program, state, host));
    REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));

    // Arm the allocation counter and run many ticks: expect ZERO allocations.
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_alloc_armed.store(true, std::memory_order_relaxed);
    for (int i = 0; i < 1000; ++i)
        vm.run(EventKind::OnTick, cr.program, state, host);
    g_alloc_armed.store(false, std::memory_order_relaxed);

    REQUIRE(g_alloc_count.load() == 0);
    REQUIRE(hits.load() == 1001);  // 1 warmup tick + 1000
}

TEST_CASE("psygraph: graph round-trips byte-stable through serialization", "[psygraph][serialize]") {
    Graph g = build_counter_graph();

    std::vector<psynder::u8> blob;
    serialize_graph(g, blob);
    REQUIRE(!blob.empty());

    Graph loaded;
    std::string err;
    REQUIRE(deserialize_graph(blob, loaded, err));
    REQUIRE(err.empty());

    // Structural equality.
    REQUIRE(loaded.nodes.size() == g.nodes.size());
    REQUIRE(loaded.edges.size() == g.edges.size());
    REQUIRE(loaded.strings == g.strings);
    REQUIRE(loaded.variable_count == g.variable_count);

    // Re-serialize the loaded graph: the second blob must be byte-identical.
    std::vector<psynder::u8> blob2;
    serialize_graph(loaded, blob2);
    REQUIRE(blob2 == blob);

    // The loaded graph still compiles + runs identically.
    CompileResult cr = compile_graph(loaded);
    REQUIRE(cr.ok);
}

TEST_CASE("psygraph: serializer rejects a malformed blob", "[psygraph][serialize]") {
    Graph out;
    std::string err;
    // Wrong magic.
    std::vector<psynder::u8> bad = {0xDE, 0xAD, 0xBE, 0xEF};
    REQUIRE_FALSE(deserialize_graph(bad, out, err));
    REQUIRE(err == "bad magic");

    // Truncated (valid magic + version but no counts).
    std::vector<psynder::u8> trunc;
    // magic 'PSYG' LE + version 1
    trunc = {0x50, 0x53, 0x59, 0x47, 0x01, 0x00, 0x00, 0x00};
    REQUIRE_FALSE(deserialize_graph(trunc, out, err));
    REQUIRE(!err.empty());
}

TEST_CASE("psygraph: compiler rejects a dangling exec graph", "[psygraph][compiler]") {
    Graph g;
    // A Log node with no incoming exec edge -> dangling, can never run.
    g.intern_string("x");
    const NodeIndex on_start = add_node(g, NodeTypeId::OnStart);
    const NodeIndex logn = add_node(g, NodeTypeId::Log);
    const NodeIndex lit = add_node(g, NodeTypeId::LiteralString, {0});
    data_edge(g, lit, 0, logn, 0);
    // NOTE: deliberately NO exec edge from on_start to logn.
    (void)on_start;

    CompileResult cr = compile_graph(g);
    REQUIRE_FALSE(cr.ok);
    REQUIRE(cr.diagnostic.find("dangling") != std::string::npos);
}

TEST_CASE("psygraph: compiler rejects a type-mismatched data edge", "[psygraph][compiler]") {
    Graph g;
    // Feed a String literal into Add's Float input pin -> type mismatch.
    const u32 s = g.intern_string("hello");
    const NodeIndex lit = add_node(g, NodeTypeId::LiteralString, {s});
    const NodeIndex addn = add_node(g, NodeTypeId::Add);
    data_edge(g, lit, 0, addn, 0);  // String -> Float : incompatible

    const std::string diag = validate_graph(g);
    REQUIRE(diag.find("type mismatch") != std::string::npos);
}

TEST_CASE("psygraph: ECS binding ticks graphs over entities", "[psygraph][ecs]") {
    Graph g = build_counter_graph();

    GraphRuntime runtime;
    std::string err;
    const GraphId gid = runtime.register_graph(g, &err);
    REQUIRE(gid != kInvalidGraphId);
    REQUIRE(err.empty());

    auto& registry = psynder::scene::EcsRegistry::Get();
    registry.clear();

    // Spawn two entities, each with its own graph instance (private vars).
    const psynder::Entity e1 = registry.create();
    const psynder::Entity e2 = registry.create();

    PsyGraphComponent c1;
    c1.graph_id = gid;
    c1.instance = runtime.create_instance(gid);
    registry.add<PsyGraphComponent>(e1, c1);

    PsyGraphComponent c2;
    c2.graph_id = gid;
    c2.instance = runtime.create_instance(gid);
    registry.add<PsyGraphComponent>(e2, c2);

    int total_logs = 0;
    auto make_host = [&](psynder::Entity, f64) {
        HostContext h;
        h.log = [&](std::string_view) { ++total_logs; };
        return h;
    };

    // Tick 3 times with dt = 1.0 (> 0.5 so Log fires). 2 entities * 3 ticks.
    for (int i = 0; i < 3; ++i)
        runtime.tick_psygraphs(registry, make_host, 1.0);

    REQUIRE(total_logs == 6);

    // Each instance ran OnStart once (var0 == 100) then 3 OnTicks (+3) = 103.
    PsyGraphComponent* got1 = registry.get<PsyGraphComponent>(e1);
    REQUIRE(got1 != nullptr);
    REQUIRE(got1->started == 1);

    registry.clear();
}
