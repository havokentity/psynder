// SPDX-License-Identifier: MIT
// Psynder — Lua binding for RmlUi event handlers. Lane 17 owns.
//
// Wave-B: register a small Lua surface that designer .rml files can hook
// through. Inline handler attributes (`onclick="lua:my_handler(event)"`)
// are dispatched here by event name; this file owns the lookup, builds a
// structured `event` table, and trampolines into the lane-15 VM via the
// `set_lua_backend` function-pointer hook.
//
// The chunk we hand to the VM looks like:
//
//     local event = { kind = "click", target_id = "quit",
//                     mouse_x = 320, mouse_y = 64, button = 0 }
//     <handler-body>
//
// So designers can write `onclick="lua:on_quit(event)"` or just
// `onclick="lua:print(event.target_id)"` and the payload Just Works.
//
// PSYNDER_VENDOR_RMLUI=ON additionally calls `Rml::Lua::Initialise`, but
// the inline-attribute fast-path stays here because RmlUi's own Lua
// plugin assumes a different event-binding shape.

#include "Rml_internal.h"

#include "core/Log.h"

#include <atomic>
#include <cstdio>
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

std::atomic<LuaExecFn> g_lua_exec{nullptr};

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

// Escape a string for embedding inside a Lua "" string literal.  Handles
// the cases that come up in .rml ids and event names: quotes, backslash,
// newline, and stray control bytes.  We deliberately don't unicode-
// normalise — the upstream RmlUi parser keeps element ids ASCII.
std::string lua_str_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\%d", static_cast<int>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Build the `local event = { ... }; ` prelude that gets prepended to the
// handler body.  Numbers use plain decimal formatting since Lua parses
// "1.5" → number unambiguously.
std::string build_event_prelude(const EventPayload& p) {
    char buf[256];
    std::snprintf(buf,
                  sizeof(buf),
                  "local event = {kind=\"%s\", target_id=\"%s\", "
                  "mouse_x=%g, mouse_y=%g, button=%d};\n",
                  lua_str_escape(p.kind).c_str(),
                  lua_str_escape(p.target_id).c_str(),
                  static_cast<double>(p.mouse_x),
                  static_cast<double>(p.mouse_y),
                  static_cast<int>(p.button));
    return std::string(buf);
}

}  // namespace

void set_lua_backend(LuaExecFn backend) noexcept {
    g_lua_exec.store(backend, std::memory_order_release);
}

// Fire a single handler with no structured payload (the empty-event
// path).  Used by tests and for the rare case where a handler body
// doesn't need the event surface.  Returns true if the installed backend
// accepted the chunk; false if there is no backend or it rejected it.
bool dispatch_handler(std::string_view event_name, std::string_view handler_body) {
    EventPayload empty{};
    empty.kind = event_name;
    return dispatch_handler(event_name, handler_body, empty);
}

// Payload-aware dispatch.  Wraps the body with a `local event = { ... }`
// prelude so the handler can reference fields like `event.target_id`
// without each .rml repeating the boilerplate.
bool dispatch_handler(std::string_view event_name,
                      std::string_view handler_body,
                      const EventPayload& payload) {
    if (handler_body.empty())
        return false;
    const auto body = strip_lua_prefix(handler_body);

    std::string chunk;
    chunk.reserve(body.size() + 128);
    chunk += build_event_prelude(payload);
    chunk.append(body.data(), body.size());

    const std::string chunk_name = std::string("rml:") + std::string(event_name);
    return current_backend()(chunk, chunk_name);
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
