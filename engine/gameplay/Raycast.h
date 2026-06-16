// SPDX-License-Identifier: MIT
// Psynder — M-COMBAT ray / segment intersection helpers.
//
// The physics public API (engine/physics/Physics.h, class World) exposes body
// creation + mutation + character/vehicle helpers but NO public scene raycast.
// So combat does its own ray-vs-HitboxComponent sweep here. These are pure,
// branch-light, alloc-free geometry primitives shared by the hitscan firing
// path and the projectile-segment hit test.
//
// Convention: a ray is {origin, dir(normalized)}; we return the nearest
// non-negative parametric distance `t` along the ray to the surface, within an
// optional [0, max_t] window. `false` => no hit in window.

#pragma once

#include "math/Math.h"

#include <cmath>

namespace psynder::gameplay {

using ::psynder::f32;

struct RayHit {
    f32 t = 0.0f;     // distance along the ray to the entry point
    bool hit = false;
};

// Ray vs sphere (center, radius). Standard quadratic; we want the nearest
// entry point with t in [0, max_t]. If the origin is inside the sphere we
// report t = 0 (an immediate hit), which is the right behaviour for a muzzle
// that overlaps a target.
[[nodiscard]] inline RayHit ray_sphere(math::Vec3 origin,
                                       math::Vec3 dir,
                                       math::Vec3 center,
                                       f32 radius,
                                       f32 max_t) noexcept {
    const math::Vec3 m = math::sub(origin, center);
    const f32 b = math::dot(m, dir);          // dir assumed normalized
    const f32 c = math::dot(m, m) - radius * radius;
    // Origin outside (c > 0) and pointing away (b > 0) => miss.
    if (c > 0.0f && b > 0.0f)
        return {};
    const f32 disc = b * b - c;
    if (disc < 0.0f)
        return {};
    const f32 sqrt_disc = std::sqrt(disc);
    f32 t = -b - sqrt_disc;
    if (t < 0.0f)
        t = 0.0f;  // origin inside the sphere
    if (t > max_t)
        return {};
    return {t, true};
}

// Ray vs axis-aligned box given as center +/- half_extent. Slab method; robust
// to zero direction components via the 1/dir reciprocal with infinities (IEEE,
// -fno-finite-math-only safe). Reports the near intersection t in [0, max_t].
[[nodiscard]] inline RayHit ray_aabb(math::Vec3 origin,
                                     math::Vec3 dir,
                                     math::Vec3 center,
                                     math::Vec3 half_extent,
                                     f32 max_t) noexcept {
    const math::Vec3 bmin{center.x - half_extent.x,
                          center.y - half_extent.y,
                          center.z - half_extent.z};
    const math::Vec3 bmax{center.x + half_extent.x,
                          center.y + half_extent.y,
                          center.z + half_extent.z};

    f32 tmin = 0.0f;
    f32 tmax = max_t;

    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {dir.x, dir.y, dir.z};
    const f32 lo[3] = {bmin.x, bmin.y, bmin.z};
    const f32 hi[3] = {bmax.x, bmax.y, bmax.z};

    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-8f) {
            // Ray parallel to the slab: miss unless the origin is between the
            // planes.
            if (o[axis] < lo[axis] || o[axis] > hi[axis])
                return {};
        } else {
            const f32 inv = 1.0f / d[axis];
            f32 t1 = (lo[axis] - o[axis]) * inv;
            f32 t2 = (hi[axis] - o[axis]) * inv;
            if (t1 > t2) {
                const f32 tmp = t1;
                t1 = t2;
                t2 = tmp;
            }
            if (t1 > tmin)
                tmin = t1;
            if (t2 < tmax)
                tmax = t2;
            if (tmin > tmax)
                return {};
        }
    }
    return {tmin, true};
}

}  // namespace psynder::gameplay
