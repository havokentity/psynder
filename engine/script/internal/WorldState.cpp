// SPDX-License-Identifier: MIT
// Psynder script lane -- internal Lua world-state helpers.

#include "WorldState.h"

#include "Registry.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

namespace {

constexpr const char* kRegistryKey = "psynder.script.registry";
constexpr const char* kComponentStorage = "psynder.script.components";
constexpr const char* kEntityCounter = "psynder.script.next_entity";

}  // namespace

ScriptRegistry* fetch_world_registry(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryKey);
    void* p = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return static_cast<ScriptRegistry*>(p);
}

void stash_world_registry(lua_State* L, ScriptRegistry* registry) {
    lua_pushlightuserdata(L, registry);
    lua_setfield(L, LUA_REGISTRYINDEX, kRegistryKey);
}

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
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kComponentStorage);
    }
}

void push_component_array(lua_State* L, scene::ComponentId id) {
    push_component_storage(L);
    lua_pushinteger(L, lua_Integer(id));
    lua_gettable(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushinteger(L, lua_Integer(id));
        lua_pushvalue(L, -2);
        lua_settable(L, -4);
    }
    lua_remove(L, -2);
}

u64 next_script_entity_id(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEntityCounter);
    lua_Integer cur = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    cur += 1;
    lua_pushinteger(L, cur);
    lua_setfield(L, LUA_REGISTRYINDEX, kEntityCounter);
    return static_cast<u64>(cur);
}

void append_component_entry(lua_State* L,
                            scene::ComponentId id,
                            lua_Integer entity,
                            int data_index,
                            std::string_view archetype) {
    const int data_abs = lua_absindex(L, data_index);
    push_component_array(L, id);
    lua_newtable(L);
    lua_pushinteger(L, entity);
    lua_setfield(L, -2, "entity");
    lua_pushvalue(L, data_abs);
    lua_setfield(L, -2, "data");
    if (!archetype.empty()) {
        lua_pushlstring(L, archetype.data(), archetype.size());
        lua_setfield(L, -2, "archetype");
    }
    const lua_Integer n = luaL_len(L, -2);
    lua_seti(L, -2, n + 1);
    lua_pop(L, 1);
}

bool append_component_bag(lua_State* L,
                          ScriptRegistry& registry,
                          lua_Integer entity,
                          int components_index,
                          std::string_view archetype,
                          std::string& error) {
    const int components_abs = lua_absindex(L, components_index);
    if (!lua_istable(L, components_abs)) {
        error = "components must be a table";
        return false;
    }

    lua_pushnil(L);
    while (lua_next(L, components_abs) != 0) {
        if (!lua_isstring(L, -2) || !lua_istable(L, -1)) {
            lua_pop(L, 2);
            error = "components must be {Name = {field=val}, ...}";
            return false;
        }
        std::size_t len = 0;
        const char* name = lua_tolstring(L, -2, &len);
        const scene::ComponentId id = registry.register_or_get(std::string_view{name, len});
        append_component_entry(L, id, entity, -1, archetype);
        lua_pop(L, 1);
    }
    return true;
}

}  // namespace psynder::script::detail
