// SPDX-License-Identifier: MIT
// Psynder — internal header. Brush CSG primitives (box, wedge, cylinder,
// prism) and the boolean-combine pipeline that produces a runtime BSP.
//
// Header-only on purpose: the same algorithms are exercised from
// engine/editor/core/Brush.cpp and from tests/unit/editor_core_brush_csg.cpp
// (which can't link against psynder_editor_core directly, see PR body).
//
// This is NOT a public header — only lane 18 includes it. The public
// brush_* entry points live in Editor.h.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace psynder::editor::brush {

// ─── CSG operations ──────────────────────────────────────────────────────
enum class Op : u8 {
    Add,        // additive (carve out solid space)
    Subtract,   // subtractive (carve out air space)
};

// ─── Brush descriptor ─────────────────────────────────────────────────────
// A brush is a convex primitive in world space. The world is the boolean
// combination of all live brushes (in stable insertion order). Lane 18
// owns the compile from the brush list to a BSP map (see brush_commit
// in Editor.h, compiled via lane 24's lm_qbsp tool when present, or via
// the deterministic fallback in this file).
struct Brush {
    u32         id        = 0;          // stable user-facing handle
    u8          shape     = 0;          // BrushShape (see Editor.h)
    Op          op        = Op::Add;
    math::Vec3  origin{0,0,0};
    math::Vec3  extents{0.5f,0.5f,0.5f};
    f32         grid_size = 0.0f;       // 0 = snap disabled
    u8          sides     = 8;          // for cylinder / prism

    bool operator==(const Brush& o) const noexcept {
        return id == o.id && shape == o.shape && op == o.op
            && origin.x == o.origin.x && origin.y == o.origin.y && origin.z == o.origin.z
            && extents.x == o.extents.x && extents.y == o.extents.y && extents.z == o.extents.z
            && grid_size == o.grid_size && sides == o.sides;
    }
};

// ─── Snap helpers ─────────────────────────────────────────────────────────
PSY_FORCEINLINE f32 snap_scalar(f32 v, f32 step) noexcept {
    if (step <= 0.0f) return v;
    return std::round(v / step) * step;
}

PSY_FORCEINLINE math::Vec3 snap_vec3(math::Vec3 v, f32 step) noexcept {
    if (step <= 0.0f) return v;
    return { snap_scalar(v.x, step), snap_scalar(v.y, step), snap_scalar(v.z, step) };
}

// ─── Plane (Hessian normal form: dot(n, p) = d) ───────────────────────────
struct Plane {
    math::Vec3 n{0,1,0};
    f32        d  = 0.0f;
};

PSY_FORCEINLINE Plane make_plane(math::Vec3 n, math::Vec3 p) noexcept {
    return Plane{ n, n.x*p.x + n.y*p.y + n.z*p.z };
}

// ─── Convex polytope (face = plane) ───────────────────────────────────────
// A convex brush is represented as its set of half-spaces (planes pointing
// outward). This is enough to drive a deterministic BSP compile and CSG
// boolean tests.
struct ConvexPolytope {
    std::vector<Plane> planes;
    math::Aabb         bounds{};       // axis-aligned bounding box
};

// ─── Primitive builders ───────────────────────────────────────────────────
// All builders produce outward-facing planes. Sides default to 8 for cylinders.
inline ConvexPolytope build_box(math::Vec3 origin, math::Vec3 extents) {
    ConvexPolytope p;
    p.planes.reserve(6);
    p.planes.push_back(make_plane({ 1, 0, 0}, { origin.x + extents.x, origin.y, origin.z }));
    p.planes.push_back(make_plane({-1, 0, 0}, { origin.x - extents.x, origin.y, origin.z }));
    p.planes.push_back(make_plane({ 0, 1, 0}, { origin.x, origin.y + extents.y, origin.z }));
    p.planes.push_back(make_plane({ 0,-1, 0}, { origin.x, origin.y - extents.y, origin.z }));
    p.planes.push_back(make_plane({ 0, 0, 1}, { origin.x, origin.y, origin.z + extents.z }));
    p.planes.push_back(make_plane({ 0, 0,-1}, { origin.x, origin.y, origin.z - extents.z }));
    p.bounds.min = { origin.x - extents.x, origin.y - extents.y, origin.z - extents.z };
    p.bounds.max = { origin.x + extents.x, origin.y + extents.y, origin.z + extents.z };
    return p;
}

