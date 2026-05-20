// SPDX-License-Identifier: MIT
// Psynder — Lua `world:spawn(archetype, kv_table)` binding (Wave B).
//
// Phase 1 of the script-side spawn API. Where `world:create_entity` (in
// Bindings.cpp) builds a per-VM component table for DOTS systems to iterate,
// `world:spawn` reaches into the shared `scene::World::Get()` singleton and
// allocates a real engine entity. Lua scripts get the entity handle back as
// an integer so they can stash it for later destruction or query.
//
// The archetype name is a hint for the eventual prefab system (lane 06
// Wave C) — for now the binding just records it on the entity via a
// side-channel string (the entity itself stays POD per DESIGN.md §3.3).
//
// The kv_table is `{ ComponentName = { fields... }, ... }`. Wave B only
// records the bag in the script-side storage table (so DOTS systems still
// observe spawned entities) — true ECS component attachment lands in
// Wave C alongside the schema-driven auto-bind.

#pragma once

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

// Adds `world:spawn(archetype_name, kv_table)` to the existing `world`
// global. Pre-conditions: `install_world_api` has already been called on
// `L`, so the `world` table exists at global scope. The function looks up
// `world`, sets the `spawn` field on it to a C closure, and returns.
//
// The closure's behaviour:
//   - First argument is the archetype name (string). Validated as string.
//   - Second argument is the kv table (`{ Position = {x=,y=,z=}, ... }`).
//     Validated as table. Each key is treated as a component name and
//     each value as the field bag, recorded in the per-VM storage table
//     keyed by `world:component(key)`.
//   - Allocates an entity via `scene::World::Get().create()`.
//   - Returns the entity handle as an integer (matching the existing
//     `create_entity` shape so script-side code can treat the two
//     interchangeably).
void register_spawn_binding(lua_State* L);

}  // namespace psynder::script::detail
