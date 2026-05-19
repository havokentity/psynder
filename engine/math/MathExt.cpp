// SPDX-License-Identifier: MIT
// Psynder — math extensions impl. Lane 02.
//
// Houses the heavier helpers declared in MathExt.h: Mat3 / Mat4 inverse,
// transpose, determinant, normal-matrix, and the full quaternion suite
// (dot, conjugate, rotate, lerp, nlerp, slerp, from/to-Euler).
//
// We accept the f32 precision limits inherent to the engine's coordinate
// budget (ADR-005); none of these functions promote to f64 internally.

#include "MathExt.h"

#include <cmath>

namespace psynder::math {

// ─── Mat3 ────────────────────────────────────────────────────────────────
// Column-major: m[col*3 + row]. The transcription from Mat4 picks the
// top-left 3×3 the same way.

Mat3 mat3_from_mat4(const Mat4& m) noexcept {
    return {{
        m.m[0], m.m[1], m.m[2],
        m.m[4], m.m[5], m.m[6],
        m.m[8], m.m[9], m.m[10],
    }};
}

Mat3 mul(const Mat3& a, const Mat3& b) noexcept {
    Mat3 r{};
    for (i32 c = 0; c < 3; ++c) {
        for (i32 row = 0; row < 3; ++row) {
            f32 s = 0.0f;
            for (i32 k = 0; k < 3; ++k) {
                s += a.m[k*3 + row] * b.m[c*3 + k];
            }
            r.m[c*3 + row] = s;
        }
    }
    return r;
}

Vec3 mul(const Mat3& m, Vec3 v) noexcept {
    return {
        m.m[0]*v.x + m.m[3]*v.y + m.m[6]*v.z,
        m.m[1]*v.x + m.m[4]*v.y + m.m[7]*v.z,
        m.m[2]*v.x + m.m[5]*v.y + m.m[8]*v.z,
    };
}

Mat3 transpose(const Mat3& m) noexcept {
    return {{
        m.m[0], m.m[3], m.m[6],
        m.m[1], m.m[4], m.m[7],
        m.m[2], m.m[5], m.m[8],
    }};
}

f32 determinant(const Mat3& m) noexcept {
    // Cofactor expansion along the first column.
    return  m.m[0] * (m.m[4]*m.m[8] - m.m[5]*m.m[7])
         -  m.m[1] * (m.m[3]*m.m[8] - m.m[5]*m.m[6])
         +  m.m[2] * (m.m[3]*m.m[7] - m.m[4]*m.m[6]);
}

Mat3 inverse(const Mat3& m) noexcept {
    f32 det = determinant(m);
    if (det == 0.0f) return identity3();  // singular — fall back

    f32 inv_det = 1.0f / det;
    Mat3 r{};
    r.m[0] =  (m.m[4]*m.m[8] - m.m[5]*m.m[7]) * inv_det;
    r.m[1] = -(m.m[1]*m.m[8] - m.m[2]*m.m[7]) * inv_det;
    r.m[2] =  (m.m[1]*m.m[5] - m.m[2]*m.m[4]) * inv_det;
    r.m[3] = -(m.m[3]*m.m[8] - m.m[5]*m.m[6]) * inv_det;
    r.m[4] =  (m.m[0]*m.m[8] - m.m[2]*m.m[6]) * inv_det;
    r.m[5] = -(m.m[0]*m.m[5] - m.m[2]*m.m[3]) * inv_det;
    r.m[6] =  (m.m[3]*m.m[7] - m.m[4]*m.m[6]) * inv_det;
    r.m[7] = -(m.m[0]*m.m[7] - m.m[1]*m.m[6]) * inv_det;
    r.m[8] =  (m.m[0]*m.m[4] - m.m[1]*m.m[3]) * inv_det;
    return r;
}

Mat3 normal_matrix(const Mat3& m) noexcept {
    return transpose(inverse(m));
}

// ─── Mat4 ────────────────────────────────────────────────────────────────
Mat4 transpose(const Mat4& m) noexcept {
    Mat4 r{};
    for (i32 row = 0; row < 4; ++row) {
        for (i32 col = 0; col < 4; ++col) {
            r.m[col*4 + row] = m.m[row*4 + col];
        }
    }
    return r;
}

// Cofactor-expansion 4×4 determinant. We accept the O(n!) cost — Mat4
// inverse is not on the hot path; the rasterizer's per-tile transforms
// are computed once per draw, not per pixel.
f32 determinant(const Mat4& m) noexcept {
    // Subdeterminants of the bottom two rows.
    f32 s0 = m.m[0]*m.m[5]  - m.m[1]*m.m[4];
    f32 s1 = m.m[0]*m.m[6]  - m.m[2]*m.m[4];
    f32 s2 = m.m[0]*m.m[7]  - m.m[3]*m.m[4];
    f32 s3 = m.m[1]*m.m[6]  - m.m[2]*m.m[5];
    f32 s4 = m.m[1]*m.m[7]  - m.m[3]*m.m[5];
    f32 s5 = m.m[2]*m.m[7]  - m.m[3]*m.m[6];

    f32 c5 = m.m[10]*m.m[15] - m.m[11]*m.m[14];
    f32 c4 = m.m[9] *m.m[15] - m.m[11]*m.m[13];
    f32 c3 = m.m[9] *m.m[14] - m.m[10]*m.m[13];
    f32 c2 = m.m[8] *m.m[15] - m.m[11]*m.m[12];
    f32 c1 = m.m[8] *m.m[14] - m.m[10]*m.m[12];
    f32 c0 = m.m[8] *m.m[13] - m.m[9] *m.m[12];

    return s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
}

Mat4 inverse(const Mat4& m) noexcept {
    f32 s0 = m.m[0]*m.m[5]  - m.m[1]*m.m[4];
    f32 s1 = m.m[0]*m.m[6]  - m.m[2]*m.m[4];
    f32 s2 = m.m[0]*m.m[7]  - m.m[3]*m.m[4];
    f32 s3 = m.m[1]*m.m[6]  - m.m[2]*m.m[5];
    f32 s4 = m.m[1]*m.m[7]  - m.m[3]*m.m[5];
    f32 s5 = m.m[2]*m.m[7]  - m.m[3]*m.m[6];

    f32 c5 = m.m[10]*m.m[15] - m.m[11]*m.m[14];
    f32 c4 = m.m[9] *m.m[15] - m.m[11]*m.m[13];
    f32 c3 = m.m[9] *m.m[14] - m.m[10]*m.m[13];
    f32 c2 = m.m[8] *m.m[15] - m.m[11]*m.m[12];
    f32 c1 = m.m[8] *m.m[14] - m.m[10]*m.m[12];
    f32 c0 = m.m[8] *m.m[13] - m.m[9] *m.m[12];

    f32 det = s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
    if (det == 0.0f) return identity4();
    f32 inv_det = 1.0f / det;

    Mat4 r{};
    r.m[0]  = ( m.m[5]*c5 - m.m[6]*c4 + m.m[7]*c3) * inv_det;
    r.m[1]  = (-m.m[1]*c5 + m.m[2]*c4 - m.m[3]*c3) * inv_det;
    r.m[2]  = ( m.m[13]*s5 - m.m[14]*s4 + m.m[15]*s3) * inv_det;
    r.m[3]  = (-m.m[9]*s5 + m.m[10]*s4 - m.m[11]*s3) * inv_det;

    r.m[4]  = (-m.m[4]*c5 + m.m[6]*c2 - m.m[7]*c1) * inv_det;
    r.m[5]  = ( m.m[0]*c5 - m.m[2]*c2 + m.m[3]*c1) * inv_det;
    r.m[6]  = (-m.m[12]*s5 + m.m[14]*s2 - m.m[15]*s1) * inv_det;
    r.m[7]  = ( m.m[8]*s5 - m.m[10]*s2 + m.m[11]*s1) * inv_det;

    r.m[8]  = ( m.m[4]*c4 - m.m[5]*c2 + m.m[7]*c0) * inv_det;
    r.m[9]  = (-m.m[0]*c4 + m.m[1]*c2 - m.m[3]*c0) * inv_det;
    r.m[10] = ( m.m[12]*s4 - m.m[13]*s2 + m.m[15]*s0) * inv_det;
    r.m[11] = (-m.m[8]*s4 + m.m[9]*s2 - m.m[11]*s0) * inv_det;

    r.m[12] = (-m.m[4]*c3 + m.m[5]*c1 - m.m[6]*c0) * inv_det;
    r.m[13] = ( m.m[0]*c3 - m.m[1]*c1 + m.m[2]*c0) * inv_det;
    r.m[14] = (-m.m[12]*s3 + m.m[13]*s1 - m.m[14]*s0) * inv_det;
    r.m[15] = ( m.m[8]*s3 - m.m[9]*s1 + m.m[10]*s0) * inv_det;

    return r;
}

Mat4 inverse_affine(const Mat4& m) noexcept {
    // Accept any matrix whose bottom row is [0,0,0,1] and whose upper-left
    // is invertible. For a rigid (orthonormal) upper-left we'd skip the
    // Mat3 inverse and just transpose, but the test costs as much as the
    // inverse itself, so we just always invert the 3×3.
    Mat3 a = inverse(mat3_from_mat4(m));
    Vec3 t = { m.m[12], m.m[13], m.m[14] };
    Vec3 inv_t = mul(a, t);

    Mat4 r{};
    r.m[0]  = a.m[0]; r.m[1]  = a.m[1]; r.m[2]  = a.m[2]; r.m[3]  = 0.0f;
    r.m[4]  = a.m[3]; r.m[5]  = a.m[4]; r.m[6]  = a.m[5]; r.m[7]  = 0.0f;
    r.m[8]  = a.m[6]; r.m[9]  = a.m[7]; r.m[10] = a.m[8]; r.m[11] = 0.0f;
    r.m[12] = -inv_t.x;
    r.m[13] = -inv_t.y;
    r.m[14] = -inv_t.z;
    r.m[15] = 1.0f;
    return r;
}

Mat4 perspective_lh(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept {
    Mat4 r{};
    f32 f = 1.0f / std::tan(fov_y_rad * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (far_z + near_z) / (far_z - near_z);
    r.m[11] = 1.0f;
    r.m[14] = -(2.0f * far_z * near_z) / (far_z - near_z);
    return r;
}

Mat4 look_at_lh(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    Vec3 f = normalize(sub(target, eye));
    Vec3 s = normalize(cross(up, f));
    Vec3 u = cross(f, s);
    Mat4 r = identity4();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    r.m[2] = f.x;  r.m[6] = f.y;  r.m[10] = f.z;
    r.m[12] = -dot(s, eye);
    r.m[13] = -dot(u, eye);
    r.m[14] = -dot(f, eye);
    return r;
}

Mat4 ortho_rh(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z) noexcept {
    Mat4 r{};
    r.m[0]  =  2.0f / (right - left);
    r.m[5]  =  2.0f / (top - bottom);
    r.m[10] = -2.0f / (far_z - near_z);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(far_z + near_z) / (far_z - near_z);
    r.m[15] = 1.0f;
    return r;
}

// ─── Quaternion ──────────────────────────────────────────────────────────
f32 quat_dot(Quat a, Quat b) noexcept {
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}

Quat quat_conjugate(Quat q) noexcept {
    return { -q.x, -q.y, -q.z, q.w };
}

Vec3 quat_rotate(Quat q, Vec3 v) noexcept {
    // Rodrigues-style: t = 2 * cross(q.xyz, v); v' = v + q.w * t + cross(q.xyz, t).
    // Two crosses, no quat multiply, no Mat3 — cheaper than the matrix path.
    Vec3 qv = { q.x, q.y, q.z };
    Vec3 t  = mul(cross(qv, v), 2.0f);
    return add(add(v, mul(t, q.w)), cross(qv, t));
}

Quat quat_lerp(Quat a, Quat b, f32 t) noexcept {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t,
    };
}

Quat quat_nlerp(Quat a, Quat b, f32 t) noexcept {
    // Short-arc fix: flip b if it's on the opposite hemisphere from a.
    Quat bb = quat_dot(a, b) < 0.0f
        ? Quat{ -b.x, -b.y, -b.z, -b.w }
        : b;
    return quat_normalize(quat_lerp(a, bb, t));
}

Quat quat_slerp(Quat a, Quat b, f32 t) noexcept {
    f32 cos_theta = quat_dot(a, b);

    // Short-arc: flip b if cos is negative.
    Quat bb = b;
    if (cos_theta < 0.0f) {
        bb = { -b.x, -b.y, -b.z, -b.w };
        cos_theta = -cos_theta;
    }

    // When the inputs are nearly parallel (cos ≈ 1) we fall back to nlerp —
    // the sin(θ) denominator otherwise rockets to ∞. 0.9995 is the
    // threshold most game-math libs use (RTR4 §5.3).
    if (cos_theta > 0.9995f) {
        return quat_nlerp(a, bb, t);
    }

    f32 theta     = std::acos(cos_theta);
    f32 sin_theta = std::sin(theta);
    f32 inv_sin   = 1.0f / sin_theta;
    f32 wa        = std::sin((1.0f - t) * theta) * inv_sin;
    f32 wb        = std::sin(t * theta) * inv_sin;

    return {
        a.x * wa + bb.x * wb,
        a.y * wa + bb.y * wb,
        a.z * wa + bb.z * wb,
        a.w * wa + bb.w * wb,
    };
}

Quat quat_from_euler(f32 roll_rad, f32 pitch_rad, f32 yaw_rad) noexcept {
    // ZYX intrinsic: q = qz * qy * qx. Standard FPS convention — yaw turns
    // the camera left/right, pitch tilts it up/down, roll banks it.
    f32 cr = std::cos(roll_rad  * 0.5f);
    f32 sr = std::sin(roll_rad  * 0.5f);
    f32 cp = std::cos(pitch_rad * 0.5f);
    f32 sp = std::sin(pitch_rad * 0.5f);
    f32 cy = std::cos(yaw_rad   * 0.5f);
    f32 sy = std::sin(yaw_rad   * 0.5f);

    return {
        sr*cp*cy - cr*sp*sy,  // x
        cr*sp*cy + sr*cp*sy,  // y
        cr*cp*sy - sr*sp*cy,  // z
        cr*cp*cy + sr*sp*sy,  // w
    };
}

Vec3 quat_to_euler(Quat q) noexcept {
    // Inverse of quat_from_euler. Standard atan2/asin extraction; we clamp
    // the asin argument to [-1, 1] to absorb any floating-point overshoot
    // (otherwise NaN at gimbal lock).
    f32 sinp = 2.0f * (q.w * q.y - q.z * q.x);
    if (sinp >  1.0f) sinp =  1.0f;
    if (sinp < -1.0f) sinp = -1.0f;

    f32 roll  = std::atan2(2.0f * (q.w*q.x + q.y*q.z),
                           1.0f - 2.0f * (q.x*q.x + q.y*q.y));
    f32 pitch = std::asin(sinp);
    f32 yaw   = std::atan2(2.0f * (q.w*q.z + q.x*q.y),
                           1.0f - 2.0f * (q.y*q.y + q.z*q.z));

    return { roll, pitch, yaw };
}

}  // namespace psynder::math
