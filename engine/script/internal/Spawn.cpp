// SPDX-License-Identifier: MIT
// Psynder — Lua spawn binding implementation. The `world:spawn(archetype,
// kv_table)` entry point allocates a real `scene::Entity` and records the
// component bag in the per-VM storage table so existing DOTS systems
// observe the new entity.

#include "Spawn.h"

#include "Registry.h"

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

#include "core/Log.h"
#include "scene/EcsRegistry.h"

#include <cstring>
#include <string>
#include <string_view>

namespace psynder::script::detail {

namespace {

// Registry keys — must match the keys used in Bindings.cpp so the two
// entry points share state. Re-declared here rather than #include'd from
// Bindings.cpp because the latter keeps these in an anonymous namespace.
constexpr const char* kRegistryKey = "psynder.script.registry";
constexpr const char* kComponentStorage = "psynder.script.components";

ScriptRegistry* fetch_registry(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRegistryKey);
    void* p = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return static_cast<ScriptRegistry*>(p);
}

void push_component_storage(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kComponentStorage);
    if (lua_isnil(L, -1)) {
        // The Vm should have initialised storage during install_world_api,
        // but be defensive: create it on the fly if absent so a script lane
        // unit test that pokes spawn directly does not crash.
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kComponentStorage);
    }
}

void push_component_array(lua_State* L, scene::ComponentId id) {
    push_component_storage(L);  // storage
    lua_pushinteger(L, lua_Integer(id));
    lua_gettable(L, -2);  // storage, arr_or_nil
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushinteger(L, lua_Integer(id));
        lua_pushvalue(L, -2);
        lua_settable(L, -4);
    }
    lua_remove(L, -2);  // arr
}

// world:spawn(archetype_name, kv_table) -> integer entity raw handle
int l_world_spawn(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);  // self (the world table)
    const char* archetype = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);  // kv table

    ScriptRegistry* reg = fetch_registry(L);
    if (!reg) {
        return luaL_error(L, "world:spawn: registry missing");
    }

    // Allocate a real engine entity. The shared `scene::EcsRegistry::Get()` is the
    // ground truth — handle.raw is non-zero when the entity is valid.
    // (`Entity` lives in the top-level psynder namespace, not psynder::scene.)
    ::psynder::Entity entity = scene::EcsRegistry::Get().create();
    if (!entity.valid()) {
        return luaL_error(L, "world:spawn: scene::EcsRegistry::create failed");
    }
    const lua_Integer entity_raw = static_cast<lua_Integer>(entity.raw);

    // Record archetype + the per-component bag in the storage table so
    // DOTS systems see the spawned entity. The wire format mirrors
    // `create_entity`: per-component array of `{ entity = raw, data = {...} }`.
    lua_pushnil(L);
    while (lua_next(L, 3) != 0) {
        // -2: key (component name), -1: value (component data table)
        if (!lua_isstring(L, -2) || !lua_istable(L, -1)) {
            lua_pop(L, 2);
            return luaL_error(L, "world:spawn: kv must be {Name = {field=val}, ...}");
        }
        std::size_t len = 0;
        const char* name = lua_tolstring(L, -2, &len);
        scene::ComponentId cid = reg->register_or_get(std::string_view(name, len));

        push_component_array(L, cid);  // ..., key, value, arr
        lua_newtable(L);               // ..., key, value, arr, entry
        lua_pushinteger(L, entity_raw);
        lua_setfield(L, -2, "entity");
        lua_pushvalue(L, -3);
        lua_setfield(L, -2, "data");
        lua_pushstring(L, archetype);
        lua_setfield(L, -2, "archetype");
        lua_Integer n = luaL_len(L, -2);
        lua_seti(L, -2, n + 1);
        lua_pop(L, 1);  // pop arr
        lua_pop(L, 1);  // pop value, keep key
    }

    // Intentionally silent on the hot spawn path — lane 19 / editor tooling
    // is the natural place to surface spawn telemetry once available.
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
    lua_pop(L, 1);  // pop world table
}

}  // namespace psynder::script::detail
