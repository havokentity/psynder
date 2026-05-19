// SPDX-License-Identifier: MIT
// Psynder — spatial-index routing surface. The ECS owns entity → cell
// membership and dispatches insert/remove/query to one of three backends
// based on entity tag at creation time. Backends (SAP, BVH8, hashed grid)
// land in Wave B; this header is the contract for them so other lanes can
// compile against the dispatcher.
//
// DESIGN.md §4.4 hints that the chunk's `dirty_mask` will drive incremental
// refit of the spatial index — we wire the surface for that here.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>

namespace psynder::scene {

// What backend a given entity / archetype should be indexed by.
// Choice is made once at archetype creation by querying a tag component.
enum class SpatialBackend : u8 {
    None,           // not indexed (UI overlays, gameplay-only entities)
    SweepAndPrune,  // moving rigid bodies — wins on temporal coherence
    Bvh,            // mostly-static world geometry
    HashedGrid,     // sparse, deeply non-overlapping (particles, debris)
};

struct SpatialKey {
    u32 raw = 0;    // backend-defined opaque handle
    constexpr bool valid() const noexcept { return raw != 0; }
};

// Backend-side API surface — Wave B fills these in. The dispatcher routes
// to the right backend per entity. None of these are virtual in the hot
// loop: the dispatcher chooses by tag enum and inlines the right call.
namespace detail {

struct ISpatialIndex {
    virtual ~ISpatialIndex() = default;
    virtual SpatialKey insert(u32 entity_index, const math::Aabb& bounds) = 0;
    virtual void       update(SpatialKey key, const math::Aabb& bounds)   = 0;
    virtual void       remove(SpatialKey key)                              = 0;
    // Hot path — NOT virtual. Backends provide a non-virtual templated
    // walk that the ECS calls directly through CRTP at the lane-07 raster
    // submission step. This virtual surface is for editor / debug only.
    virtual void       query_aabb(const math::Aabb& q,
                                  std::span<u32>  out_entities) const     = 0;
};

// Wave-B backends register here. Wave A returns null pointers — the
// dispatcher then silently no-ops.
ISpatialIndex* sap_backend() noexcept;
ISpatialIndex* bvh_backend() noexcept;
ISpatialIndex* grid_backend() noexcept;

// Resolve `backend` → pointer. Returns null if Wave B hasn't shipped yet.
ISpatialIndex* resolve(SpatialBackend backend) noexcept;

}  // namespace detail

}  // namespace psynder::scene
