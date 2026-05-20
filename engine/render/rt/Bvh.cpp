// SPDX-License-Identifier: MIT
// Psynder — Bvh8 + Tlas implementation. Lane 08 owns.
//
// Build pipeline (top-down SAH, then collapse-to-8):
//   1. Per-triangle centroids → primitive index array.
//   2. Recursive SAH-binned partition over the primitive index range,
//      producing a flat array of binary nodes.
//   3. Collapse three binary levels into one 8-wide Bvh8Node.
//
// Refit:
//   - Walk the binary tree bottom-up, rebuild AABBs from leaf primitives.
//   - Re-collapse leaves' new bounds into the existing Bvh8 wide nodes.
//   - Measure refit SAH cost vs the as-built; expose
//     `should_async_rebuild()` which trips at 1.3× per DESIGN.md §9.4.
//
// No Embree. No third-party. ADR-007 binding.

#include "Bvh.h"
#include "Bvh_internal.h"
#include "Bvh_impl.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace psynder::render::rt {

using detail::Aabb;
using detail::BinaryNode;
using detail::Bvh8Node;
using detail::kIntersectionCost;
using detail::kMaxLeafPrims;
using detail::kSahBuckets;
using detail::kTraversalCost;

namespace {

// Compute primitive AABB + centroid for the source triangles.
struct PrimRef {
    Aabb bounds;
    math::Vec3 centroid;
};

void build_prim_refs(const Triangle* tris, u32 count, std::vector<PrimRef>& refs) {
    refs.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const Triangle& t = tris[i];
        Aabb b;
        b.reset();
        b.expand(t.v0);
        b.expand(t.v1);
        b.expand(t.v2);
        refs[i].bounds = b;
        refs[i].centroid = b.centroid();
    }
}

f32 axis_component(math::Vec3 v, u32 axis) noexcept {
    return (axis == 0) ? v.x : (axis == 1) ? v.y : v.z;
}

// SAH binned split — Wald 2007 style. Returns true if a split improves
// over making this a leaf. Writes the split prim count `mid` and the
// computed left/right bounds.
struct SplitResult {
    bool found = false;
    u32 axis = 0;
    u32 mid = 0;
    Aabb left_bounds;
    Aabb right_bounds;
    f32 cost = 0.0f;
};