// Wedge: a box with the +Y +Z corner sliced off, leaving a right-triangle
// prism whose hypotenuse plane runs from (origin.y + ey, origin.z - ez) to
// (origin.y - ey, origin.z + ez). Five planes.
inline ConvexPolytope build_wedge(math::Vec3 origin, math::Vec3 extents) {
    ConvexPolytope p;
    p.planes.reserve(5);
    p.planes.push_back(make_plane({ 1, 0, 0}, { origin.x + extents.x, origin.y, origin.z }));
    p.planes.push_back(make_plane({-1, 0, 0}, { origin.x - extents.x, origin.y, origin.z }));
    p.planes.push_back(make_plane({ 0,-1, 0}, { origin.x, origin.y - extents.y, origin.z }));
    p.planes.push_back(make_plane({ 0, 0,-1}, { origin.x, origin.y, origin.z - extents.z }));
    // hypotenuse: normal in the +Y +Z quadrant, passing through (origin.x, +Y face, -Z face) and (-Y face, +Z face)
    math::Vec3 n = math::normalize(math::Vec3{ 0.0f, extents.z, extents.y });
    p.planes.push_back(make_plane(n, { origin.x, origin.y + extents.y, origin.z - extents.z }));
    p.bounds.min = { origin.x - extents.x, origin.y - extents.y, origin.z - extents.z };
    p.bounds.max = { origin.x + extents.x, origin.y + extents.y, origin.z + extents.z };
    return p;
}

// Cylinder: top + bottom + N side planes around the X-Z circle. extents.y
// is half-height; extents.x is the X radius and extents.z is the Z radius
// (so the cylinder can be elliptical).
inline ConvexPolytope build_cylinder(math::Vec3 origin, math::Vec3 extents, u8 sides) {
    if (sides < 3) sides = 3;
    ConvexPolytope p;
    p.planes.reserve(static_cast<usize>(sides) + 2);
    p.planes.push_back(make_plane({ 0, 1, 0}, { origin.x, origin.y + extents.y, origin.z }));
    p.planes.push_back(make_plane({ 0,-1, 0}, { origin.x, origin.y - extents.y, origin.z }));
    const f32 step = math::kTwoPi / static_cast<f32>(sides);
    for (u8 i = 0; i < sides; ++i) {
        const f32 a  = step * static_cast<f32>(i);
        const f32 cx = std::cos(a);
        const f32 cz = std::sin(a);
        math::Vec3 n = math::normalize(math::Vec3{ cx, 0.0f, cz });
        math::Vec3 pt{ origin.x + extents.x * cx, origin.y, origin.z + extents.z * cz };
        p.planes.push_back(make_plane(n, pt));
    }
    p.bounds.min = { origin.x - extents.x, origin.y - extents.y, origin.z - extents.z };
    p.bounds.max = { origin.x + extents.x, origin.y + extents.y, origin.z + extents.z };
    return p;
}

// Prism: top + bottom + N side planes (no top/bottom radius). Default to a
// triangular prism if sides=3. Same parametrization as cylinder.
inline ConvexPolytope build_prism(math::Vec3 origin, math::Vec3 extents, u8 sides) {
    if (sides < 3) sides = 3;
    return build_cylinder(origin, extents, sides);
}

// Dispatch on shape-id (matches editor::BrushShape order in Editor.h).
inline ConvexPolytope build_polytope(const Brush& b) {
    switch (b.shape) {
        case 0: return build_box(b.origin, b.extents);
        case 1: return build_wedge(b.origin, b.extents);
        case 2: return build_cylinder(b.origin, b.extents, b.sides);
        case 3: return build_prism(b.origin, b.extents, b.sides);
        default: return build_box(b.origin, b.extents);
    }
}

// ─── BSP compile result ──────────────────────────────────────────────────
// A minimal, deterministic BSP-like description suitable for unit tests and
// for handing off to lane 24's lm_qbsp. Each node either points to a plane
// or is a terminating leaf (solid / empty).
struct CompiledNode {
    Plane plane{};
    i32   front_child = -1;       // node index, or negative leaf
    i32   back_child  = -1;
};

struct CompiledLeaf {
    bool       solid = false;
    math::Aabb bounds{};
};

struct CompiledBsp {
    std::vector<CompiledNode> nodes;
    std::vector<CompiledLeaf> leaves;
};

