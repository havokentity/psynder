// SPDX-License-Identifier: MIT
// Psynder — lua_State* RAII + safe stdlib subset.

#include "LuaState.h"

#include "core/Log.h"

namespace psynder::script::detail {

namespace {
// Load a curated subset of Lua's stdlib. The "io" library and `os.execute` /
// `os.exit` / `os.remove` / `os.rename` are stripped because game scripts
// have no business shelling out, deleting files on the player's disk, or
// killing the process. Asset I/O goes through the engine VFS (lane 05),
// exposed later via bindings if needed.
void open_safe_stdlib(lua_State* L) {
    // luaL_openlibs would pull io + everything; instead pick what we want.
    static const luaL_Reg libs[] = {
        { LUA_GNAME,      luaopen_base      },
        { LUA_LOADLIBNAME, luaopen_package  },
        { LUA_COLIBNAME,  luaopen_coroutine },
        { LUA_TABLIBNAME, luaopen_table     },
        { LUA_STRLIBNAME, luaopen_string    },
        { LUA_MATHLIBNAME, luaopen_math     },
        { LUA_UTF8LIBNAME, luaopen_utf8     },
        // os (subset — see scrub_os below)
        { LUA_OSLIBNAME,  luaopen_os        },
        // debug — kept for REPL stack traces. Hot paths never see debug.*.
        { LUA_DBLIBNAME,  luaopen_debug     },
    };
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);  // pop the lib table luaL_requiref left on the stack
    }

    // Scrub the dangerous bits of `os`.
    lua_getglobal(L, "os");
    if (lua_istable(L, -1)) {
        for (const char* name : { "execute", "exit", "remove",
                                  "rename", "tmpname", "getenv" }) {
            lua_pushnil(L);
            lua_setfield(L, -2, name);
        }
    }
    lua_pop(L, 1);

    // No `io` at all. (Never loaded above; this is belt-and-suspenders.)
    lua_pushnil(L);
    lua_setglobal(L, "io");

    // `dofile` / `loadfile` read raw paths off the host filesystem; replace
    // with stubs that raise an error directing users to the asset VFS.
    static const auto blocked = [](lua_State* L) -> int {
        return luaL_error(L,
            "%s is disabled; load scripts via the asset VFS",
            lua_tostring(L, lua_upvalueindex(1)));
    };
    for (const char* name : { "dofile", "loadfile" }) {
        lua_pushstring(L, name);
        lua_pushcclosure(L, blocked, 1);
        lua_setglobal(L, name);
    }
}
}  // namespace

bool LuaState::open() {
    if (L_) {
        return true;
    }
    L_ = luaL_newstate();
    if (!L_) {
        PSY_LOG_ERROR("script: luaL_newstate returned null");
        return false;
    }
    open_safe_stdlib(L_);
    return true;
}

void LuaState::close() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
}

bool LuaState::pcall(int nargs, int nresults, std::string& err_out) {
    if (!L_) {
        err_out = "lua state not open";
        return false;
    }
    int rc = lua_pcall(L_, nargs, nresults, 0);
    if (rc == LUA_OK) {
        return true;
    }
    const char* msg = lua_tostring(L_, -1);
    err_out.assign(msg ? msg : "(no error message)");
    lua_pop(L_, 1);
    return false;
}

}  // namespace psynder::script::detail
