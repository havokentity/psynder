// SPDX-License-Identifier: MIT
// Psynder — Lane 08 unit test (Wave B):
// Heightmap shadow raymarcher correctness. We test two fixtures per
// the brief: a flat heightfield and a ramp heightfield. The march must
// (a) miss when the ray flies above the surface, (b) hit when the ray
// dips into the surface, and (c) handle both ramp orientations.

#include <catch2/catch_test_macros.hpp>

#include "render/rt/HeightmapShadow.h"

#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::render::rt;

namespace {

struct FlatHm {
    std::vector<f32> data;
    Heightmap hm;
    explicit FlatHm(u32 w, u32 h, f32 y, f32 cell = 1.0f) {
        data.assign(static_cast<size_t>(w) * h, y);
        hm.y_data    = data.data();
        hm.width     = w;
        hm.height    = h;
        hm.origin_xz = { 0.0f, 0.0f };
        hm.cell_size = cell;
        hm.y_min     = y;
        hm.y_max     = y;
    }
};

struct RampHm {
    std::vector<f32> data;
    Heightmap hm;
    // Linear ramp y(x, z) = base + slope_x * x_world + slope_z * z_world.
    // Across the (w, h) grid with cell=1, x_world ∈ [0, w-1].
    RampHm(u32 w, u32 h, f32 base, f32 slope_x, f32 slope_z, f32 cell = 1.0f) {
        data.assign(static_cast<size_t>(w) * h, 0.0f);
        f32 ymin = +1e30f, ymax = -1e30f;
        for (u32 j = 0; j < h; ++j) {
            for (u32 i = 0; i < w; ++i) {
                const f32 x = cell * static_cast<f32>(i);
                const f32 z = cell * static_cast<f32>(j);
                const f32 y = base + slope_x * x + slope_z * z;
                data[j * w + i] = y;
                ymin = std::min(ymin, y);
                ymax = std::max(ymax, y);
            }
        }
        hm.y_data    = data.data();
        hm.width     = w;
        hm.height    = h;
        hm.origin_xz = { 0.0f, 0.0f };
        hm.cell_size = cell;
        hm.y_min     = ymin;
        hm.y_max     = ymax;
    }
};

Ray make_ray(math::Vec3 o, math::Vec3 d, f32 t_max = 1e30f) {
    Ray r;
    r.origin    = o;
    r.direction = d;
    r.t_min     = 1e-4f;
    r.t_max     = t_max;
    return r;
}

}  // namespace

// ─── Flat fixture ────────────────────────────────────────────────────────

TEST_CASE("Heightmap shadow flat: horizontal ray above the surface — miss",
          "[render_rt][heightmap_shadow][flat]") {
    FlatHm fx(16, 16, /*y=*/2.0f);
    // Ray at y=5 moving in +X (entirely above the y=2 plane).
    Ray r = make_ray({0.5f, 5.0f, 8.0f}, {1.0f, 0.0f, 0.0f});
    REQUIRE_FALSE(trace_heightmap_shadow(fx.hm, r));
}

TEST_CASE("Heightmap shadow flat: ray dipping into surface — hit",
          "[render_rt][heightmap_shadow][flat]") {
    FlatHm fx(16, 16, /*y=*/2.0f);
    // Ray starts above (y=5) and points down +X +Y_neg into the plane.
    Ray r = make_ray({0.5f, 5.0f, 8.0f},
                     math::normalize(math::Vec3{1.0f, -1.0f, 0.0f}));
    REQUIRE(trace_heightmap_shadow(fx.hm, r));
}

TEST_CASE("Heightmap shadow flat: ray pointing up away from surface — miss",
          "[render_rt][heightmap_shadow][flat]") {
    FlatHm fx(16, 16, /*y=*/2.0f);
    Ray r = make_ray({0.5f, 5.0f, 8.0f},
                     math::normalize(math::Vec3{1.0f, 1.0f, 0.0f}));
    REQUIRE_FALSE(trace_heightmap_shadow(fx.hm, r));
}

TEST_CASE("Heightmap shadow flat: ray with t_max clamped before surface — miss",
          "[render_rt][heightmap_shadow][flat]") {
    FlatHm fx(16, 16, /*y=*/2.0f);
    // The ray would hit the surface at t=3 (descending 1 unit per t), but
    // t_max=0.5 clips it well before.
    Ray r = make_ray({0.5f, 5.0f, 8.0f},
                     math::normalize(math::Vec3{0.1f, -1.0f, 0.0f}),
                     /*t_max=*/0.5f);
    REQUIRE_FALSE(trace_heightmap_shadow(fx.hm, r));
}

