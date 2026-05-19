// SPDX-License-Identifier: MIT
// Psynder — lane-06 internal: shared types for the three spatial backends.
//
// These are pure-data structs used by SpatialBvh.cpp / SpatialSap.cpp /
// SpatialGrid.cpp. The dispatcher in Spatial.cpp resolves to one of three
// singletons that wrap a State struct from this header.

#pragma once

#include "Spatial.h"

#include "core/Types.h"
#include "math/Math.h"

#include <limits>
#include <vector>

namespace psynder::scene::detail {

// ─── AABB helpers ───────────────────────────────────────────────────────
// We define an internal Aabb on top of math::Aabb so we can carry the
// usual "reset / expand / surface_area" helpers without leaking them into
// the public math API. (Lane 08 has the same pattern for its triangle BVH
// — DESIGN-permissible since the contracts don't cross lanes.)

PSY_FORCEINLINE math::Aabb aabb_invalid() noexcept {
    const f32 inf = std::numeric_limits<f32>::infinity();
    return math::Aabb{ { +inf, +inf, +inf }, { -inf, -inf, -inf } };
}

PSY_FORCEINLINE bool aabb_valid(const math::Aabb& a) noexcept {
    return a.min.x <= a.max.x && a.min.y <= a.max.y && a.min.z <= a.max.z;
}

PSY_FORCEINLINE void aabb_expand(math::Aabb& a, const math::Aabb& o) noexcept {
    if (o.min.x < a.min.x) a.min.x = o.min.x;
    if (o.min.y < a.min.y) a.min.y = o.min.y;
    if (o.min.z < a.min.z) a.min.z = o.min.z;
    if (o.max.x > a.max.x) a.max.x = o.max.x;
    if (o.max.y > a.max.y) a.max.y = o.max.y;
    if (o.max.z > a.max.z) a.max.z = o.max.z;
}

PSY_FORCEINLINE void aabb_expand(math::Aabb& a, math::Vec3 p) noexcept {
    if (p.x < a.min.x) a.min.x = p.x;
    if (p.y < a.min.y) a.min.y = p.y;
    if (p.z < a.min.z) a.min.z = p.z;
    if (p.x > a.max.x) a.max.x = p.x;
    if (p.y > a.max.y) a.max.y = p.y;
    if (p.z > a.max.z) a.max.z = p.z;
}

PSY_FORCEINLINE bool aabb_overlap(const math::Aabb& a, const math::Aabb& b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

PSY_FORCEINLINE f32 aabb_surface_area(const math::Aabb& a) noexcept {
    if (!aabb_valid(a)) return 0.0f;
    const f32 dx = a.max.x - a.min.x;
    const f32 dy = a.max.y - a.min.y;
    const f32 dz = a.max.z - a.min.z;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

PSY_FORCEINLINE math::Vec3 aabb_centroid(const math::Aabb& a) noexcept {
    return { 0.5f * (a.min.x + a.max.x),
             0.5f * (a.min.y + a.max.y),
             0.5f * (a.min.z + a.max.z) };
}

// ─── BVH (entity-AABB) ──────────────────────────────────────────────────
// Binary BVH, top-down SAH-binned build, bottom-up refit. Leaves point
// at a contiguous slice of `prim_indices`, each of which is an index
// into `prims` (the per-entity AABB array). Refit walks the tree
// bottom-up to recompute node bounds, then compares the refit SAH cost
// against the as-built cost to decide whether to recommend an async
// rebuild (DESIGN.md §9.4 — 1.3× threshold).
//
// We carry the parent-pointer + child indices in a single flat node
// array so refit is a cache-friendly post-order walk. No `virtual`, no
// `shared_ptr`, no per-frame allocations.

inline constexpr f32 kBvhRebuildRatio = 1.3f;     // DESIGN.md §9.4
inline constexpr u32 kBvhMaxLeafPrims = 4;
inline constexpr u32 kBvhSahBuckets   = 12;

struct BvhPrim {
    math::Aabb bounds;
    u32        entity_index = 0;       // 0xFFFFFFFFu = free slot
};

struct BvhNode {
    math::Aabb bounds;
    u32  left       = 0xFFFFFFFFu;     // child node idx — 0xFFFFFFFFu = leaf
    u32  right      = 0xFFFFFFFFu;
    u32  first_prim = 0;
    u32  prim_count = 0;
    u32  parent     = 0xFFFFFFFFu;
    PSY_FORCEINLINE bool is_leaf() const noexcept { return left == 0xFFFFFFFFu; }
};

struct BvhState {
    std::vector<BvhPrim> prims;            // per-entity AABBs (slot-stable)
    std::vector<u32>     free_prims;       // free list into `prims`
    std::vector<u32>     prim_indices;     // leaf-referenced index array
    std::vector<BvhNode> nodes;
    f32  sah_cost_as_built = 0.0f;
    bool dirty             = false;        // topology changed (insert/remove) → rebuild
    bool needs_refit       = false;        // a prim's bounds changed → refit
};

// ─── SAP (sweep-and-prune) ──────────────────────────────────────────────
// Three-axis sorted endpoint lists. Each AABB has two endpoints per axis
// (min, max). Endpoints carry the originating slot index + a "is_max"
// flag. The pair list is the AND of the three per-axis overlap sets.
//
// Wave-B keeps it simple — we re-sort each axis with std::sort and
// re-scan endpoints. A true Bullet-style incremental insertion-sort lives
// on Wave-B+ but the contract (incremental, 3-axis) is the right
// vocabulary for the routing surface.

struct SapEndpoint {
    f32 value     = 0.0f;
    u32 slot      = 0;       // index into `boxes`
    u8  is_max    = 0;       // 0 = min endpoint, 1 = max
    u8  _pad[3]   = {};
};

struct SapBox {
    math::Aabb bounds;
    u32        entity_index = 0;
    bool       alive        = false;
};

struct SapState {
    std::vector<SapBox>       boxes;       // slot table
    std::vector<u32>          free_slots;
    std::vector<SapEndpoint>  ep_x;
    std::vector<SapEndpoint>  ep_y;
    std::vector<SapEndpoint>  ep_z;
    std::vector<SapPair>      pairs;       // most recent overlap set
};

// ─── Hashed grid ────────────────────────────────────────────────────────
// Sparse 3D grid with chained buckets. Cell coordinates are computed by
// dividing world-space position by `cell_size` and hashed into a fixed-
// size table; collisions chain via a linear vector per bucket.
//
// Each grid entry is one "owner" cell per AABB; for radius queries we
// iterate the cells overlapping (center ± radius) and collect entities.
// AABBs that straddle multiple cells are inserted in every overlapping
// cell — fast for the small-extent case (debris, AI agents, audio
// sources) which is exactly what the design says this index serves.

inline constexpr f32 kGridDefaultCellSize = 4.0f;     // metres
inline constexpr u32 kGridBucketCount     = 4096;     // must be a power of two

struct GridEntry {
    u32        entity_index = 0;
    math::Aabb bounds;
};

struct GridSlot {
    GridEntry entry;
    bool      alive = false;
};

struct GridCellRef {
    i32 x, y, z;
    u32 slot;
};

struct GridState {
    f32                              cell_size = kGridDefaultCellSize;
    std::vector<GridSlot>            slots;
    std::vector<u32>                 free_slots;
    // Bucket[i] is a vector of {cell coord, slot} entries. We chain on
    // collision so distinct (x,y,z) cells hashing to the same bucket
    // don't trample each other.
    std::vector<std::vector<GridCellRef>> buckets;
};

// ─── Shared per-backend state accessors ─────────────────────────────────
// These return references to the file-static singletons inside the three
// backend TUs. The dispatcher in Spatial.cpp uses them for the
// non-virtual hot helpers (bvh_refit, sap_step, grid_radius_query).

BvhState&  bvh_state()  noexcept;
SapState&  sap_state()  noexcept;
GridState& grid_state() noexcept;

// Re-export so backends can implement insert/update/remove against the
// non-virtual hot helpers in the cpps without re-listing types here.
struct ISpatialIndex;  // forward — defined in Spatial.h

}  // namespace psynder::scene::detail
