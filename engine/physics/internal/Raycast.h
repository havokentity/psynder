// SPDX-License-Identifier: MIT
// Psynder physics -- scene raycast intersection primitives (DESIGN.md 10.1).
//
// Closed-form ray-vs-shape tests backing World::raycast. Each routine takes a
// world-space ray (origin + UNIT direction) and the shape's world transform,
// and reports the nearest entry parameter `t` (distance, since the direction
// is unit-length) in [0, max_t] together with the surface normal at the hit.
//
// All functions are header-inline, branch-light, and ALLOC-FREE -- no heap, no
// containers, no statics. They are pure reads of their arguments, so a const
// raycast can call them alongside any other const reads without racing step().
//
// Determinism: only +,-,*,/ and one sqrt (capsule/sphere distance); no RNG, no
// reassociation, -fno-fast-math friendly. The psynder_physics target already
// pins the no-fast-math flags (see CMakeLists.txt), so every includer inherits
// them.
//
// half_extent layout mirrors Shape.h / Kernels.h:
//   Sphere  : half_extent.x = radius
//   Capsule : x = radius, y = half-height of the cylindrical section (LOCAL +Y)
//   Box     : x/y/z = half extents (oriented by the body rotation -> OBB)

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Shape.h"  // detail::quat_rotate / quat_conjugate

#include <cmath>

