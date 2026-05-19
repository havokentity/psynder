// SPDX-License-Identifier: MIT
// Psynder — Lane 18 unit test: constraint-graph debug visualization.
// Drives the header-only viz core in ConstraintsViz.h, asserting that each
// kind emits the expected number of screen-space line segments.

#include "editor/core/Constraints.h"
#include "editor/core/ConstraintsViz.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::editor;

namespace {

viz::Camera make_camera() {
    viz::Camera c;
    c.eye        = math::Vec3{0.0f, 4.0f, 8.0f};
    c.target     = math::Vec3{0.0f, 0.0f, 0.0f};
    c.up         = math::Vec3{0.0f, 1.0f, 0.0f};
    c.fov_y_rad  = 1.0472f;
    c.aspect     = 16.0f / 9.0f;
    c.near_z     = 0.1f;
    c.far_z      = 200.0f;
    c.viewport_w = 1920.0f;
    c.viewport_h = 1080.0f;
    return c;
}

std::vector<viz::WorldBodyPose> two_bodies() {
    std::vector<viz::WorldBodyPose> out;
    viz::WorldBodyPose a; a.id = 1; a.position = {-1.0f, 0.0f, 0.0f}; out.push_back(a);
    viz::WorldBodyPose b; b.id = 2; b.position = { 1.0f, 0.0f, 0.0f}; out.push_back(b);
    return out;
}

}  // namespace

