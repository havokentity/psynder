// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: Q24.8 edge equation sign correctness.
//
// Covers the three invariants the rasterizer leans on:
//   1. CCW triangles produce positive 2× signed area.
//   2. A point inside the triangle yields all three edge functions ≥ 0
//      (after top-left bias).
//   3. A point outside flips the sign on at least one edge.
//   4. The top-left fill rule is symmetric — touching points get the same
//      classification regardless of which triangle owns the shared edge.

#include "render/raster/EdgeEq.h"
#include "render/raster/Fixed.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::render::raster;

namespace {

math::Vec4 ndc(f32 x, f32 y, f32 z = 0.0f) noexcept {
    return math::Vec4{ x, y, z, 1.0f };
}

}  // namespace

TEST_CASE("FxQ24_8 round-trip preserves sub-pixel precision", "[raster][fixed]") {
    REQUIRE(FxQ24_8::from_float(0.5f).v   == 128);
    REQUIRE(FxQ24_8::from_float(1.0f).v   == 256);
    REQUIRE(FxQ24_8::from_float(-1.0f).v  == -256);
    REQUIRE(FxQ24_8::from_float(0.00390625f).v == 1);  // 1/256 — sub-pixel
    REQUIRE(FxQ24_8::from_float(100.0f).to_float() == 100.0f);
}

TEST_CASE("edge_func is positive on the left of a CCW edge", "[raster][edge]") {
    // Edge: a=(0,0) → b=(10,0) along +x axis. "Left" of this in our
    // y-down screen space (positive y goes down) is the -y direction.
    // Our convention has CCW area positive when y-axis is flipped — the
    // setup transform already flips y. Here we test the raw edge func.
    const auto a = math::Vec2{ 0.0f, 0.0f };
    const auto b = math::Vec2{ 10.0f, 0.0f };

    const auto ax = FxQ24_8::from_float(a.x);
    const auto ay = FxQ24_8::from_float(a.y);
    const auto bx = FxQ24_8::from_float(b.x);
    const auto by = FxQ24_8::from_float(b.y);

    // Point at (5, -5) — "above" the edge in math coords
    const i64 above = edge_func(ax, ay, bx, by,
                                FxQ24_8::from_float(5.0f),
                                FxQ24_8::from_float(-5.0f));
    const i64 below = edge_func(ax, ay, bx, by,
                                FxQ24_8::from_float(5.0f),
                                FxQ24_8::from_float( 5.0f));
    REQUIRE(above < 0);     // E(a,b,p_above)  is negative
    REQUIRE(below > 0);     // E(a,b,p_below)  is positive
    REQUIRE(above == -below);
}

TEST_CASE("setup_triangle: screen-CCW triangle is front-facing", "[raster][setup]") {
    TriSetup t{};
    // Vertices in NDC. The viewport map flips y (screen y goes down). In
    // NDC y-up, listing as (BL, BR, TOP) is CCW. After the y-flip the
    // winding becomes CW in screen space — culled. Reverse order to keep
    // the triangle front-facing post-flip: (BL, TOP, BR).
    const bool ok = setup_triangle(
        ndc(-0.5f, -0.5f), ndc(0.0f, 0.5f), ndc(0.5f, -0.5f),
        math::Vec2{0,1}, math::Vec2{0.5f,0}, math::Vec2{1,1},
        0xFF0000FFu, 0xFFFF0000u, 0xFF00FF00u,
        640, 360,
        t);
    REQUIRE(ok);
    REQUIRE(t.valid);
    REQUIRE(t.minx >= 0);
    REQUIRE(t.maxx <= 640);
    REQUIRE(t.miny >= 0);
    REQUIRE(t.maxy <= 360);
    REQUIRE(t.inv_area2x > 0.0f);
}

TEST_CASE("setup_triangle: screen-CW triangle is back-face culled", "[raster][setup]") {
    TriSetup t{};
    // (BL, BR, TOP) in NDC y-up; after y-flip becomes CW in screen space.
    const bool ok = setup_triangle(
        ndc(-0.5f, -0.5f), ndc(0.5f, -0.5f), ndc(0.0f, 0.5f),
        math::Vec2{0,1}, math::Vec2{1,1}, math::Vec2{0.5f,0},
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        640, 360,
        t);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(t.valid);
}

