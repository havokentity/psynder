// SPDX-License-Identifier: MIT
// Psynder — Lua bindings impl. The DOTS contract (§3.3) is enforced by
// SHAPE: there is no entity userdata, no methods on a per-entity object,
// no per-entity callback. The only way to mutate component data from Lua
// is to register a system that receives whole component arrays.
//
// Wave-A component storage is a per-VM table keyed by component id. Lane 06
// will eventually own the real archetype-chunked storage; the binding here
// is shaped so swapping the backing store is a localised change.

#include "Bindings.h"
#include "Registry.h"

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include "core/Log.h"
#include "scene/World.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::script::detail {

namespace {

// ─── Per-VM state stashed on the Lua registry ────────────────────────────
constexpr const char* kRegistryKey      = "psynder.script.registry";
constexpr const char* kComponentStorage = "psynder.script.components";
constexpr const char* kEntityCounter    = "psynder.script.next_entity";

ScriptRegistry* fetch_registry(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryKey);
    void* p = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return static_cast<ScriptRegistry*>(p);
}

// Push a fresh per-component storage table onto the stack and stash it on
// the registry. Returns the table reference for retrieval via the storage
// key.
void ensure_component_storage(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kComponentStorage);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, kComponentStorage);
    } else {
        lua_pop(L, 1);
    }
}

void push_component_storage(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kComponentStorage);
}

// Per-component table accessor: storage[component_id] -> array of {entity=,data=}
void push_component_array(lua_State* L, scene::ComponentId id) {
    push_component_storage(L);          // storage
    lua_pushinteger(L, lua_Integer(id));
    lua_gettable(L, -2);                // storage, arr_or_nil
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);                  // storage
        lua_newtable(L);                // storage, new_arr
        lua_pushinteger(L, lua_Integer(id));
        lua_pushvalue(L, -2);           // storage, new_arr, id, new_arr
        lua_settable(L, -4);            // storage, new_arr
    }
    lua_remove(L, -2);                  // arr
}

u64 next_entity_id(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEntityCounter);
    lua_Integer cur = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    cur += 1;
    lua_pushinteger(L, cur);
    lua_setfield(L, LUA_REGISTRYINDEX, kEntityCounter);
    return static_cast<u64>(cur);
}

// Helper: read the `reads` / `writes` list off a config table at index `t`.
// Field is an array of string component names. Each string is registered
// (idempotent) and the resulting ComponentId pushed into `out`.
bool extract_id_list(lua_State*                       L,
                     int                              t,
                     const char*                      field,
                     ScriptRegistry&                  reg,
                     std::vector<scene::ComponentId>& out) {
    lua_getfield(L, t, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "register_system: '%s' must be a table of component names",
                   field);
        return false;
    }
    lua_Integer n = luaL_len(L, -1);
    out.reserve(static_cast<usize>(n));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, -1, i);
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 2);
            luaL_error(L, "register_system: '%s'[%d] must be a string",
                       field, int(i));
            return false;
        }
        std::size_t len = 0;
        const char* s   = lua_tolstring(L, -1, &len);
        out.push_back(reg.register_or_get(std::string_view(s, len)));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

// ─── Lua-side world API ──────────────────────────────────────────────────

// world:component(name) -> integer id
int l_world_component(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self
    const char* name = luaL_checkstring(L, 2);
    ScriptRegistry* reg = fetch_registry(L);
    if (!reg) {
        return luaL_error(L, "world: registry missing");
    }
    lua_pushinteger(L, lua_Integer(reg->register_or_get(name)));
    return 1;
}

