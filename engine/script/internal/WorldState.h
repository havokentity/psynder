// SPDX-License-Identifier: MIT
// Psynder script lane -- internal Lua world-state helpers.

#pragma once

#include "core/Types.h"
#include "scene/EcsRegistry.h"

#include <string>
#include <string_view>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

class ScriptRegistry;

ScriptRegistry* fetch_world_registry(lua_State* L);
void stash_world_registry(lua_State* L, ScriptRegistry* registry);

void ensure_component_storage(lua_State* L);
void push_component_storage(lua_State* L);
void push_component_array(lua_State* L, scene::ComponentId id);

u64 next_script_entity_id(lua_State* L);

void append_component_entry(lua_State* L,
                            scene::ComponentId id,
                            lua_Integer entity,
                            int data_index,
                            std::string_view archetype);

bool append_component_bag(lua_State* L,
                          ScriptRegistry& registry,
                          lua_Integer entity,
                          int components_index,
                          std::string_view archetype,
                          std::string& error);

}  // namespace psynder::script::detail