TEST_CASE("inside test: pixel inside the triangle has all edges ≥ 0",
          "[raster][coverage]") {
    TriSetup t{};
    REQUIRE(setup_triangle(
        ndc(-0.5f, -0.5f), ndc(0.0f, 0.5f), ndc(0.5f, -0.5f),
        math::Vec2{0,1}, math::Vec2{0.5f,0}, math::Vec2{1,1},
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        640, 360,
        t));

    // Centroid in screen space ≈ ((sx0+sx1+sx2)/3, (sy0+sy1+sy2)/3)
    const f32 cx = (t.x0.to_float() + t.x1.to_float() + t.x2.to_float()) / 3.0f;
    const f32 cy = (t.y0.to_float() + t.y1.to_float() + t.y2.to_float()) / 3.0f;

    const auto fx = FxQ24_8::from_float(cx);
    const auto fy = FxQ24_8::from_float(cy);
    REQUIRE(eval_edge0(t, fx, fy) >= 0);
    REQUIRE(eval_edge1(t, fx, fy) >= 0);
    REQUIRE(eval_edge2(t, fx, fy) >= 0);
}

TEST_CASE("outside test: pixel outside the triangle has at least one negative edge",
          "[raster][coverage]") {
    TriSetup t{};
    REQUIRE(setup_triangle(
        ndc(-0.5f, -0.5f), ndc(0.0f, 0.5f), ndc(0.5f, -0.5f),
        math::Vec2{0,1}, math::Vec2{0.5f,0}, math::Vec2{1,1},
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
        640, 360,
        t));

    // Pick a corner of the viewport — guaranteed outside the triangle.
    const auto fx = FxQ24_8::from_float(1.0f);
    const auto fy = FxQ24_8::from_float(1.0f);
    const i64 e0 = eval_edge0(t, fx, fy);
    const i64 e1 = eval_edge1(t, fx, fy);
    const i64 e2 = eval_edge2(t, fx, fy);
    REQUIRE((e0 < 0 || e1 < 0 || e2 < 0));
}

TEST_CASE("top-left fill rule: shared edge between two triangles owns each pixel once",
          "[raster][fillrule]") {
    // Two triangles sharing the diagonal of a quad. A pixel-centre that
    // lies exactly on the shared edge must be covered by exactly one of
    // them — never both, never neither. Both triangles wound to be
    // front-facing after the viewport y-flip.
    TriSetup t1{}, t2{};
    REQUIRE(setup_triangle(
        ndc(-0.5f, -0.5f), ndc(-0.5f, 0.5f), ndc(0.5f, -0.5f),
        math::Vec2{0,1}, math::Vec2{0,0}, math::Vec2{1,1},
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 640, 360, t1));
    REQUIRE(setup_triangle(
        ndc(0.5f, -0.5f), ndc(-0.5f, 0.5f), ndc(0.5f, 0.5f),
        math::Vec2{1,1}, math::Vec2{0,0}, math::Vec2{1,0},
        0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 640, 360, t2));

    // Sample some pixels on the shared diagonal and adjacent. Without
    // checking exhaustive cases the easier invariant: every pixel that
    // returns inside-true for both triangles is a bug.
    u32 double_covered = 0;
    u32 covered_t1 = 0;
    u32 covered_t2 = 0;
    for (i32 y = 0; y < 360; y += 7) {
        for (i32 x = 0; x < 640; x += 7) {
            const auto fx = FxQ24_8::from_float(static_cast<f32>(x) + 0.5f);
            const auto fy = FxQ24_8::from_float(static_cast<f32>(y) + 0.5f);
            const bool in1 = eval_edge0(t1, fx, fy) >= 0
                          && eval_edge1(t1, fx, fy) >= 0
                          && eval_edge2(t1, fx, fy) >= 0;
            const bool in2 = eval_edge0(t2, fx, fy) >= 0
                          && eval_edge1(t2, fx, fy) >= 0
                          && eval_edge2(t2, fx, fy) >= 0;
            if (in1 && in2) ++double_covered;
            if (in1) ++covered_t1;
            if (in2) ++covered_t2;
        }
    }
    REQUIRE(double_covered == 0);
    REQUIRE(covered_t1 > 0);
    REQUIRE(covered_t2 > 0);
}
