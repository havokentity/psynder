// SPDX-License-Identifier: MIT
// Psynder — PsyGraph bytecode interpreter. Lane 15 owns.
//
// run() is a tight switch over the flat instruction stream. All working state
// is `state.registers_` / `state.variables_`, both sized once in reset_for().
// The only heap touched here is via host hooks (the embedder's choice); the
// interpreter core allocates nothing.

#include "Vm.h"

#include "NodeTypes.h"

namespace psynder::script::psygraph {

void VmState::reset_for(const Program& program) {
    if (registers_.size() < program.register_count)
        registers_.resize(program.register_count);
    if (variables_.size() < program.variable_count)
        variables_.resize(program.variable_count, Value::make_bool(false));
    clear_variables();
}

void VmState::clear_variables() noexcept {
    for (Value& v : variables_)
        v = Value::make_bool(false);
}

bool Vm::run(EventKind event, const Program& program, VmState& state, HostContext& host) const {
    u32 ip = 0;
    if (!program.entry_for(event, ip))
        return false;

    Value* regs = state.registers_.data();
    Value* vars = state.variables_.data();
    const Instr* code = program.code.data();
    const Value* consts = program.constants.data();
    const usize n = program.code.size();

    while (ip < n) {
        const Instr& in = code[ip];
        switch (in.op) {
            case Op::LoadConst: regs[in.a] = consts[in.b]; break;
            case Op::LoadVar: regs[in.a] = vars[in.b]; break;
            case Op::StoreVar: vars[in.a] = regs[in.b]; break;
            case Op::Move: regs[in.a] = regs[in.b]; break;

            case Op::LoadDelta: regs[in.a] = Value::make_float(host.delta_time); break;
            case Op::LoadOther: regs[in.a] = Value::make_entity(host.other_entity); break;
            case Op::LoadDamageAmount: regs[in.a] = Value::make_float(host.damage_amount); break;
            case Op::LoadDamageSource: regs[in.a] = Value::make_entity(host.damage_source); break;

            case Op::Add:
                regs[in.a] = Value::make_float(regs[in.b].as_float() + regs[in.c].as_float());
                break;
            case Op::Sub:
                regs[in.a] = Value::make_float(regs[in.b].as_float() - regs[in.c].as_float());
                break;
            case Op::Mul:
                regs[in.a] = Value::make_float(regs[in.b].as_float() * regs[in.c].as_float());
                break;
            case Op::Div: {
                const f64 d = regs[in.c].as_float();
                regs[in.a] = Value::make_float(d != 0.0 ? regs[in.b].as_float() / d : 0.0);
                break;
            }
            case Op::Neg: regs[in.a] = Value::make_float(-regs[in.b].as_float()); break;

            case Op::Equal:
                regs[in.a] = Value::make_bool(regs[in.b].as_float() == regs[in.c].as_float());
                break;
            case Op::Less:
                regs[in.a] = Value::make_bool(regs[in.b].as_float() < regs[in.c].as_float());
                break;
            case Op::Greater:
                regs[in.a] = Value::make_bool(regs[in.b].as_float() > regs[in.c].as_float());
                break;
            case Op::And:
                regs[in.a] = Value::make_bool(regs[in.b].as_bool() && regs[in.c].as_bool());
                break;
            case Op::Or:
                regs[in.a] = Value::make_bool(regs[in.b].as_bool() || regs[in.c].as_bool());
                break;
            case Op::Not: regs[in.a] = Value::make_bool(!regs[in.b].as_bool()); break;

            case Op::JumpIfFalse:
                if (!regs[in.a].as_bool()) {
                    ip = in.b;
                    continue;
                }
                break;
            case Op::Jump:
                ip = in.a;
                continue;
            case Op::Halt:
                return true;

            // ─── host-hook actions ──────────────────────────────────────────
            case Op::Log:
                if (host.log) {
                    const u32 si = static_cast<u32>(regs[in.a].u);
                    if (si < program.strings.size())
                        host.log(program.strings[si]);
                }
                break;
            case Op::SetHealth:
                if (host.set_health)
                    host.set_health(static_cast<u32>(regs[in.a].u), regs[in.b].as_float());
                break;
            case Op::ApplyDamage:
                if (host.apply_damage)
                    host.apply_damage(static_cast<u32>(regs[in.a].u), regs[in.b].as_float());
                break;
            case Op::SpawnEntity: {
                u32 spawned = 0;
                if (host.spawn_entity) {
                    const u32 si = static_cast<u32>(regs[in.a].u);
                    std::string_view prefab =
                        si < program.strings.size() ? std::string_view(program.strings[si])
                                                     : std::string_view{};
                    spawned = host.spawn_entity(prefab);
                }
                regs[in.c] = Value::make_entity(spawned);
                break;
            }
            case Op::SetActive:
                if (host.set_active)
                    host.set_active(static_cast<u32>(regs[in.a].u), regs[in.b].as_bool());
                break;
            case Op::PlaySound:
                if (host.play_sound) {
                    const u32 si = static_cast<u32>(regs[in.a].u);
                    if (si < program.strings.size())
                        host.play_sound(program.strings[si]);
                }
                break;
        }
        ++ip;
    }
    return true;
}

}  // namespace psynder::script::psygraph
