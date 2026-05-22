// SPDX-License-Identifier: MIT
// Psynder script lane -- native bytecode VM implementation.

#include "PsyVm.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace psynder::script::detail {

PsyValue PsyValue::boolean_value(bool v) noexcept {
    PsyValue value;
    value.kind = Kind::Bool;
    value.boolean = v;
    return value;
}

PsyValue PsyValue::number_value(f64 v) noexcept {
    PsyValue value;
    value.kind = Kind::Number;
    value.number = v;
    return value;
}

PsyValue PsyValue::string_value(std::string v) {
    PsyValue value;
    value.kind = Kind::String;
    value.text = std::move(v);
    return value;
}

std::string format_psy_value(const PsyValue& value) {
    switch (value.kind) {
        case PsyValue::Kind::Nil:
            return "nil";
        case PsyValue::Kind::Bool:
            return value.boolean ? "true" : "false";
        case PsyValue::Kind::Number: {
            std::ostringstream os;
            os << std::setprecision(std::numeric_limits<f64>::max_digits10) << value.number;
            return os.str();
        }
        case PsyValue::Kind::String:
            return value.text;
    }
    return "nil";
}

bool PsyVm::push(PsyValue value) {
    stack_.push_back(std::move(value));
    return true;
}

bool PsyVm::pop(PsyValue& out, std::string& diagnostic) {
    if (stack_.empty()) {
        diagnostic = "stack underflow";
        return false;
    }
    out = std::move(stack_.back());
    stack_.pop_back();
    return true;
}

bool PsyVm::binary_number(const char* op_name,
                          const PsyValue& a,
                          const PsyValue& b,
                          f64& av,
                          f64& bv,
                          std::string& diagnostic) const {
    if (a.kind != PsyValue::Kind::Number || b.kind != PsyValue::Kind::Number) {
        diagnostic = std::string{op_name} + " expects number operands";
        return false;
    }
    av = a.number;
    bv = b.number;
    return true;
}

PsyRunResult PsyVm::run(const PsyProgram& program) {
    stack_.clear();
    locals_.assign(program.locals.size(), PsyValue::nil());
    output_.clear();

    PsyRunResult result;
    for (usize ip = 0; ip < program.code.size(); ++ip) {
        const PsyInstruction ins = program.code[ip];
        switch (ins.op) {
            case PsyOp::PushConst:
                if (ins.arg >= program.constants.size()) {
                    result.diagnostic = "constant index out of range";
                    return result;
                }
                push(program.constants[ins.arg]);
                break;
            case PsyOp::LoadLocal:
                if (ins.arg >= locals_.size()) {
                    result.diagnostic = "local index out of range";
                    return result;
                }
                push(locals_[ins.arg]);
                break;
            case PsyOp::StoreLocal: {
                if (ins.arg >= locals_.size()) {
                    result.diagnostic = "local index out of range";
                    return result;
                }
                PsyValue value;
                if (!pop(value, result.diagnostic)) {
                    return result;
                }
                locals_[ins.arg] = std::move(value);
                break;
            }
            case PsyOp::Pop: {
                PsyValue ignored;
                if (!pop(ignored, result.diagnostic)) {
                    return result;
                }
                break;
            }
            case PsyOp::Add: {
                PsyValue b;
                PsyValue a;
                if (!pop(b, result.diagnostic) || !pop(a, result.diagnostic)) {
                    return result;
                }
                if (a.kind == PsyValue::Kind::String || b.kind == PsyValue::Kind::String) {
                    push(PsyValue::string_value(format_psy_value(a) + format_psy_value(b)));
                    break;
                }
                f64 av = 0.0;
                f64 bv = 0.0;
                if (!binary_number("add", a, b, av, bv, result.diagnostic)) {
                    return result;
                }
                push(PsyValue::number_value(av + bv));
                break;
            }
            case PsyOp::Sub:
            case PsyOp::Mul:
            case PsyOp::Div: {
                PsyValue b;
                PsyValue a;
                if (!pop(b, result.diagnostic) || !pop(a, result.diagnostic)) {
                    return result;
                }
                f64 av = 0.0;
                f64 bv = 0.0;
                const char* name = ins.op == PsyOp::Sub   ? "sub"
                                   : ins.op == PsyOp::Mul ? "mul"
                                                          : "div";
                if (!binary_number(name, a, b, av, bv, result.diagnostic)) {
                    return result;
                }
                if (ins.op == PsyOp::Sub) {
                    push(PsyValue::number_value(av - bv));
                } else if (ins.op == PsyOp::Mul) {
                    push(PsyValue::number_value(av * bv));
                } else {
                    if (bv == 0.0) {
                        result.diagnostic = "division by zero";
                        return result;
                    }
                    push(PsyValue::number_value(av / bv));
                }
                break;
            }
            case PsyOp::Neg: {
                PsyValue value;
                if (!pop(value, result.diagnostic)) {
                    return result;
                }
                if (value.kind != PsyValue::Kind::Number) {
                    result.diagnostic = "neg expects a number operand";
                    return result;
                }
                push(PsyValue::number_value(-value.number));
                break;
            }
            case PsyOp::Print: {
                PsyValue value;
                if (!pop(value, result.diagnostic)) {
                    return result;
                }
                output_ += format_psy_value(value);
                output_ += '\n';
                break;
            }
            case PsyOp::Return: {
                PsyValue value = PsyValue::nil();
                if (!stack_.empty()) {
                    value = std::move(stack_.back());
                    stack_.pop_back();
                }
                result.ok = true;
                result.value = std::move(value);
                result.output = std::move(output_);
                return result;
            }
        }
    }

    result.ok = true;
    result.value = PsyValue::nil();
    result.output = std::move(output_);
    return result;
}

}  // namespace psynder::script::detail