TEST_CASE("Heightmap shadow flat: ray starting below the surface — hit",
          "[render_rt][heightmap_shadow][flat]") {
    FlatHm fx(16, 16, /*y=*/5.0f);
    Ray r = make_ray({0.5f, 1.0f, 8.0f},  // below y=5
                     {1.0f, 0.0f, 0.0f});
    REQUIRE(trace_heightmap_shadow(fx.hm, r));
}

// ─── Ramp fixture ────────────────────────────────────────────────────────

TEST_CASE("Heightmap shadow ramp: ray above the rising ramp — miss",
          "[render_rt][heightmap_shadow][ramp]") {
    // y rises from 0 at x=0 to 15 at x=15; grid is 16 cells wide.
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f, /*slope_z=*/0.0f);
    // Horizontal ray at y=20 — always above the ramp's max=15.
    Ray r = make_ray({1.0f, 20.0f, 8.0f}, {1.0f, 0.0f, 0.0f});
    REQUIRE_FALSE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow ramp: horizontal ray clipped by the ramp — hit",
          "[render_rt][heightmap_shadow][ramp]") {
    // y rises from 0 to 15. A horizontal ray at y=5 will pass at low x
    // (where ramp y < 5) but the ramp rises through y=5 at x=5. At x≥5
    // the ramp is above the ray → occlusion.
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f, /*slope_z=*/0.0f);
    Ray r = make_ray({0.5f, 5.0f, 8.0f}, {1.0f, 0.0f, 0.0f});
    REQUIRE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow ramp: ray parallel to the ramp slope — miss",
          "[render_rt][heightmap_shadow][ramp]") {
    // Ramp slope +x: each +1 x adds +1 y. Ray rising at the same slope
    // (dy/dx = 1) just above the surface should miss.
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f, /*slope_z=*/0.0f);
    // Start at (x=1, y=1.5) — surface at x=1 is y=1, so we're 0.5 above.
    Ray r = make_ray({1.0f, 1.5f, 8.0f},
                     math::normalize(math::Vec3{1.0f, 1.0f, 0.0f}));
    REQUIRE_FALSE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow ramp: ray descending into ramp — hit",
          "[render_rt][heightmap_shadow][ramp]") {
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/0.0f, /*slope_z=*/1.0f);
    // Surface rises along +Z. Ray starts at y=10 moving in +Z and slightly
    // -Y so it eventually drops below the ramp surface.
    Ray r = make_ray({4.0f, 10.0f, 0.5f},
                     math::normalize(math::Vec3{0.0f, -0.5f, 1.0f}));
    REQUIRE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow ramp: ray entirely below ramp peak but starting under — hit",
          "[render_rt][heightmap_shadow][ramp]") {
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f, /*slope_z=*/0.0f);
    // Ramp y(x=10) = 10. Start ray at (10, 5, 8) — under the surface.
    Ray r = make_ray({10.0f, 5.0f, 8.0f}, {1.0f, 0.0f, 0.0f});
    REQUIRE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow ramp: vertical ray hits below — hit",
          "[render_rt][heightmap_shadow][ramp]") {
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f, /*slope_z=*/0.0f);
    // Surface at (x=5, z=5) is y=5. Vertical ray at (5,10) → (5,0): hits.
    Ray r = make_ray({5.0f, 10.0f, 5.0f}, {0.0f, -1.0f, 0.0f});
    REQUIRE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow ramp: vertical ray going up — miss",
          "[render_rt][heightmap_shadow][ramp]") {
    RampHm rx(16, 16, /*base=*/0.0f, /*slope_x=*/1.0f, /*slope_z=*/0.0f);
    // Surface at (5, 5) is y=5. Vertical ray starting at y=10 going up
    // never re-enters the surface.
    Ray r = make_ray({5.0f, 10.0f, 5.0f}, {0.0f, 1.0f, 0.0f});
    REQUIRE_FALSE(trace_heightmap_shadow(rx.hm, r));
}

TEST_CASE("Heightmap shadow flat: degenerate / null heightmap returns miss",
          "[render_rt][heightmap_shadow][edge]") {
    Heightmap empty{};
    Ray r = make_ray({0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    REQUIRE_FALSE(trace_heightmap_shadow(empty, r));
}
