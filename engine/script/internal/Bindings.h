// SPDX-License-Identifier: MIT
// Psynder — Lua bindings glue. Exposes the `world` global with a
// DOTS-shaped API. Per DESIGN.md §3.3, the API is **systems and queries
// only** — there is intentionally no `entity:tick()` method, no per-entity
// OOP escape hatch. Component reads/writes are declared up front so the
// scheduler can parallelize.

#pragma once

#include "core/Types.h"

#include <string>

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

namespace psynder::script::detail {

class ScriptRegistry;

// Installs the `world` global on `L`. The registry is associated with the
// state via a registry-table entry; binding C closures recover it on demand.
//
// Surface (mirrors §3.3):
//   world:component('Position')        -> declares / fetches a component id
//   world:register_system(             -- DOTS-only entry point
//       { reads = {'Position'},
//         writes = {'Velocity'},
//         name = 'integrate_motion'    -- optional
//       },
//       function(positions, velocities)
//           for i, p in ipairs(positions) do
//               velocities[i].x = velocities[i].x + p.x * 0.5
//           end
//       end)
//   world:run_systems(dt)              -- engine-driven; tests poke this
//   world:create_entity({Position={x=1,y=2,z=3}})
//
// What is deliberately MISSING: anything that returns an Entity object with
// methods. The Lua side gets opaque integer handles. You cannot write
// `entity:set_position(...)` because no entity userdata exists.
void install_world_api(lua_State* L, ScriptRegistry* registry);

// Drives one tick over all registered systems, dispatching component arrays
// as the system callback's positional arguments. Returns false if a system
// raised; the error is written to `err_out`. Wave A runs systems serially —
// the scheduler hookup lands once lane 04 + 06 expose the parallel API.
bool run_registered_systems(lua_State*       L,
                            ScriptRegistry&  registry,
                            f64              dt,
                            std::string&     err_out);

}  // namespace psynder::script::detail
