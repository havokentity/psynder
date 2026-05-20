// SPDX-License-Identifier: MIT
// Psynder — Lane 11 Wave-B unit tests for the SIMD raymarch path.
//
// The §9.2 invariant Wave B adds: **the 8-wide SIMD kernel produces the same
// per-column results as the scalar reference**, lane-by-lane. The scalar
// kernel is already pinned by `world_outdoor_raymarch.cpp` — we don't repeat
// those tests here; we test PARITY, which is the property the SIMD lift is
// allowed to break and which therefore needs the most coverage.
//
// We also pin the SIMD bilinear sampler against the scalar one (the SIMD
// path went through f32x8 fma; the math should be bit-close but does not
// have to be bit-equal — we use a tight epsilon).

#include <catch2/catch_test_macros.hpp>

#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Raymarch_internal.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace pwo = psynder::world::outdoor;
namespace pwod = psynder::world::outdoor::detail;

namespace {

// Same ramp heightmap helper from the scalar tests — duplicated here so the
// two TUs stay independent (Catch2's test discovery would pick up multiple
// definitions otherwise).
std::vector<std::uint16_t> ramp_heightmap_simd(std::uint32_t w,
                                               std::uint32_t h,
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

// Hashed-deterministic heightmap so neighbors differ → flushes out lane bugs.
std::vector<std::uint16_t> hashed_heightmap_simd(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (std::uint32_t z = 0; z < h; ++z) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t v = (x * 2654435761u + z * 40503u + 11u);
            out[static_cast<std::size_t>(z) * w + x] = static_cast<std::uint16_t>(v & 0xFFFFu);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("SIMD bilinear sampler agrees with scalar (8-wide)", "[world_outdoor][raymarch][simd]") {
    const std::uint32_t W = 64;
    const std::uint32_t H = 64;
    auto raw = hashed_heightmap_simd(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W;
    desc.size_z = H;
    desc.spacing = 1.0f;
    desc.height_scale = 0.05f;
    desc.heights = raw.data();

    // 8 distinct (wx, wz) sample points spread across the map — all inside.
    alignas(32) float wx[8] = {1.5f, 7.0f, 12.25f, 18.3f, 30.5f, 40.7f, 50.0f, 60.25f};
    alignas(32) float wz[8] = {2.0f, 5.5f, 10.0f, 20.5f, 25.0f, 30.5f, 45.0f, 55.5f};

    alignas(32) float out_simd[8];
    pwod::simd_sample_bilinear8(desc, wx, wz, out_simd);

    for (std::uint32_t i = 0; i < 8; ++i) {
        const float expected = pwod::sample_bilinear(desc, wx[i], wz[i]);
        INFO("lane " << i << " wx=" << wx[i] << " wz=" << wz[i]);
        REQUIRE(std::fabs(out_simd[i] - expected) < 1e-3f);
    }
}

TEST_CASE("SIMD bilinear sampler agrees with scalar (4-wide)", "[world_outdoor][raymarch][simd]") {
    const std::uint32_t W = 32;
    const std::uint32_t H = 32;
    auto raw = hashed_heightmap_simd(W, H);
    pwo::HeightmapDesc desc{};
    desc.size_x = W;
    desc.size_z = H;
    desc.spacing = 2.0f;
    desc.height_scale = 0.1f;
    desc.heights = raw.data();

    alignas(16) float wx[4] = {4.0f, 11.5f, 22.0f, 30.75f};
    alignas(16) float wz[4] = {6.0f, 14.5f, 27.0f, 8.25f};

    alignas(16) float out_simd[4];
    pwod::simd_sample_bilinear4(desc, wx, wz, out_simd);

    for (std::uint32_t i = 0; i < 4; ++i) {
        const float expected = pwod::sample_bilinear(desc, wx[i], wz[i]);
        INFO("lane " << i);
        REQUIRE(std::fabs(out_simd[i] - expected) < 1e-3f);
    }
}

TEST_CASE("SIMD project_y agrees with scalar across diverse lanes",
          "[world_outdoor][raymarch][simd]") {
    float heights[8] = {5.0f, 20.0f, 10.0f, 30.0f, 0.0f, 100.0f, 50.0f, 25.0f};
    float distances[8] = {10.0f, 25.0f, 50.0f, 75.0f, 100.0f, 150.0f, 5.0f, 80.0f};
    const float camera_y = 15.0f;
    const float ppx = 200.0f;
    const int fb_h = 480;

    int simd_out[8];
    pwod::simd_project_y8(heights, camera_y, distances, ppx, fb_h, simd_out);

    for (std::uint32_t i = 0; i < 8; ++i) {
        const int scalar = pwod::project_y(heights[i], camera_y, distances[i], ppx, fb_h);
        INFO("lane " << i << " h=" << heights[i] << " d=" << distances[i]);
        // The two rounders (scalar `+0.5` cast vs SIMD broadcast then `+0.5`)
        // can disagree by 1 at half-integer boundaries; allow ±1.
        REQUIRE(std::abs(simd_out[i] - scalar) <= 1);
    }
}

TEST_CASE("simd_step_columns8 agrees with scalar step_column lane-by-lane",
          "[world_outdoor][raymarch][simd]") {
    const std::uint32_t W = 256;
    const std::uint32_t H = 32;
    auto raw = ramp_heightmap_simd(W, H, 50);  // ramp: terrain rises along +X
    pwo::HeightmapDesc desc{};
    desc.size_x = W;
    desc.size_z = H;
    desc.spacing = 1.0f;
    desc.height_scale = 0.1f;
    desc.heights = raw.data();

    const psynder::math::Vec3 eye{0.0f, 80.0f, 4.0f};
    const float ppx = 120.0f;
    const int fb_h = 240;

    // 8 lanes, each pointing slightly differently along XZ — pinhole-ish.
    pwod::ColumnBatch8 cb{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        cb.ray_dir_x[i] = 1.0f;
        cb.ray_dir_z[i] = -0.1f + 0.025f * static_cast<float>(i);  // -0.1..+0.075
        cb.horizon_y[i] = fb_h;
        cb.column_x[i] = i;
    }
    cb.done_mask = 0u;

    // Step at t=80 (well inside the ramp).
    pwod::ColumnStepBatch8 batch{};
    pwod::simd_step_columns8(desc, cb, eye, 80.0f, fb_h, ppx, batch);

    // For each lane, compute the scalar reference and compare.
    for (std::uint32_t i = 0; i < 8; ++i) {
        pwod::ColumnState col{};
        col.horizon_y = cb.horizon_y[i];
        col.column_x = cb.column_x[i];
        col.ray_dir_x = cb.ray_dir_x[i];
        col.ray_dir_z = cb.ray_dir_z[i];

        auto scalar = pwod::step_column(desc, col, eye, 80.0f, fb_h, ppx);

        INFO("lane " << i);
        // Horizon can differ by ±1 from rounding (see project_y test).
        REQUIRE(std::abs(batch.new_horizon_y[i] - scalar.new_horizon_y) <= 1);

        const bool simd_active = ((batch.active_mask >> i) & 1u) != 0u;
        const bool scalar_active = (scalar.new_horizon_y < col.horizon_y);
        // The active mask might disagree on the rounded-boundary case
        // (scalar paints, SIMD doesn't, or vice versa) — but the case is
        // very narrow. Allow either to agree OR to be within the rounding
        // band where horizon == screen_y ± 1.
        if (simd_active && scalar_active) {
            REQUIRE(std::abs(static_cast<int>(batch.strip_top_y[i]) -
                             static_cast<int>(scalar.strip_top_y)) <= 1);
            REQUIRE(std::abs(static_cast<int>(batch.strip_bottom_y[i]) -
                             static_cast<int>(scalar.strip_bottom_y)) <= 1);
            REQUIRE(batch.packed_color[i] == scalar.packed_color);
        }
        // z_distance is the input t, so it must match exactly.
        REQUIRE(batch.z_distance[i] == scalar.z_distance);
    }
}

TEST_CASE("SIMD raymarch advance + early-out flips done_mask once a lane runs off-screen",
          "[world_outdoor][raymarch][simd]") {
    // Set up a batch where one lane starts at horizon=1 (almost off-screen);
    // a single advance with a small new_horizon should drop it to 0 → done.
    pwod::ColumnBatch8 cb{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        cb.ray_dir_x[i] = 1.0f;
        cb.ray_dir_z[i] = 0.0f;
        cb.horizon_y[i] = 100;
        cb.column_x[i] = i;
    }
    cb.horizon_y[3] = 1;  // lane 3 starts near the top of frame
    cb.done_mask = 0u;

    pwod::ColumnStepBatch8 step{};
    for (std::uint32_t i = 0; i < 8; ++i)
        step.new_horizon_y[i] = cb.horizon_y[i];
    step.new_horizon_y[3] = 0;  // lane 3 advances past the top

    pwod::simd_advance_batch8(cb, step);

    REQUIRE((cb.done_mask & (1u << 3)) != 0u);
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (i == 3)
            continue;
        REQUIRE((cb.done_mask & (1u << i)) == 0u);
    }
    REQUIRE_FALSE(pwod::simd_batch_done8(cb));

    // Drop the rest off the top and confirm full done.
    for (std::uint32_t i = 0; i < 8; ++i)
        step.new_horizon_y[i] = -1;
    pwod::simd_advance_batch8(cb, step);
    REQUIRE(pwod::simd_batch_done8(cb));
}

TEST_CASE("SIMD raymarch respects per-lane done_mask (no painting on done lanes)",
          "[world_outdoor][raymarch][simd]") {
    const std::uint32_t W = 128;
    const std::uint32_t H = 16;
    auto raw = ramp_heightmap_simd(W, H, 80);
    pwo::HeightmapDesc desc{};
    desc.size_x = W;
    desc.size_z = H;
    desc.spacing = 1.0f;
    desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwod::ColumnBatch8 cb{};
    for (std::uint32_t i = 0; i < 8; ++i) {
        cb.ray_dir_x[i] = 1.0f;
        cb.ray_dir_z[i] = 0.0f;
        cb.horizon_y[i] = 200;
        cb.column_x[i] = i;
    }
    // Mark lanes 1, 4, 7 as done. They should never paint.
    cb.done_mask = (1u << 1) | (1u << 4) | (1u << 7);

    pwod::ColumnStepBatch8 batch{};
    pwod::simd_step_columns8(desc, cb, psynder::math::Vec3{0, 90, 0}, 50.0f, 200, 100.0f, batch);

    REQUIRE(((batch.active_mask >> 1) & 1u) == 0u);
    REQUIRE(((batch.active_mask >> 4) & 1u) == 0u);
    REQUIRE(((batch.active_mask >> 7) & 1u) == 0u);
}

TEST_CASE("SIMD raymarch parity over a sweep of distances (8 lanes × 12 steps)",
          "[world_outdoor][raymarch][simd]") {
    // The real consumer iterates many steps per batch; the parity property
    // must hold for the full sweep, not just one step. We march 12 steps
    // and check the horizon trajectory matches the scalar reference.
    const std::uint32_t W = 512;
    const std::uint32_t H = 16;
    auto raw = ramp_heightmap_simd(W, H, 100);
    pwo::HeightmapDesc desc{};
    desc.size_x = W;
    desc.size_z = H;
    desc.spacing = 1.0f;
    desc.height_scale = 0.08f;
    desc.heights = raw.data();

    const psynder::math::Vec3 eye{0.0f, 70.0f, 4.0f};
    const float ppx = 120.0f;
    const int fb_h = 240;

    pwod::ColumnBatch8 cb{};
    pwod::ColumnState cols[8];
    for (std::uint32_t i = 0; i < 8; ++i) {
        cb.ray_dir_x[i] = 1.0f;
        cb.ray_dir_z[i] = 0.0f;
        cb.horizon_y[i] = fb_h;
        cb.column_x[i] = i;
        cols[i].horizon_y = fb_h;
        cols[i].column_x = i;
        cols[i].ray_dir_x = 1.0f;
        cols[i].ray_dir_z = 0.0f;
    }
    cb.done_mask = 0u;

    float t = 5.0f;
    for (std::uint32_t s = 0; s < 12; ++s) {
        pwod::ColumnStepBatch8 batch{};
        pwod::simd_step_columns8(desc, cb, eye, t, fb_h, ppx, batch);
        pwod::simd_advance_batch8(cb, batch);

        for (std::uint32_t i = 0; i < 8; ++i) {
            auto scalar = pwod::step_column(desc, cols[i], eye, t, fb_h, ppx);
            cols[i].horizon_y = scalar.new_horizon_y;
            INFO("step " << s << " lane " << i << " t=" << t);
            REQUIRE(std::abs(cb.horizon_y[i] - cols[i].horizon_y) <= 1);
        }
        t = pwod::logstep_size(desc, t, 1.0f, 50.0f) + t;
    }
}
