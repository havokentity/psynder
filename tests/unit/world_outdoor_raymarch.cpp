// SPDX-License-Identifier: MIT
// Psynder — Lane 11 unit tests for the heightmap raymarcher (Backend B).
//
// The §9.2 invariant we pin here: **the raymarcher hits the heightmap
// surface correctly**. Given a synthetic heightmap whose closed-form Y(x,z)
// we know exactly, a ray cast from above must hit at a (t, pos) whose Y
// coordinate agrees with the heightmap to within sub-texel error.
//
// We also check the projection helper and the per-column step kernel —
// these are the load-bearing math the Wave-B SIMD lift will batch up.

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Raymarch_internal.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace pwo  = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

namespace {

// A linear ramp heightmap: terrain Y rises along +X. With spacing=1,
// height_scale=1, and h[x][z] = ramp_slope * x, we have a perfectly known
// surface to hit-test against.
std::vector<std::uint16_t> ramp_heightmap(std::uint32_t w, std::uint32_t h,
                                          std::uint16_t slope_per_step) {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (std::uint32_t z = 0; z < h; ++z) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t val = static_cast<std::uint32_t>(x) * slope_per_step;
            out[static_cast<std::size_t>(z) * w + x] =
                static_cast<std::uint16_t>(val > 0xFFFFu ? 0xFFFFu : val);
        }
    }
    return out;
}

// Flat heightmap at a constant level. Used to sanity-check the
// "first-step-below" path of march_ray.
std::vector<std::uint16_t> flat_heightmap(std::uint32_t w, std::uint32_t h,
                                          std::uint16_t level) {
    return std::vector<std::uint16_t>(static_cast<std::size_t>(w) * h, level);
}

}  // namespace

TEST_CASE("raymarch hits a flat heightmap at the expected distance",
          "[world_outdoor][raymarch]") {
    const std::uint32_t W = 64;
    const std::uint32_t H = 64;
    auto raw = flat_heightmap(W, H, 100);   // h_world = 100 * height_scale

    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.1f;   // → ground at y = 10.0
    desc.heights      = raw.data();

    // Eye at (10, 50, 10), ray pointing straight down. Should hit at y=10.
    // Expected ray param t = (50 - 10) / 1 = 40.
    pwod::RayHit hit{};
    const bool ok = pwod::march_ray(
        desc,
        psynder::math::Vec3{10.0f, 50.0f, 10.0f},
        psynder::math::Vec3{0.0f,  -1.0f, 0.0f},
        0.5f,   // step
        100.0f, // max_t
        hit);
    REQUIRE(ok);
    REQUIRE(hit.t > 39.5f);
    REQUIRE(hit.t < 40.5f);
    REQUIRE(hit.pos.y > 9.0f);
    REQUIRE(hit.pos.y < 11.0f);
}

TEST_CASE("raymarch hits a linear ramp heightmap correctly",
          "[world_outdoor][raymarch]") {
    const std::uint32_t W = 128;
    const std::uint32_t H = 128;
    // Slope: 100 units per texel → with height_scale=0.05, slope=5 m/texel.
    auto raw = ramp_heightmap(W, H, 100);

    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.05f;
    desc.heights      = raw.data();

    // Eye at (0, 30, 5), ray heading along +X. The terrain at x has
    // Y(x) = 5*x, so a horizontal ray at Y=30 hits at x=6 (Y=30=5*6).
    pwod::RayHit hit{};
    const bool ok = pwod::march_ray(
        desc,
        psynder::math::Vec3{0.0f, 30.0f, 5.0f},
        psynder::math::Vec3{1.0f, 0.0f,  0.0f},
        0.25f,
        100.0f,
        hit);
    REQUIRE(ok);
    // Expected hit at x ≈ 6 (with some bilinear smoothing leeway).
    REQUIRE(hit.pos.x > 5.5f);
    REQUIRE(hit.pos.x < 6.5f);
    // The ray was horizontal, so the hit Y should match the eye Y.
    REQUIRE(hit.pos.y > 29.5f);
    REQUIRE(hit.pos.y < 30.5f);
    // And the terrain at that hit point should be at the same Y (within
    // sub-texel error — bilinear sample under the hit).
    const float terrain_y = pwod::sample_bilinear(desc, hit.pos.x, hit.pos.z);
    REQUIRE(std::fabs(terrain_y - hit.pos.y) < 0.5f);
}

TEST_CASE("raymarch returns false when the ray misses",
          "[world_outdoor][raymarch]") {
    const std::uint32_t W = 32;
    const std::uint32_t H = 32;
    auto raw = flat_heightmap(W, H, 50);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.1f;   // ground at 5
    desc.heights      = raw.data();

    pwod::RayHit hit{};
    // Ray points UP from above the terrain — never hits.
    const bool ok = pwod::march_ray(
        desc,
        psynder::math::Vec3{16.0f, 20.0f, 16.0f},
        psynder::math::Vec3{0.0f,  1.0f,  0.0f},
        0.5f, 100.0f, hit);
    REQUIRE_FALSE(ok);
}

