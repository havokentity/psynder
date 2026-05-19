// SPDX-License-Identifier: MIT
// Psynder — REPL backend hook (Wave B). Function-pointer indirection so the
// editor-ipc lane (19) can call into `Vm::execute_repl` from its WebSocket
// console handler without taking a build-time dependency on the script lane's
// internals.
//
// The shape mirrors lane 17's `set_lua_backend(...)` precisely: a free
// function `set_repl_backend(fn)` swaps the active evaluator, with a default
// implementation that calls into the live Vm singleton. Setting `nullptr`
// restores the default.
//
// Why a dedicated header rather than extending Script.h: the public Vm
// contract is frozen (Wave A). Lane 19 needs an indirection point that does
// NOT pull in `lua_State*` machinery transitively; this sibling header keeps
// the Vm header byte-identical while giving lane 19 a stable C call.

#pragma once

#include "core/Types.h"

#include <string>
#include <string_view>

namespace psynder::script {

// Signature of a REPL evaluator. Receives a single line of Lua source and
// writes the formatted result (or error message) into `out`. Returns true on
// success, false on syntax / runtime error. Must be noexcept — lane 19
// invokes it from a task that does not have a Lua-aware exception handler.
using ReplBackendFn = bool (*)(std::string_view line, std::string& out) noexcept;

// Default backend: forwards to `Vm::Get().execute_repl(line, out)`. Defined
// in ReplHook.cpp so it can include Script.h without exposing the Vm symbol
// here.
bool default_repl_backend(std::string_view line, std::string& out) noexcept;

// Install a custom REPL backend. Pass `nullptr` to restore the default. The
// function pointer is read under a lock-free atomic load on dispatch, so a
// hot-swap from lane 19 is safe even while the game thread is running.
void set_repl_backend(ReplBackendFn fn) noexcept;

// Return the currently-installed backend. Never returns nullptr — a
// `set_repl_backend(nullptr)` is restored to `&default_repl_backend`.
ReplBackendFn repl_backend() noexcept;

// Convenience dispatcher: looks up the active backend and invokes it. Lane
// 19's WS console handler calls this from its dispatch task.
bool dispatch_repl(std::string_view line, std::string& out);

}  // namespace psynder::script
