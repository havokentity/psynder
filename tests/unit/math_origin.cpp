// SPDX-License-Identifier: MIT
// Psynder — Lane 02 origin re-centering tests.

#include "math/Origin.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::math;

namespace {
bool approx_eq(f32 a, f32 b, f32 tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}
}  // namespace

TEST_CASE("origin_recenter is a no-op when inside the trigger radius", "[math][origin]") {
    Vec3 world_origin{0, 0, 0};
    Vec3 camera{10, 20, 30};

    auto r = origin_recenter(world_origin, camera);
    REQUIRE_FALSE(r.changed);
    REQUIRE(approx_eq(r.shift.x, 0.0f));
    REQUIRE(approx_eq(r.shift.y, 0.0f));
    REQUIRE(approx_eq(r.shift.z, 0.0f));
    REQUIRE(approx_eq(world_origin.x, 0.0f));
    REQUIRE(approx_eq(world_origin.y, 0.0f));
    REQUIRE(approx_eq(world_origin.z, 0.0f));
}

TEST_CASE("origin_recenter snaps to the cell containing the camera", "[math][origin]") {
    Vec3 world_origin{0, 0, 0};
    Vec3 camera{1500, 0, -2200};  // far beyond the default 512 m trigger

    OriginRecenterConfig cfg{};  // 1024 m cells, 512 m trigger
    auto r = origin_recenter(world_origin, camera, cfg);
    REQUIRE(r.changed);

    // After snapping, the camera relative to the new origin should sit
    // inside a single cell — |camera - origin| < cell_size for each axis.
    f32 dx = camera.x - world_origin.x;
    f32 dy = camera.y - world_origin.y;
    f32 dz = camera.z - world_origin.z;
    REQUIRE(std::fabs(dx) <= cfg.cell_size);
    REQUIRE(std::fabs(dy) <= cfg.cell_size);
    REQUIRE(std::fabs(dz) <= cfg.cell_size);

    // The new origin is aligned to the cell grid.
    REQUIRE(approx_eq(std::fmod(world_origin.x, cfg.cell_size), 0.0f, 1e-3f));
    REQUIRE(approx_eq(std::fmod(world_origin.z, cfg.cell_size), 0.0f, 1e-3f));
}

TEST_CASE("origin_recenter reports the applied shift", "[math][origin]") {
    Vec3 world_origin{0, 0, 0};
    Vec3 camera{2048, 0, 0};  // exact multiple of cell_size = 1024

    auto r = origin_recenter(world_origin, camera);
    REQUIRE(r.changed);
    REQUIRE(approx_eq(r.shift.x, 2048.0f));
    REQUIRE(approx_eq(r.shift.y, 0.0f));
    REQUIRE(approx_eq(r.shift.z, 0.0f));
    REQUIRE(approx_eq(world_origin.x, 2048.0f));
}

TEST_CASE("origin_would_recenter agrees with origin_recenter", "[math][origin]") {
    OriginRecenterConfig cfg{};
    Vec3 origin{0, 0, 0};

    // Just inside the trigger: should NOT fire.
    Vec3 near_cam{cfg.trigger_distance - 1.0f, 0, 0};
    REQUIRE_FALSE(origin_would_recenter(origin, near_cam, cfg));

    // Just beyond it: should fire.
    Vec3 far_cam{cfg.trigger_distance + 10.0f, 0, 0};
    REQUIRE(origin_would_recenter(origin, far_cam, cfg));
}

TEST_CASE("origin_recenter respects a custom cell size", "[math][origin]") {
    OriginRecenterConfig cfg{256.0f, 128.0f};
    Vec3 origin{0, 0, 0};
    Vec3 camera{1000, 0, 0};

    auto r = origin_recenter(origin, camera, cfg);
    REQUIRE(r.changed);
    // Snap to the nearest 256 m. 1000 → 1024 (closer than 768).
    REQUIRE(approx_eq(origin.x, 1024.0f));
}