TEST_CASE("viz: project_point lands inside the viewport for a point in front of the eye",
          "[editor][constraints][viz]") {
    const auto cam = make_camera();
    const viz::Projected p = viz::project_point(cam, math::Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(p.visible);
    REQUIRE(p.screen.x > 0.0f);
    REQUIRE(p.screen.x < cam.viewport_w);
    REQUIRE(p.screen.y > 0.0f);
    REQUIRE(p.screen.y < cam.viewport_h);
}

TEST_CASE("viz: project_point reports invisible for a point behind the eye",
          "[editor][constraints][viz]") {
    const auto cam = make_camera();
    // Eye at (0,4,8), target at origin → forward = -Z-ish.
    // (0, 4, 100) sits BEHIND the eye.
    const viz::Projected p = viz::project_point(cam, math::Vec3{0.0f, 4.0f, 100.0f});
    REQUIRE_FALSE(p.visible);
}

TEST_CASE("viz: Weld constraint emits 2 segments (the spine + a 1-px shadow)",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();
    constraints::Constraint c = constraints::make_weld(1, 2, math::Vec3{0,0,0});
    c.anchor_a = math::Vec3{0,0,0};
    c.anchor_b = math::Vec3{0,0,0};

    std::vector<viz::ScreenSegment> segs;
    viz::build_constraint_lines(segs, cam, c, bodies);
    REQUIRE(segs.size() == 2);
    REQUIRE(segs[0].rgba == viz::color_for_kind(constraints::Kind::Weld));
}

TEST_CASE("viz: Axis constraint emits 2 segments (spine + axis tick)",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();
    constraints::Constraint c = constraints::make_axis(1, 2,
                                                       math::Vec3{0,0,0},
                                                       math::Vec3{0,1,0},
                                                       -math::kPi, math::kPi);
    c.anchor_a = math::Vec3{0,0,0};
    c.anchor_b = math::Vec3{0,0,0};

    std::vector<viz::ScreenSegment> segs;
    viz::build_constraint_lines(segs, cam, c, bodies);
    REQUIRE(segs.size() == 2);
}

TEST_CASE("viz: BallSocket constraint emits 3 segments (spine + cross)",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();
    constraints::Constraint c = constraints::make_ball_socket(1, 2,
                                                              math::Vec3{0,0,0});
    c.anchor_a = math::Vec3{0,0,0};
    c.anchor_b = math::Vec3{0,0,0};

    std::vector<viz::ScreenSegment> segs;
    viz::build_constraint_lines(segs, cam, c, bodies);
    REQUIRE(segs.size() == 3);
}

TEST_CASE("viz: Slider constraint emits 3 segments (spine + 2 perpendicular ticks)",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();
    constraints::Constraint c = constraints::make_slider(1, 2,
                                                         math::Vec3{0,0,0},
                                                         math::Vec3{1,0,0},
                                                         -0.5f, 0.5f);
    c.anchor_a = math::Vec3{0,0,0};
    c.anchor_b = math::Vec3{0,0,0};

    std::vector<viz::ScreenSegment> segs;
    viz::build_constraint_lines(segs, cam, c, bodies);
    REQUIRE(segs.size() == 3);
}

TEST_CASE("viz: Rope constraint emits 1 segment (spine only)",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();
    constraints::Constraint c = constraints::make_rope(1, 2,
                                                       math::Vec3{0,0,0},
                                                       math::Vec3{0,0,0},
                                                       /*max_length=*/2.0f);

    std::vector<viz::ScreenSegment> segs;
    viz::build_constraint_lines(segs, cam, c, bodies);
    REQUIRE(segs.size() == 1);
}

TEST_CASE("viz: Elastic constraint emits 2 segments (spine + mid-dash)",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();
    constraints::Constraint c = constraints::make_elastic(1, 2,
                                                          math::Vec3{0,0,0},
                                                          math::Vec3{0,0,0},
                                                          /*rest=*/2.0f,
                                                          /*stiffness=*/100.0f,
                                                          /*damping=*/10.0f);

    std::vector<viz::ScreenSegment> segs;
    viz::build_constraint_lines(segs, cam, c, bodies);
    REQUIRE(segs.size() == 2);
}

TEST_CASE("viz: full graph builder runs over every kind without crashing",
          "[editor][constraints][viz]") {
    const auto cam    = make_camera();
    const auto bodies = two_bodies();

    constraints::Graph g;
    g.add(constraints::make_weld(1, 2, math::Vec3{0,0,0}));
    g.add(constraints::make_axis(1, 2, math::Vec3{0,0,0}, math::Vec3{0,1,0}, -1, 1));
    g.add(constraints::make_slider(1, 2, math::Vec3{0,0,0}, math::Vec3{1,0,0}, -1, 1));
    g.add(constraints::make_ball_socket(1, 2, math::Vec3{0,0,0}));
    g.add(constraints::make_rope(1, 2, math::Vec3{0,0,0}, math::Vec3{0,0,0}, 2.0f));
    g.add(constraints::make_elastic(1, 2, math::Vec3{0,0,0}, math::Vec3{0,0,0}, 1.0f, 50.0f, 1.0f));

    std::vector<viz::ScreenSegment> segs;
    viz::build_graph_lines(segs, cam, g, bodies);
    // weld(2) + axis(2) + slider(3) + ball(3) + rope(1) + elastic(2) = 13
    REQUIRE(segs.size() == 13);
}

TEST_CASE("viz: kind palette is unique per constraint kind",
          "[editor][constraints][viz]") {
    const u32 weld   = viz::color_for_kind(constraints::Kind::Weld);
    const u32 axis   = viz::color_for_kind(constraints::Kind::Axis);
    const u32 slider = viz::color_for_kind(constraints::Kind::Slider);
    const u32 ball   = viz::color_for_kind(constraints::Kind::BallSocket);
    const u32 rope   = viz::color_for_kind(constraints::Kind::Rope);
    const u32 ela    = viz::color_for_kind(constraints::Kind::Elastic);
    REQUIRE(weld != axis);
    REQUIRE(weld != slider);
    REQUIRE(weld != ball);
    REQUIRE(weld != rope);
    REQUIRE(weld != ela);
    REQUIRE(axis != slider);
    REQUIRE(slider != ball);
    REQUIRE(ball != rope);
    REQUIRE(rope != ela);
}
