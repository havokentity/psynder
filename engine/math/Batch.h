// SPDX-License-Identifier: MIT
// Psynder — batched point-transform helpers. Lane 02, Wave B.
//
// Lane 07 (raster) needs to transform arrays of vertex positions by a
// single Mat4 per mesh, every frame. `Math.h::mul(const Mat4&, Vec4)` is
// the scalar primitive but it's awkward to call in a tight loop because
// of the Vec3↔Vec4 packing. This header exposes the array form the
// renderer actually wants.
//
// Wave B ships the scalar 4-component homogeneous transform — readable,
// correct, no SIMD intrinsics required so it builds on every host we
// care about. Wave C can vectorize (8-wide AVX2 / 4-wide NEON) behind
// the same signature; the contract here is "matches the scalar
// `mul(m, Vec4{in[i], 1}).xyz` exactly", so a vectorized replacement
// drops in transparently.
//
// Header-only by design — the body is short and consumers want it inlined
// inside their per-frame loops.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

namespace psynder::math {

#if defined(_MSC_VER)
#define PSY_MATH_RESTRICT __restrict
#elif defined(__clang__) || defined(__GNUC__)
#define PSY_MATH_RESTRICT __restrict__
#else
#define PSY_MATH_RESTRICT
#endif

namespace detail {

struct Mat4LinearRows {
    f32 m00, m01, m02;
    f32 m10, m11, m12;
    f32 m20, m21, m22;
};

PSY_FORCEINLINE Mat4LinearRows linear_rows(const Mat4& m) noexcept {
    return Mat4LinearRows{
        m.m[0],
        m.m[4],
        m.m[8],
        m.m[1],
        m.m[5],
        m.m[9],
        m.m[2],
        m.m[6],
        m.m[10],
    };
}

PSY_FORCEINLINE Vec3 transform_point_one(const Mat4LinearRows& r, f32 tx, f32 ty, f32 tz, Vec3 v) noexcept {
    return Vec3{
        r.m00 * v.x + r.m01 * v.y + r.m02 * v.z + tx,
        r.m10 * v.x + r.m11 * v.y + r.m12 * v.z + ty,
        r.m20 * v.x + r.m21 * v.y + r.m22 * v.z + tz,
    };
}

PSY_FORCEINLINE Vec3 transform_dir_one(const Mat4LinearRows& r, Vec3 v) noexcept {
    return Vec3{
        r.m00 * v.x + r.m01 * v.y + r.m02 * v.z,
        r.m10 * v.x + r.m11 * v.y + r.m12 * v.z,
        r.m20 * v.x + r.m21 * v.y + r.m22 * v.z,
    };
}

}  // namespace detail

// Transform `n` points by `m`, treating each input as homogeneous w = 1
// (translation column applies). Writes the transformed points to `out`.
//
// Numerical contract: `out[i]` matches `mul(m, Vec4{in[i].x, in[i].y,
// in[i].z, 1.0f}).xyz` exactly — the loop body computes the same scalar
// expression in the same evaluation order.
//
// `in` and `out` may not alias. Passing `n == 0` is a no-op.
PSY_FORCEINLINE void transform_points(const Mat4& m, const Vec3* in, Vec3* out, usize n) noexcept {
    const auto r = detail::linear_rows(m);
    const f32 tx = m.m[12];
    const f32 ty = m.m[13];
    const f32 tz = m.m[14];
    const Vec3* PSY_MATH_RESTRICT src = in;
    Vec3* PSY_MATH_RESTRICT dst = out;

    usize i = 0;
    for (; i + 4 <= n; i += 4) {
        const Vec3 p0 = detail::transform_point_one(r, tx, ty, tz, src[i + 0]);
        const Vec3 p1 = detail::transform_point_one(r, tx, ty, tz, src[i + 1]);
        const Vec3 p2 = detail::transform_point_one(r, tx, ty, tz, src[i + 2]);
        const Vec3 p3 = detail::transform_point_one(r, tx, ty, tz, src[i + 3]);
        dst[i + 0] = p0;
        dst[i + 1] = p1;
        dst[i + 2] = p2;
        dst[i + 3] = p3;
    }
    for (; i < n; ++i) {
        dst[i] = detail::transform_point_one(r, tx, ty, tz, src[i]);
    }
}

// Direction-vector flavour: treats each input as homogeneous w = 0, so
// the translation column does *not* contribute. Use this for normals
// and tangents. Caller is responsible for any subsequent normalization.
PSY_FORCEINLINE void transform_dirs(const Mat4& m, const Vec3* in, Vec3* out, usize n) noexcept {
    const auto r = detail::linear_rows(m);
    const Vec3* PSY_MATH_RESTRICT src = in;
    Vec3* PSY_MATH_RESTRICT dst = out;

    usize i = 0;
    for (; i + 4 <= n; i += 4) {
        const Vec3 d0 = detail::transform_dir_one(r, src[i + 0]);
        const Vec3 d1 = detail::transform_dir_one(r, src[i + 1]);
        const Vec3 d2 = detail::transform_dir_one(r, src[i + 2]);
        const Vec3 d3 = detail::transform_dir_one(r, src[i + 3]);
        dst[i + 0] = d0;
        dst[i + 1] = d1;
        dst[i + 2] = d2;
        dst[i + 3] = d3;
    }
    for (; i < n; ++i) {
        dst[i] = detail::transform_dir_one(r, src[i]);
    }
}

#undef PSY_MATH_RESTRICT

}  // namespace psynder::math
