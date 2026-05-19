// SPDX-License-Identifier: MIT
// Psynder — Lane 08 unit test: single-triangle hit / miss / occlusion.
//
// Verifies the most-important invariant: a single triangle in a Bvh8 is
// correctly hit when a ray pierces it, missed when it doesn't, and the
// occlusion query agrees with intersect().

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/rt/Bvh.h"

using namespace psynder;
using namespace psynder::render::rt;

namespace {

Triangle make_triangle_xy_at(f32 z) {
    return Triangle{
        math::Vec3{ -1.0f, -1.0f, z },
        math::Vec3{  1.0f, -1.0f, z },
        math::Vec3{  0.0f,  1.0f, z },
    };
}

}  // namespace

TEST_CASE("Bvh8 single triangle: ray-through-centroid hits", "[render_rt][single_triangle]") {
    Triangle t = make_triangle_xy_at(5.0f);
    Bvh8 bvh;
    bvh.build(&t, 1);

    REQUIRE(bvh.node_count() >= 1u);

    Ray r;
    r.origin    = { 0.0f, 0.0f, 0.0f };
    r.direction = { 0.0f, 0.0f, 1.0f };
    r.t_min     = 1e-4f;
    r.t_max     = 1e30f;

    Hit h = bvh.intersect(r);
    REQUIRE(h.hit);
    REQUIRE_THAT(static_cast<double>(h.t), Catch::Matchers::WithinAbs(5.0, 1e-3));
    REQUIRE(h.primitive == 0u);
}

TEST_CASE("Bvh8 single triangle: ray-aside misses", "[render_rt][single_triangle]") {
    Triangle t = make_triangle_xy_at(5.0f);
    Bvh8 bvh;
    bvh.build(&t, 1);

    Ray r;
    r.origin    = { 10.0f, 10.0f, 0.0f };
    r.direction = { 0.0f, 0.0f, 1.0f };
    r.t_min     = 1e-4f;
    r.t_max     = 1e30f;

    Hit h = bvh.intersect(r);
    REQUIRE_FALSE(h.hit);
}

TEST_CASE("Bvh8 single triangle: occluded() agrees with intersect()",
          "[render_rt][single_triangle]") {
    Triangle t = make_triangle_xy_at(5.0f);
    Bvh8 bvh;
    bvh.build(&t, 1);

    Ray r;
    r.origin    = { 0.0f, 0.0f, 0.0f };
    r.direction = { 0.0f, 0.0f, 1.0f };
    r.t_min     = 1e-4f;
    r.t_max     = 1e30f;

    REQUIRE(bvh.occluded(r));

    r.origin = { 10.0f, 10.0f, 0.0f };
    REQUIRE_FALSE(bvh.occluded(r));
}

TEST_CASE("Bvh8 single triangle: backface hit (Möller–Trumbore double-sided)",
          "[render_rt][single_triangle]") {
    Triangle t = make_triangle_xy_at(5.0f);
    Bvh8 bvh;
    bvh.build(&t, 1);

    // Ray from behind the triangle going +Z still hits (Wave-A MT is
    // two-sided; we'd add a face-cull flag later if needed).
    Ray r;
    r.origin    = { 0.0f, 0.0f, 10.0f };
    r.direction = { 0.0f, 0.0f, -1.0f };
    r.t_min     = 1e-4f;
    r.t_max     = 1e30f;

    Hit h = bvh.intersect(r);
    REQUIRE(h.hit);
    REQUIRE_THAT(static_cast<double>(h.t), Catch::Matchers::WithinAbs(5.0, 1e-3));
}

TEST_CASE("Bvh8: t_max clamp respected", "[render_rt][single_triangle]") {
    Triangle t = make_triangle_xy_at(5.0f);
    Bvh8 bvh;
    bvh.build(&t, 1);

    Ray r;
    r.origin    = { 0.0f, 0.0f, 0.0f };
    r.direction = { 0.0f, 0.0f, 1.0f };
    r.t_min     = 1e-4f;
    r.t_max     = 4.0f;  // doesn't reach the triangle

    Hit h = bvh.intersect(r);
    REQUIRE_FALSE(h.hit);
    REQUIRE_FALSE(bvh.occluded(r));
}
