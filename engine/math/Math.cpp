// SPDX-License-Identifier: MIT
// Psynder — math impl. Lane 02 fleshes out heavy ops; this is the Phase-0
// scaffold sufficient for the M0/M1 sample binaries to link.

#include "Math.h"

#include <cmath>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace psynder::math {
namespace {

PSY_FORCEINLINE void mul_affine_column(const Mat4& a,
                                       const f32* b_col,
                                       f32* out_col,
                                       bool translation) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    const float32x4_t c0 = vld1q_f32(a.m + 0);
    const float32x4_t c1 = vld1q_f32(a.m + 4);
    const float32x4_t c2 = vld1q_f32(a.m + 8);
    const float32x4_t c3 = vld1q_f32(a.m + 12);
    float32x4_t r = vmulq_n_f32(c0, b_col[0]);
    r = vfmaq_n_f32(r, c1, b_col[1]);
    r = vfmaq_n_f32(r, c2, b_col[2]);
    if (translation)
        r = vaddq_f32(r, c3);
    vst1q_f32(out_col, r);
#elif defined(__x86_64__) || defined(_M_X64)
    const __m128 c0 = _mm_loadu_ps(a.m + 0);
    const __m128 c1 = _mm_loadu_ps(a.m + 4);
    const __m128 c2 = _mm_loadu_ps(a.m + 8);
    const __m128 c3 = _mm_loadu_ps(a.m + 12);
    __m128 r = _mm_mul_ps(c0, _mm_set1_ps(b_col[0]));
#if defined(__FMA__)
    r = _mm_fmadd_ps(c1, _mm_set1_ps(b_col[1]), r);
    r = _mm_fmadd_ps(c2, _mm_set1_ps(b_col[2]), r);
#else
    r = _mm_add_ps(r, _mm_mul_ps(c1, _mm_set1_ps(b_col[1])));
    r = _mm_add_ps(r, _mm_mul_ps(c2, _mm_set1_ps(b_col[2])));
#endif
    if (translation)
        r = _mm_add_ps(r, c3);
    _mm_storeu_ps(out_col, r);
#else
    out_col[0] = a.m[0] * b_col[0] + a.m[4] * b_col[1] + a.m[8] * b_col[2] +
                 (translation ? a.m[12] : 0.0f);
    out_col[1] = a.m[1] * b_col[0] + a.m[5] * b_col[1] + a.m[9] * b_col[2] +
                 (translation ? a.m[13] : 0.0f);
    out_col[2] = a.m[2] * b_col[0] + a.m[6] * b_col[1] + a.m[10] * b_col[2] +
                 (translation ? a.m[14] : 0.0f);
    out_col[3] = translation ? 1.0f : 0.0f;
#endif
}

}  // namespace

Mat4 identity4() {
    return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
}

Mat4 perspective_rh(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) {
    Mat4 r{};
    f32 f = 1.0f / std::tan(fov_y_rad * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (far_z + near_z) / (near_z - far_z);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    return r;
}

Mat4 look_at_rh(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(sub(target, eye));
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r = identity4();
    r.m[0] = s.x;
    r.m[4] = s.y;
    r.m[8] = s.z;
    r.m[1] = u.x;
    r.m[5] = u.y;
    r.m[9] = u.z;
    r.m[2] = -f.x;
    r.m[6] = -f.y;
    r.m[10] = -f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = dot(f, eye);
    return r;
}

Mat4 translate(Vec3 t) {
    Mat4 r = identity4();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

Mat4 scale(Vec3 s) {
    Mat4 r{};
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    r.m[15] = 1.0f;
    return r;
}

Mat4 rotate_quat(Quat q) {
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 r{};
    r.m[0] = 1 - 2 * (yy + zz);
    r.m[1] = 2 * (xy + wz);
    r.m[2] = 2 * (xz - wy);
    r.m[4] = 2 * (xy - wz);
    r.m[5] = 1 - 2 * (xx + zz);
    r.m[6] = 2 * (yz + wx);
    r.m[8] = 2 * (xz + wy);
    r.m[9] = 2 * (yz - wx);
    r.m[10] = 1 - 2 * (xx + yy);
    r.m[15] = 1.0f;
    return r;
}

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};

    const f32 a00 = a.m[0], a01 = a.m[4], a02 = a.m[8], a03 = a.m[12];
    const f32 a10 = a.m[1], a11 = a.m[5], a12 = a.m[9], a13 = a.m[13];
    const f32 a20 = a.m[2], a21 = a.m[6], a22 = a.m[10], a23 = a.m[14];
    const f32 a30 = a.m[3], a31 = a.m[7], a32 = a.m[11], a33 = a.m[15];

    for (int c = 0; c < 4; ++c) {
        const f32 b0 = b.m[c * 4 + 0];
        const f32 b1 = b.m[c * 4 + 1];
        const f32 b2 = b.m[c * 4 + 2];
        const f32 b3 = b.m[c * 4 + 3];

        r.m[c * 4 + 0] = a00 * b0 + a01 * b1 + a02 * b2 + a03 * b3;
        r.m[c * 4 + 1] = a10 * b0 + a11 * b1 + a12 * b2 + a13 * b3;
        r.m[c * 4 + 2] = a20 * b0 + a21 * b1 + a22 * b2 + a23 * b3;
        r.m[c * 4 + 3] = a30 * b0 + a31 * b1 + a32 * b2 + a33 * b3;
    }
    return r;
}

Mat4 mul_affine(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r{};
    mul_affine_column(a, b.m + 0, r.m + 0, false);
    mul_affine_column(a, b.m + 4, r.m + 4, false);
    mul_affine_column(a, b.m + 8, r.m + 8, false);
    mul_affine_column(a, b.m + 12, r.m + 12, true);
    r.m[3] = 0.0f;
    r.m[7] = 0.0f;
    r.m[11] = 0.0f;
    r.m[15] = 1.0f;
    return r;
}

void mul_affine_batch(const Mat4* parents, const Mat4* locals, Mat4* out, usize count) noexcept {
    if (!parents || !locals || !out)
        return;
    for (usize i = 0; i < count; ++i)
        out[i] = mul_affine(parents[i], locals[i]);
}

Vec4 mul(const Mat4& m, Vec4 v) {
    const f32 x = v.x;
    const f32 y = v.y;
    const f32 z = v.z;
    const f32 w = v.w;
    return {
        m.m[0] * x + m.m[4] * y + m.m[8] * z + m.m[12] * w,
        m.m[1] * x + m.m[5] * y + m.m[9] * z + m.m[13] * w,
        m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14] * w,
        m.m[3] * x + m.m[7] * y + m.m[11] * z + m.m[15] * w,
    };
}

Quat quat_from_axis_angle(Vec3 axis, f32 angle_rad) {
    f32 h = angle_rad * 0.5f;
    f32 s = std::sin(h);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}

Quat quat_mul(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat quat_normalize(Quat q) {
    f32 l2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (l2 <= 0)
        return {0, 0, 0, 1};
    f32 inv = 1.0f / std::sqrt(l2);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

}  // namespace psynder::math
