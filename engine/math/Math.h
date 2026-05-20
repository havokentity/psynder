// SPDX-License-Identifier: MIT
// Psynder — vec / mat / quat / transform public API. Lane 02 owns.
// Conventions: right-handed, column-major matrices, radians, metric units.
// 1 world unit = 1 metre (DESIGN.md §10.1 and the user's broader preference).

#pragma once

#include "core/Types.h"

#include <cmath>

namespace psynder::math {

// ─── Vectors ─────────────────────────────────────────────────────────────
struct Vec2 {
    f32 x, y;
};
struct Vec3 {
    f32 x, y, z;
};
struct Vec4 {
    f32 x, y, z, w;
};

struct IVec2 {
    i32 x, y;
};
struct IVec3 {
    i32 x, y, z;
};

// ─── Matrices ────────────────────────────────────────────────────────────
struct Mat3 {
    f32 m[9];
};
struct Mat4 {
    f32 m[16];
};

struct Quat {
    f32 x, y, z, w;
};

// ─── AABB / sphere ───────────────────────────────────────────────────────
struct Aabb {
    Vec3 min{0, 0, 0};
    Vec3 max{0, 0, 0};
};
struct Sphere {
    Vec3 center{0, 0, 0};
    f32 radius = 0.0f;
};

// ─── Free-function ops (header-inline for the hot ones) ──────────────────
constexpr Vec3 add(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 sub(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 mul(Vec3 a, f32 s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
}
constexpr f32 dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline f32 length(Vec3 v) noexcept {
    return std::sqrt(dot(v, v));
}
inline Vec3 normalize(Vec3 v) noexcept {
    f32 l = length(v);
    return l > 0.0f ? mul(v, 1.0f / l) : v;
}

// ─── Mat4 constructors (lane 02 implements the heavier ones) ─────────────
Mat4 identity4();
Mat4 perspective_rh(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z);
Mat4 look_at_rh(Vec3 eye, Vec3 target, Vec3 up);
Mat4 translate(Vec3 t);
Mat4 rotate_quat(Quat q);
Mat4 scale(Vec3 s);
Mat4 mul(const Mat4& a, const Mat4& b);
Vec4 mul(const Mat4& m, Vec4 v);

// ─── Quaternion helpers ──────────────────────────────────────────────────
Quat quat_from_axis_angle(Vec3 axis, f32 angle_rad);
Quat quat_mul(Quat a, Quat b);
Quat quat_normalize(Quat q);

// ─── Constants ───────────────────────────────────────────────────────────
inline constexpr f32 kPi = 3.14159265358979323846f;
inline constexpr f32 kTwoPi = 2.0f * kPi;
inline constexpr f32 kHalfPi = 0.5f * kPi;
inline constexpr f32 kDegToRad = kPi / 180.0f;
inline constexpr f32 kRadToDeg = 180.0f / kPi;

}  // namespace psynder::math
