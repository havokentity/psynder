// SPDX-License-Identifier: MIT
// Psynder — Lane 11 Wave-B unit tests for SplineEditor (track-authoring
// data ops). Lane 18 consumes these to drive the editor's track tool.
//
// Properties pinned:
//  - `insert_control_point` PRESERVES the curve geometry (de Casteljau split).
//  - `move_control_point` translates a single control point without touching
//    the others.
//  - `delete_control_point` removes the right segment, stitches neighbors.
//  - `set_banking_at_t` writes the banking on the correct segment.

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/Spline_internal.h"
#include "world/outdoor/Terrain.h"

#include <cmath>

namespace pwo  = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

namespace {

pwo::SplineRoadSegment make_arc_segment(float radius, float angle_deg) {
    // Build a Bezier approximation of a horizontal circular arc — control
    // points are spread along an arc in the XZ plane, so the curve is
    // non-trivial (insertion/morph tests would miss things on a straight line).
    const float t  = angle_deg * (3.14159265358979f / 180.0f);
    const float c0 = std::cos(0.0f),         s0 = std::sin(0.0f);
    const float c1 = std::cos(t / 3.0f),     s1 = std::sin(t / 3.0f);
    const float c2 = std::cos(2.0f * t / 3.0f), s2 = std::sin(2.0f * t / 3.0f);
    const float c3 = std::cos(t),            s3 = std::sin(t);
    pwo::SplineRoadSegment seg{};
    seg.p0 = { radius * c0, 0.0f, radius * s0 };
    seg.p1 = { radius * c1, 0.0f, radius * s1 };
    seg.p2 = { radius * c2, 0.0f, radius * s2 };
    seg.p3 = { radius * c3, 0.0f, radius * s3 };
    seg.half_width  = 5.0f;
    seg.banking_rad = 0.0f;
    return seg;
}

bool vec3_close(psynder::math::Vec3 a, psynder::math::Vec3 b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) < eps &&
           std::fabs(a.y - b.y) < eps &&
           std::fabs(a.z - b.z) < eps;
}

}  // namespace

TEST_CASE("SplineEditor empty track has zero segments + zero CPs",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    REQUIRE(ed.segment_count() == 0u);
    REQUIRE(ed.control_point_count() == 0u);
    REQUIRE_FALSE(ed.insert_control_point(0, 0.5f));   // OOB
    REQUIRE(ed.set_banking_at_t(0.5f, 0.1f) == -1);
}

TEST_CASE("SplineEditor append_segment grows the track",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    auto seg = make_arc_segment(20.0f, 60.0f);
    const std::uint32_t idx = ed.append_segment(seg);
    REQUIRE(idx == 0u);
    REQUIRE(ed.segment_count() == 1u);
    REQUIRE(ed.control_point_count() == 4u);
}

TEST_CASE("SplineEditor::insert_control_point preserves curve geometry",
          "[world_outdoor][spline][editor]") {
    // After a de Casteljau split at t, sampling the union of the two
    // halves must reproduce the original curve exactly (up to fp noise).
    pwod::SplineEditor ed;
    auto seg = make_arc_segment(30.0f, 90.0f);
    ed.append_segment(seg);

    // Pre-insert: sample the original at 11 values of t.
    constexpr int N = 11;
    psynder::math::Vec3 before[N];
    for (int i = 0; i < N; ++i) {
        const float t = static_cast<float>(i) / (N - 1);
        before[i] = pwod::bezier_eval(seg, t);
    }

    // Insert a control point at t=0.4. The split makes A cover [0, 0.4]
    // and B cover [0.4, 1] of the original parameterization.
    const float t_split = 0.4f;
    REQUIRE(ed.insert_control_point(0, t_split));
    REQUIRE(ed.segment_count() == 2u);

    // Post-insert: sample the union. For t < t_split we sample segment 0
    // at u = t / t_split; for t > t_split we sample segment 1 at
    // u = (t - t_split) / (1 - t_split). The geometry must match.
    for (int i = 0; i < N; ++i) {
        const float t = static_cast<float>(i) / (N - 1);
        psynder::math::Vec3 sampled{};
        if (t < t_split) {
            const float u = t / t_split;
            sampled = pwod::bezier_eval(ed.segments()[0], u);
        } else {
            const float u = (t - t_split) / (1.0f - t_split);
            sampled = pwod::bezier_eval(ed.segments()[1], u);
        }
        INFO("t=" << t);
        REQUIRE(vec3_close(before[i], sampled, 1e-3f));
    }
}

