// SPDX-License-Identifier: MIT
// Psynder — Lua binding for RmlUi event handlers. Lane 17 owns.
//
// Wave-A: register a small Lua surface that designer .rml files can hook
// through. Inline handler attributes (`onclick="lua:my_handler()"`) are
// dispatched here by event name; this file owns the lookup + Lua VM
// trampoline.
//
// Lane 15 (Vm) is stubbed today; we register through `execute_string` so
// the binding lights up the moment the VM starts running real Lua.
// PSYNDER_VENDOR_RMLUI=ON adds the upstream `Rml::Lua::Initialise` call,
// but this dispatcher keeps owning the inline-attribute fast path.

#include "Rml_internal.h"

#include "core/Log.h"

#include <atomic>
#include <string>
#include <string_view>

// Lane 15 (script) ships only a public header in Wave-A — `Vm::Get` and
// `Vm::execute_string` are not defined yet.  Calling them directly would
// fail linkage in any binary that pulls in `psynder_ui_rml` but not a
// future `psynder_script` implementation.  Instead we install a tiny
// function-pointer hook: the script lane (or the test harness, or the
// upstream RmlUi Lua plugin once vendored) installs a real backend by
// calling `detail::set_lua_backend` (declared in Rml_internal.h).  Until
// then, dispatch is a structured no-op that still logs the handler that
// would have fired.

namespace psynder::ui::rml::detail {

namespace {

std::atomic<LuaExecFn> g_lua_exec{ nullptr };

bool default_lua_exec(std::string_view source, std::string_view name) noexcept {
    // Wave-A default backend: log + acknowledge.  Lane 15 swaps this.
    PSY_LOG_INFO("rml.lua[{}]: {} byte(s) deferred (no VM)",
                 std::string(name),
                 static_cast<unsigned>(source.size()));
    return false;
}

// Strip a "lua:" prefix; if missing, the handler body is treated as raw
// Lua source.  Returns the stripped substring (no copy).
std::string_view strip_lua_prefix(std::string_view src) noexcept {
    constexpr std::string_view kPrefix = "lua:";
    if (src.size() >= kPrefix.size() && src.compare(0, kPrefix.size(), kPrefix) == 0) {
        return src.substr(kPrefix.size());
    }
    return src;
}

LuaExecFn current_backend() noexcept {
    auto p = g_lua_exec.load(std::memory_order_acquire);
    return p ? p : &default_lua_exec;
}

}  // namespace

void set_lua_backend(LuaExecFn backend) noexcept {
    g_lua_exec.store(backend, std::memory_order_release);
}

// Fire a single handler.  Returns true if the installed backend accepted
// the chunk; false if there is no backend or it rejected it.
bool dispatch_handler(std::string_view event_name,
                      std::string_view handler_body) {
    if (handler_body.empty()) return false;
    const auto body = strip_lua_prefix(handler_body);
    const std::string chunk_name = std::string("rml:") + std::string(event_name);
    return current_backend()(body, chunk_name);
}

// Register the cross-cutting Lua surface that .rml files can reference.
// Today: forwarded to the installed backend (or the no-op default) so
// the call exercises the binding wiring even pre-lane-15.  Idempotent
// so initialize() can call it freely.
void register_lua_surface() {
    constexpr std::string_view kBootstrap =
        "if rml == nil then rml = {} end\n"
        "rml._documents = rml._documents or {}\n";
    (void)current_backend()(kBootstrap, "rml.bootstrap");
}

}  // namespace psynder::ui::rml::detail
