// SPDX-License-Identifier: MIT
// Psynder — PsyGraph value type. Lane 15 (script) owns the psygraph module.
//
// `Value` is the single runtime cell that flows along data pins and lives in
// VM registers / variable slots. It is a small tagged union of POD scalars so
// that VmState buffers are trivially copyable and the interpreter never
// touches the heap during a tick. Strings are NOT stored inline: a graph keeps
// a string pool and a string is referenced by a 32-bit index, so `Value`
// stays a fixed 16-byte POD.

#pragma once

#include "core/Types.h"

namespace psynder::script::psygraph {

// The set of pin / value types the MVP supports. `Exec` is the colourless
// control-flow type (Unreal Blueprints' white exec wires); it never carries
// a data payload — it only sequences node execution.
enum class ValueType : u8 {
    Exec = 0,  // control-flow pin (no data); used by exec edges only
    Bool = 1,
    Int = 2,
    Float = 3,
    Entity = 4,  // an ECS entity handle (raw u32)
    String = 5,  // index into the graph string pool
    Any = 6,     // wildcard for variable get/set; resolved at compile time
};

// A runtime value cell. Trivially copyable POD so VM register / variable
// banks can be memset / memcpy'd and reserved once. 16 bytes.
struct Value {
    ValueType type = ValueType::Bool;
    // The scalar payload. Only one member is meaningful per `type`:
    //   Bool   -> i (0/1)
    //   Int    -> i
    //   Float  -> f
    //   Entity -> u (raw entity handle)
    //   String -> u (index into the graph string pool)
    union {
        i64 i;
        f64 f;
        u64 u;
    };

    static constexpr Value make_bool(bool v) noexcept {
        Value out;
        out.type = ValueType::Bool;
        out.i = v ? 1 : 0;
        return out;
    }
    static constexpr Value make_int(i64 v) noexcept {
        Value out;
        out.type = ValueType::Int;
        out.i = v;
        return out;
    }
    static constexpr Value make_float(f64 v) noexcept {
        Value out;
        out.type = ValueType::Float;
        out.f = v;
        return out;
    }
    static constexpr Value make_entity(u32 raw) noexcept {
        Value out;
        out.type = ValueType::Entity;
        out.u = raw;
        return out;
    }
    static constexpr Value make_string(u32 pool_index) noexcept {
        Value out;
        out.type = ValueType::String;
        out.u = pool_index;
        return out;
    }

    constexpr bool as_bool() const noexcept {
        switch (type) {
            case ValueType::Bool:
            case ValueType::Int:
            case ValueType::Entity:
            case ValueType::String:
                return i != 0;
            case ValueType::Float:
                return f != 0.0;
            default:
                return false;
        }
    }
    constexpr f64 as_float() const noexcept {
        return type == ValueType::Float ? f : static_cast<f64>(i);
    }
    constexpr i64 as_int() const noexcept {
        return type == ValueType::Float ? static_cast<i64>(f) : i;
    }
};

static_assert(sizeof(Value) == 16, "Value must stay a 16-byte POD");
static_assert(__is_trivially_copyable(Value), "Value must be trivially copyable");

// Two data values are "type-compatible" along an edge if their concrete types
// match, or either side is `Any` (variable wildcard, resolved at compile
// time), or they are numeric (Int<->Float implicit promotion).
inline constexpr bool types_compatible(ValueType producer, ValueType consumer) noexcept {
    if (producer == consumer)
        return true;
    if (producer == ValueType::Any || consumer == ValueType::Any)
        return true;
    const bool prod_num = producer == ValueType::Int || producer == ValueType::Float;
    const bool cons_num = consumer == ValueType::Int || consumer == ValueType::Float;
    return prod_num && cons_num;
}

}  // namespace psynder::script::psygraph
