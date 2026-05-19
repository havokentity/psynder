// SPDX-License-Identifier: MIT
// Psynder — component registry. Tracks size / alignment / name for every
// type registered via PSYNDER_COMPONENT(...). Lane 06 internal.
//
// Concurrency: registration happens at static init (single-threaded by
// definition) and once again from World::add when a new type appears at
// runtime. Lookups are read-only after registration. A single mutex
// protects insertion.

#pragma once

#include "core/Types.h"

#include <mutex>
#include <vector>

namespace psynder::scene {
// `ComponentId` is also defined in the public `World.h`. We mirror the
// typedef here to break the include cycle (`World.h` → `World_Internal.h`
// → `Registry.h`). The C++ rules permit identical typedef redeclarations.
using ComponentId = u32;
}  // namespace psynder::scene

namespace psynder::scene::detail {

// POD record we store per registered component. Mirrors `ComponentTypeInfo`
// in the public header but lives in our `detail` namespace so it doesn't
// clash. The `ComponentRegistry` translates between the two at the API
// boundary.
struct ComponentRecord {
    ComponentId  id    = 0;
    u32          size  = 0;
    u32          align = 0;
    const char*  name  = "";
};

class ComponentRegistry {
public:
    static ComponentRegistry& Get();

    // Returns the (possibly newly-assigned) ComponentId.
    ComponentId register_type(u32 size, u32 align, const char* name);

    ComponentRecord lookup(ComponentId id) const;

    static u32 to_index(ComponentId id) noexcept { return id - 1; }
    static ComponentId from_index(u32 i) noexcept { return static_cast<ComponentId>(i + 1); }

    u32 count() const;

private:
    ComponentRegistry() = default;
    mutable std::mutex            mutex_;
    std::vector<ComponentRecord>  records_;   // indexed by (id - 1)
};

}  // namespace psynder::scene::detail
