// SPDX-License-Identifier: MIT
// Psynder script lane -- first native bytecode VM.

#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace psynder::script::detail {

struct PsyValue {
    enum class Kind : u8 {
        Nil,
        Bool,
        Number,
        String,
    };

    Kind kind = Kind::Nil;
    bool boolean = false;
    f64 number = 0.0;
    std::string text;

    static PsyValue nil() noexcept { return {}; }
    static PsyValue boolean_value(bool v) noexcept;
    static PsyValue number_value(f64 v) noexcept;
    static PsyValue string_value(std::string v);
};

enum class PsyOp : u8 {
    PushConst,
    LoadLocal,
    StoreLocal,
    Pop,
    Add,
    Sub,
    Mul,
    Div,
    Neg,
    Print,
    Return,
};

struct PsyInstruction {
    PsyOp op = PsyOp::Return;
    u32 arg = 0;
};

struct PsyProgram {
    std::vector<PsyValue> constants;
    std::vector<PsyInstruction> code;
    std::vector<std::string> locals;
};

struct PsyRunResult {
    bool ok = false;
    PsyValue value;
    std::string output;
    std::string diagnostic;
};

std::string format_psy_value(const PsyValue& value);

class PsyVm {
   public:
    PsyRunResult run(const PsyProgram& program);

   private:
    std::vector<PsyValue> stack_;
    std::vector<PsyValue> locals_;
    std::string output_;

    bool push(PsyValue value);
    bool pop(PsyValue& out, std::string& diagnostic);
    bool binary_number(const char* op_name,
                       const PsyValue& a,
                       const PsyValue& b,
                       f64& av,
                       f64& bv,
                       std::string& diagnostic) const;
};

}  // namespace psynder::script::detail
