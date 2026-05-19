// SPDX-License-Identifier: MIT
// Psynder — REPL hook implementation. Holds the active backend pointer in
// an atomic so lane 19's WS handler can swap evaluators without racing the
// game thread's dispatch path.

#include "ReplHook.h"

#include "script/Script.h"

#include <atomic>

namespace psynder::script {

bool default_repl_backend(std::string_view line, std::string& out) noexcept {
    // The Vm singleton owns the live Lua state. We forward unconditionally;
    // `execute_repl` itself reports an explicit "(vm not started)" error if
    // it is called before `start()`, so the hook layer does not need to
    // duplicate that check.
    try {
        return Vm::Get().execute_repl(line, out);
    } catch (...) {
        // Vm::execute_repl is itself noexcept in practice (it uses
        // lua_pcall and translates Lua errors into `out`), but we belt-and-
        // suspenders here because the hook signature is `noexcept`.
        out.assign("(internal: backend threw)");
        return false;
    }
}

namespace {

// Atomic so lane 19 can install a backend at any point in the lifecycle.
// Initial value is null — `current_backend()` returns the default in that
// case so consumers do not have to worry about ordering between static
// initialisation of this TU and a caller that calls `dispatch_repl` from
// an earlier static initialiser.
std::atomic<ReplBackendFn> g_backend{nullptr};

ReplBackendFn current_backend() noexcept {
    ReplBackendFn fn = g_backend.load(std::memory_order_acquire);
    return fn ? fn : &default_repl_backend;
}

}  // namespace

void set_repl_backend(ReplBackendFn fn) noexcept {
    g_backend.store(fn, std::memory_order_release);
}

ReplBackendFn repl_backend() noexcept {
    return current_backend();
}

bool dispatch_repl(std::string_view line, std::string& out) {
    return current_backend()(line, out);
}

}  // namespace psynder::script
