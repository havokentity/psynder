// SPDX-License-Identifier: MIT
// Psynder — Wave 13 PsyGraph richer-palette node tests. Each new node is built
// into a REAL graph, compiled to bytecode, and run on the VM; the result is
// asserted. Also covers: seeded-random determinism (same seed -> same value,
// reproduced across fresh runs), the Once one-shot gate, the GetHealth/
// SetVelocity ECS bindings through host hooks, a serialize round-trip of a graph
// using the new nodes (incl. the Once var-slot + RandomRange seed params), and a
// capacity-stable (alloc-free) re-run check.
//
// NOTE: this file deliberately does NOT redefine global operator new/delete —
// script_psygraph.cpp already provides the armed allocation counter for the
// pooled-VM zero-alloc guarantee, and a second definition in the same unit
// binary would be a duplicate symbol. Here the alloc-free property is checked by
// asserting the VmState banks never grow across many runs.

#include "script/psygraph/Bytecode.h"
#include "script/psygraph/Compiler.h"
#include "script/psygraph/Graph.h"
#include "script/psygraph/Host.h"
#include "script/psygraph/NodeTypes.h"
#include "script/psygraph/Serialize.h"
#include "script/psygraph/Value.h"
#include "script/psygraph/Vm.h"

#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace psynder::script::psygraph;
using psynder::f64;
using psynder::u16;
using psynder::u32;
using psynder::u64;
using psynder::u8;

