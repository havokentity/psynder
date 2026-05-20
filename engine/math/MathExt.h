// SPDX-License-Identifier: MIT
// Psynder — math extensions for Lane 02.
//
// The frozen public surface lives in Math.h. The orchestrator capped that
// header at Phase 0; everything Wave A adds — Mat3, Mat4 inverse / transpose,
// the full quaternion suite (slerp, lerp, from-Euler, to-Euler), and a few
// loose helpers (Vec2/Vec4 ops, Vec3 reflect, lerp/clamp) — lives in this
// internal extension header instead. Lanes that need richer math include
// "math/MathExt.h" after Math.h. See wave-a-bar.md §1 (header contract) and
// the lane-02 Issue body.
//
// Conventions match Math.h:
//   - right-handed, column-major matrices
//   - radians, metric units (1 world unit = 1 metre, DESIGN.md §10.1)
//   - Quat layout {x,y,z,w} (vector part first, scalar last)

#pragma once

#include "Math.h"

#include <cmath>

namespace psynder::math {

// ─── Vec2 / Vec4 free-function ops ───────────────────────────────────────
constexpr Vec2 add(Vec2 a, Vec2 b) noexcept {
    return {a.x + b.x, a.y + b.y};
}
constexpr Vec2 sub(Vec2 a, Vec2 b) noexcept {
    return {a.x - b.x, a.y - b.y};
}
constexpr Vec2 mul(Vec2 a, f32 s) noexcept {
    return {a.x * s, a.y * s};
}
constexpr f32 dot(Vec2 a, Vec2 b) noexcept {
    return a.x * b.x + a.y * b.y;
}

constexpr Vec4 add(Vec4 a, Vec4 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
constexpr Vec4 sub(Vec4 a, Vec4 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
constexpr Vec4 mul(Vec4 a, f32 s) noexcept {
    return {a.x * s, a.y * s, a.z * s, a.w * s};
}
constexpr f32 dot(Vec4 a, Vec4 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

inline f32 length(Vec2 v) noexcept {
    return std::sqrt(dot(v, v));
}
inline Vec2 normalize(Vec2 v) noexcept {
    f32 l = length(v);
    return l > 0.0f ? mul(v, 1.0f / l) : v;
}

inline f32 length(Vec4 v) noexcept {
    return std::sqrt(dot(v, v));
}
inline Vec4 normalize(Vec4 v) noexcept {
    f32 l = length(v);
    return l > 0.0f ? mul(v, 1.0f / l) : v;
}

// ─── Generic helpers ─────────────────────────────────────────────────────
template <class T>
constexpr T clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr f32 lerp(f32 a, f32 b, f32 t) noexcept {
    return a + (b - a) * t;
}
constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) noexcept {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)};
}

constexpr Vec3 reflect(Vec3 v, Vec3 n) noexcept {
    return sub(v, mul(n, 2.0f * dot(v, n)));
}

inline f32 distance(Vec3 a, Vec3 b) noexcept {
    return length(sub(a, b));
}
constexpr f32 distance2(Vec3 a, Vec3 b) noexcept {
    Vec3 d = sub(a, b);
    return dot(d, d);
}

// ─── Mat3 (column-major) ─────────────────────────────────────────────────
// Storage is the same column-major scheme as Mat4: m[col*3 + row]. Mat3 is
// the 3×3 linear part used for normal transforms and bone bases.

inline Mat3 identity3() noexcept {
    return {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
}

Mat3 mat3_from_mat4(const Mat4& m) noexcept;
Mat3 mul(const Mat3& a, const Mat3& b) noexcept;
Vec3 mul(const Mat3& m, Vec3 v) noexcept;
Mat3 transpose(const Mat3& m) noexcept;
f32 determinant(const Mat3& m) noexcept;
Mat3 inverse(const Mat3& m) noexcept;

// The classic "normal matrix" — `transpose(inverse(mat3))`. Free function so
// callers don't have to remember the trick. Falls back to the input on a
// near-singular linear part.
Mat3 normal_matrix(const Mat3& m) noexcept;

// ─── Mat4 extensions ─────────────────────────────────────────────────────
Mat4 transpose(const Mat4& m) noexcept;
f32 determinant(const Mat4& m) noexcept;
Mat4 inverse(const Mat4& m) noexcept;

// Quick affine inverse for the common case where the upper-left 3×3 is
// orthonormal and the last row is [0,0,0,1] (rigid transforms). Cheaper
// than the full inverse; falls back to that path if the matrix isn't rigid.
Mat4 inverse_affine(const Mat4& m) noexcept;

// Left-handed perspective variant for tools / debug visualizations that
// prefer the Direct3D convention. The right-handed default in Math.h stays
// canonical for the engine.
Mat4 perspective_lh(f32 fov_y_rad, f32 aspect, f32 near_z, f32 far_z) noexcept;
Mat4 look_at_lh(Vec3 eye, Vec3 target, Vec3 up) noexcept;
Mat4 ortho_rh(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z) noexcept;

// ─── Quaternion extensions ───────────────────────────────────────────────
inline constexpr Quat quat_identity() noexcept {
    return {0, 0, 0, 1};
}

f32 quat_dot(Quat a, Quat b) noexcept;
Quat quat_conjugate(Quat q) noexcept;
Vec3 quat_rotate(Quat q, Vec3 v) noexcept;

// Linear interpolation (constant-velocity but path is not on the unit
// sphere — fast and acceptable for short-arc tweens).
Quat quat_lerp(Quat a, Quat b, f32 t) noexcept;

// Normalized linear interpolation: lerp + renormalize. Cheaper than slerp,
// commutative path, sufficient when `a` and `b` are close.
Quat quat_nlerp(Quat a, Quat b, f32 t) noexcept;

// Spherical linear interpolation; constant angular velocity. Falls back to
// nlerp when the quaternions are nearly parallel (sin(θ)→0 numerical death
// trap). Picks the short arc by flipping signs when dot < 0.
Quat quat_slerp(Quat a, Quat b, f32 t) noexcept;

// Euler convention: ZYX intrinsic, applied as yaw (Z) → pitch (Y) → roll (X).
// This matches the FPS-style camera convention used by samples 03/06.
Quat quat_from_euler(f32 roll_rad, f32 pitch_rad, f32 yaw_rad) noexcept;
Vec3 quat_to_euler(Quat q) noexcept;  // returns {roll, pitch, yaw}

}  // namespace psynder::math
