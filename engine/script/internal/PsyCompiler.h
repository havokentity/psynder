// SPDX-License-Identifier: MIT
// Psynder script lane -- compiler for the native `.psy` language.

#pragma once

#include "PsyVm.h"

#include <string>
#include <string_view>

namespace psynder::script::detail {

struct PsyCompileResult {
    bool ok = false;
    PsyProgram program;
    std::string diagnostic;
};

// First language slice:
//   let name = expr;
//   name = expr;
//   print expr;
//   return expr;
//
// Expressions support numbers, strings, true/false/nil, locals, grouping,
// unary minus, and + - * /. `+` concatenates if either operand is a string.
PsyCompileResult compile_psy_source(std::string_view source, std::string_view name);

bool is_psy_script_name(std::string_view name) noexcept;
bool is_psy_repl_command(std::string_view line) noexcept;
std::string_view psy_repl_payload(std::string_view line) noexcept;
bool has_psy_script_marker(std::string_view source) noexcept;
std::string_view strip_psy_script_marker(std::string_view source) noexcept;

}  // namespace psynder::script::detail
