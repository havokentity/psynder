// SPDX-License-Identifier: MIT
// Psynder — entity-AABB BVH backend (lane-06 internal, Wave B).
//
// Build pipeline (top-down SAH binning):
//   1. Per-entity AABBs already live in `state.prims` (slot table).
//   2. Build a flat primitive-index array of currently-live slots, then
//      recursively partition by SAH-binned split.
//
// Refit (every frame, dynamic actors):
//   - Walk nodes in reverse (children built first ⇒ children have lower
//     indices than their parents in our top-down build), recomputing each
//     interior node's bounds as the union of its children.
//   - Measure post-refit SAH cost vs the as-built cost. The DESIGN.md
//     §9.4 trigger fires when ratio > 1.3.
//
// Query (`query_aabb`):
//   - Stack-based depth-first walk; bounce off non-overlapping subtrees.
//
// Hot rules respected: no `virtual` calls inside refit / query inner loop;
// no `shared_ptr`; no per-frame `new`/`delete` (containers reuse capacity).

#include "Spatial.h"
#include "Spatial_Internal.h"

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace psynder::scene::detail {

namespace {

// ─── State singleton ────────────────────────────────────────────────────
PSY_CACHELINE_ALIGN BvhState g_bvh;

// Pack a slot index into a SpatialKey. Reserve 0 as "invalid".
PSY_FORCEINLINE SpatialKey pack_key(u32 slot) noexcept {
    return SpatialKey{slot + 1u};
}
PSY_FORCEINLINE u32 unpack_slot(SpatialKey k) noexcept {
    return k.raw == 0 ? 0xFFFFFFFFu : (k.raw - 1u);
}

// ─── SAH-binned partition ───────────────────────────────────────────────
struct SahBucket {
    math::Aabb bounds = aabb_invalid();
    u32 count = 0;
};

f32 axis_component(math::Vec3 v, u32 axis) noexcept {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

struct SahSplit {
    bool found = false;
    u32 axis = 0;
    u32 mid = 0;  // position in [first, first+count)
    math::Aabb left_bounds = aabb_invalid();
    math::Aabb right_bounds = aabb_invalid();
    f32 cost = 0.0f;
};

SahSplit best_split(std::vector<u32>& prim_idx,
                    const std::vector<BvhPrim>& prims,
                    u32 first,
                    u32 count,
                    const math::Aabb& centroid_bounds,
                    const math::Aabb& node_bounds) {
    SahSplit out;
    if (count <= 1)
        return out;

    const f32 node_sa = aabb_surface_area(node_bounds);
    if (node_sa <= 0.0f)
        return out;

    const f32 leaf_cost = static_cast<f32>(count);
    f32 best_cost = std::numeric_limits<f32>::infinity();
    u32 best_axis = 0;
    u32 best_split_bucket = 0;

    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 cmin = axis_component(centroid_bounds.min, axis);
        const f32 cmax = axis_component(centroid_bounds.max, axis);
        const f32 extent = cmax - cmin;
        if (extent <= 0.0f)
            continue;
        const f32 inv_extent = 1.0f / extent;

        SahBucket buckets[kBvhSahBuckets];

        for (u32 i = 0; i < count; ++i) {
            const u32 pi = prim_idx[first + i];
            const f32 c = axis_component(aabb_centroid(prims[pi].bounds), axis);
            u32 b = static_cast<u32>((c - cmin) * inv_extent * kBvhSahBuckets);
            if (b >= kBvhSahBuckets)
                b = kBvhSahBuckets - 1;
            aabb_expand(buckets[b].bounds, prims[pi].bounds);
            ++buckets[b].count;
        }

        // Forward + reverse prefix scans.
        math::Aabb left_bounds[kBvhSahBuckets];
        u32 left_counts[kBvhSahBuckets];
        math::Aabb right_bounds[kBvhSahBuckets];
        u32 right_counts[kBvhSahBuckets];

        math::Aabb running = aabb_invalid();
        u32 running_count = 0;
        for (u32 b = 0; b < kBvhSahBuckets; ++b) {
            aabb_expand(running, buckets[b].bounds);
            running_count += buckets[b].count;
            left_bounds[b] = running;
            left_counts[b] = running_count;
        }
        running = aabb_invalid();
        running_count = 0;
        for (i32 b = kBvhSahBuckets - 1; b >= 0; --b) {
            aabb_expand(running, buckets[b].bounds);
            running_count += buckets[b].count;
            right_bounds[b] = running;
            right_counts[b] = running_count;
        }

        for (u32 b = 0; b + 1 < kBvhSahBuckets; ++b) {
            if (left_counts[b] == 0 || right_counts[b + 1] == 0)
                continue;
            const f32 sa_l = aabb_surface_area(left_bounds[b]);
            const f32 sa_r = aabb_surface_area(right_bounds[b + 1]);
            const f32 cost = 1.0f + (sa_l * static_cast<f32>(left_counts[b]) +
                                     sa_r * static_cast<f32>(right_counts[b + 1])) /
                                        node_sa;
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split_bucket = b;
                out.left_bounds = left_bounds[b];
                out.right_bounds = right_bounds[b + 1];
            }
        }
    }

    if (best_cost >= leaf_cost)
        return out;

    // Partition `prim_idx[first .. first+count)` by axis component vs the
    // bucket boundary. We re-bucketize because the bucket index has to be
    // stable with the partition predicate.
    const f32 cmin = axis_component(centroid_bounds.min, best_axis);
    const f32 cmax = axis_component(centroid_bounds.max, best_axis);
    const f32 extent = cmax - cmin;
    if (extent <= 0.0f)
        return out;
    const f32 inv_extent = 1.0f / extent;

    auto bucket_of = [&](u32 pi) noexcept {
        const f32 c = axis_component(aabb_centroid(prims[pi].bounds), best_axis);
        u32 b = static_cast<u32>((c - cmin) * inv_extent * kBvhSahBuckets);
        if (b >= kBvhSahBuckets)
            b = kBvhSahBuckets - 1;
        return b;
    };

    auto mid_iter =
        std::partition(prim_idx.begin() + first,
                       prim_idx.begin() + first + count,
                       [&](u32 pi) noexcept { return bucket_of(pi) <= best_split_bucket; });

    const u32 mid = static_cast<u32>(mid_iter - (prim_idx.begin() + first)) + first;

    if (mid == first || mid == first + count)
        return out;

    out.found = true;
    out.axis = best_axis;
    out.mid = mid;
    out.cost = best_cost;
    return out;
}

// Recursive top-down build. Returns the index of the produced node.
u32 build_recursive(BvhState& s,
                    u32 first,
                    u32 count,
                    const math::Aabb& node_bounds,
                    const math::Aabb& centroid_bounds,
                    u32 parent) {
    const u32 idx = static_cast<u32>(s.nodes.size());
    s.nodes.emplace_back();
    s.nodes[idx].bounds = node_bounds;
    s.nodes[idx].parent = parent;

    if (count <= kBvhMaxLeafPrims) {
        s.nodes[idx].left = 0xFFFFFFFFu;
        s.nodes[idx].right = 0xFFFFFFFFu;
        s.nodes[idx].first_prim = first;
        s.nodes[idx].prim_count = count;
        return idx;
    }

    const auto split = best_split(s.prim_indices, s.prims, first, count, centroid_bounds, node_bounds);
    if (!split.found) {
        s.nodes[idx].left = 0xFFFFFFFFu;
        s.nodes[idx].right = 0xFFFFFFFFu;
        s.nodes[idx].first_prim = first;
        s.nodes[idx].prim_count = count;
        return idx;
    }

    // Compute child centroid bounds in a single pass.
    math::Aabb left_centroids = aabb_invalid();
    math::Aabb right_centroids = aabb_invalid();
    for (u32 i = first; i < split.mid; ++i)
        aabb_expand(left_centroids, aabb_centroid(s.prims[s.prim_indices[i]].bounds));
    for (u32 i = split.mid; i < first + count; ++i)
        aabb_expand(right_centroids, aabb_centroid(s.prims[s.prim_indices[i]].bounds));

    const u32 left_idx =
        build_recursive(s, first, split.mid - first, split.left_bounds, left_centroids, idx);
    const u32 right_idx =
        build_recursive(s, split.mid, first + count - split.mid, split.right_bounds, right_centroids, idx);

    s.nodes[idx].left = left_idx;
    s.nodes[idx].right = right_idx;
    return idx;
}

// SAH cost of the tree as it currently stands. Uses absolute surface
// areas (not normalized to root) so a leaf wandering far away — which
// inflates every interior node on its path — shows up as a clear cost
// increase that the §9.4 1.3× rebuild trigger can detect. (Normalizing
// to root SA cancels exactly the signal we want.)
f32 measure_sah_cost(const BvhState& s) noexcept {
    if (s.nodes.empty())
        return 0.0f;
    f32 cost = 0.0f;
    for (const auto& n : s.nodes) {
        const f32 sa = aabb_surface_area(n.bounds);
        if (sa <= 0.0f)
            continue;
        if (n.is_leaf()) {
            cost += sa * static_cast<f32>(n.prim_count);
        } else {
            cost += sa;  // traversal cost, unit weight
        }
    }
    return cost;
}

void rebuild(BvhState& s) noexcept {
    s.nodes.clear();
    s.prim_indices.clear();

    // Collect live prim indices.
    s.prim_indices.reserve(s.prims.size());
    math::Aabb node_bounds = aabb_invalid();
    math::Aabb centroid_bounds = aabb_invalid();
    for (u32 i = 0; i < s.prims.size(); ++i) {
        if (s.prims[i].entity_index == 0xFFFFFFFFu)
            continue;
        s.prim_indices.push_back(i);
        aabb_expand(node_bounds, s.prims[i].bounds);
        aabb_expand(centroid_bounds, aabb_centroid(s.prims[i].bounds));
    }

    if (s.prim_indices.empty()) {
        s.sah_cost_as_built = 0.0f;
        s.dirty = false;
        s.needs_refit = false;
        return;
    }

    build_recursive(s, 0, static_cast<u32>(s.prim_indices.size()), node_bounds, centroid_bounds, 0xFFFFFFFFu);
    s.sah_cost_as_built = measure_sah_cost(s);
    s.dirty = false;
    s.needs_refit = false;
}

// ─── ISpatialIndex backend ──────────────────────────────────────────────
class BvhBackend final : public ISpatialIndex {
   public:
    SpatialKey insert(u32 entity_index, const math::Aabb& bounds) override {
        u32 slot;
        if (!g_bvh.free_prims.empty()) {
            slot = g_bvh.free_prims.back();
            g_bvh.free_prims.pop_back();
            g_bvh.prims[slot] = BvhPrim{bounds, entity_index};
        } else {
            slot = static_cast<u32>(g_bvh.prims.size());
            g_bvh.prims.push_back(BvhPrim{bounds, entity_index});
        }
        g_bvh.dirty = true;
        return pack_key(slot);
    }

    void update(SpatialKey key, const math::Aabb& bounds) override {
        const u32 slot = unpack_slot(key);
        if (slot >= g_bvh.prims.size())
            return;
        if (g_bvh.prims[slot].entity_index == 0xFFFFFFFFu)
            return;
        // Deliberately does NOT set `dirty` (which would force a full
        // rebuild). Instead we mark `needs_refit` — the next refit walks
        // the tree bottom-up using the fresh prim bounds, and the
        // refit-vs-as-built SAH ratio drives the §9.4 rebuild trigger.
        g_bvh.prims[slot].bounds = bounds;
        g_bvh.needs_refit = true;
    }

    void remove(SpatialKey key) override {
        const u32 slot = unpack_slot(key);
        if (slot >= g_bvh.prims.size())
            return;
        if (g_bvh.prims[slot].entity_index == 0xFFFFFFFFu)
            return;
        g_bvh.prims[slot].entity_index = 0xFFFFFFFFu;
        g_bvh.free_prims.push_back(slot);
        g_bvh.dirty = true;
    }

    void query_aabb(const math::Aabb& q, std::span<u32> out_entities) const override {
        if (g_bvh.dirty || g_bvh.nodes.empty()) {
            // Lazy build so external callers see a fresh tree after edits.
            // (The world's per-frame refit loop drives this normally.)
            rebuild(const_cast<BvhState&>(g_bvh));
        } else if (g_bvh.needs_refit) {
            // Prim bounds changed but topology is intact → bottom-up
            // refit. Callers that drive the world loop normally call
            // `bvh_refit()` once per frame; this branch protects ad-hoc
            // callers (editor, tests).
            (void)bvh_refit();
        }
        if (g_bvh.nodes.empty() || out_entities.empty())
            return;

        // Stack-based DFS. We size the stack to log2 of node count with a
        // generous upper bound — for the entity counts at play (<<1M)
        // this fits in a single cache line.
        u32 stack[64];
        i32 top = 0;
        stack[top++] = 0;

        usize written = 0;
        while (top > 0 && written < out_entities.size()) {
            const u32 ni = stack[--top];
            const BvhNode& n = g_bvh.nodes[ni];
            if (!aabb_overlap(n.bounds, q))
                continue;
            if (n.is_leaf()) {
                for (u32 i = 0; i < n.prim_count && written < out_entities.size(); ++i) {
                    const u32 pi = g_bvh.prim_indices[n.first_prim + i];
                    if (g_bvh.prims[pi].entity_index == 0xFFFFFFFFu)
                        continue;
                    if (aabb_overlap(g_bvh.prims[pi].bounds, q)) {
                        out_entities[written++] = g_bvh.prims[pi].entity_index;
                    }
                }
            } else {
                if (top + 2 < static_cast<i32>(sizeof(stack) / sizeof(stack[0]))) {
                    stack[top++] = n.right;
                    stack[top++] = n.left;
                }
            }
        }
    }
};

PSY_CACHELINE_ALIGN BvhBackend g_bvh_backend{};

}  // namespace