namespace psynder::physics::detail {

// Result of one ray-vs-shape test. `t` is the distance along the (unit) ray to
// the entry point; `normal` is the outward surface normal there.
struct RayShapeHit {
    f32 t = 0.0f;
    math::Vec3 point{0, 0, 0};
    math::Vec3 normal{0, 1, 0};
    bool hit = false;
};

// --- Ray vs axis-aligned box (slab method) ---------------------------------
// Broad prefilter AND the AABB/fallback narrow test. Returns the nearest entry
// t in [0, max_t]; tmin<0<tmax (origin inside the box) reports t=0. `dir` need
// not be unit for the slab test itself, but raycast passes a unit dir so t is a
// true distance. Uses 1/dir per component; an exactly-axis-parallel component
// yields +/-inf which the min/max ordering handles correctly (IEEE-754, which
// -fno-finite-math-only preserves).
[[nodiscard]] PSY_FORCEINLINE bool ray_aabb(math::Vec3 origin,
                                            math::Vec3 dir,
                                            math::Aabb box,
                                            f32 max_t,
                                            f32& out_tmin) noexcept {
    const f32 invx = 1.0f / dir.x;
    const f32 invy = 1.0f / dir.y;
    const f32 invz = 1.0f / dir.z;

    f32 t1 = (box.min.x - origin.x) * invx;
    f32 t2 = (box.max.x - origin.x) * invx;
    f32 tmin = std::fmin(t1, t2);
    f32 tmax = std::fmax(t1, t2);

    t1 = (box.min.y - origin.y) * invy;
    t2 = (box.max.y - origin.y) * invy;
    tmin = std::fmax(tmin, std::fmin(t1, t2));
    tmax = std::fmin(tmax, std::fmax(t1, t2));

    t1 = (box.min.z - origin.z) * invz;
    t2 = (box.max.z - origin.z) * invz;
    tmin = std::fmax(tmin, std::fmin(t1, t2));
    tmax = std::fmin(tmax, std::fmax(t1, t2));

    if (tmax < std::fmax(tmin, 0.0f))
        return false;            // misses, or the box is entirely behind the ray
    const f32 t = (tmin >= 0.0f) ? tmin : 0.0f;  // origin inside -> entry at 0
    if (t > max_t)
        return false;            // first hit is beyond the segment
    out_tmin = t;
    return true;
}

// --- Ray vs oriented box (OBB) ---------------------------------------------
// Transform the ray into the box's LOCAL frame (where the box is an AABB
// [-he, +he]) with R^T, run the slab test, capture which slab supplied tmin to
// recover the local face normal, then rotate the hit point + normal back to
// world. Exact for a rotated Box; also the documented fallback for ConvexHull /
// Compound / Heightfield / TriangleMesh (treated as the OBB of their
// half_extent until lane-13 Wave B fleshes those shapes out).
[[nodiscard]] PSY_FORCEINLINE bool ray_obb(math::Vec3 origin,
                                           math::Vec3 dir,
                                           math::Vec3 center,
                                           math::Quat rotation,
                                           math::Vec3 half_extent,
                                           f32 max_t,
                                           RayShapeHit& out) noexcept {
    // World -> local: subtract centre, rotate by the inverse (conjugate) quat.
    const math::Quat inv = detail::quat_conjugate(rotation);
    const math::Vec3 lo = detail::quat_rotate(inv, math::sub(origin, center));
    const math::Vec3 ld = detail::quat_rotate(inv, dir);

    const f32 invx = 1.0f / ld.x;
    const f32 invy = 1.0f / ld.y;
    const f32 invz = 1.0f / ld.z;

    f32 t1 = (-half_extent.x - lo.x) * invx;
    f32 t2 = (half_extent.x - lo.x) * invx;
    f32 tmin = std::fmin(t1, t2);
    f32 tmax = std::fmax(t1, t2);
    int axis = 0;
    f32 ax_sign = (t1 <= t2) ? -1.0f : 1.0f;

    t1 = (-half_extent.y - lo.y) * invy;
    t2 = (half_extent.y - lo.y) * invy;
    f32 ny = std::fmin(t1, t2);
    if (ny > tmin) {
        tmin = ny;
        axis = 1;
        ax_sign = (t1 <= t2) ? -1.0f : 1.0f;
    }
    tmax = std::fmin(tmax, std::fmax(t1, t2));

    t1 = (-half_extent.z - lo.z) * invz;
    t2 = (half_extent.z - lo.z) * invz;
    f32 nz = std::fmin(t1, t2);
    if (nz > tmin) {
        tmin = nz;
        axis = 2;
        ax_sign = (t1 <= t2) ? -1.0f : 1.0f;
    }
    tmax = std::fmin(tmax, std::fmax(t1, t2));

    if (tmax < std::fmax(tmin, 0.0f))
        return false;

    f32 t = tmin;
    math::Vec3 local_n{0, 0, 0};
    if (axis == 0)
        local_n.x = ax_sign;
    else if (axis == 1)
        local_n.y = ax_sign;
    else
        local_n.z = ax_sign;

    if (t < 0.0f) {
        // Origin inside the box: report entry at t=0; outward normal undefined,
        // pick the ray-opposing local axis so callers still get a sane value.
        t = 0.0f;
        local_n = {0, 0, 0};
    }
    if (t > max_t)
        return false;

    out.t = t;
    out.point = math::add(origin, math::mul(dir, t));
    out.normal = detail::quat_rotate(rotation, local_n);  // local -> world
    out.hit = true;
    return true;
}

// --- Ray vs sphere ---------------------------------------------------------
// Geometric solve of |origin + t*dir - centre|^2 = r^2 for a UNIT dir. Returns
// the nearest non-negative root in [0, max_t]; an origin inside the sphere
// reports t=0 with the outward normal at the origin direction.
[[nodiscard]] PSY_FORCEINLINE bool ray_sphere(math::Vec3 origin,
                                              math::Vec3 dir,
                                              math::Vec3 center,
                                              f32 radius,
                                              f32 max_t,
                                              RayShapeHit& out) noexcept {
    const math::Vec3 oc = math::sub(origin, center);
    const f32 b = math::dot(oc, dir);              // dir is unit -> a == 1
    const f32 c = math::dot(oc, oc) - radius * radius;
    if (c > 0.0f && b > 0.0f)
        return false;                              // origin outside, pointing away
    const f32 disc = b * b - c;
    if (disc < 0.0f)
        return false;                              // misses
    const f32 sq = std::sqrt(disc);
    f32 t = -b - sq;                               // near root
    if (t < 0.0f)
        t = 0.0f;                                  // origin inside the sphere
    if (t > max_t)
        return false;
    const math::Vec3 p = math::add(origin, math::mul(dir, t));
    out.t = t;
    out.point = p;
    out.normal = math::normalize(math::sub(p, center));
    out.hit = true;
    return true;
}

// --- Ray vs capsule --------------------------------------------------------
// Capsule = the swept-sphere of the LOCAL +Y segment [center - R*yhat*h,
// center + R*yhat*h] inflated by `radius`. We march the ray analytically against
// the infinite cylinder around the segment axis, clamp to the segment, and fall
// back to the two hemispherical end-caps. Returns the nearest entry in
// [0, max_t] with the outward normal (from the nearest axis point to the hit).
[[nodiscard]] PSY_FORCEINLINE bool ray_capsule(math::Vec3 origin,
                                               math::Vec3 dir,
                                               math::Vec3 center,
                                               math::Quat rotation,
                                               f32 radius,
                                               f32 half_h,
                                               f32 max_t,
                                               RayShapeHit& out) noexcept {
    // Segment endpoints in world space (axis = local +Y rotated to world).
    const math::Vec3 axis = detail::quat_rotate(rotation, {0.0f, half_h, 0.0f});
    const math::Vec3 pa = math::sub(center, axis);  // bottom cap centre
    const math::Vec3 pb = math::add(center, axis);  // top cap centre
    const math::Vec3 ab = math::sub(pb, pa);
    const f32 ab2 = math::dot(ab, ab);

    f32 best_t = max_t;
    bool found = false;
    math::Vec3 best_axis_pt{0, 0, 0};

    // 1) Infinite cylinder around the segment axis. Solve the quadratic for the
    //    component of (origin + t*dir - pa) perpendicular to `ab`.
    if (ab2 > 1e-20f) {
        const math::Vec3 ao = math::sub(origin, pa);
        const f32 d_ab = math::dot(dir, ab);
        const f32 ao_ab = math::dot(ao, ab);
        // Perpendicular parts: x = ao - (ao.ab/ab2)ab ; m = dir - (dir.ab/ab2)ab
        const f32 a = math::dot(dir, dir) - (d_ab * d_ab) / ab2;
        const f32 bq = math::dot(dir, ao) - (d_ab * ao_ab) / ab2;
        const f32 cq = math::dot(ao, ao) - (ao_ab * ao_ab) / ab2 - radius * radius;
        if (a > 1e-12f) {
            const f32 disc = bq * bq - a * cq;
            if (disc >= 0.0f) {
                const f32 sq = std::sqrt(disc);
                f32 t = (-bq - sq) / a;
                if (t < 0.0f)
                    t = (-bq + sq) / a;  // origin inside the cylinder shell
                if (t >= 0.0f && t <= best_t) {
                    // Project the hit onto the axis; accept only if it lands on
                    // the finite segment (caps handle the rest).
                    const math::Vec3 hit = math::add(origin, math::mul(dir, t));
                    const f32 s = math::dot(math::sub(hit, pa), ab) / ab2;
                    if (s >= 0.0f && s <= 1.0f) {
                        best_t = t;
                        best_axis_pt = math::add(pa, math::mul(ab, s));
                        found = true;
                    }
                }
            }
        }
    }

    // 2) The two hemispherical end caps (spheres of `radius` at pa, pb).
    const math::Vec3 caps[2] = {pa, pb};
    for (int i = 0; i < 2; ++i) {
        const math::Vec3 oc = math::sub(origin, caps[i]);
        const f32 b = math::dot(oc, dir);
        const f32 c = math::dot(oc, oc) - radius * radius;
        const f32 disc = b * b - c;
        if (disc < 0.0f)
            continue;
        const f32 sq = std::sqrt(disc);
        f32 t = -b - sq;
        if (t < 0.0f)
            t = -b + sq;  // origin inside this cap sphere
        if (t < 0.0f || t > best_t)
            continue;
        best_t = t;
        best_axis_pt = caps[i];
        found = true;
    }

    if (!found)
        return false;
    const math::Vec3 p = math::add(origin, math::mul(dir, best_t));
    out.t = best_t;
    out.point = p;
    out.normal = math::normalize(math::sub(p, best_axis_pt));
    out.hit = true;
    return true;
}

}  // namespace psynder::physics::detail