TEST_CASE("raymarch detects already-underground origin as t=0 hit",
          "[world_outdoor][raymarch]") {
    const std::uint32_t W = 16;
    const std::uint32_t H = 16;
    auto raw = flat_heightmap(W, H, 200);
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.1f;   // ground at 20
    desc.heights      = raw.data();

    pwod::RayHit hit{};
    // Origin below the terrain — instant hit at t=0.
    const bool ok = pwod::march_ray(
        desc,
        psynder::math::Vec3{8.0f, 5.0f, 8.0f},
        psynder::math::Vec3{1.0f, 0.0f, 0.0f},
        0.5f, 100.0f, hit);
    REQUIRE(ok);
    REQUIRE(hit.t == 0.0f);
}

TEST_CASE("project_y maps height to screen Y predictably",
          "[world_outdoor][raymarch]") {
    // A point AT the eye Y should land on the horizon (fb_height/2).
    const int y_horizon = pwod::project_y(/*height_world=*/10.0f,
                                          /*camera_y=*/    10.0f,
                                          /*distance=*/    50.0f,
                                          /*ppx_at_unit=*/ 100.0f,
                                          /*fb_height=*/   200);
    REQUIRE(y_horizon == 100);

    // A point ABOVE the eye should project higher (smaller screen Y).
    const int y_above = pwod::project_y(20.0f, 10.0f, 50.0f, 100.0f, 200);
    REQUIRE(y_above < 100);

    // A point BELOW the eye should project lower (larger screen Y).
    const int y_below = pwod::project_y(0.0f, 10.0f, 50.0f, 100.0f, 200);
    REQUIRE(y_below > 100);
}

TEST_CASE("per-column step advances horizon on a rising silhouette",
          "[world_outdoor][raymarch]") {
    const std::uint32_t W = 256;
    const std::uint32_t H = 4;
    // Two-plateau heightmap: low for x<128, high for x>=128. As the column
    // sweeps over the boundary, the high plateau's silhouette projects
    // higher on the screen (smaller screen Y), advancing the horizon.
    std::vector<std::uint16_t> raw(static_cast<std::size_t>(W) * H, 0);
    for (std::uint32_t z = 0; z < H; ++z) {
        for (std::uint32_t x = 0; x < W; ++x) {
            raw[static_cast<std::size_t>(z) * W + x] = (x >= 128) ? 1000 : 100;
        }
    }
    pwo::HeightmapDesc desc{};
    desc.size_x       = W;
    desc.size_z       = H;
    desc.spacing      = 1.0f;
    desc.height_scale = 0.1f;   // low=10, high=100
    desc.heights      = raw.data();

    pwod::ColumnState col{};
    col.horizon_y = 200;
    col.column_x  = 0;
    col.ray_dir_x = 1.0f;
    col.ray_dir_z = 0.0f;

    const float ppx = 100.0f;
    const auto eye  = psynder::math::Vec3{0.0f, 110.0f, 2.0f};

    // Step at t=50 (over the low plateau): the low terrain pokes above
    // the horizon, advancing it some.
    auto step_lo = pwod::step_column(desc, col, eye, 50.0f, /*fb_h=*/200, ppx);
    REQUIRE(step_lo.new_horizon_y < col.horizon_y);
    const std::int32_t lo_horizon = step_lo.new_horizon_y;

    // Reset horizon and step at t=140 over the high plateau: the high
    // terrain projects much higher (much smaller screen Y), so the
    // advance is much larger than from the low plateau.
    auto step_hi = pwod::step_column(desc, col, eye, 140.0f, /*fb_h=*/200, ppx);
    REQUIRE(step_hi.new_horizon_y < lo_horizon);
    REQUIRE(step_hi.strip_bottom_y >= step_hi.strip_top_y);
    REQUIRE(step_hi.packed_color != 0u);

    // And once a column has painted past the high plateau's screen Y,
    // a subsequent step over the LOWER terrain must NOT regress the
    // horizon — that's the horizon invariant the per-column march
    // relies on.
    pwod::ColumnState col_hi = col;
    col_hi.horizon_y = step_hi.new_horizon_y;
    auto step_lo2 = pwod::step_column(desc, col_hi, eye, 50.0f, /*fb_h=*/200, ppx);
    REQUIRE(step_lo2.new_horizon_y == col_hi.horizon_y);
}

TEST_CASE("logstep grows with distance", "[world_outdoor][raymarch]") {
    pwo::HeightmapDesc desc{};
    desc.spacing = 1.0f;
    const float near_step = 0.5f;
    const float falloff   = 50.0f;
    const float s_near = pwod::logstep_size(desc, 1.0f,   near_step, falloff);
    const float s_far  = pwod::logstep_size(desc, 500.0f, near_step, falloff);
    REQUIRE(s_far > s_near);
    REQUIRE(s_near > 0.0f);
}
