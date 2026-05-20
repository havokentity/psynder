// SPDX-License-Identifier: MIT
// Psynder — Lane 02 AABB / sphere tests.

#include "math/Math.h"
#include "math/MathExt.h"
#include "math/Bounds.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::math;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

bool vec3_approx_eq(Vec3 a, Vec3 b, f32 tol = 1e-4f) {
    return approx_eq(a.x, b.x, tol) && approx_eq(a.y, b.y, tol) && approx_eq(a.z, b.z, tol);
}

bool aabb_approx_eq(const Aabb& a, const Aabb& b, f32 tol = 1e-4f) {
    return vec3_approx_eq(a.min, b.min, tol) && vec3_approx_eq(a.max, b.max, tol);
}

}  // namespace

TEST_CASE("Aabb empty unions to the other operand", "[math][aabb]") {
    Aabb empty = aabb_empty();
    Aabb a{Vec3{-1, -1, -1}, Vec3{1, 1, 1}};
    REQUIRE(aabb_approx_eq(aabb_union(empty, a), a));
    REQUIRE(aabb_approx_eq(aabb_union(a, empty), a));
}

TEST_CASE("Aabb union widens to enclose both", "[math][aabb]") {
    Aabb a{Vec3{-1, -1, -1}, Vec3{1, 1, 1}};
    Aabb b{Vec3{2, 2, 2}, Vec3{4, 4, 4}};
    Aabb u = aabb_union(a, b);
    REQUIRE(vec3_approx_eq(u.min, Vec3{-1, -1, -1}));
    REQUIRE(vec3_approx_eq(u.max, Vec3{4, 4, 4}));
}

TEST_CASE("Aabb union with a point", "[math][aabb]") {
    Aabb a{Vec3{0, 0, 0}, Vec3{1, 1, 1}};
    Aabb u = aabb_union(a, Vec3{-2, 3, 0.5f});
    REQUIRE(vec3_approx_eq(u.min, Vec3{-2, 0, 0}));
    REQUIRE(vec3_approx_eq(u.max, Vec3{1, 3, 1}));
}

TEST_CASE("Aabb contains and intersects", "[math][aabb]") {
    Aabb outer{Vec3{-10, -10, -10}, Vec3{10, 10, 10}};
    Aabb inner{Vec3{-1, -1, -1}, Vec3{1, 1, 1}};
    Aabb apart{Vec3{20, 20, 20}, Vec3{21, 21, 21}};

    REQUIRE(contains(outer, Vec3{0, 5, -3}));
    REQUIRE_FALSE(contains(outer, Vec3{0, 100, 0}));

    REQUIRE(contains(outer, inner));
    REQUIRE_FALSE(contains(inner, outer));

    REQUIRE(intersects(outer, inner));
    REQUIRE_FALSE(intersects(outer, apart));
}

TEST_CASE("Aabb expand widens symmetrically", "[math][aabb]") {
    Aabb a{Vec3{0, 0, 0}, Vec3{2, 2, 2}};
    Aabb ex = expand(a, 0.5f);
    REQUIRE(vec3_approx_eq(ex.min, Vec3{-0.5f, -0.5f, -0.5f}));
    REQUIRE(vec3_approx_eq(ex.max, Vec3{2.5f, 2.5f, 2.5f}));
}

TEST_CASE("Aabb transform by identity is a no-op", "[math][aabb]") {
    Aabb a{Vec3{-1, -2, -3}, Vec3{4, 5, 6}};
    Aabb t = transform(a, identity4());
    REQUIRE(aabb_approx_eq(t, a));
}

TEST_CASE("Aabb transform by translation shifts both corners", "[math][aabb]") {
    Aabb a{Vec3{-1, -1, -1}, Vec3{1, 1, 1}};
    Mat4 m = translate({10, 20, 30});
    Aabb t = transform(a, m);
    REQUIRE(vec3_approx_eq(t.min, Vec3{9, 19, 29}));
    REQUIRE(vec3_approx_eq(t.max, Vec3{11, 21, 31}));
}

TEST_CASE("Aabb transform by a quarter turn swaps axes", "[math][aabb]") {
    // Box [-1,1]³ rotated 90° around Z → still [-1,1]³ as an AABB (it's a
    // symmetric cube). Use an asymmetric box so the transform is testable.
    Aabb a{Vec3{-1, -2, -0.5f}, Vec3{1, 2, 0.5f}};
    Quat q = quat_from_axis_angle({0, 0, 1}, kHalfPi);
    Mat4 r = rotate_quat(quat_normalize(q));
    Aabb t = transform(a, r);
    // After a 90° Z-rotation the X extent and Y extent should swap.
    REQUIRE(approx_eq(t.max.x - t.min.x, 4.0f));  // was 2 in Y
    REQUIRE(approx_eq(t.max.y - t.min.y, 2.0f));  // was 2 in X
    REQUIRE(approx_eq(t.max.z - t.min.z, 1.0f));  // unchanged
}

TEST_CASE("Aabb transform under non-uniform scale", "[math][aabb]") {
    Aabb a{Vec3{-1, -1, -1}, Vec3{1, 1, 1}};
    Mat4 s = scale({2, 3, 4});
    Aabb t = transform(a, s);
    REQUIRE(vec3_approx_eq(t.min, Vec3{-2, -3, -4}));
    REQUIRE(vec3_approx_eq(t.max, Vec3{2, 3, 4}));
}

TEST_CASE("Sphere intersects AABB at the corner case", "[math][sphere]") {
    Aabb box{Vec3{0, 0, 0}, Vec3{1, 1, 1}};
    Sphere just_inside{Vec3{1.1f, 1.1f, 1.1f}, 0.2f};  // touches the corner
    Sphere far_away{Vec3{5, 5, 5}, 0.5f};
    REQUIRE(intersects(just_inside, box));
    REQUIRE_FALSE(intersects(far_away, box));
}

TEST_CASE("Sphere union encloses both", "[math][sphere]") {
    Sphere a{Vec3{0, 0, 0}, 1.0f};
    Sphere b{Vec3{5, 0, 0}, 1.0f};
    Sphere u = sphere_union(a, b);
    // The union should contain both endpoints of the two spheres along x.
    REQUIRE(contains(u, Vec3{-1, 0, 0}));
    REQUIRE(contains(u, Vec3{6, 0, 0}));
}

TEST_CASE("Bounding-sphere of an AABB contains the corners", "[math][sphere]") {
    Aabb a{Vec3{-1, -2, -3}, Vec3{4, 5, 6}};
    Sphere s = bounding_sphere(a);
    REQUIRE(contains(s, a.min));
    REQUIRE(contains(s, a.max));
    REQUIRE(contains(s, Vec3{a.min.x, a.max.y, a.min.z}));
}
