// SPDX-License-Identifier: MIT
// Psynder — Lane 02 quaternion tests.

#include "math/Math.h"
#include "math/MathExt.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::math;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-5f) {
    return std::fabs(a - b) <= tol;
}

bool quat_approx_eq(Quat a, Quat b, f32 tol = 1e-5f) {
    // Quaternions q and -q represent the same rotation; accept either.
    f32 d = quat_dot(a, b);
    Quat bb = d < 0.0f ? Quat{-b.x, -b.y, -b.z, -b.w} : b;
    return approx_eq(a.x, bb.x, tol) && approx_eq(a.y, bb.y, tol) && approx_eq(a.z, bb.z, tol) &&
           approx_eq(a.w, bb.w, tol);
}

bool vec3_approx_eq(Vec3 a, Vec3 b, f32 tol = 1e-4f) {
    return approx_eq(a.x, b.x, tol) && approx_eq(a.y, b.y, tol) && approx_eq(a.z, b.z, tol);
}

}  // namespace

TEST_CASE("quat_identity is identity", "[math][quat]") {
    Quat q = quat_identity();
    REQUIRE(approx_eq(q.x, 0.0f));
    REQUIRE(approx_eq(q.y, 0.0f));
    REQUIRE(approx_eq(q.z, 0.0f));
    REQUIRE(approx_eq(q.w, 1.0f));
}

TEST_CASE("quat from axis angle has correct norm", "[math][quat]") {
    Quat q = quat_from_axis_angle({1, 0, 0}, kHalfPi);
    REQUIRE(approx_eq(quat_dot(q, q), 1.0f));
}

TEST_CASE("quat_slerp returns endpoints at t equals 0 and 1", "[math][quat]") {
    Quat a = quat_from_axis_angle({0, 0, 1}, 0.2f);
    Quat b = quat_from_axis_angle({0, 0, 1}, 1.5f);
    a = quat_normalize(a);
    b = quat_normalize(b);

    REQUIRE(quat_approx_eq(quat_slerp(a, b, 0.0f), a));
    REQUIRE(quat_approx_eq(quat_slerp(a, b, 1.0f), b));
}

TEST_CASE("quat_slerp midpoint is the half-angle rotation", "[math][quat]") {
    // Avoid the 180° antipodal singularity (where slerp's shortest-arc
    // choice is ambiguous). At π·0.6 the short-arc midpoint at t=0.5 is a
    // π·0.3 rotation around the same axis.
    Vec3 axis{0, 1, 0};
    f32 theta = kPi * 0.6f;
    Quat a = quat_identity();
    Quat b = quat_normalize(quat_from_axis_angle(axis, theta));

    Quat mid = quat_slerp(a, b, 0.5f);
    Quat expected = quat_from_axis_angle(axis, theta * 0.5f);

    // Apply the rotation to a probe vector — the rotated probes should
    // match exactly, modulo our usual f32 tolerance.
    Vec3 probe{1, 0, 0};
    Vec3 v_mid = quat_rotate(mid, probe);
    Vec3 v_exp = quat_rotate(expected, probe);
    REQUIRE(vec3_approx_eq(v_mid, v_exp, 1e-4f));
}

TEST_CASE("quat_slerp picks the short arc across hemispheres", "[math][quat]") {
    // a and b on opposite hemispheres — slerp should still produce the
    // shorter path even though dot(a,b) < 0.
    Quat a = quat_from_axis_angle({0, 0, 1}, 0.1f);
    Quat b = quat_from_axis_angle({0, 0, 1}, 0.1f);
    b = Quat{-b.x, -b.y, -b.z, -b.w};  // identical rotation, flipped sign

    Quat mid = quat_slerp(a, b, 0.5f);
    // The midpoint should still be a rotation by ~0.1 rad around z (up to
    // quaternion double-cover), not the long-way-round 2π - 0.1.
    Vec3 v_a = quat_rotate(a, {1, 0, 0});
    Vec3 v_mid = quat_rotate(mid, {1, 0, 0});
    REQUIRE(vec3_approx_eq(v_mid, v_a, 1e-4f));
}

TEST_CASE("quat_nlerp endpoints match inputs", "[math][quat]") {
    Quat a = quat_from_axis_angle({1, 0, 0}, 0.3f);
    Quat b = quat_from_axis_angle({0, 1, 0}, 0.7f);
    a = quat_normalize(a);
    b = quat_normalize(b);

    REQUIRE(quat_approx_eq(quat_nlerp(a, b, 0.0f), a));
    REQUIRE(quat_approx_eq(quat_nlerp(a, b, 1.0f), b));
}

TEST_CASE("quat_from_euler and quat_to_euler round-trip", "[math][quat]") {
    // Avoid the gimbal-lock singularity (|pitch| ≈ π/2). Stay well inside it.
    f32 roll = 0.4f;
    f32 pitch = 0.6f;
    f32 yaw = 1.1f;

    Quat q = quat_from_euler(roll, pitch, yaw);
    Vec3 e = quat_to_euler(q);
    REQUIRE(approx_eq(e.x, roll, 1e-4f));
    REQUIRE(approx_eq(e.y, pitch, 1e-4f));
    REQUIRE(approx_eq(e.z, yaw, 1e-4f));
}

TEST_CASE("quat_rotate matches the rotation matrix path", "[math][quat]") {
    Quat q = quat_from_axis_angle({0, 1, 0}, 0.6f);
    q = quat_normalize(q);
    Vec3 v = {1, 2, 3};

    Vec3 via_quat = quat_rotate(q, v);
    Mat4 R = rotate_quat(q);
    Vec4 via_mat = mul(R, Vec4{v.x, v.y, v.z, 0});

    REQUIRE(vec3_approx_eq(via_quat, Vec3{via_mat.x, via_mat.y, via_mat.z}));
}

TEST_CASE("quat_conjugate inverts unit quat rotations", "[math][quat]") {
    Quat q = quat_from_axis_angle({0.4f, 0.5f, 0.7f}, 0.9f);
    q = quat_normalize(q);
    Vec3 v = {0.3f, -1.1f, 0.6f};

    Vec3 rotated = quat_rotate(q, v);
    Vec3 back = quat_rotate(quat_conjugate(q), rotated);
    REQUIRE(vec3_approx_eq(back, v));
}
