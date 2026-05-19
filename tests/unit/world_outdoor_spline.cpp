// SPDX-License-Identifier: MIT
// Psynder — Lane 11 unit tests for the spline track extruder (DESIGN.md §9.2).
//
// We check: (a) Bezier endpoints hit the control points exactly, (b) the
// extruded strip has the right vertex / index counts and triangle winding,
// (c) banking rotates the road frame as expected, (d) the strip is flagged
// as a drivable surface (DrawItem::flags bit 0) so the physics lane can
// pick it up.

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/Spline_internal.h"
#include "world/outdoor/Terrain.h"

#include <cmath>

namespace pwo  = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

namespace {

pwo::SplineRoadSegment make_straight_segment(float length, float half_width) {
    pwo::SplineRoadSegment s{};
    // Straight segment from (0,0,0) to (length,0,0), control points evenly
    // spaced. This guarantees a constant tangent and trivial arc length.
    s.p0 = {0.0f,         0.0f, 0.0f};
    s.p1 = {length/3.0f,  0.0f, 0.0f};
    s.p2 = {2.0f*length/3.0f, 0.0f, 0.0f};
    s.p3 = {length,       0.0f, 0.0f};
    s.half_width = half_width;
    s.banking_rad = 0.0f;
    return s;
}

}  // namespace

TEST_CASE("Bezier endpoints reproduce the control points",
          "[world_outdoor][spline]") {
    auto seg = make_straight_segment(40.0f, 4.0f);
    const auto p0 = pwod::bezier_eval(seg, 0.0f);
    const auto p1 = pwod::bezier_eval(seg, 1.0f);
    REQUIRE(p0.x == 0.0f);
    REQUIRE(p1.x == 40.0f);
}

TEST_CASE("Bezier tangent is non-zero along the curve",
          "[world_outdoor][spline]") {
    auto seg = make_straight_segment(40.0f, 4.0f);
    for (int i = 0; i <= 10; ++i) {
        const float t = static_cast<float>(i) * 0.1f;
        const auto tan = pwod::bezier_tangent(seg, t);
        REQUIRE(psynder::math::length(tan) > 1e-4f);
    }
}

TEST_CASE("Bezier arc length approximates the chord for a straight segment",
          "[world_outdoor][spline]") {
    auto seg = make_straight_segment(40.0f, 4.0f);
    const float arc = pwod::bezier_arc_length(seg, 32);
    REQUIRE(arc > 39.0f);
    REQUIRE(arc < 41.0f);
}

TEST_CASE("Extruded strip has the expected vertex / index counts",
          "[world_outdoor][spline]") {
    auto seg = make_straight_segment(50.0f, 4.0f);
    const auto strip = pwod::extrude_segment(seg, /*samples=*/8, /*uv_repeat=*/8.0f);
    REQUIRE(strip.vertices.size() == 16u);    // 2 verts per sample × 8 samples
    REQUIRE(strip.indices.size()  == 42u);    // 6 indices per quad × 7 quads
    REQUIRE((strip.indices.size() % 3u) == 0u);
    REQUIRE((strip.flags & pwod::kDrawItemFlagDrivable) != 0u);
}

TEST_CASE("Straight extrusion produces a flat strip of the requested width",
          "[world_outdoor][spline]") {
    const float half_w = 4.0f;
    auto seg = make_straight_segment(40.0f, half_w);
    const auto strip = pwod::extrude_segment(seg, 8, 8.0f);

    // For a straight +X segment, tangent = (1,0,0) so the planar-perp
    // right vector = (0,0,-1). The L vert is centerline - right*half_w
    // = (x, 0, +half_w); R is (x, 0, -half_w). The two together span
    // 2*half_w on Z, centered on the centerline (z=0).
    for (std::size_t i = 0; i + 1 < strip.vertices.size(); i += 2) {
        const auto& L = strip.vertices[i];
        const auto& R = strip.vertices[i + 1];
        REQUIRE(std::fabs(L.position.z - half_w) < 1e-3f);
        REQUIRE(std::fabs(R.position.z + half_w) < 1e-3f);
        // No banking → both points stay at y=0.
        REQUIRE(std::fabs(L.position.y) < 1e-3f);
        REQUIRE(std::fabs(R.position.y) < 1e-3f);
        // The road's up vector should be +Y for the unbanked frame.
        REQUIRE(L.normal.y > 0.99f);
    }
}

TEST_CASE("Banking rolls the road frame about the tangent",
          "[world_outdoor][spline]") {
    auto seg = make_straight_segment(40.0f, 4.0f);
    seg.banking_rad = psynder::math::kHalfPi * 0.5f;  // 45° banking
    const auto strip = pwod::extrude_segment(seg, 4, 8.0f);

    // The L and R verts should now be tilted along Y — under positive
    // banking, R goes up and L goes down (or vice-versa). The two must
    // differ in Y.
    bool found_y_diff = false;
    for (std::size_t i = 0; i + 1 < strip.vertices.size(); i += 2) {
        const float dy = strip.vertices[i].position.y - strip.vertices[i+1].position.y;
        if (std::fabs(dy) > 1e-3f) { found_y_diff = true; break; }
    }
    REQUIRE(found_y_diff);
}

TEST_CASE("Bezier midpoint of straight segment matches linear",
          "[world_outdoor][spline]") {
    auto seg = make_straight_segment(60.0f, 3.0f);
    const auto m = pwod::bezier_eval(seg, 0.5f);
    REQUIRE(std::fabs(m.x - 30.0f) < 1e-3f);
    REQUIRE(std::fabs(m.y -  0.0f) < 1e-3f);
    REQUIRE(std::fabs(m.z -  0.0f) < 1e-3f);
}
