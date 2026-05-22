// SPDX-License-Identifier: MIT
// Psynder — math impl. Lane 02 fleshes out heavy ops; this is the Phase-0
// scaffold sufficient for the M0/M1 sample binaries to link.

#include "Math.h"

#include <cmath>

namespace psynder::math {

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