SplitResult sah_partition(std::vector<u32>& prim_idx,
                          const std::vector<PrimRef>& refs,
                          u32 first,
                          u32 count,
                          const Aabb& centroid_bounds,
                          const Aabb& node_bounds) {
    SplitResult out;
    if (count <= 1)
        return out;

    const f32 node_sa = node_bounds.surface_area();
    if (node_sa <= 0.0f)
        return out;

    const f32 leaf_cost = kIntersectionCost * static_cast<f32>(count);

    // Per-bucket structure for SAH binning.
    struct Bucket {
        Aabb bounds;
        u32 count = 0;
    };

    f32 best_cost = std::numeric_limits<f32>::infinity();
    u32 best_axis = 0;
    u32 best_split_bucket = 0;

    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 ext_min = axis_component(centroid_bounds.min, axis);
        const f32 ext_max = axis_component(centroid_bounds.max, axis);
        const f32 ext = ext_max - ext_min;
        if (ext <= 0.0f)
            continue;

        Bucket buckets[kSahBuckets]{};
        for (u32 i = 0; i < count; ++i) {
            const u32 pid = prim_idx[first + i];
            const f32 c = axis_component(refs[pid].centroid, axis);
            f32 rel = (c - ext_min) / ext;
            // Clamp to [0, kSahBuckets - 1]
            i32 bi = static_cast<i32>(rel * static_cast<f32>(kSahBuckets));
            if (bi < 0)
                bi = 0;
            if (bi >= static_cast<i32>(kSahBuckets))
                bi = static_cast<i32>(kSahBuckets) - 1;
            buckets[bi].bounds.expand(refs[pid].bounds);
            buckets[bi].count += 1;
        }

        // Prefix / suffix bounds + counts to evaluate the kSahBuckets-1 splits.
        Aabb left_bounds[kSahBuckets];
        u32 left_count[kSahBuckets]{};
        Aabb cur;
        cur.reset();
        u32 cnt = 0;
        for (u32 b = 0; b < kSahBuckets; ++b) {
            cur.expand(buckets[b].bounds);
            cnt += buckets[b].count;
            left_bounds[b] = cur;
            left_count[b] = cnt;
        }
        Aabb right_bounds[kSahBuckets];
        u32 right_count[kSahBuckets]{};
        cur.reset();
        cnt = 0;
        for (i32 b = static_cast<i32>(kSahBuckets) - 1; b >= 0; --b) {
            cur.expand(buckets[static_cast<u32>(b)].bounds);
            cnt += buckets[static_cast<u32>(b)].count;
            right_bounds[b] = cur;
            right_count[b] = cnt;
        }

        // Evaluate split candidates between bucket b and b+1.
        for (u32 b = 0; b + 1 < kSahBuckets; ++b) {
            const u32 lc = left_count[b];
            const u32 rc = right_count[b + 1];
            if (lc == 0 || rc == 0)
                continue;
            const f32 lsa = left_bounds[b].surface_area();
            const f32 rsa = right_bounds[b + 1].surface_area();
            const f32 cost = kTraversalCost +
                             (lsa / node_sa) * kIntersectionCost * static_cast<f32>(lc) +
                             (rsa / node_sa) * kIntersectionCost * static_cast<f32>(rc);
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split_bucket = b;
            }
        }
    }

    if (!std::isfinite(best_cost))
        return out;
    if (best_cost >= leaf_cost && count <= kMaxLeafPrims)
        return out;

    // Partition prim_idx in place by the winning axis + bucket boundary.
    const f32 ext_min = axis_component(centroid_bounds.min, best_axis);
    const f32 ext_max = axis_component(centroid_bounds.max, best_axis);
    const f32 ext = ext_max - ext_min;
    if (ext <= 0.0f)
        return out;

    auto bucket_of = [&](u32 pid) -> u32 {
        const f32 c = axis_component(refs[pid].centroid, best_axis);
        f32 rel = (c - ext_min) / ext;
        i32 bi = static_cast<i32>(rel * static_cast<f32>(kSahBuckets));
        if (bi < 0)
            bi = 0;
        if (bi >= static_cast<i32>(kSahBuckets))
            bi = static_cast<i32>(kSahBuckets) - 1;
        return static_cast<u32>(bi);
    };

    u32 lo = first;
    u32 hi = first + count - 1;
    while (lo <= hi) {
        if (bucket_of(prim_idx[lo]) <= best_split_bucket) {
            ++lo;
        } else {
            std::swap(prim_idx[lo], prim_idx[hi]);
            if (hi == 0)
                break;
            --hi;
        }
    }
    const u32 mid = lo;
    if (mid == first || mid == first + count) {
        // Degenerate fallback: median split on best_axis.
        std::nth_element(prim_idx.begin() + first,
                         prim_idx.begin() + first + count / 2,
                         prim_idx.begin() + first + count,
                         [&](u32 a, u32 b) {
                             return axis_component(refs[a].centroid, best_axis) <
                                    axis_component(refs[b].centroid, best_axis);
                         });
        const u32 mid2 = first + count / 2;
        Aabb lb;
        lb.reset();
        Aabb rb;
        rb.reset();
        for (u32 i = first; i < mid2; ++i)
            lb.expand(refs[prim_idx[i]].bounds);
        for (u32 i = mid2; i < first + count; ++i)
            rb.expand(refs[prim_idx[i]].bounds);
        out.found = true;
        out.axis = best_axis;
        out.mid = mid2;
        out.left_bounds = lb;
        out.right_bounds = rb;
        out.cost = best_cost;
        return out;
    }

    Aabb lb;
    lb.reset();
    Aabb rb;
    rb.reset();
    for (u32 i = first; i < mid; ++i)
        lb.expand(refs[prim_idx[i]].bounds);
    for (u32 i = mid; i < first + count; ++i)
        rb.expand(refs[prim_idx[i]].bounds);
    out.found = true;
    out.axis = best_axis;
    out.mid = mid;
    out.left_bounds = lb;
    out.right_bounds = rb;
    out.cost = best_cost;
    return out;
}