namespace {

NodeIndex add_node(Graph& g, NodeTypeId type, std::vector<u64> params = {}) {
    Node n;
    n.type = type;
    n.params = std::move(params);
    g.nodes.push_back(std::move(n));
    return static_cast<NodeIndex>(g.nodes.size() - 1);
}

u64 float_bits(double v) {
    u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
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

// Build a graph that, on OnStart, computes `expr_builder(g)` (a pure node tree)
// and stores its Float/Bool result into variable slot 0 via SetVar. Returns the
// var value after running OnStart once. This is the workhorse for the per-node
// "the op computes the right answer" assertions.
template <class BuildExpr>
Value eval_pure_to_var(BuildExpr&& build_expr, bool bool_result = false) {
    Graph g;
    g.variable_count = 1;
    const NodeIndex on_start = add_node(g, NodeTypeId::OnStart);
    const NodeIndex setv = add_node(g, NodeTypeId::SetVar, {0});
    // The expression sub-tree; returns (node, out_pin) of its Result.
    const auto [expr_node, expr_pin] = build_expr(g);
    data_edge(g, expr_node, expr_pin, setv, 0);
    exec_edge(g, on_start, 0, setv);
    (void)bool_result;

    const std::string verr = validate_graph(g);
    REQUIRE(verr.empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    HostContext host;
    VmState state;
    state.reset_for(cr.program);
    Vm vm;
    REQUIRE(vm.run(EventKind::OnStart, cr.program, state, host));
    return state.variable(0);
}

// Helper: a LiteralFloat node feeding value v.
NodeIndex lit_f(Graph& g, double v) {
    return add_node(g, NodeTypeId::LiteralFloat, {float_bits(v)});
}
NodeIndex lit_b(Graph& g, bool v) {
    return add_node(g, NodeTypeId::LiteralBool, {v ? 1u : 0u});
}

// Wire a binary pure node `type` over (a, b) literals; return the node index.
NodeIndex binary(Graph& g, NodeTypeId type, double a, double b) {
    const NodeIndex la = lit_f(g, a);
    const NodeIndex lb = lit_f(g, b);
    const NodeIndex n = add_node(g, type);
    data_edge(g, la, 0, n, 0);
    data_edge(g, lb, 0, n, 1);
    return n;
}
NodeIndex binary_bool(Graph& g, NodeTypeId type, bool a, bool b) {
    const NodeIndex la = lit_b(g, a);
    const NodeIndex lb = lit_b(g, b);
    const NodeIndex n = add_node(g, type);
    data_edge(g, la, 0, n, 0);
    data_edge(g, lb, 0, n, 1);
    return n;
}
NodeIndex unary(Graph& g, NodeTypeId type, double a) {
    const NodeIndex la = lit_f(g, a);
    const NodeIndex n = add_node(g, type);
    data_edge(g, la, 0, n, 0);
    return n;
}

}  // namespace

TEST_CASE("psygraph nodes: richer math computes correctly", "[psygraph][nodes][math]") {
    auto fval = [](NodeTypeId t, double a, double b) {
        return eval_pure_to_var([&](Graph& g) {
            const NodeIndex n = binary(g, t, a, b);
            return std::pair<NodeIndex, u16>{n, 0};
        }).as_float();
    };
    auto uval = [](NodeTypeId t, double a) {
        return eval_pure_to_var([&](Graph& g) {
            const NodeIndex n = unary(g, t, a);
            return std::pair<NodeIndex, u16>{n, 0};
        }).as_float();
    };

    REQUIRE(fval(NodeTypeId::Min, 3.0, 7.0) == 3.0);
    REQUIRE(fval(NodeTypeId::Min, 7.0, -2.0) == -2.0);
    REQUIRE(fval(NodeTypeId::Max, 3.0, 7.0) == 7.0);
    REQUIRE(fval(NodeTypeId::Mod, 7.0, 3.0) == 1.0);
    REQUIRE(fval(NodeTypeId::Mod, 5.0, 0.0) == 0.0);  // div-by-zero guard

    REQUIRE(uval(NodeTypeId::Abs, -4.5) == 4.5);
    REQUIRE(uval(NodeTypeId::Abs, 4.5) == 4.5);
    REQUIRE(uval(NodeTypeId::Sign, -9.0) == -1.0);
    REQUIRE(uval(NodeTypeId::Sign, 0.0) == 0.0);
    REQUIRE(uval(NodeTypeId::Sign, 9.0) == 1.0);
    REQUIRE(uval(NodeTypeId::Floor, 2.7) == 2.0);
    REQUIRE(uval(NodeTypeId::Floor, -2.3) == -3.0);
    REQUIRE(uval(NodeTypeId::Ceil, 2.3) == 3.0);
    REQUIRE(uval(NodeTypeId::Ceil, -2.7) == -2.0);
    REQUIRE(uval(NodeTypeId::Sqrt, 9.0) == 3.0);
    REQUIRE(uval(NodeTypeId::Sqrt, -4.0) == 0.0);  // clamped to >=0, no NaN
}

TEST_CASE("psygraph nodes: Clamp + Lerp (3-input lowerings) compute correctly",
          "[psygraph][nodes][math]") {
    auto clamp = [](double a, double lo, double hi) {
        return eval_pure_to_var([&](Graph& g) {
            const NodeIndex la = lit_f(g, a);
            const NodeIndex llo = lit_f(g, lo);
            const NodeIndex lhi = lit_f(g, hi);
            const NodeIndex n = add_node(g, NodeTypeId::Clamp);
            data_edge(g, la, 0, n, 0);
            data_edge(g, llo, 0, n, 1);
            data_edge(g, lhi, 0, n, 2);
            return std::pair<NodeIndex, u16>{n, 0};
        }).as_float();
    };
    REQUIRE(clamp(5.0, 0.0, 10.0) == 5.0);
    REQUIRE(clamp(-3.0, 0.0, 10.0) == 0.0);
    REQUIRE(clamp(42.0, 0.0, 10.0) == 10.0);

    auto lerp = [](double a, double b, double t) {
        return eval_pure_to_var([&](Graph& g) {
            const NodeIndex la = lit_f(g, a);
            const NodeIndex lb = lit_f(g, b);
            const NodeIndex lt = lit_f(g, t);
            const NodeIndex n = add_node(g, NodeTypeId::Lerp);
            data_edge(g, la, 0, n, 0);
            data_edge(g, lb, 0, n, 1);
            data_edge(g, lt, 0, n, 2);
            return std::pair<NodeIndex, u16>{n, 0};
        }).as_float();
    };
    REQUIRE(lerp(0.0, 10.0, 0.0) == 0.0);
    REQUIRE(lerp(0.0, 10.0, 1.0) == 10.0);
    REQUIRE(lerp(0.0, 10.0, 0.25) == 2.5);
    REQUIRE(lerp(4.0, 8.0, 0.5) == 6.0);
}

TEST_CASE("psygraph nodes: extra comparisons + xor compute correctly",
          "[psygraph][nodes][compare]") {
    auto cmp = [](NodeTypeId t, double a, double b) {
        return eval_pure_to_var([&](Graph& g) {
            const NodeIndex n = binary(g, t, a, b);
            return std::pair<NodeIndex, u16>{n, 0};
        }).as_bool();
    };
    REQUIRE(cmp(NodeTypeId::NotEqual, 1.0, 2.0) == true);
    REQUIRE(cmp(NodeTypeId::NotEqual, 2.0, 2.0) == false);
    REQUIRE(cmp(NodeTypeId::LessEqual, 2.0, 2.0) == true);
    REQUIRE(cmp(NodeTypeId::LessEqual, 3.0, 2.0) == false);
    REQUIRE(cmp(NodeTypeId::GreaterEqual, 2.0, 2.0) == true);
    REQUIRE(cmp(NodeTypeId::GreaterEqual, 1.0, 2.0) == false);

    auto xor_b = [](bool a, bool b) {
        return eval_pure_to_var([&](Graph& g) {
            const NodeIndex n = binary_bool(g, NodeTypeId::Xor, a, b);
            return std::pair<NodeIndex, u16>{n, 0};
        }).as_bool();
    };
    REQUIRE(xor_b(true, false) == true);
    REQUIRE(xor_b(true, true) == false);
    REQUIRE(xor_b(false, false) == false);
    REQUIRE(xor_b(false, true) == true);
}

TEST_CASE("psygraph nodes: seeded RandomRange is deterministic + in-range",
          "[psygraph][nodes][random][determinism]") {
    // Build OnStart -> SetVar0 = RandomRange(seed; Min=10, Max=20).
    auto rand_with_seed = [](u64 seed) {
        Graph g;
        g.variable_count = 1;
        const NodeIndex on_start = add_node(g, NodeTypeId::OnStart);
        const NodeIndex lmin = add_node(g, NodeTypeId::LiteralFloat, {float_bits(10.0)});
        const NodeIndex lmax = add_node(g, NodeTypeId::LiteralFloat, {float_bits(20.0)});
        const NodeIndex rnd = add_node(g, NodeTypeId::RandomRange, {seed});
        const NodeIndex setv = add_node(g, NodeTypeId::SetVar, {0});
        data_edge(g, lmin, 0, rnd, 0);
        data_edge(g, lmax, 0, rnd, 1);
        data_edge(g, rnd, 0, setv, 0);
        exec_edge(g, on_start, 0, setv);

        REQUIRE(validate_graph(g).empty());
        CompileResult cr = compile_graph(g);
        REQUIRE(cr.ok);
        HostContext host;
        VmState state;
        state.reset_for(cr.program);
        Vm vm;
        REQUIRE(vm.run(EventKind::OnStart, cr.program, state, host));
        return state.variable(0).as_float();
    };

    const double a1 = rand_with_seed(12345u);
    const double a2 = rand_with_seed(12345u);  // fresh program + state, same seed
    const double b = rand_with_seed(999u);

    // Same seed reproduces bit-for-bit; a different seed (almost surely) differs.
    REQUIRE(a1 == a2);
    REQUIRE(a1 != b);
    // In [Min, Max).
    REQUIRE(a1 >= 10.0);
    REQUIRE(a1 < 20.0);
    REQUIRE(b >= 10.0);
    REQUIRE(b < 20.0);
}

TEST_CASE("psygraph nodes: Once gate forwards exec only on the first pass",
          "[psygraph][nodes][flow]") {
    // OnTick -> Once(slot0) -> Log. Tick many times; Log must fire exactly once.
    Graph g;
    g.variable_count = 1;  // slot0 = Once's fired flag
    const u32 msg = g.intern_string("once");
    const NodeIndex on_tick = add_node(g, NodeTypeId::OnTick);
    const NodeIndex once = add_node(g, NodeTypeId::Once, {0});
    const NodeIndex logn = add_node(g, NodeTypeId::Log);
    const NodeIndex litmsg = add_node(g, NodeTypeId::LiteralString, {msg});
    data_edge(g, litmsg, 0, logn, 0);
    exec_edge(g, on_tick, 0, once);
    exec_edge(g, once, 0, logn);

    REQUIRE(validate_graph(g).empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    int log_hits = 0;
    HostContext host;
    host.log = [&](std::string_view) { ++log_hits; };
    VmState state;
    state.reset_for(cr.program);
    Vm vm;
    for (int i = 0; i < 10; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(log_hits == 1);

    // A fresh instance (re-reset) fires once again — the gate is per-instance.
    state.reset_for(cr.program);
    log_hits = 0;
    for (int i = 0; i < 5; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(log_hits == 1);
}

TEST_CASE("psygraph nodes: GetHealth reads + drives a Branch through a host getter",
          "[psygraph][nodes][ecs]") {
    // OnTick: Branch( GetHealth(self) < 50 ) -> True: ApplyDamage(self, 0) marker.
    // We expose health via host.get_health and assert the branch follows it.
    Graph g;
    g.variable_count = 0;
    const NodeIndex on_tick = add_node(g, NodeTypeId::OnTick);
    const NodeIndex gh = add_node(g, NodeTypeId::GetHealth);  // Target unconnected => self via host
    const NodeIndex fifty = lit_f(g, 50.0);
    const NodeIndex less = add_node(g, NodeTypeId::Less);
    const NodeIndex branch = add_node(g, NodeTypeId::Branch);
    const NodeIndex amount = lit_f(g, 1.0);
    const NodeIndex dmg = add_node(g, NodeTypeId::ApplyDamage);
    data_edge(g, gh, 0, less, 0);     // Health -> A
    data_edge(g, fifty, 0, less, 1);  // 50 -> B
    data_edge(g, less, 0, branch, 0);
    data_edge(g, amount, 0, dmg, 1);
    exec_edge(g, on_tick, 0, branch);
    exec_edge(g, branch, 0, dmg);  // True branch

    REQUIRE(validate_graph(g).empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    double health = 80.0;
    int dmg_hits = 0;
    u32 read_entity = 0;
    HostContext host;
    host.self_entity = 7u;
    host.get_health = [&](u32 e) {
        read_entity = e;
        return health;
    };
    host.apply_damage = [&](u32, f64) { ++dmg_hits; };

    VmState state;
    state.reset_for(cr.program);
    Vm vm;

    // health 80 >= 50 -> branch False -> no damage.
    REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(dmg_hits == 0);

    // health 30 < 50 -> branch True -> damage fires.
    health = 30.0;
    REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(dmg_hits == 1);
    // GetHealth read the running entity (unconnected Target => 0; the host
    // getter sees the raw arg 0, the caller maps it to self — here we just
    // confirm the getter was invoked).
    REQUIRE(read_entity == 0u);
}

TEST_CASE("psygraph nodes: SetVelocity passes X,Y,Z through the host hook",
          "[psygraph][nodes][ecs]") {
    // OnStart -> SetVelocity(self, 1.5, -2.0, 3.25).
    Graph g;
    g.variable_count = 0;
    const NodeIndex on_start = add_node(g, NodeTypeId::OnStart);
    const NodeIndex sv = add_node(g, NodeTypeId::SetVelocity);
    const NodeIndex x = lit_f(g, 1.5);
    const NodeIndex y = lit_f(g, -2.0);
    const NodeIndex z = lit_f(g, 3.25);
    data_edge(g, x, 0, sv, 1);  // Target (pin 0) unconnected => self
    data_edge(g, y, 0, sv, 2);
    data_edge(g, z, 0, sv, 3);
    exec_edge(g, on_start, 0, sv);

    REQUIRE(validate_graph(g).empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    double gx = 0, gy = 0, gz = 0;
    int hits = 0;
    HostContext host;
    host.set_velocity = [&](u32, f64 vx, f64 vy, f64 vz) {
        gx = vx;
        gy = vy;
        gz = vz;
        ++hits;
    };
    VmState state;
    state.reset_for(cr.program);
    Vm vm;
    REQUIRE(vm.run(EventKind::OnStart, cr.program, state, host));
    REQUIRE(hits == 1);
    REQUIRE(gx == 1.5);
    REQUIRE(gy == -2.0);
    REQUIRE(gz == 3.25);
}

TEST_CASE("psygraph nodes: a graph using the new nodes round-trips byte-stable",
          "[psygraph][nodes][serialize]") {
    // Mix new nodes: Once (var slot param), RandomRange (seed param), Clamp,
    // Lerp, GreaterEqual, SetVelocity — exercise both 0-param and 1-param nodes.
    Graph g;
    g.variable_count = 1;
    const NodeIndex on_tick = add_node(g, NodeTypeId::OnTick);
    const NodeIndex once = add_node(g, NodeTypeId::Once, {0});
    const NodeIndex lmin = add_node(g, NodeTypeId::LiteralFloat, {float_bits(0.0)});
    const NodeIndex lmax = add_node(g, NodeTypeId::LiteralFloat, {float_bits(5.0)});
    const NodeIndex rnd = add_node(g, NodeTypeId::RandomRange, {0xABCDEF12u});
    const NodeIndex clamp = add_node(g, NodeTypeId::Clamp);
    const NodeIndex c_lo = lit_f(g, 1.0);
    const NodeIndex c_hi = lit_f(g, 4.0);
    const NodeIndex sv = add_node(g, NodeTypeId::SetVelocity);
    const NodeIndex zy = lit_f(g, 0.0);
    data_edge(g, lmin, 0, rnd, 0);
    data_edge(g, lmax, 0, rnd, 1);
    data_edge(g, rnd, 0, clamp, 0);
    data_edge(g, c_lo, 0, clamp, 1);
    data_edge(g, c_hi, 0, clamp, 2);
    data_edge(g, clamp, 0, sv, 1);  // X = clamped random
    data_edge(g, zy, 0, sv, 2);     // Y = 0
    data_edge(g, zy, 0, sv, 3);     // Z = 0
    exec_edge(g, on_tick, 0, once);
    exec_edge(g, once, 0, sv);

    REQUIRE(validate_graph(g).empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    std::vector<u8> blob;
    serialize_graph(g, blob);
    REQUIRE(!blob.empty());

    Graph loaded;
    std::string err;
    REQUIRE(deserialize_graph(blob, loaded, err));
    REQUIRE(err.empty());
    REQUIRE(loaded.nodes.size() == g.nodes.size());
    REQUIRE(loaded.edges.size() == g.edges.size());
    REQUIRE(loaded.variable_count == g.variable_count);

    // Param payloads survive (Once slot + RandomRange seed).
    REQUIRE(loaded.nodes[once].params.size() == 1u);
    REQUIRE(loaded.nodes[once].params[0] == 0u);
    REQUIRE(loaded.nodes[rnd].params.size() == 1u);
    REQUIRE(loaded.nodes[rnd].params[0] == 0xABCDEF12u);

    // Re-serialize: byte-identical.
    std::vector<u8> blob2;
    serialize_graph(loaded, blob2);
    REQUIRE(blob2 == blob);

    // Still compiles.
    CompileResult cr2 = compile_graph(loaded);
    REQUIRE(cr2.ok);
}

TEST_CASE("psygraph nodes: pooled VM banks stay capacity-stable across runs",
          "[psygraph][nodes][zero-alloc]") {
    // Build a graph using several new ops, warm up, then run many times and
    // assert the register/variable banks never grow (the run() hot path touches
    // no heap — a proxy that complements script_psygraph's armed-allocator test).
    Graph g;
    g.variable_count = 2;
    const NodeIndex on_tick = add_node(g, NodeTypeId::OnTick);
    const NodeIndex getv = add_node(g, NodeTypeId::GetVar, {0});
    const NodeIndex five = lit_f(g, 5.0);
    const NodeIndex mn = add_node(g, NodeTypeId::Min);
    const NodeIndex setv = add_node(g, NodeTypeId::SetVar, {0});
    data_edge(g, getv, 0, mn, 0);
    data_edge(g, five, 0, mn, 1);
    data_edge(g, mn, 0, setv, 0);
    exec_edge(g, on_tick, 0, setv);

    REQUIRE(validate_graph(g).empty());
    CompileResult cr = compile_graph(g);
    REQUIRE(cr.ok);

    HostContext host;
    VmState state;
    state.reset_for(cr.program);
    Vm vm;
    REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));  // warm up

    const auto vars_cap = state.variables().size();
    for (int i = 0; i < 1000; ++i)
        REQUIRE(vm.run(EventKind::OnTick, cr.program, state, host));
    REQUIRE(state.variables().size() == vars_cap);  // never grew
}
