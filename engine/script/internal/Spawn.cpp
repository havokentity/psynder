// SPDX-License-Identifier: MIT
// Psynder — Lua spawn binding implementation. The `world:spawn(archetype,
// kv_table)` entry point allocates a real `scene::Entity` and records the
// component bag in the per-VM storage table so existing DOTS systems
// observe the new entity.

#include "Spawn.h"

#include "Registry.h"
#include "WorldState.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "scene/EcsRegistry.h"

#include <string>
#include <string_view>

namespace psynder::script::detail {

namespace {

bool create_engine_entity(lua_State* L, const char* caller, ::psynder::Entity& entity) {
    entity = scene::EcsRegistry::Get().create();
    if (!entity.valid()) {
        luaL_error(L, "%s: scene::EcsRegistry::create failed", caller);
        return false;
    }
    return true;
}

void copy_table_entries(lua_State* L, int source_index, int dest_index) {
    const int source_abs = lua_absindex(L, source_index);
    const int dest_abs = lua_absindex(L, dest_index);
    lua_pushnil(L);
    while (lua_next(L, source_abs) != 0) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_settable(L, dest_abs);
    }
}

bool copy_position(lua_State* L, int options_index, int dest_index) {
    const int options_abs = lua_absindex(L, options_index);
    const int dest_abs = lua_absindex(L, dest_index);
    lua_getfield(L, options_abs, "position");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return true;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_newtable(L);
    lua_geti(L, -2, 1);
    lua_setfield(L, -2, "x");
    lua_geti(L, -2, 2);
    lua_setfield(L, -2, "y");
    lua_geti(L, -2, 3);
    lua_setfield(L, -2, "z");
    lua_setfield(L, dest_abs, "Position");
    lua_pop(L, 1);
    return true;
}

// world:spawn(archetype_name, kv_table) -> integer entity raw handle
int l_world_spawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self (the world table)
    const char* archetype = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);  // kv table

    ScriptRegistry* reg = fetch_world_registry(L);
    if (!reg) {
        return luaL_error(L, "world:spawn: registry missing");
    }

    ::psynder::Entity entity;
    if (!create_engine_entity(L, "world:spawn", entity)) {
        return 0;
    }

    std::string error;
    const lua_Integer entity_raw = static_cast<lua_Integer>(entity.raw);
    if (!append_component_bag(L, *reg, entity_raw, 3, archetype, error)) {
        return luaL_error(L, "world:spawn: %s", error.c_str());
    }

    lua_pushinteger(L, entity_raw);
    return 1;
}

// world:spawn_prop(prop_id[, { position={x,y,z}, components={...} }]) -> entity
int l_world_spawn_prop(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* prop_id = luaL_checkstring(L, 2);
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
    }

    ScriptRegistry* reg = fetch_world_registry(L);
    if (!reg) {
        return luaL_error(L, "world:spawn_prop: registry missing");
    }

    lua_newtable(L);
    const int components = lua_gettop(L);

    lua_newtable(L);
    lua_pushstring(L, prop_id);
    lua_setfield(L, -2, "id");
    lua_setfield(L, components, "Prop");

    if (!lua_isnoneornil(L, 3)) {
        lua_getfield(L, 3, "components");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                lua_pop(L, 2);
                return luaL_error(L, "world:spawn_prop: components must be a table");
            }
            copy_table_entries(L, -1, components);
        }
        lua_pop(L, 1);

        lua_getfield(L, 3, "overrides");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                lua_pop(L, 2);
                return luaL_error(L, "world:spawn_prop: overrides must be a table");
            }
            copy_table_entries(L, -1, components);
        }
        lua_pop(L, 1);

        if (!copy_position(L, 3, components)) {
            lua_pop(L, 1);
            return luaL_error(L, "world:spawn_prop: position must be an array table");
        }
    }

    ::psynder::Entity entity;
    if (!create_engine_entity(L, "world:spawn_prop", entity)) {
        lua_pop(L, 1);
        return 0;
    }

    std::string archetype = "prop:";
    archetype += prop_id;
    std::string error;
    const lua_Integer entity_raw = static_cast<lua_Integer>(entity.raw);
    if (!append_component_bag(L, *reg, entity_raw, components, archetype, error)) {
        lua_pop(L, 1);
        return luaL_error(L, "world:spawn_prop: %s", error.c_str());
    }

    lua_pop(L, 1);

    lua_pushinteger(L, entity_raw);
    return 1;
}

}  // namespace

void register_spawn_binding(lua_State* L) {
    lua_getglobal(L, "world");
    if (!lua_istable(L, -1)) {
        // install_world_api was not called; nothing to attach to. Drop the
        // bogus value and bail silently — the next start() that calls
        // install_world_api will re-invoke us at the appropriate time.
        lua_pop(L, 1);
        return;
    }
    lua_pushcfunction(L, l_world_spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, l_world_spawn_prop);
    lua_setfield(L, -2, "spawn_prop");
    lua_pop(L, 1);  // pop world table
}

}  // namespace psynder::script::detail