// Recursive top-down SAH build over [first, first+count).
u32 build_binary(std::vector<u32>& prim_idx,
                 const std::vector<PrimRef>& refs,
                 std::vector<BinaryNode>& nodes,
                 u32 first,
                 u32 count,
                 const Aabb& node_bounds) {
    const u32 node_id = static_cast<u32>(nodes.size());
    nodes.emplace_back();
    nodes[node_id].bounds = node_bounds;

    if (count <= 1) {
        nodes[node_id].first_prim = first;
        nodes[node_id].prim_count = count;
        return node_id;
    }

    Aabb cb;
    cb.reset();
    for (u32 i = 0; i < count; ++i) {
        cb.expand(refs[prim_idx[first + i]].centroid);
    }

    SplitResult sr = sah_partition(prim_idx, refs, first, count, cb, node_bounds);
    if (!sr.found || count <= kMaxLeafPrims) {
        const f32 leaf_cost = kIntersectionCost * static_cast<f32>(count);
        if (!sr.found || sr.cost >= leaf_cost) {
            nodes[node_id].first_prim = first;
            nodes[node_id].prim_count = count;
            return node_id;
        }
    }

    const u32 left_count = sr.mid - first;
    const u32 right_count = count - left_count;
    const u32 left_id = build_binary(prim_idx, refs, nodes, first, left_count, sr.left_bounds);
    const u32 right_id = build_binary(prim_idx, refs, nodes, sr.mid, right_count, sr.right_bounds);
    nodes[node_id].left = left_id;
    nodes[node_id].right = right_id;
    return node_id;
}

// ─── Collapse 3 binary levels → 1 Bvh8 wide node ─────────────────────────
//
// gather_children: starting from `bin_id`, walk down up to `depth` levels,
// pushing a child entry for every leaf encountered before depth and every
// node reached at depth. Children are stored in DFS order; up to 8 fit.

struct ChildEntry {
    u32 bin_id;  // index into binary_nodes; or, for leaves, the binary leaf node
    bool is_leaf;
};

void gather_children(const std::vector<BinaryNode>& bin,
                     u32 bin_id,
                     u32 depth,
                     std::vector<ChildEntry>& out) {
    const BinaryNode& n = bin[bin_id];
    if (n.is_leaf() || depth == 0) {
        out.push_back({bin_id, n.is_leaf()});
        return;
    }
    gather_children(bin, n.left, depth - 1, out);
    gather_children(bin, n.right, depth - 1, out);
}

