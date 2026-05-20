// SPDX-License-Identifier: MIT
// Psynder — bounding-volume helpers impl. Lane 02.

#include "Bounds.h"
#include "MathExt.h"

#include <cmath>

namespace psynder::math {

Aabb transform(const Aabb& a, const Mat4& m) noexcept {
    // Arvo 1990: transform center + extents independently. The new center
    // is the matrix's full transform of the old center; the new extents
    // are |M_ij| * old_extents_j, where |M_ij| is the column-magnitude of
    // the upper-left 3×3 (the linear part). This produces the tight AABB
    // of the transformed (possibly rotated, possibly skewed) box without
    // touching the 8 corners.
    if (is_empty(a))
        return a;

    Vec3 c = center(a);
    Vec3 e = extents(a);

    // Transform center as a position (w = 1).
    Vec4 ct = mul(m, Vec4{c.x, c.y, c.z, 1.0f});
    Vec3 new_center = {ct.x, ct.y, ct.z};

    // Transform extents using the absolute-valued linear part.
    Vec3 new_extents;
    new_extents.x = std::fabs(m.m[0]) * e.x + std::fabs(m.m[4]) * e.y + std::fabs(m.m[8]) * e.z;
    new_extents.y = std::fabs(m.m[1]) * e.x + std::fabs(m.m[5]) * e.y + std::fabs(m.m[9]) * e.z;
    new_extents.z = std::fabs(m.m[2]) * e.x + std::fabs(m.m[6]) * e.y + std::fabs(m.m[10]) * e.z;

    return aabb_from_center_extents(new_center, new_extents);
}

bool intersects(const Sphere& s, const Aabb& a) noexcept {
    // Squared distance from the sphere center to the AABB. Each axis
    // contributes max(0, distance-to-slab)² independently — the classic
    // arvo "closest-point-on-box" test, but we never materialize the
    // closest point.
    f32 d2 = 0.0f;
    if (s.center.x < a.min.x) {
        f32 d = a.min.x - s.center.x;
        d2 += d * d;
    }
    if (s.center.x > a.max.x) {
        f32 d = s.center.x - a.max.x;
        d2 += d * d;
    }
    if (s.center.y < a.min.y) {
        f32 d = a.min.y - s.center.y;
        d2 += d * d;
    }
    if (s.center.y > a.max.y) {
        f32 d = s.center.y - a.max.y;
        d2 += d * d;
    }
    if (s.center.z < a.min.z) {
        f32 d = a.min.z - s.center.z;
        d2 += d * d;
    }
    if (s.center.z > a.max.z) {
        f32 d = s.center.z - a.max.z;
        d2 += d * d;
    }
    return d2 <= s.radius * s.radius;
}

Sphere sphere_union(const Sphere& a, const Sphere& b) noexcept {
    if (is_empty(a))
        return b;
    if (is_empty(b))
        return a;

    Vec3 ab = sub(b.center, a.center);
    f32 d = length(ab);

    // Either sphere already contains the other.
    if (d + b.radius <= a.radius)
        return a;
    if (d + a.radius <= b.radius)
        return b;

    f32 new_radius = (d + a.radius + b.radius) * 0.5f;
    // Position the new center on the line from a→b at the appropriate t.
    // When d == 0 the centers coincide; we just take the larger radius.
    if (d > 0.0f) {
        f32 t = (new_radius - a.radius) / d;
        return Sphere{add(a.center, mul(ab, t)), new_radius};
    }
    return Sphere{a.center, a.radius > b.radius ? a.radius : b.radius};
}

Sphere transform(const Sphere& s, const Mat4& m) noexcept {
    if (is_empty(s))
        return s;

    // Center transforms as a position.
    Vec4 ct = mul(m, Vec4{s.center.x, s.center.y, s.center.z, 1.0f});
    Vec3 new_center = {ct.x, ct.y, ct.z};

    // Largest column length of the upper-left 3×3 is an upper bound on
    // the matrix's max singular value — i.e. the max stretch the matrix
    // can apply in any direction. Multiplying the radius by it produces a
    // conservative sphere bound for any affine transform.
    f32 c0 = m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2];
    f32 c1 = m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6];
    f32 c2 = m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10];
    f32 max_c2 = c0;
    if (c1 > max_c2)
        max_c2 = c1;
    if (c2 > max_c2)
        max_c2 = c2;
    f32 scale = std::sqrt(max_c2);

    return Sphere{new_center, s.radius * scale};
}

Sphere bounding_sphere(const Aabb& a) noexcept {
    if (is_empty(a))
        return sphere_empty();
    Vec3 c = center(a);
    Vec3 e = extents(a);
    return Sphere{c, std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z)};
}

Aabb bounding_aabb(const Sphere& s) noexcept {
    if (is_empty(s))
        return aabb_empty();
    return Aabb{
        Vec3{s.center.x - s.radius, s.center.y - s.radius, s.center.z - s.radius},
        Vec3{s.center.x + s.radius, s.center.y + s.radius, s.center.z + s.radius},
    };
}

}  // namespace psynder::math
