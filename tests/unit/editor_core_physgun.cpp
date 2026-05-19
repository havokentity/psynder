// SPDX-License-Identifier: MIT
// Psynder — Lane 18 unit test: physgun pick / drag / rotate / weld.
// Header-only inclusion of editor::physgun::* lets the test exercise the
// pure ray-AABB pick + pose-compose helpers without linking against the
// editor library.

#include "editor/core/Physgun.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::editor;

namespace {

constexpr f32 kEps = 1e-4f;

PSY_FORCEINLINE bool approx_eq(f32 a, f32 b) noexcept {
    const f32 d = a - b;
    return d > -kEps && d < kEps;
}

physgun::PickInput make_body(u32 id, f32 x, f32 y, f32 z, f32 he = 0.5f) {
    physgun::PickInput p;
    p.id          = id;
    p.position    = math::Vec3{x, y, z};
    p.half_extent = math::Vec3{he, he, he};
    return p;
}

}  // namespace

TEST_CASE("physgun: ray-AABB intersect picks the nearest body",
          "[editor][physgun][pick]") {
    std::vector<physgun::PickInput> bodies = {
        make_body(1, 0, 0, -3),     // straight ahead, 3 m away
        make_body(2, 0, 0, -6),     // further along the same ray
        make_body(3, 5, 0,  0),     // off to one side
    };

    const physgun::PickResult r =
        physgun::pick(bodies, math::Vec3{0,0,0}, math::Vec3{0,0,-1});
    REQUIRE(r.body_id == 1);
    REQUIRE(approx_eq(r.hit_distance, 2.5f));  // ray enters at -2.5 along -Z
}

TEST_CASE("physgun: ray that misses every AABB returns 0",
          "[editor][physgun][pick]") {
    std::vector<physgun::PickInput> bodies = {
        make_body(7, 10, 10, 10),
    };

    const physgun::PickResult r =
        physgun::pick(bodies, math::Vec3{0,0,0}, math::Vec3{1,0,0});
    REQUIRE(r.body_id == 0);
}

TEST_CASE("physgun: ray behind the camera is rejected", "[editor][physgun][pick]") {
    std::vector<physgun::PickInput> bodies = {
        make_body(1, 0, 0,  5),   // body is in +Z (behind, if we look at -Z)
    };

    const physgun::PickResult r =
        physgun::pick(bodies, math::Vec3{0,0,0}, math::Vec3{0,0,-1});
    REQUIRE(r.body_id == 0);
}

TEST_CASE("physgun: compute_pose places body at cursor + grab_distance",
          "[editor][physgun][drag]") {
    physgun::State s;
    s.body_id       = 5;
    s.cursor_world  = math::Vec3{0,0,0};
    s.orient        = math::Quat{0,0,0,1};
    s.scale         = math::Vec3{1,1,1};
    s.active        = true;

    const physgun::PoseTarget t =
        physgun::compute_pose(s,
                              /*origin=*/math::Vec3{0, 1, 0},
                              /*dir=*/math::Vec3{0, 0,-1},
                              /*grab_distance=*/4.0f,
                              math::Quat{0,0,0,1},
                              /*delta_scale=*/1.0f);
    REQUIRE(approx_eq(t.position.x,  0.0f));
    REQUIRE(approx_eq(t.position.y,  1.0f));
    REQUIRE(approx_eq(t.position.z, -4.0f));
    REQUIRE(approx_eq(t.scale.x, 1.0f));
}

TEST_CASE("physgun: compute_pose scales multiplicatively",
          "[editor][physgun][drag]") {
    physgun::State s;
    s.scale = math::Vec3{2.0f, 2.0f, 2.0f};

    const physgun::PoseTarget t =
        physgun::compute_pose(s, math::Vec3{0,0,0}, math::Vec3{0,0,-1},
                              0.5f, math::Quat{0,0,0,1}, /*delta_scale=*/1.5f);
    REQUIRE(approx_eq(t.scale.x, 3.0f));
    REQUIRE(approx_eq(t.scale.y, 3.0f));
    REQUIRE(approx_eq(t.scale.z, 3.0f));
}

TEST_CASE("physgun: weld request midpoints the two body positions",
          "[editor][physgun][weld]") {
    const physgun::WeldRequest w =
        physgun::make_weld(11, 22,
                           math::Vec3{ 4, 0, 0},
                           math::Vec3{-2, 0, 0});
    REQUIRE(w.body_a == 11);
    REQUIRE(w.body_b == 22);
    REQUIRE(approx_eq(w.anchor.x, 1.0f));
    REQUIRE(approx_eq(w.anchor.y, 0.0f));
    REQUIRE(approx_eq(w.anchor.z, 0.0f));
}

TEST_CASE("physgun: is_active is false on cold state and after a drop",
          "[editor][physgun][lifecycle]") {
    physgun::State s;
    REQUIRE_FALSE(physgun::is_active(s));

    s.body_id = 9;
    s.active  = true;
    REQUIRE(physgun::is_active(s));

    s.body_id = 0;   // drop
    REQUIRE_FALSE(physgun::is_active(s));
}