// Build the wide nodes by walking the binary tree, collapsing 3 levels at a
// time. Returns the new index into wide_nodes for `bin_id`.
u32 build_wide(const std::vector<BinaryNode>& bin, u32 bin_id, std::vector<Bvh8Node>& wide_nodes) {
    const u32 wide_id = static_cast<u32>(wide_nodes.size());
    wide_nodes.emplace_back();
    Bvh8Node& w = wide_nodes[wide_id];
    std::memset(&w, 0, sizeof(Bvh8Node));
    for (u32 i = 0; i < 8; ++i) {
        w.child_kind[i] = 2;  // empty
        // Fill empty AABB so SIMD slab tests can never report a hit.
        w.min_x[i] = +std::numeric_limits<f32>::infinity();
        w.min_y[i] = +std::numeric_limits<f32>::infinity();
        w.min_z[i] = +std::numeric_limits<f32>::infinity();
        w.max_x[i] = -std::numeric_limits<f32>::infinity();
        w.max_y[i] = -std::numeric_limits<f32>::infinity();
        w.max_z[i] = -std::numeric_limits<f32>::infinity();
    }

    // If this binary node is itself a leaf, emit a single-child wide leaf.
    if (bin[bin_id].is_leaf()) {
        const BinaryNode& n = bin[bin_id];
        w.min_x[0] = n.bounds.min.x;
        w.min_y[0] = n.bounds.min.y;
        w.min_z[0] = n.bounds.min.z;
        w.max_x[0] = n.bounds.max.x;
        w.max_y[0] = n.bounds.max.y;
        w.max_z[0] = n.bounds.max.z;
        w.child_index[0] = n.first_prim;
        w.child_count[0] = n.prim_count;
        w.child_kind[0] = 1;
        w.child_mask = 0x01;
        return wide_id;
    }

    std::vector<ChildEntry> kids;
    kids.reserve(8);
    gather_children(bin, bin_id, 3, kids);
    if (kids.size() > 8)
        kids.resize(8);

    u8 mask = 0;
    for (u32 i = 0; i < static_cast<u32>(kids.size()); ++i) {
        const BinaryNode& cn = bin[kids[i].bin_id];
        w.min_x[i] = cn.bounds.min.x;
        w.min_y[i] = cn.bounds.min.y;
        w.min_z[i] = cn.bounds.min.z;
        w.max_x[i] = cn.bounds.max.x;
        w.max_y[i] = cn.bounds.max.y;
        w.max_z[i] = cn.bounds.max.z;
        if (kids[i].is_leaf) {
            w.child_index[i] = cn.first_prim;
            w.child_count[i] = cn.prim_count;
            w.child_kind[i] = 1;
        } else {
            w.child_kind[i] = 0;
            // Placeholder index; filled after recursion below.
        }
        mask = static_cast<u8>(mask | (1u << i));
    }
    wide_nodes[wide_id].child_mask = mask;

    // Recurse on inner children. We must build *first*, then patch indices,
    // because vector reallocation during recursion would invalidate refs.
    u32 child_wide_ids[8] = {0};
    for (u32 i = 0; i < static_cast<u32>(kids.size()); ++i) {
        if (!kids[i].is_leaf) {
            child_wide_ids[i] = build_wide(bin, kids[i].bin_id, wide_nodes);
        }
    }
    for (u32 i = 0; i < static_cast<u32>(kids.size()); ++i) {
        if (!kids[i].is_leaf) {
            wide_nodes[wide_id].child_index[i] = child_wide_ids[i];
        }
    }
    return wide_id;
}

// Sum-of-leaf-SA refit walker. Updates a binary tree's bounds bottom-up.
void refit_binary(std::vector<BinaryNode>& bin,
                  const Triangle* tris,
                  u32 tri_count,
                  const std::vector<u32>& prim_idx,
                  u32 bin_id) {
    BinaryNode& n = bin[bin_id];
    if (n.is_leaf()) {
        Aabb b;
        b.reset();
        for (u32 i = 0; i < n.prim_count; ++i) {
            const u32 pid = prim_idx[n.first_prim + i];
            if (pid >= tri_count)
                continue;
            const Triangle& t = tris[pid];
            b.expand(t.v0);
            b.expand(t.v1);
            b.expand(t.v2);
        }
        n.bounds = b;
        return;
    }
    refit_binary(bin, tris, tri_count, prim_idx, n.left);
    refit_binary(bin, tris, tri_count, prim_idx, n.right);
    Aabb b;
    b.reset();
    b.expand(bin[n.left].bounds);
    b.expand(bin[n.right].bounds);
    n.bounds = b;
}

