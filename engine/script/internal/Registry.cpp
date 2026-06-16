// SPDX-License-Identifier: MIT
// Psynder — script-lane registry impl.

#include "Registry.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lauxlib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <string>
#include <utility>

namespace psynder::script::detail {

namespace {
// Component ids minted by the script lane start at 0x80000000 so they never
// collide with engine-side ids (which start at 1, see scene/EcsRegistry.cpp).
// When lane 06 lands a real "find by name" shim this can collapse down to a
// single id space; the bit is the temporary boundary.
inline constexpr scene::ComponentId kScriptIdBase = 0x80000000u;
}  // namespace

scene::ComponentId ScriptRegistry::register_or_get(std::string_view name) {
    std::string key{name};
    auto it = names_.find(key);
    if (it != names_.end()) {
        return it->second;
    }
    if (names_by_id_.empty()) {
        // Reserve slot 0 so a 0 id is reliably "invalid".
        names_by_id_.emplace_back();
    }
    scene::ComponentId id = kScriptIdBase + static_cast<scene::ComponentId>(names_by_id_.size());
    names_.emplace(key, id);
    names_by_id_.push_back(std::move(key));
    return id;
}

scene::ComponentId ScriptRegistry::find(std::string_view name) const {
    auto it = names_.find(std::string{name});
    return it == names_.end() ? 0u : it->second;
}

std::string_view ScriptRegistry::name_of(scene::ComponentId id) const {
    if (id < kScriptIdBase) {
        return {};
    }
    const usize idx = id - kScriptIdBase;
    if (idx == 0 || idx >= names_by_id_.size()) {
        return {};
    }
    return names_by_id_[idx];
}

usize ScriptRegistry::add_system(LuaSystem s) {
    systems_.push_back(std::move(s));
    return systems_.size() - 1;
}

std::span<LuaSystem> ScriptRegistry::systems() noexcept {
    return std::span<LuaSystem>(systems_);
}

std::span<const LuaSystem> ScriptRegistry::systems() const noexcept {
    return std::span<const LuaSystem>(systems_);
}

void ScriptRegistry::release_refs(lua_State* L) {
    if (!L) {
        systems_.clear();
        return;
    }
    for (auto& s : systems_) {
        if (s.fn_ref != LUA_NOREF && s.fn_ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, s.fn_ref);
            s.fn_ref = LUA_NOREF;
        }
    }
    systems_.clear();
}

}  // namespace psynder::script::detail