// world:register_system(config_table, function)
// config_table: { reads = {'A','B'}, writes = {'C'}, name = 'optional' }
int l_world_register_system(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self
    luaL_checktype(L, 2, LUA_TTABLE);  // config
    luaL_checktype(L, 3, LUA_TFUNCTION);

    ScriptRegistry* reg = fetch_registry(L);
    if (!reg) {
        return luaL_error(L, "world: registry missing");
    }

    LuaSystem sys;

    // Optional name
    lua_getfield(L, 2, "name");
    if (lua_isstring(L, -1)) {
        sys.name = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    if (!extract_id_list(L, 2, "reads",  *reg, sys.reads))  { return 0; }
    if (!extract_id_list(L, 2, "writes", *reg, sys.writes)) { return 0; }

    // Take a registry reference to the function (top of stack at index 3).
    lua_pushvalue(L, 3);
    sys.fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    usize idx = reg->add_system(std::move(sys));
    lua_pushinteger(L, lua_Integer(idx));
    return 1;
}

// world:create_entity(table_of_components) -> integer entity id
// table_of_components: { Position = {x=,y=,z=}, Velocity = {...}, ... }
int l_world_create_entity(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);   // self
    luaL_checktype(L, 2, LUA_TTABLE);   // components

    ScriptRegistry* reg = fetch_registry(L);
    if (!reg) {
        return luaL_error(L, "world: registry missing");
    }

    const u64 entity_id = next_entity_id(L);

    // For each k,v in the components table: register the component name and
    // append {entity=entity_id, data=v} to the per-component array.
    lua_pushnil(L);   // first key
    while (lua_next(L, 2) != 0) {
        // -2: key (component name), -1: value (component data table)
        if (!lua_isstring(L, -2) || !lua_istable(L, -1)) {
            lua_pop(L, 2);
            return luaL_error(L,
                "create_entity: components must be {Name = {field=val}, ...}");
        }
        std::size_t len = 0;
        const char* name = lua_tolstring(L, -2, &len);
        scene::ComponentId cid = reg->register_or_get(
            std::string_view(name, len));

        push_component_array(L, cid);   // ..., key, value, arr
        // Build entry: {entity=entity_id, data=value (deep copied via ref)}
        lua_newtable(L);                // ..., key, value, arr, entry
        lua_pushinteger(L, lua_Integer(entity_id));
        lua_setfield(L, -2, "entity");
        lua_pushvalue(L, -3);           // copy value to top
        lua_setfield(L, -2, "data");
        lua_Integer n = luaL_len(L, -2);
        lua_seti(L, -2, n + 1);         // arr[n+1] = entry
        lua_pop(L, 1);                  // pop arr
        // Pop value, leave key for next iteration.
        lua_pop(L, 1);
    }

    lua_pushinteger(L, lua_Integer(entity_id));
    return 1;
}

// world:run_systems(dt). Internal driver for tests; the engine scheduler
// will invoke a parallel form later (lane 04).
int l_world_run_systems(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    f64 dt = luaL_optnumber(L, 2, 0.0);
    ScriptRegistry* reg = fetch_registry(L);
    if (!reg) {
        return luaL_error(L, "world: registry missing");
    }
    std::string err;
    if (!run_registered_systems(L, *reg, dt, err)) {
        return luaL_error(L, "system raised: %s", err.c_str());
    }
    return 0;
}

// world:system_count() -> n. Convenience for tests / introspection.
int l_world_system_count(lua_State* L) {
    ScriptRegistry* reg = fetch_registry(L);
    if (!reg) {
        return luaL_error(L, "world: registry missing");
    }
    lua_pushinteger(L, lua_Integer(reg->systems().size()));
    return 1;
}

const luaL_Reg kWorldMethods[] = {
    { "component",       l_world_component       },
    { "register_system", l_world_register_system },
    { "create_entity",   l_world_create_entity   },
    { "run_systems",     l_world_run_systems     },
    { "system_count",    l_world_system_count    },
    { nullptr, nullptr }
};

}  // namespace

void install_world_api(lua_State* L, ScriptRegistry* registry) {
    // Stash the registry pointer on Lua's registry so binding C closures
    // can recover it without a global.
    lua_pushlightuserdata(L, registry);
    lua_setfield(L, LUA_REGISTRYINDEX, kRegistryKey);

    ensure_component_storage(L);

    // Build the `world` table with methods. We deliberately do NOT install
    // any per-entity helpers (no `world.entity`, no `:tick`, no GameObject
    // userdata). The only way to touch component data is the system
    // callback, which receives whole arrays — DOTS-compliant by construction.
    lua_newtable(L);
    luaL_setfuncs(L, kWorldMethods, 0);
    lua_setglobal(L, "world");

    // Refuse to load user code that tries to install an `Entity` global —
    // protective shim, not a real sandbox. The DOTS contract is enforced
    // primarily by the absence of entity-shaped APIs above.
}

bool run_registered_systems(lua_State*       L,
                            ScriptRegistry&  registry,
                            f64              dt,
                            std::string&     err_out) {
    auto systems = registry.systems();
    for (LuaSystem& sys : systems) {
        if (sys.fn_ref == LUA_NOREF || sys.fn_ref == LUA_REFNIL) {
            continue;
        }
        // Push the function.
        lua_rawgeti(L, LUA_REGISTRYINDEX, sys.fn_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            continue;
        }

        // Push read arrays then write arrays as positional arguments. The
        // user's Lua function gets:  fn(reads..., writes...)
        // — this is the only API by which their code touches component
        // data, which is exactly the DOTS rule from §3.3.
        int nargs = 0;
        for (scene::ComponentId cid : sys.reads) {
            push_component_array(L, cid);
            ++nargs;
        }
        for (scene::ComponentId cid : sys.writes) {
            push_component_array(L, cid);
            ++nargs;
        }
        // Trailing dt for convenience.
        lua_pushnumber(L, dt);
        ++nargs;

        int rc = lua_pcall(L, nargs, 0, 0);
        if (rc != LUA_OK) {
            const char* msg = lua_tostring(L, -1);
            err_out.assign(msg ? msg : "(no error message)");
            lua_pop(L, 1);
            return false;
        }
    }
    return true;
}

}  // namespace psynder::script::detail