// Compute the SAH cost of a binary tree (used by the 1.3× rebuild gate).
f32 tree_sah_cost(const std::vector<BinaryNode>& bin) {
    if (bin.empty())
        return 0.0f;
    const f32 root_sa = bin[0].bounds.surface_area();
    if (root_sa <= 0.0f)
        return 0.0f;
    f64 total = 0.0;
    for (const BinaryNode& n : bin) {
        const f32 sa = n.bounds.surface_area();
        if (n.is_leaf()) {
            total += static_cast<f64>(sa / root_sa) * static_cast<f64>(kIntersectionCost) *
                     static_cast<f64>(n.prim_count);
        } else {
            total += static_cast<f64>(sa / root_sa) * static_cast<f64>(kTraversalCost);
        }
    }
    return static_cast<f32>(total);
}

void rebuild_wide_from_binary(const std::vector<BinaryNode>& bin, std::vector<Bvh8Node>& wide_nodes) {
    wide_nodes.clear();
    if (bin.empty())
        return;
    build_wide(bin, 0, wide_nodes);
}

}  // anonymous namespace

// ───────────────────────────────────────────────────────────────────────
// Public Bvh8 surface
// ───────────────────────────────────────────────────────────────────────

// Bvh8 PImpl-style state lives in detail::Bvh8State (declared in Bvh_impl.h
// so Intersect.cpp / Tlas can reach it without a public-header change).

namespace detail {

// ─── State storage (out-of-band, since the public types are empty) ──────
//
// `Bvh8` and `Tlas` are declared in the FROZEN public Bvh.h with zero
// member fields. We can't add storage there without breaking the contract.
// Instead we keep state in a singleton map keyed by `this`. The map mutex
// covers create/erase; the state structs themselves are not synchronized
// (callers must serialize build/refit against intersect, which DESIGN.md
// §9.4 already requires — refit runs in its own job).

namespace {

template <typename T>
struct StateRegistry {
    std::unordered_map<const void*, T*> map;
    std::mutex mu;

    ~StateRegistry() {
        for (auto& kv : map)
            delete kv.second;
        map.clear();
    }

    T& get_or_create(const void* key) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = map.find(key);
        if (it != map.end())
            return *it->second;
        auto* s = new T();
        map.emplace(key, s);
        return *s;
    }

    T* find(const void* key) const {
        // Safe const_cast: the registry is mutated under mu; const lookup
        // takes the lock too.
        auto& self = const_cast<StateRegistry<T>&>(*this);
        std::lock_guard<std::mutex> lk(self.mu);
        auto it = self.map.find(key);
        return (it == self.map.end()) ? nullptr : it->second;
    }
};

StateRegistry<Bvh8State>& bvh_registry() {
    static StateRegistry<Bvh8State> r;
    return r;
}
StateRegistry<TlasState>& tlas_registry() {
    static StateRegistry<TlasState> r;
    return r;
}

}  // anonymous namespace

Bvh8State& state_of(Bvh8& b) noexcept {
    return bvh_registry().get_or_create(&b);
}
const Bvh8State& state_of(const Bvh8& b) noexcept {
    Bvh8State* s = bvh_registry().find(&b);
    if (s)
        return *s;
    // First-touch on a const ref creates the slot; the cost is one map
    // insert. This is rare (intersect-before-build is a user bug).
    return bvh_registry().get_or_create(&b);
}
TlasState& state_of(Tlas& t) noexcept {
    return tlas_registry().get_or_create(&t);
}
const TlasState& state_of(const Tlas& t) noexcept {
    TlasState* s = tlas_registry().find(&t);
    if (s)
        return *s;
    return tlas_registry().get_or_create(&t);
}

// ─── Affine helpers (translation + rotation + non-uniform scale) ────────

Aabb transform_aabb(const Aabb& b, const math::Mat4& m) noexcept {
    if (!b.valid())
        return b;
    Aabb out;
    out.reset();
    for (u32 i = 0; i < 8; ++i) {
        const f32 x = (i & 1) ? b.max.x : b.min.x;
        const f32 y = (i & 2) ? b.max.y : b.min.y;
        const f32 z = (i & 4) ? b.max.z : b.min.z;
        math::Vec4 v{x, y, z, 1.0f};
        math::Vec4 r = math::mul(m, v);
        out.expand(math::Vec3{r.x, r.y, r.z});
    }
    return out;
}

