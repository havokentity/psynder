// SPDX-License-Identifier: MIT
// Psynder — batched point-transform helpers. Lane 02, Wave B.
//
// Lane 07 (raster) needs to transform arrays of vertex positions by a
// single Mat4 per mesh, every frame. `Math.h::mul(const Mat4&, Vec4)` is
// the scalar primitive but it's awkward to call in a tight loop because
// of the Vec3↔Vec4 packing. This header exposes the array form the
// renderer actually wants.
//
// The hot loop uses a 4-wide SIMD block where the platform gives us one
// cheaply, then falls through to the same scalar tail. The contract here is
// "matches `mul(m, Vec4{in[i], 1}).xyz`", so callers never care which backend
// ran.
//
// Header-only by design — the body is short and consumers want it inlined
// inside their per-frame loops.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace psynder::math {

#if defined(_MSC_VER)
#define PSY_MATH_RESTRICT __restrict
#elif defined(__clang__) || defined(__GNUC__)
#define PSY_MATH_RESTRICT __restrict__
#else
#define PSY_MATH_RESTRICT
#endif

namespace detail {

static_assert(sizeof(Vec3) == sizeof(f32) * 3u, "Vec3 batch SIMD assumes tightly packed xyz");

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

PSY_FORCEINLINE void transform_point_block4(const Mat4LinearRows& r,
                                            f32 tx,
                                            f32 ty,
                                            f32 tz,
                                            const Vec3* PSY_MATH_RESTRICT src,
                                            Vec3* PSY_MATH_RESTRICT dst) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    const float32x4x3_t v = vld3q_f32(reinterpret_cast<const f32*>(src));

    float32x4_t x = vfmaq_n_f32(vmulq_n_f32(v.val[0], r.m00), v.val[1], r.m01);
    x = vaddq_f32(vfmaq_n_f32(x, v.val[2], r.m02), vdupq_n_f32(tx));

    float32x4_t y = vfmaq_n_f32(vmulq_n_f32(v.val[0], r.m10), v.val[1], r.m11);
    y = vaddq_f32(vfmaq_n_f32(y, v.val[2], r.m12), vdupq_n_f32(ty));

    float32x4_t z = vfmaq_n_f32(vmulq_n_f32(v.val[0], r.m20), v.val[1], r.m21);
    z = vaddq_f32(vfmaq_n_f32(z, v.val[2], r.m22), vdupq_n_f32(tz));

    const float32x4x3_t out{{x, y, z}};
    vst3q_f32(reinterpret_cast<f32*>(dst), out);
#elif defined(__x86_64__) || defined(_M_X64)
    const __m128 x = _mm_setr_ps(src[0].x, src[1].x, src[2].x, src[3].x);
    const __m128 y = _mm_setr_ps(src[0].y, src[1].y, src[2].y, src[3].y);
    const __m128 z = _mm_setr_ps(src[0].z, src[1].z, src[2].z, src[3].z);

    __m128 ox = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(r.m00)), _mm_mul_ps(y, _mm_set1_ps(r.m01)));
    ox = _mm_add_ps(_mm_add_ps(ox, _mm_mul_ps(z, _mm_set1_ps(r.m02))), _mm_set1_ps(tx));

    __m128 oy = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(r.m10)), _mm_mul_ps(y, _mm_set1_ps(r.m11)));
    oy = _mm_add_ps(_mm_add_ps(oy, _mm_mul_ps(z, _mm_set1_ps(r.m12))), _mm_set1_ps(ty));

    __m128 oz = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(r.m20)), _mm_mul_ps(y, _mm_set1_ps(r.m21)));
    oz = _mm_add_ps(_mm_add_ps(oz, _mm_mul_ps(z, _mm_set1_ps(r.m22))), _mm_set1_ps(tz));

    alignas(16) f32 xs[4];
    alignas(16) f32 ys[4];
    alignas(16) f32 zs[4];
    _mm_store_ps(xs, ox);
    _mm_store_ps(ys, oy);
    _mm_store_ps(zs, oz);
    for (int lane = 0; lane < 4; ++lane)
        dst[lane] = Vec3{xs[lane], ys[lane], zs[lane]};
#else
    for (int lane = 0; lane < 4; ++lane)
        dst[lane] = transform_point_one(r, tx, ty, tz, src[lane]);
#endif
}

PSY_FORCEINLINE void transform_dir_block4(const Mat4LinearRows& r,
                                          const Vec3* PSY_MATH_RESTRICT src,
                                          Vec3* PSY_MATH_RESTRICT dst) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    const float32x4x3_t v = vld3q_f32(reinterpret_cast<const f32*>(src));

    float32x4_t x = vfmaq_n_f32(vmulq_n_f32(v.val[0], r.m00), v.val[1], r.m01);
    x = vfmaq_n_f32(x, v.val[2], r.m02);

    float32x4_t y = vfmaq_n_f32(vmulq_n_f32(v.val[0], r.m10), v.val[1], r.m11);
    y = vfmaq_n_f32(y, v.val[2], r.m12);

    float32x4_t z = vfmaq_n_f32(vmulq_n_f32(v.val[0], r.m20), v.val[1], r.m21);
    z = vfmaq_n_f32(z, v.val[2], r.m22);

    const float32x4x3_t out{{x, y, z}};
    vst3q_f32(reinterpret_cast<f32*>(dst), out);
#elif defined(__x86_64__) || defined(_M_X64)
    const __m128 x = _mm_setr_ps(src[0].x, src[1].x, src[2].x, src[3].x);
    const __m128 y = _mm_setr_ps(src[0].y, src[1].y, src[2].y, src[3].y);
    const __m128 z = _mm_setr_ps(src[0].z, src[1].z, src[2].z, src[3].z);

    __m128 ox = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(r.m00)), _mm_mul_ps(y, _mm_set1_ps(r.m01)));
    ox = _mm_add_ps(ox, _mm_mul_ps(z, _mm_set1_ps(r.m02)));

    __m128 oy = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(r.m10)), _mm_mul_ps(y, _mm_set1_ps(r.m11)));
    oy = _mm_add_ps(oy, _mm_mul_ps(z, _mm_set1_ps(r.m12)));

    __m128 oz = _mm_add_ps(_mm_mul_ps(x, _mm_set1_ps(r.m20)), _mm_mul_ps(y, _mm_set1_ps(r.m21)));
    oz = _mm_add_ps(oz, _mm_mul_ps(z, _mm_set1_ps(r.m22)));

    alignas(16) f32 xs[4];
    alignas(16) f32 ys[4];
    alignas(16) f32 zs[4];
    _mm_store_ps(xs, ox);
    _mm_store_ps(ys, oy);
    _mm_store_ps(zs, oz);
    for (int lane = 0; lane < 4; ++lane)
        dst[lane] = Vec3{xs[lane], ys[lane], zs[lane]};
#else
    for (int lane = 0; lane < 4; ++lane)
        dst[lane] = transform_dir_one(r, src[lane]);
#endif
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
        detail::transform_point_block4(r, tx, ty, tz, src + i, dst + i);
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
        detail::transform_dir_block4(r, src + i, dst + i);
    }
    for (; i < n; ++i) {
        dst[i] = detail::transform_dir_one(r, src[i]);
    }
}

#undef PSY_MATH_RESTRICT

}  // namespace psynder::math
