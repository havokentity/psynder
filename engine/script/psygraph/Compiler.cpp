// SPDX-License-Identifier: MIT
// Psynder — PsyGraph compiler. Lane 15 owns.
//
// See Compiler.h for the algorithm overview. The implementation keeps all
// scratch state in a `Ctx` struct local to one compile_graph() call; the
// emitted Program owns no back-pointers to the Graph.

#include "Compiler.h"

#include "NodeTypes.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace psynder::script::psygraph {

namespace {

constexpr u32 kNoReg = 0xFFFFFFFFu;

// Map a NodeTypeId event node to the runtime EventKind, if it is an event.
bool event_kind_of(NodeTypeId type, EventKind& out) noexcept {
    switch (type) {
        case NodeTypeId::OnStart: out = EventKind::OnStart; return true;
        case NodeTypeId::OnTick: out = EventKind::OnTick; return true;
        case NodeTypeId::OnTrigger: out = EventKind::OnTrigger; return true;
        case NodeTypeId::OnDamaged: out = EventKind::OnDamaged; return true;
        default: return false;
    }
}

struct Ctx {
    const Graph& graph;
    Program program;
    std::string error;

    // Per data-out pin register cache, keyed (node << 8 | pin). kNoReg = not
    // yet materialized.
    std::vector<u32> out_reg;  // size nodes*maxpins; flat
    u32 max_out_pins = 1;
    u32 next_reg = 0;

    // Cycle guard for pure-node materialization.
    std::vector<u8> visiting;  // per-node mark (0/1)

    explicit Ctx(const Graph& g) : graph(g) {}

    u32 alloc_reg() { return next_reg++; }

    u32& out_reg_slot(NodeIndex node, u16 pin) {
        return out_reg[node * max_out_pins + pin];
    }

    u32 add_const(Value v) {
        program.constants.push_back(v);
        return static_cast<u32>(program.constants.size() - 1);
    }

