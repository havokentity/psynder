// SPDX-License-Identifier: MIT
// Psynder — Vm (Lua 5.4) public surface. Per DESIGN.md §10.5, the VM runs
// on a dedicated game thread; engine workers never enter Lua. All public
// entry points assume single-thread access.

#include "Script.h"

#include "core/Log.h"
#include "internal/LuaState.h"
#include "internal/Registry.h"
#include "internal/Bindings.h"
#include "internal/Spawn.h"
#include "internal/PsyCompiler.h"
#include "internal/PsyVm.h"
#include "internal/VisualGraphCompiler.h"

#include <fstream>
#include <sstream>
#include <string>

namespace psynder::script {

namespace {

// Pimpl-style state owned by the singleton Vm. Hidden in this TU so the
// public header stays a thin contract (frozen — see Script.h).
struct VmImpl {
    detail::LuaState lua;
    detail::ScriptRegistry registry;
    detail::PsyVm psy;
    bool started = false;
};

VmImpl& vm_impl() {
    static VmImpl impl;
    return impl;
}

// Format a Lua value at index `idx` for REPL display. Mirrors what
// stock Lua's `print` does, with table summarisation for unprintables.
void format_value(lua_State* L, int idx, std::string& out) {
    int t = lua_type(L, idx);
    switch (t) {
        case LUA_TNIL:
            out += "nil";
            return;
        case LUA_TBOOLEAN:
            out += lua_toboolean(L, idx) ? "true" : "false";
            return;
        case LUA_TNUMBER:
        case LUA_TSTRING: {
            std::size_t len = 0;
            const char* s = luaL_tolstring(L, idx, &len);  // pushes copy
            out.append(s, len);
            lua_pop(L, 1);
            return;
        }
        case LUA_TTABLE:
            out += "table: ";
            out += lua_typename(L, t);
            // Best-effort summary length.
            {
                lua_Integer n = luaL_len(L, idx);
                if (n > 0) {
                    out += " [n=";
                    out += std::to_string(n);
                    out += "]";
                }
            }
            return;
        default: {
            std::size_t len = 0;
            const char* s = luaL_tolstring(L, idx, &len);
            out.append(s, len);
            lua_pop(L, 1);
            return;
        }
    }
}

bool run_loaded_chunk(lua_State* L, int top_before, std::string& out) {
    int rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        out.assign(msg ? msg : "(runtime error)");
        lua_pop(L, 1);
        return false;
    }

