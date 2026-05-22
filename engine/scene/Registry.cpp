// SPDX-License-Identifier: MIT
// Psynder — component registry impl. Lane 06.

// Full definition of `ComponentTypeInfo` lives in `EcsRegistry.h`; pulling it in
// here lets `register_component` translate to/from our internal POD.
#include "EcsRegistry.h"
#include "Registry.h"

#include <atomic>

namespace psynder::scene::detail {

ComponentRegistry& ComponentRegistry::Get() {
    static ComponentRegistry r;
    return r;
}

ComponentId ComponentRegistry::register_type(u32 size, u32 align, const char* name) {
    std::lock_guard<std::mutex> lk(mutex_);
    ComponentRecord rec{};
    rec.id = from_index(static_cast<u32>(records_.size()));
    rec.size = size;
    rec.align = align;
    rec.name = name ? name : "";
    records_.push_back(rec);
    return rec.id;
}

ComponentRecord ComponentRegistry::lookup(ComponentId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = to_index(id);
    if (idx >= records_.size())
        return ComponentRecord{};
    return records_[idx];
}

u32 ComponentRegistry::count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<u32>(records_.size());
}

}  // namespace psynder::scene::detail

// ─── Public-facing C++ glue ───────────────────────────────────────────
namespace psynder::scene {

ComponentId register_component(const ComponentTypeInfo& info) {
    return detail::ComponentRegistry::Get().register_type(info.size, info.align, info.name);
}

}  // namespace psynder::scene