    void emit(Op op, u32 a = 0, u32 b = 0, u32 c = 0) {
        Instr in;
        in.op = op;
        in.a = a;
        in.b = b;
        in.c = c;
        program.code.push_back(in);
    }
};

// ─── Edge lookup helpers ──────────────────────────────────────────────────

// Find the single data edge feeding (node, input pin). Returns true + the
// producer node/pin, or false if the pin is unconnected.
bool data_source(const Graph& g, NodeIndex node, u16 in_pin, NodeIndex& src_node, u16& src_pin) {
    for (const Edge& e : g.edges) {
        if (e.kind == EdgeKind::Data && e.to_node == node && e.to_pin == in_pin) {
            src_node = e.from_node;
            src_pin = e.from_pin;
            return true;
        }
    }
    return false;
}

// Find the exec target of (node, exec-out pin). Returns kInvalidNode if none.
NodeIndex exec_target(const Graph& g, NodeIndex node, u16 out_pin) {
    for (const Edge& e : g.edges) {
        if (e.kind == EdgeKind::Exec && e.from_node == node && e.from_pin == out_pin)
            return e.to_node;
    }
    return kInvalidNode;
}

// ─── Validation ────────────────────────────────────────────────────────────

std::string validate(const Graph& g) {
    // 1. Every node has a known type with matching param count.
    for (NodeIndex i = 0; i < g.nodes.size(); ++i) {
        const Node& n = g.nodes[i];
        const NodeTypeInfo* info = find_node_type(n.type);
        if (!info)
            return "node " + std::to_string(i) + ": unknown node type";
        if (n.params.size() != info->param_count)
            return "node " + std::to_string(i) + " (" + info->name +
                   "): expected " + std::to_string(info->param_count) +
                   " params, got " + std::to_string(n.params.size());
        // Variable slot bounds.
        if (n.type == NodeTypeId::GetVar || n.type == NodeTypeId::SetVar) {
            if (n.params[0] >= g.variable_count)
                return "node " + std::to_string(i) + ": variable slot out of range";
        }
        if (n.type == NodeTypeId::LiteralString) {
            if (n.params[0] >= g.strings.size())
                return "node " + std::to_string(i) + ": string index out of range";
        }
    }

    // 2. Edge endpoints + pin indices valid; data edges type-checked.
    for (usize k = 0; k < g.edges.size(); ++k) {
        const Edge& e = g.edges[k];
        if (e.from_node >= g.nodes.size() || e.to_node >= g.nodes.size())
            return "edge " + std::to_string(k) + ": endpoint node out of range";
        const NodeTypeInfo* from = find_node_type(g.nodes[e.from_node].type);
        const NodeTypeInfo* to = find_node_type(g.nodes[e.to_node].type);
        if (!from || !to)
            return "edge " + std::to_string(k) + ": endpoint has unknown type";

        if (e.kind == EdgeKind::Exec) {
            if (e.from_pin >= from->exec_out.size())
                return "edge " + std::to_string(k) + ": exec-out pin out of range";
            if (to->exec_in == 0)
                return "edge " + std::to_string(k) + " -> " + to->name +
                       ": target has no exec input (dangling exec)";
        } else {  // Data
            if (e.from_pin >= from->data_out.size())
                return "edge " + std::to_string(k) + ": data-out pin out of range";
            if (e.to_pin >= to->data_in.size())
                return "edge " + std::to_string(k) + ": data-in pin out of range";
            const ValueType pt = from->data_out[e.from_pin].type;
            const ValueType ct = to->data_in[e.to_pin].type;
            if (!types_compatible(pt, ct))
                return "edge " + std::to_string(k) + " (" + from->name + "." +
                       from->data_out[e.from_pin].name + " -> " + to->name + "." +
                       to->data_in[e.to_pin].name + "): type mismatch";
        }
    }

    // 3. Exec-driven nodes (non-pure, non-event) must be fed by an exec edge.
    //    A non-pure node with no incoming exec edge can never run -> dangling.
    for (NodeIndex i = 0; i < g.nodes.size(); ++i) {
        const NodeTypeInfo* info = find_node_type(g.nodes[i].type);
        if (info->is_event || info->is_pure)
            continue;
        bool fed = false;
        for (const Edge& e : g.edges) {
            if (e.kind == EdgeKind::Exec && e.to_node == i) {
                fed = true;
                break;
            }
        }
        if (!fed)
            return "node " + std::to_string(i) + " (" + info->name +
                   "): non-pure node has no incoming exec edge (dangling)";
    }

    return std::string{};
}

// ─── Codegen: materialize a pure data-producer output into a register ───────

u32 materialize_output(Ctx& ctx, NodeIndex node, u16 pin);

// Read a node's data INPUT pin into a register (recursively materializing the
// producer). If unconnected, emits a zero/default constant.
u32 read_input(Ctx& ctx, NodeIndex node, u16 in_pin, ValueType expected) {
    NodeIndex src_node;
    u16 src_pin;
    if (data_source(ctx.graph, node, in_pin, src_node, src_pin)) {
        return materialize_output(ctx, src_node, src_pin);
    }
    // Unconnected -> default constant for the expected type.
    Value def;
    switch (expected) {
        case ValueType::Float: def = Value::make_float(0.0); break;
        case ValueType::Int: def = Value::make_int(0); break;
        case ValueType::Entity: def = Value::make_entity(0); break;
        case ValueType::String: def = Value::make_string(0); break;
        default: def = Value::make_bool(false); break;
    }
    const u32 r = ctx.alloc_reg();
    ctx.emit(Op::LoadConst, r, ctx.add_const(def));
    return r;
}

// Materialize the output of a pure (or event-data) node into a register,
// caching the result so shared sub-expressions compute once.
u32 materialize_output(Ctx& ctx, NodeIndex node, u16 pin) {
    u32& cached = ctx.out_reg_slot(node, pin);
    if (cached != kNoReg)
        return cached;

    const Node& n = ctx.graph.nodes[node];

    // Cycle guard: a pure node depending on itself is invalid; bail to a 0.
    if (ctx.visiting[node]) {
        if (ctx.error.empty())
            ctx.error = "node " + std::to_string(node) + ": cyclic data dependency";
        const u32 r = ctx.alloc_reg();
        ctx.emit(Op::LoadConst, r, ctx.add_const(Value::make_float(0.0)));
        cached = r;
        return r;
    }
    ctx.visiting[node] = 1;

    u32 result = kNoReg;
    switch (n.type) {
        // Event data-out pins -> host loads.
        case NodeTypeId::OnTick: {
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadDelta, result);
            break;
        }
        case NodeTypeId::OnTrigger: {
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadOther, result);
            break;
        }
        case NodeTypeId::OnDamaged: {
            result = ctx.alloc_reg();
            if (pin == 0)
                ctx.emit(Op::LoadDamageAmount, result);
            else
                ctx.emit(Op::LoadDamageSource, result);
            break;
        }

