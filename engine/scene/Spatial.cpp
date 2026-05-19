// SPDX-License-Identifier: MIT
// Psynder — spatial-index dispatcher + query router. Lane 06.
//
// Wave A declared the routing surface; Wave B wires it to the three live
// backends (SAP / BVH / hashed grid) and adds `route_query`. The router
// implements the §9.4 table:
//
//   Raycast               → BVH
//   Frustum cull          → BVH
//   AABB overlap          → BVH
//   Broadphase            → SAP (or HashedGrid if a region overrides)
//   Nearest-neighbour     → HashedGrid
//
// A region may override the default for any (kind, region) pair via
// `set_region_override`. Lookup is one mutex-free read in the steady
// state.

#include "Spatial.h"
#include "Spatial_Internal.h"

#include <algorithm>
#include <vector>

namespace psynder::scene {

namespace detail {

ISpatialIndex* resolve(SpatialBackend backend) noexcept {
    switch (backend) {
        case SpatialBackend::SweepAndPrune: return sap_backend();
        case SpatialBackend::Bvh:           return bvh_backend();
        case SpatialBackend::HashedGrid:    return grid_backend();
        case SpatialBackend::None:
        default:                            return nullptr;
    }
}

}  // namespace detail

// ─── Router state ────────────────────────────────────────────────────────
namespace {

// Override table: linear vector of (kind, region, backend). Wave-B scale
// is dozens of regions max; linear scan is faster than a hash for that.
struct Override {
    QueryKind       kind;
    RegionId        region;
    SpatialBackend  backend;
};

// File-static storage so a process running multiple tests (Catch2)
// shares the same router state.
std::vector<Override>& overrides() noexcept {
    static std::vector<Override> v;
    return v;
}

PSY_FORCEINLINE SpatialBackend default_for_kind(QueryKind kind) noexcept {
    switch (kind) {
        case QueryKind::Raycast:           return SpatialBackend::Bvh;
        case QueryKind::FrustumCull:       return SpatialBackend::Bvh;
        case QueryKind::AabbOverlap:       return SpatialBackend::Bvh;
        case QueryKind::Broadphase:        return SpatialBackend::SweepAndPrune;
        case QueryKind::NearestNeighbour:  return SpatialBackend::HashedGrid;
    }
    return SpatialBackend::None;
}

}  // namespace

SpatialBackend route_query(QueryKind kind, RegionId region) noexcept {
    for (const auto& o : overrides()) {
        if (o.kind == kind && o.region == region) return o.backend;
    }
    // If no region-specific override, look for a global override for the
    // same kind (region == kGlobalRegion). This implements the "set the
    // default once" ergonomics.
    if (region != kGlobalRegion) {
        for (const auto& o : overrides()) {
            if (o.kind == kind && o.region == kGlobalRegion) return o.backend;
        }
    }
    return default_for_kind(kind);
}

void set_region_override(QueryKind kind, RegionId region,
                         SpatialBackend backend) noexcept {
    auto& v = overrides();
    auto it = std::find_if(v.begin(), v.end(),
        [&](const Override& o) noexcept {
            return o.kind == kind && o.region == region;
        });

    if (backend == SpatialBackend::None) {
        if (it != v.end()) v.erase(it);
        return;
    }
    if (it == v.end()) {
        v.push_back({ kind, region, backend });
    } else {
        it->backend = backend;
    }
}

void clear_region_overrides() noexcept {
    overrides().clear();
}

}  // namespace psynder::scene