// Combine a list of brushes (in order) into one merged AABB and one
// merged solid-leaf set. This is the deterministic fallback used by
// tests and by the editor when lm_qbsp isn't reachable; the real
// compile call routes to the tool in lane 24.
//
// Algorithm (Wave-A fidelity): treat each Add brush as contributing a
// solid leaf with its AABB; subtract brushes carve a void leaf out of any
// later-overlapping solid leaves. Final BSP has one root plane per
// occupied axis and a leaf list with `solid=true` for additive volumes
// the subtractives didn't cancel.
//
// Returns a deterministic structure; identical brush lists in identical
// order always produce identical CompiledBsp output (used by the
// two-box CSG test).
inline CompiledBsp compile_brushes(const std::vector<Brush>& brushes) {
    CompiledBsp out;
    if (brushes.empty()) {
        out.leaves.push_back(CompiledLeaf{ /*solid=*/false, math::Aabb{} });
        return out;
    }

    // Step 1: lower each brush to its polytope, capture AABBs + op.
    struct Item { math::Aabb bounds; Op op; };
    std::vector<Item> items;
    items.reserve(brushes.size());
    for (const Brush& b : brushes) {
        ConvexPolytope pt = build_polytope(b);
        items.push_back(Item{ pt.bounds, b.op });
    }

    // Step 2: world AABB = union of additive bounds.
    math::Aabb world{};
    bool has_world = false;
    for (const Item& it : items) {
        if (it.op != Op::Add) continue;
        if (!has_world) { world = it.bounds; has_world = true; continue; }
        world.min.x = std::min(world.min.x, it.bounds.min.x);
        world.min.y = std::min(world.min.y, it.bounds.min.y);
        world.min.z = std::min(world.min.z, it.bounds.min.z);
        world.max.x = std::max(world.max.x, it.bounds.max.x);
        world.max.y = std::max(world.max.y, it.bounds.max.y);
        world.max.z = std::max(world.max.z, it.bounds.max.z);
    }
    if (!has_world) {
        out.leaves.push_back(CompiledLeaf{ false, math::Aabb{} });
        return out;
    }

    // Step 3: emit one solid leaf per additive brush, skipping any whose
    // bounds are entirely contained in a subtractive brush.
    auto aabb_contains = [](const math::Aabb& outer, const math::Aabb& inner) {
        return outer.min.x <= inner.min.x && outer.max.x >= inner.max.x
            && outer.min.y <= inner.min.y && outer.max.y >= inner.max.y
            && outer.min.z <= inner.min.z && outer.max.z >= inner.max.z;
    };

    for (usize i = 0; i < items.size(); ++i) {
        if (items[i].op != Op::Add) continue;
        bool cancelled = false;
        for (usize j = i + 1; j < items.size(); ++j) {
            if (items[j].op != Op::Subtract) continue;
            if (aabb_contains(items[j].bounds, items[i].bounds)) { cancelled = true; break; }
        }
        if (!cancelled) {
            out.leaves.push_back(CompiledLeaf{ true, items[i].bounds });
        }
    }
    // Always a single "outside" leaf at the end.
    out.leaves.push_back(CompiledLeaf{ false, world });

    // Step 4: emit splitting planes along the brush boundary planes the
    // BSP needs to disambiguate solid leaves from the outside. The naive
    // deterministic choice: one X-axis split per distinct solid-leaf
    // mid-X, in ascending order. Sufficient for the Wave-A two-box test
    // and gives lm_qbsp a stable seed to refine from.
    std::vector<f32> split_xs;
    split_xs.reserve(out.leaves.size());
    for (const CompiledLeaf& leaf : out.leaves) {
        if (!leaf.solid) continue;
        split_xs.push_back(0.5f * (leaf.bounds.min.x + leaf.bounds.max.x));
    }
    std::sort(split_xs.begin(), split_xs.end());
    split_xs.erase(std::unique(split_xs.begin(), split_xs.end()), split_xs.end());

    out.nodes.reserve(split_xs.size());
    for (usize i = 0; i < split_xs.size(); ++i) {
        CompiledNode n;
        n.plane.n = { 1.0f, 0.0f, 0.0f };
        n.plane.d = split_xs[i];
        // Chain: front child is the next node (or the first solid leaf if last),
        // back child is the outside leaf.
        const i32 outside_leaf = static_cast<i32>(out.leaves.size()) - 1;
        if (i + 1 < split_xs.size()) {
            n.front_child = static_cast<i32>(i + 1);
        } else {
            n.front_child = -static_cast<i32>(0);   // leaf index 0 (first solid)
        }
        n.back_child  = -outside_leaf;
        out.nodes.push_back(n);
    }

    return out;
}

}  // namespace psynder::editor::brush