    const int n_results = lua_gettop(L) - top_before;
    for (int i = 1; i <= n_results; ++i) {
        if (i > 1)
            out += '\t';
        format_value(L, top_before + i, out);
    }
    if (n_results > 0) {
        lua_pop(L, n_results);
    }
    return true;
}

bool execute_visual_graph(lua_State* L, std::string_view source, std::string_view name, std::string& out) {
    detail::VisualCompileResult compiled = detail::compile_visual_graph(source);
    if (!compiled.ok) {
        out = compiled.diagnostic;
        return false;
    }

    const int top_before = lua_gettop(L);
    std::string chunk_name{name.empty() ? "=visual-graph" : std::string{name}};
    if (!chunk_name.empty() && chunk_name.front() != '=') {
        chunk_name.insert(chunk_name.begin(), '=');
    }
    if (luaL_loadbuffer(L, compiled.lua_source.data(), compiled.lua_source.size(), chunk_name.c_str()) !=
        LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        out.assign(msg ? msg : "(load error)");
        lua_pop(L, 1);
        return false;
    }
    return run_loaded_chunk(L, top_before, out);
}

bool execute_psy_script(detail::PsyVm& vm,
                        std::string_view source,
                        std::string_view name,
                        std::string& out) {
    detail::PsyCompileResult compiled = detail::compile_psy_source(source, name);
    if (!compiled.ok) {
        out = compiled.diagnostic;
        return false;
    }

    detail::PsyRunResult run = vm.run(compiled.program);
    if (!run.ok) {
        out = run.diagnostic;
        return false;
    }

    out = std::move(run.output);
    if (run.value.kind != detail::PsyValue::Kind::Nil) {
        out += detail::format_psy_value(run.value);
    }
    return true;
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────────

Vm& Vm::Get() {
    static Vm v;
    return v;
}

bool Vm::start() {
    auto& impl = vm_impl();
    if (impl.started) {
        return true;
    }
    if (!impl.lua.open()) {
        PSY_LOG_ERROR("script: Vm::start failed (lua state)");
        return false;
    }
    detail::install_world_api(impl.lua.handle(), &impl.registry);
    // Wave B: attach `world:spawn(...)` after the base world table is up so
    // it can extend rather than replace the existing methods.
    detail::register_spawn_binding(impl.lua.handle());
    impl.started = true;
    PSY_LOG_INFO("script: Vm started (Lua {}.{})", LUA_VERSION_MAJOR, LUA_VERSION_MINOR);
    return true;
}

void Vm::shutdown() {
    auto& impl = vm_impl();
    if (!impl.started) {
        return;
    }
    impl.registry.release_refs(impl.lua.handle());
    impl.lua.close();
    impl.started = false;
}

bool Vm::execute_string(std::string_view source, std::string_view name) {
    auto& impl = vm_impl();
    if (!impl.started) {
        PSY_LOG_ERROR("script: execute_string called before start()");
        return false;
    }
    lua_State* L = impl.lua.handle();
    if (detail::is_psy_script_name(name) || detail::has_psy_script_marker(source)) {
        std::string out;
        const bool ok = execute_psy_script(impl.psy, source, name.empty() ? "<psy>" : name, out);
        if (!ok) {
            PSY_LOG_ERROR("script: psy vm failed: {}", out);
        } else if (!out.empty()) {
            PSY_LOG_INFO("script: psy vm result: {}", out);
        }
        return ok;
    }

    if (detail::is_visual_graph_name(name) || detail::has_visual_graph_marker(source)) {
        std::string out;
        const bool ok = execute_visual_graph(L,
                                             detail::strip_visual_graph_marker(source),
                                             name.empty() ? "visual-graph" : name,
                                             out);
        if (!ok) {
            PSY_LOG_ERROR("script: visual graph failed: {}", out);
        } else if (!out.empty()) {
            PSY_LOG_INFO("script: visual graph result: {}", out);
        }
        return ok;
    }

    std::string chunk_name{name.empty() ? "<string>" : name};
    if (luaL_loadbuffer(L, source.data(), source.size(), chunk_name.c_str()) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        PSY_LOG_ERROR("script: load failed: {}", msg ? msg : "?");
        lua_pop(L, 1);
        return false;
    }
    std::string err;
    if (!impl.lua.pcall(0, 0, err)) {
        PSY_LOG_ERROR("script: pcall failed: {}", err);
        return false;
    }
    return true;
}

bool Vm::execute_file(std::string_view virtual_path) {
    // Wave A: the asset VFS (lane 05) does not yet expose a script-readable
    // text reader. For now `execute_file` is implemented in terms of the
    // host filesystem so tests + samples can drive it. When lane 05 ships
    // its `psynder::asset::read_text` we will swap this body for a VFS
    // load with no caller-visible signature change.
    auto& impl = vm_impl();
    if (!impl.started) {
        PSY_LOG_ERROR("script: execute_file called before start()");
        return false;
    }
    lua_State* L = impl.lua.handle();
    std::string path{virtual_path};
    if (detail::is_psy_script_name(path)) {
        std::ifstream file(path);
        if (!file) {
            PSY_LOG_ERROR("script: psy '{}' could not be opened", path);
            return false;
        }
        std::ostringstream buf;
        buf << file.rdbuf();
        std::string out;
        const bool ok = execute_psy_script(impl.psy, buf.str(), path, out);
        if (!ok) {
            PSY_LOG_ERROR("script: psy '{}' failed: {}", path, out);
        } else if (!out.empty()) {
            PSY_LOG_INFO("script: psy '{}' result: {}", path, out);
        }
        return ok;
    }

    if (detail::is_visual_graph_name(path)) {
        std::ifstream file(path);
        if (!file) {
            PSY_LOG_ERROR("script: visual graph '{}' could not be opened", path);
            return false;
        }
        std::ostringstream buf;
        buf << file.rdbuf();
        std::string out;
        const bool ok = execute_visual_graph(L, buf.str(), path, out);
        if (!ok) {
            PSY_LOG_ERROR("script: visual graph '{}' failed: {}", path, out);
        } else if (!out.empty()) {
            PSY_LOG_INFO("script: visual graph '{}' result: {}", path, out);
        }
        return ok;
    }

    if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        PSY_LOG_ERROR("script: loadfile '{}' failed: {}", path, msg ? msg : "?");
        lua_pop(L, 1);
        return false;
    }
    std::string err;
    if (!impl.lua.pcall(0, 0, err)) {
        PSY_LOG_ERROR("script: pcall failed: {}", err);
        return false;
    }
    return true;
}

bool Vm::execute_repl(std::string_view line, std::string& out) {
    auto& impl = vm_impl();
    out.clear();
    if (!impl.started) {
        out = "(vm not started)";
        return false;
    }
    lua_State* L = impl.lua.handle();
    const int top_before = lua_gettop(L);

    if (detail::is_psy_repl_command(line)) {
        const std::string_view payload = detail::psy_repl_payload(line);
        if (payload.empty()) {
            out = "psy repl command requires source after :psy";
            return false;
        }
        return execute_psy_script(impl.psy, payload, "<psy-repl>", out);
    }

    if (detail::is_visual_graph_repl_command(line)) {
        detail::VisualCompileResult compiled = detail::compile_visual_graph_repl(line);
        if (!compiled.ok) {
            out = compiled.diagnostic;
            return false;
        }
        if (luaL_loadbuffer(L,
                            compiled.lua_source.data(),
                            compiled.lua_source.size(),
                            "=visual-graph-repl") != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            out.assign(msg ? msg : "(load error)");
            lua_pop(L, 1);
            return false;
        }
        return run_loaded_chunk(L, top_before, out);
    }

    // First try as `return <expr>` so expressions echo their value (matches
    // the stock Lua REPL). On failure, fall back to a full statement.
    std::string wrapped;
    wrapped.reserve(line.size() + 8);
    wrapped.append("return ");
    wrapped.append(line);

    bool loaded = false;
    if (luaL_loadbuffer(L, wrapped.data(), wrapped.size(), "=repl") == LUA_OK) {
        loaded = true;
    } else {
        // Discard the error from the expression attempt; try as statement.
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, line.data(), line.size(), "=repl") == LUA_OK) {
            loaded = true;
        } else {
            const char* msg = lua_tostring(L, -1);
            out.assign(msg ? msg : "(load error)");
            lua_pop(L, 1);
            return false;
        }
    }
    (void)loaded;

    return run_loaded_chunk(L, top_before, out);
}

}  // namespace psynder::script
