// SPDX-License-Identifier: MIT
// Psynder — PsyGraph built-in node-type registry. Lane 15 owns.
//
// Each node in a graph references a `NodeType` by stable id. A NodeType
// declares its pin signature: ordered input/output exec pins and ordered
// input/output data pins (with their ValueType). The compiler validates a
// concrete node against this signature and the VM dispatches on the id.
//
// The catalog is a fixed, code-defined table (not user-extensible at runtime)
// for the MVP — mirroring how Unreal Blueprints' "K2 node" palette is
// engine-defined while the *graph* (instances + wiring) is user data.

#pragma once

#include "Value.h"

#include "core/Types.h"

#include <span>

namespace psynder::script::psygraph {

// Stable node-type identifiers. Persisted in graph blobs, so values are
// frozen — append only, never renumber.
enum class NodeTypeId : u16 {
    // ─── Events (entry points; one exec-out, no exec-in) ──────────────────
    OnStart = 1,    // fires once when the graph instance starts
    OnTick = 2,     // fires every tick; data-out: DeltaTime (Float)
    OnTrigger = 3,  // fires on trigger/overlap; data-out: Other (Entity)
    OnDamaged = 4,  // fires on damage; data-out: Amount (Float), Source (Entity)

    // ─── Flow ─────────────────────────────────────────────────────────────
    Branch = 20,    // data-in: Condition (Bool); exec-out: True, False
    Sequence = 21,  // exec-out: Then0, Then1 (fired in order)
    // Wave 13: a one-shot gate. param0 = var slot used to remember it fired;
    // forwards exec to Then only the FIRST time it is reached per instance.
    Once = 22,  // param0 = var slot; exec-out: Then

    // ─── Math (pure; no exec pins) ─────────────────────────────────────────
    Add = 40,  // in: A, B (Float); out: Result (Float)
    Sub = 41,
    Mul = 42,
    Div = 43,
    Neg = 44,  // in: A (Float); out: Result (Float)
    // Wave 13: richer math (all pure; no params unless noted).
    Min = 45,    // in: A, B (Float); out: Result (Float)
    Max = 46,    // in: A, B (Float); out: Result (Float)
    Abs = 47,    // in: A (Float); out: Result (Float)
    Sign = 48,   // in: A (Float); out: Result (Float) -> -1/0/+1
    Floor = 49,  // in: A (Float); out: Result (Float)
    Ceil = 50,   // in: A (Float); out: Result (Float)
    Sqrt = 51,   // in: A (Float); out: Result (Float) -> sqrt(max(A,0))
    Mod = 52,    // in: A, B (Float); out: Result (Float) -> A mod B (0 if B==0)
    Clamp = 53,  // in: A, Min, Max (Float); out: Result (Float)
    Lerp = 54,   // in: A, B, T (Float); out: Result (Float) -> A+(B-A)*T
    // Wave 13: a SEEDED deterministic random in [Min, Max). param0 = u64 seed.
    // Pure: the same (seed, Min, Max) always yields the same value (a hash, not
    // a stateful stream) so the VM stays fully deterministic + reproducible.
    RandomRange = 55,  // param0 = seed; in: Min, Max (Float); out: Result (Float)

    // ─── Compare (pure; no exec pins) ──────────────────────────────────────
    Equal = 60,    // in: A, B (Float); out: Result (Bool)
    Less = 61,     // in: A, B (Float); out: Result (Bool)
    Greater = 62,  // in: A, B (Float); out: Result (Bool)
    And = 63,      // in: A, B (Bool); out: Result (Bool)
    Or = 64,
    Not = 65,  // in: A (Bool); out: Result (Bool)
    // Wave 13: the remaining comparisons + logical XOR (all pure).
    NotEqual = 66,      // in: A, B (Float); out: Result (Bool)
    LessEqual = 67,     // in: A, B (Float); out: Result (Bool)
    GreaterEqual = 68,  // in: A, B (Float); out: Result (Bool)
    Xor = 69,           // in: A, B (Bool); out: Result (Bool)

    // ─── Variables (scoped per VM instance) ────────────────────────────────
    GetVar = 80,  // param0 = var slot; out: Value (Any)
    SetVar = 81,  // param0 = var slot; exec; data-in: Value (Any)

    // ─── Literals (pure) ───────────────────────────────────────────────────
    LiteralFloat = 90,   // param0 bits = f64; out: Value (Float)
    LiteralBool = 91,    // param0 = 0/1; out: Value (Bool)
    LiteralInt = 92,     // param0 = i64; out: Value (Int)
    LiteralString = 93,  // param0 = string-pool index; out: Value (String)

    // ─── Actions (exec; call through host hooks) ───────────────────────────
    Log = 100,          // exec; data-in: Message (String)
    SetHealth = 101,    // exec; data-in: Target (Entity), Health (Float)
    ApplyDamage = 102,  // exec; data-in: Target (Entity), Amount (Float)
    SpawnEntity = 103,  // exec; data-in: Prefab (String); out: Spawned (Entity)
    SetActive = 104,    // exec; data-in: Target (Entity), Active (Bool)
    PlaySound = 105,    // exec; data-in: Sound (String)
    // Wave 13: two more ECS component bindings (through host hooks).
    SetVelocity = 106,  // exec; data-in: Target (Entity), X, Y, Z (Float)

    // ─── ECS reads (pure-style data nodes; resolved through a host getter) ──
    // GetHealth reads a component field back into the data graph. It is not a
    // constant-foldable pure node (it queries host state), so the compiler
    // materializes it via a dedicated host-load op, like the event-data pins.
    GetHealth = 120,  // in: Target (Entity); out: Health (Float)
};

// A single declared pin on a node type.
struct PinSpec {
    const char* name;
    ValueType type;
};

// The pin signature + classification of a node type.
struct NodeTypeInfo {
    NodeTypeId id;
    const char* name;
    bool is_event;  // entry point: scheduled by run(event), not by exec edges
    bool is_pure;   // no exec pins; output computed on demand (data-only)
    u8 exec_in;     // number of exec input pins (0 or 1 for MVP)
    std::span<const PinSpec> exec_out;   // named exec output pins
    std::span<const PinSpec> data_in;    // ordered data input pins
    std::span<const PinSpec> data_out;   // ordered data output pins
    // Number of inline params the node carries (var slot, literal bits, etc.)
    u8 param_count;
};

// Look up a node-type descriptor. Returns nullptr for an unknown id.
const NodeTypeInfo* find_node_type(NodeTypeId id) noexcept;

// Full catalog, for editor palette enumeration.
std::span<const NodeTypeInfo> all_node_types() noexcept;

}  // namespace psynder::script::psygraph