math::Mat4 affine_inverse(const math::Mat4& m) noexcept {
    // Treat m as an affine transform: upper-left 3×3 rotation+scale, last
    // column is translation. Compute inv(3×3) by cofactor / det then apply
    // to the translation. This is enough for instance object-space rays;
    // we don't need a fully general 4×4 inverse for TLAS.
    const f32 a00 = m.m[0], a01 = m.m[4], a02 = m.m[8];
    const f32 a10 = m.m[1], a11 = m.m[5], a12 = m.m[9];
    const f32 a20 = m.m[2], a21 = m.m[6], a22 = m.m[10];
    const f32 tx = m.m[12], ty = m.m[13], tz = m.m[14];

    const f32 c00 = (a11 * a22 - a12 * a21);
    const f32 c01 = -(a10 * a22 - a12 * a20);
    const f32 c02 = (a10 * a21 - a11 * a20);
    const f32 c10 = -(a01 * a22 - a02 * a21);
    const f32 c11 = (a00 * a22 - a02 * a20);
    const f32 c12 = -(a00 * a21 - a01 * a20);
    const f32 c20 = (a01 * a12 - a02 * a11);
    const f32 c21 = -(a00 * a12 - a02 * a10);
    const f32 c22 = (a00 * a11 - a01 * a10);

    const f32 det = a00 * c00 + a01 * c01 + a02 * c02;
    math::Mat4 r{};
    if (det == 0.0f) {
        // Singular — return identity so we degrade gracefully.
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    const f32 inv = 1.0f / det;

    r.m[0] = c00 * inv;
    r.m[1] = c01 * inv;
    r.m[2] = c02 * inv;
    r.m[3] = 0.0f;
    r.m[4] = c10 * inv;
    r.m[5] = c11 * inv;
    r.m[6] = c12 * inv;
    r.m[7] = 0.0f;
    r.m[8] = c20 * inv;
    r.m[9] = c21 * inv;
    r.m[10] = c22 * inv;
    r.m[11] = 0.0f;

    r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
    r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
    r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
    r.m[15] = 1.0f;
    return r;
}

}  // namespace detail

void Bvh8::build(const Triangle* tris, u32 count) {
    auto& s = detail::state_of(*this);
    s.triangles.assign(tris, tris + count);
    s.prim_indices.resize(count);
    for (u32 i = 0; i < count; ++i)
        s.prim_indices[i] = i;

    s.binary_nodes.clear();
    s.wide_nodes.clear();

    if (count == 0) {
        s.as_built_cost = 0.0f;
        return;
    }

    std::vector<PrimRef> refs;
    build_prim_refs(tris, count, refs);

    Aabb root_bounds;
    root_bounds.reset();
    for (u32 i = 0; i < count; ++i)
        root_bounds.expand(refs[i].bounds);

    s.binary_nodes.reserve(static_cast<size_t>(count) * 2);
    build_binary(s.prim_indices, refs, s.binary_nodes, 0, count, root_bounds);

    rebuild_wide_from_binary(s.binary_nodes, s.wide_nodes);
    s.as_built_cost = tree_sah_cost(s.binary_nodes);
    s.refit_cost = s.as_built_cost;
}

void Bvh8::refit() {
    auto& s = detail::state_of(*this);
    if (s.binary_nodes.empty())
        return;
    refit_binary(s.binary_nodes,
                 s.triangles.data(),
                 static_cast<u32>(s.triangles.size()),
                 s.prim_indices,
                 0);
    rebuild_wide_from_binary(s.binary_nodes, s.wide_nodes);
    s.refit_cost = tree_sah_cost(s.binary_nodes);
}

