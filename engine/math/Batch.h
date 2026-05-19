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

// Transform `n` points by `m`, treating each input as homogeneous w = 1
// (translation column applies). Writes the transformed points to `out`.
//
// Numerical contract: `out[i]` matches `mul(m, Vec4{in[i].x, in[i].y,
// in[i].z, 1.0f}).xyz` exactly — the loop body computes the same scalar
// expression in the same evaluation order.
//
// `in` and `out` may not alias. Passing `n == 0` is a no-op.
inline void transform_points(const Mat4& m,
                             const Vec3* in,
                             Vec3* out,
                             usize n) noexcept {
    // Hoist matrix columns into locals so the compiler can keep them in
    // registers across the loop. Mat4 is column-major: m.m[col*4 + row].
    const f32 m00 = m.m[0],  m10 = m.m[1],  m20 = m.m[2];
    const f32 m01 = m.m[4],  m11 = m.m[5],  m21 = m.m[6];
    const f32 m02 = m.m[8],  m12 = m.m[9],  m22 = m.m[10];
    const f32 m03 = m.m[12], m13 = m.m[13], m23 = m.m[14];

    for (usize i = 0; i < n; ++i) {
        f32 x = in[i].x;
        f32 y = in[i].y;
        f32 z = in[i].z;
        out[i].x = m00 * x + m01 * y + m02 * z + m03;
        out[i].y = m10 * x + m11 * y + m12 * z + m13;
        out[i].z = m20 * x + m21 * y + m22 * z + m23;
    }
}

// Direction-vector flavour: treats each input as homogeneous w = 0, so
// the translation column does *not* contribute. Use this for normals
// and tangents. Caller is responsible for any subsequent normalization.
inline void transform_dirs(const Mat4& m,
                           const Vec3* in,
                           Vec3* out,
                           usize n) noexcept {
    const f32 m00 = m.m[0],  m10 = m.m[1],  m20 = m.m[2];
    const f32 m01 = m.m[4],  m11 = m.m[5],  m21 = m.m[6];
    const f32 m02 = m.m[8],  m12 = m.m[9],  m22 = m.m[10];

    for (usize i = 0; i < n; ++i) {
        f32 x = in[i].x;
        f32 y = in[i].y;
        f32 z = in[i].z;
        out[i].x = m00 * x + m01 * y + m02 * z;
        out[i].y = m10 * x + m11 * y + m12 * z;
        out[i].z = m20 * x + m21 * y + m22 * z;
    }
}

}  // namespace psynder::math
