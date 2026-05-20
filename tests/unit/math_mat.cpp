// SPDX-License-Identifier: MIT
// Psynder — Lane 02 math tests for Mat3 / Mat4.

#include "math/Math.h"
#include "math/MathExt.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::math;

namespace {

// Approximate-equality helpers tuned for f32 round-trips through a few
// matrix multiplies — the inverse path's worst case is ~1e-5 in our
// experiments, but a 1e-4 ceiling absorbs that comfortably without
// hiding actual bugs.
bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

bool mat4_approx_eq(const Mat4& a, const Mat4& b, f32 tol = 1e-4f) {
    for (int i = 0; i < 16; ++i) {
        if (!approx_eq(a.m[i], b.m[i], tol))
            return false;
    }
    return true;
}

Mat4 sample_affine_mat4() {
    // A non-trivial affine: rotation × non-uniform scale × translation.
    // Just enough off-diagonal mixing to surface a buggy inverse / mul.
    Quat q = quat_from_axis_angle({0.3f, 0.8f, 0.5f}, 1.1f);
    q = quat_normalize(q);
    Mat4 r = rotate_quat(q);
    Mat4 s = scale({1.3f, 0.7f, 1.8f});
    Mat4 t = translate({2.5f, -1.0f, 0.4f});
    return mul(t, mul(r, s));
}

}  // namespace

TEST_CASE("Mat4 identity is identity", "[math][mat4]") {
    Mat4 i = identity4();
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            f32 expected = (row == col) ? 1.0f : 0.0f;
            REQUIRE(approx_eq(i.m[col * 4 + row], expected));
        }
    }
}

TEST_CASE("Mat4 inverse round-trips identity", "[math][mat4]") {
    Mat4 m = sample_affine_mat4();
    Mat4 inv = inverse(m);

    Mat4 a = mul(m, inv);
    Mat4 b = mul(inv, m);
    REQUIRE(mat4_approx_eq(a, identity4()));
    REQUIRE(mat4_approx_eq(b, identity4()));
}

TEST_CASE("Mat4 inverse_affine matches general inverse for rigid transforms", "[math][mat4]") {
    // For an affine matrix with a non-uniform scale baked in, the
    // affine-inverse must still recover identity when composed with the
    // forward transform.
    Mat4 m = sample_affine_mat4();
    Mat4 inv_aff = inverse_affine(m);
    Mat4 roundtrip = mul(m, inv_aff);
    REQUIRE(mat4_approx_eq(roundtrip, identity4(), 1e-3f));
}

TEST_CASE("Mat4 transpose swaps rows and columns", "[math][mat4]") {
    Mat4 m = sample_affine_mat4();
    Mat4 t = transpose(m);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            REQUIRE(approx_eq(t.m[col * 4 + row], m.m[row * 4 + col]));
        }
    }
    Mat4 tt = transpose(t);
    REQUIRE(mat4_approx_eq(tt, m));
}

TEST_CASE("Mat4 determinant matches inverse existence", "[math][mat4]") {
    Mat4 m = sample_affine_mat4();
    f32 d = determinant(m);
    REQUIRE(std::fabs(d) > 1e-3f);

    // Translation-only matrices have det = 1.
    Mat4 t = translate({3, 4, 5});
    REQUIRE(approx_eq(determinant(t), 1.0f));

    // Uniform-scale: det = s^3.
    Mat4 s = scale({2, 2, 2});
    REQUIRE(approx_eq(determinant(s), 8.0f));
}

TEST_CASE("Mat4 transform preserves homogeneous vectors", "[math][mat4]") {
    Mat4 t = translate({10.0f, 20.0f, 30.0f});
    Vec4 p = mul(t, Vec4{1, 2, 3, 1});
    REQUIRE(approx_eq(p.x, 11.0f));
    REQUIRE(approx_eq(p.y, 22.0f));
    REQUIRE(approx_eq(p.z, 33.0f));
    REQUIRE(approx_eq(p.w, 1.0f));

    // Direction vectors (w = 0) don't pick up the translation.
    Vec4 d = mul(t, Vec4{1, 0, 0, 0});
    REQUIRE(approx_eq(d.x, 1.0f));
    REQUIRE(approx_eq(d.y, 0.0f));
    REQUIRE(approx_eq(d.z, 0.0f));
}

TEST_CASE("Mat3 identity is identity", "[math][mat3]") {
    Mat3 i = identity3();
    REQUIRE(approx_eq(i.m[0], 1.0f));
    REQUIRE(approx_eq(i.m[4], 1.0f));
    REQUIRE(approx_eq(i.m[8], 1.0f));
    REQUIRE(approx_eq(i.m[1], 0.0f));
    REQUIRE(approx_eq(i.m[5], 0.0f));
}

TEST_CASE("Mat3 inverse round-trips identity", "[math][mat3]") {
    // Build a Mat3 from a non-singular linear part.
    Mat4 m4 = sample_affine_mat4();
    Mat3 m = mat3_from_mat4(m4);
    Mat3 inv = inverse(m);

    Mat3 ab = mul(m, inv);
    Mat3 ba = mul(inv, m);
    for (int i = 0; i < 9; ++i) {
        REQUIRE(approx_eq(ab.m[i], identity3().m[i]));
        REQUIRE(approx_eq(ba.m[i], identity3().m[i]));
    }
}

TEST_CASE("Mat3 transpose is involutive", "[math][mat3]") {
    Mat4 m4 = sample_affine_mat4();
    Mat3 m = mat3_from_mat4(m4);
    Mat3 tt = transpose(transpose(m));
    for (int i = 0; i < 9; ++i)
        REQUIRE(approx_eq(tt.m[i], m.m[i]));
}

TEST_CASE("perspective_rh and look_at_rh produce sane matrices", "[math][mat4]") {
    Mat4 p = perspective_rh(kHalfPi, 16.0f / 9.0f, 0.1f, 1000.0f);
    // m[11] is the conventional -1 entry that turns w into -z in RH.
    REQUIRE(approx_eq(p.m[11], -1.0f));

    // Looking down -Z: forward = (0,0,-1), so the 3rd row entries that
    // dot the forward axis should produce identity-ish behavior.
    Mat4 v = look_at_rh({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    Vec4 origin = mul(v, Vec4{0, 0, 0, 1});
    REQUIRE(approx_eq(origin.z, -5.0f, 1e-3f));
}

TEST_CASE("ortho_rh maps the box corners to the canonical clip cube", "[math][mat4]") {
    Mat4 o = ortho_rh(-2, 2, -1, 1, 0.1f, 10.0f);
    Vec4 lo = mul(o, Vec4{-2, -1, -0.1f, 1});  // near-bottom-left
    Vec4 hi = mul(o, Vec4{2, 1, -10.0f, 1});   // far-top-right
    REQUIRE(approx_eq(lo.x, -1.0f));
    REQUIRE(approx_eq(lo.y, -1.0f));
    REQUIRE(approx_eq(hi.x, 1.0f));
    REQUIRE(approx_eq(hi.y, 1.0f));
}