u32 Bvh8::node_count() const noexcept {
    const auto& s = detail::state_of(*this);
    return static_cast<u32>(s.wide_nodes.size());
}

Hit Bvh8::intersect(const Ray& ray) const {
    const auto& s = detail::state_of(*this);
    detail::LocalHit lh = detail::traverse_scalar(s, ray);
    Hit h;
    h.hit = lh.hit;
    h.t = lh.t;
    h.normal = lh.normal;
    h.primitive = lh.primitive;
    h.instance = 0;
    return h;
}

bool Bvh8::occluded(const Ray& ray) const {
    const auto& s = detail::state_of(*this);
    return detail::occluded_scalar(s, ray);
}

// ───────────────────────────────────────────────────────────────────────
// TLAS — top-level over BLAS instances
// ───────────────────────────────────────────────────────────────────────

void Tlas::build(const InstanceDesc* instances, u32 count) {
    auto& s = detail::state_of(*this);
    s.instances.assign(instances, instances + count);
    s.binary_nodes.clear();
    s.wide_nodes.clear();
    s.world_bounds.resize(count);
    s.inv_transform.resize(count);

    if (count == 0) {
        s.as_built_cost = 0.0f;
        s.refit_cost = 0.0f;
        return;
    }

    // World-space AABB of each instance: transform the BLAS root bounds.
    std::vector<PrimRef> refs;
    refs.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const auto& inst = instances[i];
        Aabb blas_b;
        blas_b.reset();
        if (inst.blas && !detail::state_of(*inst.blas).binary_nodes.empty()) {
            blas_b = detail::state_of(*inst.blas).binary_nodes[0].bounds;
        }
        s.world_bounds[i] = detail::transform_aabb(blas_b, inst.transform);
        s.inv_transform[i] = detail::affine_inverse(inst.transform);
        refs[i].bounds = s.world_bounds[i];
        refs[i].centroid = s.world_bounds[i].centroid();
    }

    s.prim_indices.resize(count);
    for (u32 i = 0; i < count; ++i)
        s.prim_indices[i] = i;

    Aabb root_b;
    root_b.reset();
    for (u32 i = 0; i < count; ++i)
        root_b.expand(refs[i].bounds);

    s.binary_nodes.reserve(static_cast<size_t>(count) * 2);
    build_binary(s.prim_indices, refs, s.binary_nodes, 0, count, root_b);
    rebuild_wide_from_binary(s.binary_nodes, s.wide_nodes);
    s.as_built_cost = tree_sah_cost(s.binary_nodes);
    s.refit_cost = s.as_built_cost;
}

void Tlas::refit() {
    auto& s = detail::state_of(*this);
    const u32 count = static_cast<u32>(s.instances.size());
    if (count == 0 || s.binary_nodes.empty())
        return;

    for (u32 i = 0; i < count; ++i) {
        const auto& inst = s.instances[i];
        Aabb blas_b;
        blas_b.reset();
        if (inst.blas && !detail::state_of(*inst.blas).binary_nodes.empty()) {
            blas_b = detail::state_of(*inst.blas).binary_nodes[0].bounds;
        }
        s.world_bounds[i] = detail::transform_aabb(blas_b, inst.transform);
        s.inv_transform[i] = detail::affine_inverse(inst.transform);
    }

    // Refit binary tree bottom-up using s.world_bounds[].
    auto refit_node = [&](auto& self, u32 nid) -> void {
        BinaryNode& n = s.binary_nodes[nid];
        if (n.is_leaf()) {
            Aabb b;
            b.reset();
            for (u32 i = 0; i < n.prim_count; ++i) {
                const u32 pid = s.prim_indices[n.first_prim + i];
                if (pid < count)
                    b.expand(s.world_bounds[pid]);
            }
            n.bounds = b;
            return;
        }
        self(self, n.left);
        self(self, n.right);
        Aabb b;
        b.reset();
        b.expand(s.binary_nodes[n.left].bounds);
        b.expand(s.binary_nodes[n.right].bounds);
        n.bounds = b;
    };
    refit_node(refit_node, 0);
    rebuild_wide_from_binary(s.binary_nodes, s.wide_nodes);
    s.refit_cost = tree_sah_cost(s.binary_nodes);
}

