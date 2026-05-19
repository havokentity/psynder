// SPDX-License-Identifier: MIT
// Psynder — bounding-volume helpers. Lane 02.
//
// The `Aabb` and `Sphere` types live in Math.h (frozen public API). This
// header layers the operations on top — union, contains, intersects,
// expand, transform-by-Mat4 — without touching the type definitions.
//
// Conventions:
//   - Aabb is axis-aligned in world space. `min` and `max` are inclusive.
//   - A degenerate Aabb has `min > max` on any axis; we treat that as
//     "empty" for union / intersect purposes.
//   - Sphere transform uses the matrix's largest column-scale to bound the
//     radius after non-uniform scale (a conservative — possibly loose — but
//     always-correct bound).

#pragma once

#include "Math.h"

#include <limits>

namespace psynder::math {

// ─── Aabb construction ───────────────────────────────────────────────────
// The empty AABB — `union(empty, x) == x` for every x. Used as the seed
// when accumulating a bound over a set of points.
inline Aabb aabb_empty() noexcept {
    constexpr f32 inf = std::numeric_limits<f32>::infinity();
    return Aabb{ Vec3{ inf,  inf,  inf}, Vec3{-inf, -inf, -inf} };
}

inline Aabb aabb_from_center_extents(Vec3 center, Vec3 half_extents) noexcept {
    return Aabb{
        sub(center, half_extents),
        add(center, half_extents),
    };
}

// ─── Aabb queries ────────────────────────────────────────────────────────
constexpr bool is_empty(const Aabb& b) noexcept {
    return b.min.x > b.max.x || b.min.y > b.max.y || b.min.z > b.max.z;
}

constexpr Vec3 center(const Aabb& b) noexcept {
    return { (b.min.x + b.max.x) * 0.5f,
             (b.min.y + b.max.y) * 0.5f,
             (b.min.z + b.max.z) * 0.5f };
}

constexpr Vec3 extents(const Aabb& b) noexcept {
    return { (b.max.x - b.min.x) * 0.5f,
             (b.max.y - b.min.y) * 0.5f,
             (b.max.z - b.min.z) * 0.5f };
}

constexpr Vec3 size(const Aabb& b) noexcept {
    return { b.max.x - b.min.x, b.max.y - b.min.y, b.max.z - b.min.z };
}

constexpr bool contains(const Aabb& b, Vec3 p) noexcept {
    return p.x >= b.min.x && p.x <= b.max.x
        && p.y >= b.min.y && p.y <= b.max.y
        && p.z >= b.min.z && p.z <= b.max.z;
}

constexpr bool contains(const Aabb& outer, const Aabb& inner) noexcept {
    return inner.min.x >= outer.min.x && inner.max.x <= outer.max.x
        && inner.min.y >= outer.min.y && inner.max.y <= outer.max.y
        && inner.min.z >= outer.min.z && inner.max.z <= outer.max.z;
}

constexpr bool intersects(const Aabb& a, const Aabb& b) noexcept {
    return a.min.x <= b.max.x && a.max.x >= b.min.x
        && a.min.y <= b.max.y && a.max.y >= b.min.y
        && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// ─── Aabb mutators (return-by-value, immutable inputs) ───────────────────
constexpr Aabb aabb_union(const Aabb& a, const Aabb& b) noexcept {
    Aabb r;
    r.min.x = a.min.x < b.min.x ? a.min.x : b.min.x;
    r.min.y = a.min.y < b.min.y ? a.min.y : b.min.y;
    r.min.z = a.min.z < b.min.z ? a.min.z : b.min.z;
    r.max.x = a.max.x > b.max.x ? a.max.x : b.max.x;
    r.max.y = a.max.y > b.max.y ? a.max.y : b.max.y;
    r.max.z = a.max.z > b.max.z ? a.max.z : b.max.z;
    return r;
}

constexpr Aabb aabb_union(const Aabb& a, Vec3 p) noexcept {
    Aabb r;
    r.min.x = a.min.x < p.x ? a.min.x : p.x;
    r.min.y = a.min.y < p.y ? a.min.y : p.y;
    r.min.z = a.min.z < p.z ? a.min.z : p.z;
    r.max.x = a.max.x > p.x ? a.max.x : p.x;
    r.max.y = a.max.y > p.y ? a.max.y : p.y;
    r.max.z = a.max.z > p.z ? a.max.z : p.z;
    return r;
}

constexpr Aabb expand(const Aabb& a, f32 margin) noexcept {
    return Aabb{
        Vec3{ a.min.x - margin, a.min.y - margin, a.min.z - margin },
        Vec3{ a.max.x + margin, a.max.y + margin, a.max.z + margin },
    };
}

constexpr Aabb expand(const Aabb& a, Vec3 margin) noexcept {
    return Aabb{
        Vec3{ a.min.x - margin.x, a.min.y - margin.y, a.min.z - margin.z },
        Vec3{ a.max.x + margin.x, a.max.y + margin.y, a.max.z + margin.z },
    };
}

// Transform an axis-aligned box by an affine Mat4 and return the AABB that
// snugly contains the resulting (possibly rotated/skewed) box. Uses the
// "absolute-value-of-rotation" trick (Arvo 1990; also in Real-Time
// Collision Detection §4.2.6) — O(1), no per-corner transform loop.
Aabb transform(const Aabb& a, const Mat4& m) noexcept;

// ─── Sphere ──────────────────────────────────────────────────────────────
inline Sphere sphere_empty() noexcept {
    return Sphere{ Vec3{0,0,0}, -1.0f };
}

constexpr bool is_empty(const Sphere& s) noexcept { return s.radius < 0.0f; }

constexpr bool contains(const Sphere& s, Vec3 p) noexcept {
    Vec3 d = sub(p, s.center);
    return dot(d, d) <= s.radius * s.radius;
}

constexpr bool intersects(const Sphere& a, const Sphere& b) noexcept {
    Vec3 d = sub(a.center, b.center);
    f32 r = a.radius + b.radius;
    return dot(d, d) <= r * r;
}

bool intersects(const Sphere& s, const Aabb& a) noexcept;

// Wrap two spheres in the smallest enclosing sphere. Handy when building
// up a coarse bound for a chain of primitives during BVH refit.
Sphere sphere_union(const Sphere& a, const Sphere& b) noexcept;

constexpr Sphere expand(const Sphere& s, f32 margin) noexcept {
    return Sphere{ s.center, s.radius + margin };
}

// Transforming a sphere by a Mat4 yields an ellipsoid in general; we keep
// the sphere wrapper conservative by scaling the radius by the largest
// column length (max singular value upper bound).
Sphere transform(const Sphere& s, const Mat4& m) noexcept;

// Bridge: smallest enclosing sphere of an AABB and vice versa.
Sphere bounding_sphere(const Aabb& a) noexcept;
Aabb   bounding_aabb(const Sphere& s) noexcept;

}  // namespace psynder::math
