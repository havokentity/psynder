// SPDX-License-Identifier: MIT
// Psynder physics — shape primitives (DESIGN.md §10.1).
// Wave A covers sphere / capsule / box / convex hull; compound / heightfield
// / triangle-mesh appear in the public enum but resolve to the same kernels
// as their bounding primitives until lane-13 Wave B fleshes them out.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::physics::detail {

// All shapes are stored in body-local space; the body transform maps them
// into world space. half_extent layout per shape:
//   Sphere       : half_extent.x = radius
//   Capsule      : x = radius, y = half-height of cylindrical section
//   Box          : x/y/z = half extents
//   ConvexHull   : x = bounding radius (verts stored in a side table)
// World-space AABB factors in the conservative bound = bounding-radius
// expansion for non-axis-aligned shapes.

PSY_FORCEINLINE math::Vec3 quat_rotate(math::Quat q, math::Vec3 v) noexcept {
    // q * v * q^-1 — standard Rodrigues path. Compiles to ~14 fma on AVX2.
    math::Vec3 u{q.x, q.y, q.z};
    f32 s = q.w;
    math::Vec3 t = math::mul(math::cross(u, v), 2.0f);
    return math::add(math::add(v, math::mul(t, s)), math::cross(u, t));
}

// Conjugate (== inverse for a unit quaternion). Physics-internal so callers
// qualify `detail::quat_conjugate` and never trip the math/physics ADL
// ambiguity the qualified quat_rotate path documents above.
PSY_FORCEINLINE math::Quat quat_conjugate(math::Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

// Apply a body's inverse-inertia tensor to a WORLD-space angular quantity
// (torque or angular impulse). Body inertia is a diagonal in the body's LOCAL
// principal frame; the world tensor is R * I_local^-1 * R^T. We never form the
// 3x3 — instead rotate `v` into local with R^T, scale by the diagonal, rotate
// the result back to world with R:
//     out_world = R * ( inv_local (.) ( R^T * v_world ) )
// Exact for a rotated asymmetric body (produces physical precession), and
// reduces to the old `inv_local (.) v` when rotation is identity. Alloc-free
// and -fno-fast-math friendly (no reciprocals beyond the precomputed diagonal).
PSY_FORCEINLINE math::Vec3 apply_inv_inertia_world(math::Quat rotation,
                                                   math::Vec3 inv_local,
                                                   math::Vec3 v_world) noexcept {
    const math::Vec3 v_local = quat_rotate(quat_conjugate(rotation), v_world);
    const math::Vec3 a_local{v_local.x * inv_local.x,
                             v_local.y * inv_local.y,
                             v_local.z * inv_local.z};
    return quat_rotate(rotation, a_local);
}

inline math::Aabb aabb_world(u8 shape,
                             math::Vec3 half_extent,
                             math::Vec3 position,
                             math::Quat rotation) noexcept {
    // Conservative: use bounding-sphere radius for everything except the
    // axis-aligned box's identity-rotation case. The broadphase deals in
    // AABBs anyway; an extra-fat AABB just costs a few false-positive
    // narrowphase pairs.
    f32 r;
    switch (shape) {
        case 0:
            r = half_extent.x;
            break;  // Sphere
        case 1:
            r = half_extent.x + half_extent.y;
            break;  // Capsule
        case 2: {   // Box
            math::Vec3 ex = half_extent;
            // Qualified to suppress ADL: a TU that also includes
            // math/MathExt.h would otherwise see math::quat_rotate via
            // argument-dependent lookup and the call would be ambiguous.
            math::Vec3 ax = detail::quat_rotate(rotation, {ex.x, 0, 0});
            math::Vec3 ay = detail::quat_rotate(rotation, {0, ex.y, 0});
            math::Vec3 az = detail::quat_rotate(rotation, {0, 0, ex.z});
            math::Vec3 e{std::fabs(ax.x) + std::fabs(ay.x) + std::fabs(az.x),
                         std::fabs(ax.y) + std::fabs(ay.y) + std::fabs(az.y),
                         std::fabs(ax.z) + std::fabs(ay.z) + std::fabs(az.z)};
            return {math::sub(position, e), math::add(position, e)};
        }
        default:
            r = half_extent.x;
            break;  // hull/etc.
    }
    return {{position.x - r, position.y - r, position.z - r},
            {position.x + r, position.y + r, position.z + r}};
}

// Inertia tensors (uniform density). Returns diagonal entries.
inline math::Vec3 inertia_sphere(f32 mass, f32 radius) noexcept {
    f32 v = (2.0f / 5.0f) * mass * radius * radius;
    return {v, v, v};
}

inline math::Vec3 inertia_box(f32 mass, math::Vec3 half_extent) noexcept {
    f32 x = half_extent.x * 2.0f;
    f32 y = half_extent.y * 2.0f;
    f32 z = half_extent.z * 2.0f;
    f32 c = mass * (1.0f / 12.0f);
    return {c * (y * y + z * z), c * (x * x + z * z), c * (x * x + y * y)};
}

inline math::Vec3 inertia_capsule(f32 mass, f32 radius, f32 half_h) noexcept {
    // Cylinder + two hemispheres approximated as a single capsule formula.
    f32 r2 = radius * radius;
    f32 h = half_h * 2.0f;
    f32 c = mass;
    f32 ix = c * (h * h * (1.0f / 12.0f) + r2 * 0.25f) + c * 0.4f * r2;
    f32 iy = c * 0.5f * r2 + c * 0.4f * r2;
    return {ix, iy, ix};
}

}  // namespace psynder::physics::detail
