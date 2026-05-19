// SPDX-License-Identifier: MIT
// Psynder — ECS world stub. Lane 06 fleshes this out (archetype-chunked
// storage, queries, structural batching, spatial indices). Phase-0 stub
// gives non-crashing defaults so dependent code compiles + links.

#include "World.h"

#include <atomic>

namespace psynder::scene {

namespace {
std::atomic<ComponentId> g_next_component_id{1};
}  // namespace

ComponentId register_component(const ComponentTypeInfo& /*info*/) {
    return g_next_component_id.fetch_add(1, std::memory_order_relaxed);
}

World& World::Get() {
    static World w;
    return w;
}

Entity World::create()                          { return Entity{}; }
void   World::destroy(Entity /*e*/)             {}
bool   World::alive(Entity e) const noexcept    { return e.valid(); }

// The templated add/get/remove are intentionally left to lane 06 to
// implement against the real chunk storage.

}  // namespace psynder::scene