// ─── Exports ────────────────────────────────────────────────────────────
BvhState& bvh_state() noexcept {
    return g_bvh;
}

ISpatialIndex* bvh_backend() noexcept {
    return &g_bvh_backend;
}

BvhRefitStats bvh_refit() noexcept {
    BvhRefitStats stats;
    if (g_bvh.nodes.empty() || g_bvh.dirty) {
        rebuild(g_bvh);
        stats.sah_cost_as_built = g_bvh.sah_cost_as_built;
        stats.sah_cost = g_bvh.sah_cost_as_built;
        stats.should_async_rebuild = false;
        return stats;
    }

    // Bottom-up refit: walk nodes in reverse (children before parents
    // because the top-down build emits the parent first then the children
    // — but the parent's two recursive calls return after writing all
    // descendant nodes, so the reverse order visits leaves first).
    for (i32 i = static_cast<i32>(g_bvh.nodes.size()) - 1; i >= 0; --i) {
        BvhNode& n = g_bvh.nodes[static_cast<usize>(i)];
        if (n.is_leaf()) {
            math::Aabb b = aabb_invalid();
            for (u32 k = 0; k < n.prim_count; ++k) {
                const u32 pi = g_bvh.prim_indices[n.first_prim + k];
                if (g_bvh.prims[pi].entity_index == 0xFFFFFFFFu)
                    continue;
                aabb_expand(b, g_bvh.prims[pi].bounds);
            }
            n.bounds = b;
        } else {
            math::Aabb b = g_bvh.nodes[n.left].bounds;
            aabb_expand(b, g_bvh.nodes[n.right].bounds);
            n.bounds = b;
        }
    }

    stats.sah_cost = measure_sah_cost(g_bvh);
    stats.sah_cost_as_built = g_bvh.sah_cost_as_built;
    stats.should_async_rebuild = g_bvh.sah_cost_as_built > 0.0f &&
                                 stats.sah_cost > g_bvh.sah_cost_as_built * kBvhRebuildRatio;
    g_bvh.needs_refit = false;
    return stats;
}

}  // namespace psynder::scene::detail