TEST_CASE("SplineEditor::insert_control_point inherits half_width / banking",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    auto seg = make_arc_segment(20.0f, 45.0f);
    seg.half_width  = 6.5f;
    seg.banking_rad = 0.12f;
    ed.append_segment(seg);

    REQUIRE(ed.insert_control_point(0, 0.25f));
    REQUIRE(ed.segments()[0].half_width  == 6.5f);
    REQUIRE(ed.segments()[1].half_width  == 6.5f);
    REQUIRE(ed.segments()[0].banking_rad == 0.12f);
    REQUIRE(ed.segments()[1].banking_rad == 0.12f);
}

TEST_CASE("SplineEditor::insert_control_point rejects t outside (0,1)",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 45.0f));
    REQUIRE_FALSE(ed.insert_control_point(0, 0.0f));
    REQUIRE_FALSE(ed.insert_control_point(0, 1.0f));
    REQUIRE_FALSE(ed.insert_control_point(0, -0.1f));
    REQUIRE_FALSE(ed.insert_control_point(0, 1.1f));
    REQUIRE(ed.segment_count() == 1u);
}

TEST_CASE("SplineEditor::move_control_point updates only the named CP",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));

    // Cache originals.
    const auto seg0_p0 = ed.segments()[0].p0;
    const auto seg0_p1 = ed.segments()[0].p1;
    const auto seg0_p2 = ed.segments()[0].p2;
    const auto seg0_p3 = ed.segments()[0].p3;
    const auto seg1_p0 = ed.segments()[1].p0;

    // Move CP index 1 (segment 0, local 1) by absolute set.
    const psynder::math::Vec3 new_pos{ 1.0f, 2.0f, 3.0f };
    REQUIRE(ed.move_control_point(/*global=*/1, new_pos));

    REQUIRE(vec3_close(ed.segments()[0].p0, seg0_p0));
    REQUIRE(ed.segments()[0].p1.x == new_pos.x);
    REQUIRE(ed.segments()[0].p1.y == new_pos.y);
    REQUIRE(ed.segments()[0].p1.z == new_pos.z);
    REQUIRE(vec3_close(ed.segments()[0].p2, seg0_p2));
    REQUIRE(vec3_close(ed.segments()[0].p3, seg0_p3));
    REQUIRE(vec3_close(ed.segments()[1].p0, seg1_p0));
}

TEST_CASE("SplineEditor::translate_control_point adds a delta",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(15.0f, 25.0f));

    const auto p2_before = ed.segments()[0].p2;
    REQUIRE(ed.translate_control_point(/*global=*/2,
                                       psynder::math::Vec3{ 5.0f, 1.0f, -2.0f }));
    const auto p2_after = ed.segments()[0].p2;

    REQUIRE(std::fabs(p2_after.x - (p2_before.x + 5.0f)) < 1e-5f);
    REQUIRE(std::fabs(p2_after.y - (p2_before.y + 1.0f)) < 1e-5f);
    REQUIRE(std::fabs(p2_after.z - (p2_before.z - 2.0f)) < 1e-5f);
}

TEST_CASE("SplineEditor::delete_control_point removes the segment containing it",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));
    ed.append_segment(make_arc_segment(20.0f, 90.0f));
    REQUIRE(ed.segment_count() == 3u);

    // CP global index 5 = segment 1 (the middle one). Deleting it should
    // collapse the track to two segments and stitch them at the seam.
    REQUIRE(ed.delete_control_point(5));
    REQUIRE(ed.segment_count() == 2u);
}

