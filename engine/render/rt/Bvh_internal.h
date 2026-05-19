// SPDX-License-Identifier: MIT
// Psynder — internal BVH types. NOT part of the public Bvh.h contract.
// Lane 08 owns. Layout is private; sibling lanes never include this header.
//
// Build pipeline:
//   1. SAH top-down binary build over per-triangle centroids, producing
//      a flat array of `BinaryNode`.
//   2. "Collapse" three levels of the binary tree into one 8-wide node.
//      Each `Bvh8Node` carries up to 8 children (inner nodes or leaves),
//      laid out as SoA AABB arrays so an 8-wide AVX2 ray packet can be
//      tested against all 8 children in one batch.
//
// References (no Embree code lifted; ADR-007 binding):
//   Wald 2007 "On fast Construction of SAH-based BVHs",
//   Hapala & Havran 2011 "Review: Kd-tree Traversal Algorithms",
//   pbrt-v4 §4.3 (binary BVH SAH), §7.3 (wide-BVH packet traversal).
//
// SAH cost (DESIGN.md §9.4):
//   c_node = 1, c_prim = 1; node SA / parent SA × #children + leaves.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <limits>

namespace psynder::render::rt::detail {

// ─── AABB helpers ────────────────────────────────────────────────────────

struct Aabb {
    math::Vec3 min{ +std::numeric_limits<f32>::infinity(),
                    +std::numeric_limits<f32>::infinity(),
                    +std::numeric_limits<f32>::infinity() };
    math::Vec3 max{ -std::numeric_limits<f32>::infinity(),
                    -std::numeric_limits<f32>::infinity(),
                    -std::numeric_limits<f32>::infinity() };

    void reset() noexcept {
        min = {  std::numeric_limits<f32>::infinity(),
                 std::numeric_limits<f32>::infinity(),
                 std::numeric_limits<f32>::infinity() };
        max = { -std::numeric_limits<f32>::infinity(),
                -std::numeric_limits<f32>::infinity(),
                -std::numeric_limits<f32>::infinity() };
    }

    void expand(math::Vec3 p) noexcept {
        if (p.x < min.x) min.x = p.x;
        if (p.y < min.y) min.y = p.y;
        if (p.z < min.z) min.z = p.z;
        if (p.x > max.x) max.x = p.x;
        if (p.y > max.y) max.y = p.y;
        if (p.z > max.z) max.z = p.z;
    }

    void expand(const Aabb& o) noexcept {
        if (o.min.x < min.x) min.x = o.min.x;
        if (o.min.y < min.y) min.y = o.min.y;
        if (o.min.z < min.z) min.z = o.min.z;
        if (o.max.x > max.x) max.x = o.max.x;
        if (o.max.y > max.y) max.y = o.max.y;
        if (o.max.z > max.z) max.z = o.max.z;
    }

    bool valid() const noexcept {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    f32 surface_area() const noexcept {
        if (!valid()) return 0.0f;
        const f32 dx = max.x - min.x;
        const f32 dy = max.y - min.y;
        const f32 dz = max.z - min.z;
        return 2.0f * (dx*dy + dy*dz + dz*dx);
    }

    math::Vec3 centroid() const noexcept {
        return { 0.5f*(min.x + max.x),
                 0.5f*(min.y + max.y),
                 0.5f*(min.z + max.z) };
    }
};

// ─── Binary intermediate node (build-time only) ──────────────────────────
//
// During SAH construction we build a binary BVH. `BinaryNode` is the node
// type. After the binary tree is built we collapse three consecutive
// levels into one `Bvh8Node` (so up to 2^3 = 8 children per wide node).
//
// `left == 0xFFFFFFFFu` marks a leaf.

struct BinaryNode {
    Aabb bounds;
    u32  left      = 0xFFFFFFFFu;   // index into nodes[] or 0xFFFFFFFFu for leaf
    u32  right     = 0xFFFFFFFFu;
    u32  first_prim = 0;            // offset into the primitive index array (leaf only)
    u32  prim_count = 0;            // > 0 iff leaf
    constexpr bool is_leaf() const noexcept { return left == 0xFFFFFFFFu; }
};

// ─── Wide (8-way) runtime node ───────────────────────────────────────────
//
// SoA layout: eight AABBs stored as eight floats per component. This is
// what the AVX2 packet kernel loads when testing 8 rays vs 1 node's 8
// children, and also what the scalar ray-vs-8-children intersection uses.
//
// `child_kind[i]` encodes whether child i is an inner Bvh8Node (0), a
// leaf (1), or empty (2 — node has fewer than 8 children).
// For inner: child_index[i] = index into nodes_.
// For leaf:  child_index[i] = offset into prim_indices_;
//            child_count[i] = prim count.

struct alignas(32) Bvh8Node {
    f32 min_x[8];
    f32 min_y[8];
    f32 min_z[8];
    f32 max_x[8];
    f32 max_y[8];
    f32 max_z[8];

    u32 child_index[8];    // inner: node idx; leaf: prim offset
    u32 child_count[8];    // leaf: prim count; inner: 0
    u8  child_kind[8];     // 0=inner, 1=leaf, 2=empty
    u8  child_mask;        // bit i set ⇔ child i is non-empty
    u8  _pad[7];           // pad to keep alignment
};

static_assert(sizeof(Bvh8Node) % 32 == 0, "Bvh8Node must keep 32-byte alignment");

// ─── SAH config ──────────────────────────────────────────────────────────

inline constexpr u32 kSahBuckets       = 12;
inline constexpr u32 kMaxLeafPrims     = 4;
inline constexpr f32 kTraversalCost    = 1.0f;
inline constexpr f32 kIntersectionCost = 1.0f;
inline constexpr f32 kRefitRebuildRatio = 1.3f;  // DESIGN.md §9.4

}  // namespace psynder::render::rt::detail