Hit Tlas::intersect(const Ray& ray) const {
    const auto& s = detail::state_of(*this);
    Hit best;
    best.hit = false;
    best.t = ray.t_max;

    // For Wave A we linearly walk the instances and ask each one (after
    // its bounds slab-test passes). The wide-tree traversal could short-
    // circuit instance tests with an outer Bvh8 walk; that lands in Wave B.
    // Correctness-wise this is identical.
    for (u32 i = 0; i < s.instances.size(); ++i) {
        const auto& inst = s.instances[i];
        if (!inst.blas)
            continue;

        // Object-space ray = inv(transform) * world ray
        const math::Mat4& inv = s.inv_transform[i];
        math::Vec4 o4{ray.origin.x, ray.origin.y, ray.origin.z, 1.0f};
        math::Vec4 d4{ray.direction.x, ray.direction.y, ray.direction.z, 0.0f};
        math::Vec4 oo = math::mul(inv, o4);
        math::Vec4 dd = math::mul(inv, d4);

        Ray local;
        local.origin = {oo.x, oo.y, oo.z};
        local.direction = {dd.x, dd.y, dd.z};
        local.t_min = ray.t_min;
        local.t_max = best.t;

        const auto& bs = detail::state_of(*inst.blas);
        detail::LocalHit lh = detail::traverse_scalar(bs, local);
        if (lh.hit && lh.t < best.t) {
            best.hit = true;
            best.t = lh.t;
            best.primitive = lh.primitive;
            best.instance = i;
            // Transform normal back into world space using the upper-3×3
            // of the instance transform. For pure rotation + uniform scale
            // this is exact; for non-uniform scale a proper inverse-
            // transpose would be needed (Wave B).
            const math::Mat4& tr = inst.transform;
            math::Vec3 n = lh.normal;
            math::Vec3 wn{
                tr.m[0] * n.x + tr.m[4] * n.y + tr.m[8] * n.z,
                tr.m[1] * n.x + tr.m[5] * n.y + tr.m[9] * n.z,
                tr.m[2] * n.x + tr.m[6] * n.y + tr.m[10] * n.z,
            };
            best.normal = math::normalize(wn);
        }
    }
    return best;
}

bool Tlas::occluded(const Ray& ray) const {
    const auto& s = detail::state_of(*this);
    for (u32 i = 0; i < s.instances.size(); ++i) {
        const auto& inst = s.instances[i];
        if (!inst.blas)
            continue;

        const math::Mat4& inv = s.inv_transform[i];
        math::Vec4 o4{ray.origin.x, ray.origin.y, ray.origin.z, 1.0f};
        math::Vec4 d4{ray.direction.x, ray.direction.y, ray.direction.z, 0.0f};
        math::Vec4 oo = math::mul(inv, o4);
        math::Vec4 dd = math::mul(inv, d4);
        Ray local;
        local.origin = {oo.x, oo.y, oo.z};
        local.direction = {dd.x, dd.y, dd.z};
        local.t_min = ray.t_min;
        local.t_max = ray.t_max;

        if (detail::occluded_scalar(detail::state_of(*inst.blas), local)) {
            return true;
        }
    }
    return false;
}

namespace detail {

bool tlas_should_async_rebuild(const TlasState& s) noexcept {
    if (s.as_built_cost <= 0.0f)
        return false;
    return s.refit_cost > kRefitRebuildRatio * s.as_built_cost;
}

bool bvh_should_async_rebuild(const Bvh8State& s) noexcept {
    if (s.as_built_cost <= 0.0f)
        return false;
    return s.refit_cost > kRefitRebuildRatio * s.as_built_cost;
}

}  // namespace detail

}  // namespace psynder::render::rt
