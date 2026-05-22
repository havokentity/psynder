// SPDX-License-Identifier: MIT
// Psynder — script-lane internal registry of Lua systems + components.
//
// Components mirror what `PSYNDER_COMPONENT(...)` (see engine/scene/EcsRegistry.h)
// registers on the engine side: a name keyed onto a u32 ComponentId. The
// scripting layer keeps its own name→id table because Lua refers to
// components by string (the API shape in DESIGN.md §3.3 is
// `reads={'Position'}, writes={'Velocity'}` — strings, not C++ tags).
//
// Systems are stored as Lua registry references (LUA_REGISTRYINDEX). The
// scheduler — eventually wired by lane 06 — pulls reads/writes off each
// entry to parallelize. Wave A just runs them serially when the engine asks.

#pragma once

#include "core/Types.h"
#include "scene/EcsRegistry.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

struct LuaSystem {
    std::string name;                        // optional human-readable
    std::vector<scene::ComponentId> reads;   // declared read set
    std::vector<scene::ComponentId> writes;  // declared write set
    int fn_ref = LUA_NOREF;                  // Lua-side function
};

// Per-VM bookkeeping that the binding layer pokes at. One instance lives in
// the Vm impl's pimpl; the lua_State carries a pointer to it via the
// registry so C closures can recover their owning Vm.
class ScriptRegistry {
   public:
    // Component name lookup / registration. Names live until the registry
    // dies, which matches the engine-wide rule that components register
    // at start-up and never unregister.
    scene::ComponentId register_or_get(std::string_view name);
    scene::ComponentId find(std::string_view name) const;  // 0 if missing
    std::string_view name_of(scene::ComponentId id) const;

    // Returns the new system's index. The Lua-side reference is owned by
    // the registry and released on shutdown.
    usize add_system(LuaSystem s);

    [[nodiscard]] std::span<LuaSystem> systems() noexcept;
    [[nodiscard]] std::span<const LuaSystem> systems() const noexcept;

    // Release all Lua-side function references. Safe to call repeatedly; the
    // caller passes the still-live lua_State so we can `luaL_unref`.
    void release_refs(lua_State* L);

   private:
    // Component name → id. We do NOT call scene::register_component here
    // because doing so from script-land would race with the engine's POD
    // registration. Lane 06 will eventually expose a "look up by name"
    // shim; until then, scripts that need a component declare it via
    // `world:component('Position')` and we mint an id off our own table.
    std::unordered_map<std::string, scene::ComponentId> names_;
    std::vector<std::string> names_by_id_;  // 0 unused
    std::vector<LuaSystem> systems_;
};

}  // namespace psynder::script::detail
