// SPDX-License-Identifier: MIT
// Psynder — spatial-index routing surface. The ECS owns entity → cell
// membership and dispatches insert/remove/query to one of three backends
// based on entity tag at creation time.
//
// Wave A landed the routing skeleton (enum + ISpatialIndex contract +
// nullptr backends). Wave B fills in:
//   - SAP (sweep-and-prune) — incremental 3-axis broadphase
//   - BVH (entity-AABB) — refit per frame, async rebuild trigger at 1.3×
//   - Hashed grid — sparse 3D bucketed nearest-neighbour
//   - Query router — pick the cheapest backend per `QueryKind` per region
//
// DESIGN.md §9.4 — routing & maintenance table.

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
    u32 raw = 0;  // backend-defined opaque handle
    constexpr bool valid() const noexcept { return raw != 0; }
};

// ─── Query routing (Wave B) ──────────────────────────────────────────────
//
// The router maps a `QueryKind` + region to one of the live backends, per
// the DESIGN.md §9.4 table:
//
//   Raycast / shadow ray              → Bvh
//   Frustum cull, dynamic actors      → Bvh
//   Physics broadphase                → SweepAndPrune (or HashedGrid at high N)
//   AI / audio nearest-neighbour      → HashedGrid
//
// A region may opt into a non-default backend via `set_region_override`.

enum class QueryKind : u8 {
    Raycast,           // BVH
    FrustumCull,       // BVH
    Broadphase,        // SAP (or HashedGrid)
    NearestNeighbour,  // HashedGrid
    AabbOverlap,       // BVH (general spatial overlap test)
};

// Region identifier — opaque u32 chosen by the world (per indoor cell /
// outdoor patch / level subdivision). Region 0 = the global default region.
using RegionId = u32;
inline constexpr RegionId kGlobalRegion = 0;

// Returns the backend choice for `(kind, region)`. The hot path is one
// load + one switch, no virtual dispatch.
SpatialBackend route_query(QueryKind kind, RegionId region = kGlobalRegion) noexcept;

// Override the default routing for a single region. `backend == None`
// clears the override (falls back to the table for `kind`). Pass `region
// == kGlobalRegion` to override the global default for that kind.
void set_region_override(QueryKind kind, RegionId region, SpatialBackend backend) noexcept;

// Clear every override. Used by tests and the world reset path.
void clear_region_overrides() noexcept;

// ─── Backend-side API surface ────────────────────────────────────────────
// Wave B fills these in. The dispatcher routes to the right backend per
// entity. None of these are virtual in the hot loop: the dispatcher chooses
// by tag enum and inlines the right call.
namespace detail {

struct ISpatialIndex {
    virtual ~ISpatialIndex() = default;
    virtual SpatialKey insert(u32 entity_index, const math::Aabb& bounds) = 0;
    virtual void update(SpatialKey key, const math::Aabb& bounds) = 0;
    virtual void remove(SpatialKey key) = 0;
    // Hot path — NOT virtual. Backends provide a non-virtual templated
    // walk that the ECS calls directly through CRTP at the lane-07 raster
    // submission step. This virtual surface is for editor / debug only.
    virtual void query_aabb(const math::Aabb& q, std::span<u32> out_entities) const = 0;
};

// Wave-B backends. Each returns a non-null singleton for its lifetime.
ISpatialIndex* sap_backend() noexcept;
ISpatialIndex* bvh_backend() noexcept;
ISpatialIndex* grid_backend() noexcept;

// Resolve `backend` → pointer. Returns null for `None`.
ISpatialIndex* resolve(SpatialBackend backend) noexcept;

// ─── Wave-B extended surface (non-virtual hot helpers) ─────────────────
//
// These are concrete, non-virtual helpers the world / physics broadphase
// call directly. They live on the lane-internal contract; sibling lanes
// reach them through the resolve() pointer + a static_cast (NOT yet — keep
// the cross-lane surface to the virtual ISpatialIndex until the consumers
// land in Wave B+).

// BVH — measure refit SAH cost vs as-built and recommend async rebuild
// when ratio > 1.3 (DESIGN.md §9.4).
struct BvhRefitStats {
    f32 sah_cost = 0.0f;
    f32 sah_cost_as_built = 0.0f;
    bool should_async_rebuild = false;
};
BvhRefitStats bvh_refit() noexcept;

// SAP — drain the most recent overlap-pair list. Each entry is a pair of
// entity indices (lower index first). Cleared on next `sap_step()`.
struct SapPair {
    u32 a;
    u32 b;
};
std::span<const SapPair> sap_overlap_pairs() noexcept;
// Walk endpoint arrays + emit overlap pairs. O(n + k) per axis where k is
// the number of swaps from the previous step.
void sap_step() noexcept;

// Hashed grid — radius query around `center`, writes up to `out.size()`
// entity indices, returns how many were written.
u32 grid_radius_query(math::Vec3 center, f32 radius, std::span<u32> out) noexcept;

}  // namespace detail

}  // namespace psynder::scene