TEST_CASE("SplineEditor::delete_segment stitches neighbors at the seam",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));
    ed.append_segment(make_arc_segment(20.0f, 90.0f));

    // Cache the would-be successor's p0 — the predecessor's p3 should
    // be rewritten to this value after the delete.
    const auto orig_seg2_p0 = ed.segments()[2].p0;
    REQUIRE(ed.delete_segment(1));
    REQUIRE(vec3_close(ed.segments()[0].p3, orig_seg2_p0));
}

TEST_CASE("SplineEditor::delete_segment at the edges (no neighbor to stitch)",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));

    // Deleting segment 0 (no predecessor) — should not crash, just trim.
    REQUIRE(ed.delete_segment(0));
    REQUIRE(ed.segment_count() == 1u);

    // Now delete the last remaining one.
    REQUIRE(ed.delete_segment(0));
    REQUIRE(ed.segment_count() == 0u);
}

TEST_CASE("SplineEditor::set_banking_at_t writes the correct segment",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));
    ed.append_segment(make_arc_segment(20.0f, 90.0f));
    ed.append_segment(make_arc_segment(20.0f, 120.0f));

    // 4 segments; uniform parameterization → seg 0 covers [0, 0.25),
    // seg 1 covers [0.25, 0.50), seg 2 covers [0.50, 0.75), seg 3 covers
    // [0.75, 1.0].
    REQUIRE(ed.set_banking_at_t(0.10f, 0.5f)  == 0);
    REQUIRE(ed.set_banking_at_t(0.40f, 0.7f)  == 1);
    REQUIRE(ed.set_banking_at_t(0.60f, -0.3f) == 2);
    REQUIRE(ed.set_banking_at_t(0.95f, 1.0f)  == 3);

    REQUIRE(ed.segments()[0].banking_rad ==  0.5f);
    REQUIRE(ed.segments()[1].banking_rad ==  0.7f);
    REQUIRE(ed.segments()[2].banking_rad == -0.3f);
    REQUIRE(ed.segments()[3].banking_rad ==  1.0f);
}

TEST_CASE("SplineEditor::set_banking_at_t clamps t and lands on the last segment at 1.0",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));

    REQUIRE(ed.set_banking_at_t(2.0f, 0.42f)  == 1);
    REQUIRE(ed.set_banking_at_t(-1.0f, 0.21f) == 0);
    REQUIRE(ed.segments()[0].banking_rad == 0.21f);
    REQUIRE(ed.segments()[1].banking_rad == 0.42f);
}

TEST_CASE("SplineEditor::banking_at_t round-trips with set_banking_at_t",
          "[world_outdoor][spline][editor]") {
    pwod::SplineEditor ed;
    ed.append_segment(make_arc_segment(20.0f, 30.0f));
    ed.append_segment(make_arc_segment(20.0f, 60.0f));
    ed.append_segment(make_arc_segment(20.0f, 90.0f));
    ed.set_banking_at_t(0.20f, 0.50f);
    ed.set_banking_at_t(0.55f, 0.70f);
    REQUIRE(ed.banking_at_t(0.20f) == 0.50f);
    REQUIRE(ed.banking_at_t(0.55f) == 0.70f);
}

TEST_CASE("SplineEditor constructs over an existing segment vector",
          "[world_outdoor][spline][editor]") {
    std::vector<pwo::SplineRoadSegment> initial;
    initial.push_back(make_arc_segment(10.0f, 45.0f));
    initial.push_back(make_arc_segment(15.0f, 60.0f));

    pwod::SplineEditor ed(std::move(initial));
    REQUIRE(ed.segment_count() == 2u);
    REQUIRE(ed.control_point_count() == 8u);
}
