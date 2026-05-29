// SPDX-License-Identifier: MIT
// Psynder — PsyGraph compiled bytecode. Lane 15 owns.
//
// The compiler lowers a Graph into a flat program: a single
// `std::vector<Instr>` plus a constant pool and a per-event entry table. This
// is a register-based bytecode — every pure data-producer node is assigned a
// register slot, and an instruction's operands name registers / constants by
// index. Lowering to a flat array (vs. interpreting the node graph directly)
// is the standard "compile the behaviour graph once, then run a tight loop"
// approach used by mature visual-scripting VMs; it keeps the hot interpreter
// branch-predictable and lets `run()` execute with zero heap traffic.
//
// Instr is a fixed 16-byte POD so the program can be serialized verbatim.

#pragma once

#include "Value.h"

#include "core/Types.h"

#include <string>
#include <vector>

namespace psynder::script::psygraph {

// VM opcodes. Persisted inside compiled programs only transiently (programs
// are recompiled from the graph), but kept stable and explicit for clarity
// and for the bytecode round-trip test.
enum class Op : u16 {
    // ─── data / register ops ──────────────────────────────────────────────
    LoadConst = 1,   // r[a] = const[b]
    LoadVar = 2,     // r[a] = var[b]
    StoreVar = 3,    // var[a] = r[b]
    Move = 4,        // r[a] = r[b]

    // ─── host event-input loads (deterministic; data only via host) ────────
    LoadDelta = 5,         // r[a] = host.delta_time (Float)
    LoadOther = 6,         // r[a] = host.other_entity (Entity)
    LoadDamageAmount = 7,  // r[a] = host.damage_amount (Float)
    LoadDamageSource = 8,  // r[a] = host.damage_source (Entity)

    // ─── arithmetic (float domain) ────────────────────────────────────────
    Add = 10,  // r[a] = r[b] + r[c]
    Sub = 11,
    Mul = 12,
    Div = 13,  // division by zero yields 0.0 (deterministic, no trap)
    Neg = 14,  // r[a] = -r[b]

    // ─── compare / logic (result is Bool) ──────────────────────────────────
    Equal = 20,    // r[a] = (r[b] == r[c])
    Less = 21,
    Greater = 22,
    And = 23,
    Or = 24,
    Not = 25,  // r[a] = !r[b]

    // ─── control flow ──────────────────────────────────────────────────────
    JumpIfFalse = 30,  // if (!r[a]) ip = b
    Jump = 31,         // ip = a
    Halt = 32,         // stop this event's execution

    // ─── host-hook actions (a/b/c name registers holding the args) ──────────
    Log = 40,          // host.log(string_const[a])  (a = const index)
    SetHealth = 41,    // host.set_health(r[a]=entity, r[b]=health)
    ApplyDamage = 42,  // host.apply_damage(r[a]=entity, r[b]=amount)
    SpawnEntity = 43,  // r[c] = host.spawn(string_const[a])
    SetActive = 44,    // host.set_active(r[a]=entity, r[b]=active)
    PlaySound = 45,    // host.play_sound(string_const[a])
};

// One instruction. Three operand slots cover every op above. 16 bytes.
struct Instr {
    Op op = Op::Halt;
    u16 pad = 0;  // keeps the struct 4-byte aligned + 16 bytes total
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
};
static_assert(sizeof(Instr) == 16, "Instr must stay a 16-byte POD");
static_assert(__is_trivially_copyable(Instr), "Instr must be trivially copyable");

// Which event a code range implements, used as a run() entry key.
enum class EventKind : u16 {
    OnStart = 1,
    OnTick = 2,
    OnTrigger = 3,
    OnDamaged = 4,
};

// One compiled event handler: an instruction-pointer offset into Program::code
// where this event's straight-line code begins.
struct EventEntry {
    EventKind event;
    u32 code_offset;  // index into Program::code of the first instruction
};

// A fully compiled, ready-to-run program. Owns its code, constant pool, the
// event entry table, and the string pool copied from the source graph (so the
// VM / host can resolve string constants without the graph).
struct Program {
    std::vector<Instr> code;
    std::vector<Value> constants;
    std::vector<EventEntry> entries;
    std::vector<std::string> strings;
    u32 register_count = 0;  // size of the per-instance register bank
    u32 variable_count = 0;  // size of the per-instance variable bank

    // Find the code offset for an event; returns false if the graph has no
    // handler for it.
    bool entry_for(EventKind event, u32& out_offset) const noexcept {
        for (const EventEntry& e : entries) {
            if (e.event == event) {
                out_offset = e.code_offset;
                return true;
            }
        }
        return false;
    }
};

}  // namespace psynder::script::psygraph