        // Literals.
        case NodeTypeId::LiteralFloat: {
            f64 v;
            std::memcpy(&v, &n.params[0], sizeof(v));
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadConst, result, ctx.add_const(Value::make_float(v)));
            break;
        }
        case NodeTypeId::LiteralInt: {
            i64 v;
            std::memcpy(&v, &n.params[0], sizeof(v));
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadConst, result, ctx.add_const(Value::make_int(v)));
            break;
        }
        case NodeTypeId::LiteralBool: {
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadConst, result,
                     ctx.add_const(Value::make_bool(n.params[0] != 0)));
            break;
        }
        case NodeTypeId::LiteralString: {
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadConst, result,
                     ctx.add_const(Value::make_string(static_cast<u32>(n.params[0]))));
            break;
        }

        // Variable read.
        case NodeTypeId::GetVar: {
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadVar, result, static_cast<u32>(n.params[0]));
            break;
        }

        // Binary math / compare.
        case NodeTypeId::Add:
        case NodeTypeId::Sub:
        case NodeTypeId::Mul:
        case NodeTypeId::Div:
        case NodeTypeId::Equal:
        case NodeTypeId::Less:
        case NodeTypeId::Greater:
        case NodeTypeId::And:
        case NodeTypeId::Or: {
            const ValueType in_t =
                (n.type == NodeTypeId::And || n.type == NodeTypeId::Or)
                    ? ValueType::Bool
                    : ValueType::Float;
            const u32 ra = read_input(ctx, node, 0, in_t);
            const u32 rb = read_input(ctx, node, 1, in_t);
            result = ctx.alloc_reg();
            Op op = Op::Add;
            switch (n.type) {
                case NodeTypeId::Add: op = Op::Add; break;
                case NodeTypeId::Sub: op = Op::Sub; break;
                case NodeTypeId::Mul: op = Op::Mul; break;
                case NodeTypeId::Div: op = Op::Div; break;
                case NodeTypeId::Equal: op = Op::Equal; break;
                case NodeTypeId::Less: op = Op::Less; break;
                case NodeTypeId::Greater: op = Op::Greater; break;
                case NodeTypeId::And: op = Op::And; break;
                case NodeTypeId::Or: op = Op::Or; break;
                default: break;
            }
            ctx.emit(op, result, ra, rb);
            break;
        }
        case NodeTypeId::Neg: {
            const u32 ra = read_input(ctx, node, 0, ValueType::Float);
            result = ctx.alloc_reg();
            ctx.emit(Op::Neg, result, ra);
            break;
        }
        case NodeTypeId::Not: {
            const u32 ra = read_input(ctx, node, 0, ValueType::Bool);
            result = ctx.alloc_reg();
            ctx.emit(Op::Not, result, ra);
            break;
        }

        default:
            // A non-pure node used as a data source (e.g. SpawnEntity's
            // Spawned pin) is resolved during exec emission, not here. If we
            // reach this path the graph wired a non-materializable pin; emit 0.
            result = ctx.alloc_reg();
            ctx.emit(Op::LoadConst, result, ctx.add_const(Value::make_entity(0)));
            break;
    }

    ctx.visiting[node] = 0;
    cached = result;
    return result;
}

// ─── Codegen: emit straight-line exec flow from an exec node ────────────────

void emit_exec(Ctx& ctx, NodeIndex node);

void emit_successor(Ctx& ctx, NodeIndex node, u16 out_pin) {
    const NodeIndex next = exec_target(ctx.graph, node, out_pin);
    if (next != kInvalidNode)
        emit_exec(ctx, next);
}

