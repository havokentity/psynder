// SPDX-License-Identifier: MIT
// Psynder — PsyGraph bytecode VM. Lane 15 owns.
//
// `VmState` is the per-instance, reusable execution context: a register bank
// and a variable bank, each reserved once to the program's declared sizes.
// `run()` executes one event's straight-line code over those banks and
// performs ZERO heap allocations after the initial reserve(): all working
// storage is the pre-sized vectors, indexed by the compiler-assigned slots.
//
// Determinism: the VM has no RNG and no clock. Time only enters via
// HostContext::delta_time (an OnTick input); side effects only leave via host
// hooks. Two runs with identical (program, state, host inputs) produce
// identical results and identical hook call sequences.
//
// Re-entrancy: each graph instance owns its own VmState, so concurrent or
// nested graphs never share registers/variables. `run()` itself keeps no
// static state.

#pragma once

#include "Bytecode.h"
#include "Host.h"
#include "Value.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::script::psygraph {

// Per-instance VM execution context. Construct once, call reset_for() once per
// program (or whenever the bound program changes), then run() any number of
// events with no further allocation.
class VmState {
   public:
    // Size the register + variable banks for `program`. Variables are
    // zero-initialized (Bool false). Safe to call repeatedly; only grows the
    // backing storage (never shrinks, so warmed-up capacity is retained).
    void reset_for(const Program& program);

    // Reset variables to their default (Bool false) without reallocating.
    // Use between independent activations of the same instance if desired.
    void clear_variables() noexcept;

    std::span<const Value> variables() const noexcept { return variables_; }
    Value variable(u32 slot) const noexcept {
        return slot < variables_.size() ? variables_[slot] : Value::make_bool(false);
    }
    void set_variable(u32 slot, Value v) noexcept {
        if (slot < variables_.size())
            variables_[slot] = v;
    }

   private:
    friend class Vm;
    std::vector<Value> registers_;
    std::vector<Value> variables_;
};

// The interpreter. Stateless across runs; holds only a pointer to the program
// for the duration of a run() call. One Vm can drive many VmStates.
class Vm {
   public:
    // Execute the handler for `event`. Returns false (without side effects)
    // if the program has no handler for that event. `state` must have been
    // reset_for(program). `host` supplies action hooks + event input data.
    //
    // Zero heap allocations occur in this call once `state` is warmed up.
    bool run(EventKind event, const Program& program, VmState& state, HostContext& host) const;
};

}  // namespace psynder::script::psygraph
