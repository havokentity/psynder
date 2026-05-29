// SPDX-License-Identifier: MIT
// Psynder — PsyGraph host-hook boundary. Lane 15 owns.
//
// The VM is dependency-light: it knows nothing about gameplay, rendering,
// audio, or spawning. Every action node that needs to touch another subsystem
// reaches out through a `HostContext` callback. The embedding application (the
// editor play-runtime, the arcade game, or a unit test) fills in only the
// hooks it cares about; an unset hook is simply a no-op. This is the single
// seam through which side effects leave the deterministic core, which keeps
// the bytecode/VM pure and trivially testable.
//
// HostContext is constructed once per embedding (or per test) and passed by
// reference into run(); it carries no per-tick allocation.

#pragma once

#include "Value.h"

#include "core/Types.h"

#include <functional>
#include <string_view>

namespace psynder::script::psygraph {

// `self` is the entity the running graph instance is attached to (may be
// kInvalidEntity for standalone tests). The host can use it as an implicit
// target when an action's Target pin is unset.
struct HostContext {
    // The entity this graph instance is bound to (0 = none).
    u32 self_entity = 0;

    // ─── Action hooks (side effects) ───────────────────────────────────────
    // Each takes already-resolved primitive args; the VM never passes a
    // Value through, so hosts stay decoupled from the VM's value layout.
    std::function<void(std::string_view message)> log;
    std::function<void(u32 entity, f64 health)> set_health;
    std::function<void(u32 entity, f64 amount)> apply_damage;
    std::function<u32(std::string_view prefab)> spawn_entity;  // returns new entity raw
    std::function<void(u32 entity, bool active)> set_active;
    std::function<void(std::string_view sound)> play_sound;

    // ─── Event input data (filled by the caller before dispatch) ───────────
    // OnTick reads `delta_time`; OnTrigger reads `other_entity`; OnDamaged
    // reads `damage_amount` + `damage_source`. The compiler wires the event
    // node's data-out pins to read these fields.
    f64 delta_time = 0.0;
    u32 other_entity = 0;
    f64 damage_amount = 0.0;
    u32 damage_source = 0;
};

}  // namespace psynder::script::psygraph