void emit_exec(Ctx& ctx, NodeIndex node) {
    const Node& n = ctx.graph.nodes[node];
    switch (n.type) {
        case NodeTypeId::Branch: {
            const u32 cond = read_input(ctx, node, 0, ValueType::Bool);
            // JumpIfFalse cond -> patch to False branch start.
            const usize jmp = ctx.program.code.size();
            ctx.emit(Op::JumpIfFalse, cond, 0);
            // True branch.
            emit_successor(ctx, node, 0);
            // Jump past the False branch.
            const usize skip = ctx.program.code.size();
            ctx.emit(Op::Jump, 0);
            // False branch lands here.
            ctx.program.code[jmp].b = static_cast<u32>(ctx.program.code.size());
            emit_successor(ctx, node, 1);
            ctx.program.code[skip].a = static_cast<u32>(ctx.program.code.size());
            break;
        }
        case NodeTypeId::Sequence: {
            emit_successor(ctx, node, 0);
            emit_successor(ctx, node, 1);
            break;
        }
        case NodeTypeId::SetVar: {
            const u32 r = read_input(ctx, node, 0, ValueType::Any);
            ctx.emit(Op::StoreVar, static_cast<u32>(n.params[0]), r);
            emit_successor(ctx, node, 0);
            break;
        }
        case NodeTypeId::Log: {
            // Message pin must be a String literal/var; resolve to a const idx
            // when possible, else load via register then log the register.
            const u32 r = read_input(ctx, node, 0, ValueType::String);
            ctx.emit(Op::Log, r);
            emit_successor(ctx, node, 0);
            break;
        }
        case NodeTypeId::SetHealth: {
            const u32 e = read_input(ctx, node, 0, ValueType::Entity);
            const u32 h = read_input(ctx, node, 1, ValueType::Float);
            ctx.emit(Op::SetHealth, e, h);
            emit_successor(ctx, node, 0);
            break;
        }
        case NodeTypeId::ApplyDamage: {
            const u32 e = read_input(ctx, node, 0, ValueType::Entity);
            const u32 a = read_input(ctx, node, 1, ValueType::Float);
            ctx.emit(Op::ApplyDamage, e, a);
            emit_successor(ctx, node, 0);
            break;
        }
        case NodeTypeId::SpawnEntity: {
            const u32 p = read_input(ctx, node, 0, ValueType::String);
            const u32 out = ctx.alloc_reg();
            ctx.emit(Op::SpawnEntity, p, 0, out);
            ctx.out_reg_slot(node, 0) = out;  // expose Spawned for data readers
            emit_successor(ctx, node, 0);
            break;
        }
        case NodeTypeId::SetActive: {
            const u32 e = read_input(ctx, node, 0, ValueType::Entity);
            const u32 a = read_input(ctx, node, 1, ValueType::Bool);
            ctx.emit(Op::SetActive, e, a);
            emit_successor(ctx, node, 0);
            break;
        }
        case NodeTypeId::PlaySound: {
            const u32 s = read_input(ctx, node, 0, ValueType::String);
            ctx.emit(Op::PlaySound, s);
            emit_successor(ctx, node, 0);
            break;
        }
        default:
            // Pure nodes never appear in exec flow; ignore defensively.
            break;
    }
}

}  // namespace

std::string validate_graph(const Graph& graph) {
    return validate(graph);
}

CompileResult compile_graph(const Graph& graph) {
    CompileResult result;
    result.diagnostic = validate(graph);
    if (!result.diagnostic.empty())
        return result;

    Ctx ctx(graph);

    // Determine the widest data-out pin count so the flat out_reg cache is
    // correctly strided.
    for (const Node& n : graph.nodes) {
        const NodeTypeInfo* info = find_node_type(n.type);
        if (info && info->data_out.size() > ctx.max_out_pins)
            ctx.max_out_pins = static_cast<u32>(info->data_out.size());
    }
    if (ctx.max_out_pins == 0)
        ctx.max_out_pins = 1;

    ctx.out_reg.assign(graph.nodes.size() * ctx.max_out_pins, kNoReg);
    ctx.visiting.assign(graph.nodes.size(), 0);

    ctx.program.strings = graph.strings;
    ctx.program.variable_count = graph.variable_count;

    // Emit one straight-line handler per event node. Each handler gets its own
    // fresh register-cache view (clear cache so registers aren't shared across
    // events — a tick must not reuse OnStart's stale registers).
    for (NodeIndex i = 0; i < graph.nodes.size(); ++i) {
        EventKind kind;
        if (!event_kind_of(graph.nodes[i].type, kind))
            continue;

        // Reset per-event materialization cache (registers persist; only the
        // memo map clears so each handler recomputes its inputs).
        std::fill(ctx.out_reg.begin(), ctx.out_reg.end(), kNoReg);

        EventEntry entry;
        entry.event = kind;
        entry.code_offset = static_cast<u32>(ctx.program.code.size());
        ctx.program.entries.push_back(entry);

        emit_successor(ctx, i, 0);  // event's single exec-out
        ctx.emit(Op::Halt);

        if (!ctx.error.empty()) {
            result.diagnostic = ctx.error;
            return result;
        }
    }

    ctx.program.register_count = ctx.next_reg;
    result.program = std::move(ctx.program);
    result.ok = true;
    return result;
}

}  // namespace psynder::script::psygraph
