// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: Doppler pitch shift.
//
// Wave-B adds a per-voice Doppler ratio derived from the relative velocity
// of source and listener, capped at [0.5×, 1.5×] to avoid SR-aliasing in
// the linear-interp resampler. These tests verify:
//
//   1. Listener and source at rest → ratio = 1.0.
//   2. Source approaching listener → ratio > 1 (frequency rises).
//   3. Source receding from listener → ratio < 1.
//   4. Coincident source and listener → ratio = 1 (degenerate case guard).
//   5. The cap clamps very-fast closing speeds to 1.5×.
//   6. doppler_render_sine() reflects the ratio in the rendered period.

#include "audio/internal/Doppler.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using psynder::audio::detail::doppler_ratio;
using psynder::audio::detail::doppler_render_sine;
using psynder::audio::detail::kDopplerMinRatio;
using psynder::audio::detail::kDopplerMaxRatio;
using psynder::math::Vec3;

TEST_CASE("audio: Doppler at rest is unity", "[audio][doppler]") {
    const psynder::f32 r = doppler_ratio(/*listener pos*/ Vec3{0, 0, 0},
                                         /*listener vel*/ Vec3{0, 0, 0},
                                         /*source pos*/   Vec3{0, 0, 10.0f},
                                         /*source vel*/   Vec3{0, 0, 0});
    REQUIRE(std::fabs(r - 1.0f) < 1e-6f);
}

TEST_CASE("audio: Doppler source approaching listener raises pitch", "[audio][doppler]") {
    // source at +Z = 10 m, moving in -Z at 34.3 m/s ≈ 10% of c.
    const psynder::f32 r = doppler_ratio(Vec3{0, 0, 0}, Vec3{0, 0, 0},
                                         Vec3{0, 0, 10.0f}, Vec3{0, 0, -34.3f});
    INFO("approach ratio: " << r);
    REQUIRE(r > 1.0f);
    // 343 / (343 - 34.3) ≈ 1.111
    REQUIRE(std::fabs(r - 1.111f) < 0.01f);
}

TEST_CASE("audio: Doppler source receding lowers pitch", "[audio][doppler]") {
    const psynder::f32 r = doppler_ratio(Vec3{0, 0, 0}, Vec3{0, 0, 0},
                                         Vec3{0, 0, 10.0f}, Vec3{0, 0, +34.3f});
    INFO("recede ratio: " << r);
    REQUIRE(r < 1.0f);
    // 343 / (343 + 34.3) ≈ 0.909
    REQUIRE(std::fabs(r - 0.909f) < 0.01f);
}

TEST_CASE("audio: Doppler listener moving toward source raises pitch", "[audio][doppler]") {
    // listener moving in +Z at 34.3 m/s; source at rest at +Z = 10 m.
    const psynder::f32 r = doppler_ratio(Vec3{0, 0, 0}, Vec3{0, 0, +34.3f},
                                         Vec3{0, 0, 10.0f}, Vec3{0, 0, 0});
    INFO("listener-toward ratio: " << r);
    REQUIRE(r > 1.0f);
    // (343 + 34.3) / 343 ≈ 1.100
    REQUIRE(std::fabs(r - 1.100f) < 0.01f);
}

TEST_CASE("audio: Doppler coincident source/listener returns unity", "[audio][doppler]") {
    const psynder::f32 r = doppler_ratio(Vec3{1, 2, 3}, Vec3{5, 0, 0},
                                         Vec3{1, 2, 3}, Vec3{0, 5, 0});
    REQUIRE(std::fabs(r - 1.0f) < 1e-6f);
}

TEST_CASE("audio: Doppler is clamped to [0.5, 1.5]", "[audio][doppler]") {
    // very fast approach (source toward listener at 300 m/s)
    const psynder::f32 r_fast = doppler_ratio(Vec3{0, 0, 0}, Vec3{0, 0, 0},
                                              Vec3{0, 0, 10.0f}, Vec3{0, 0, -300.0f});
    INFO("fast approach: " << r_fast);
    REQUIRE(r_fast <= kDopplerMaxRatio + 1e-6f);

    // very fast recede
    const psynder::f32 r_slow = doppler_ratio(Vec3{0, 0, 0}, Vec3{0, 0, 0},
                                              Vec3{0, 0, 10.0f}, Vec3{0, 0, +300.0f});
    INFO("fast recede: " << r_slow);
    REQUIRE(r_slow >= kDopplerMinRatio - 1e-6f);
}

TEST_CASE("audio: Doppler sine renderer reflects pitch ratio", "[audio][doppler]") {
    constexpr psynder::u32 kSr     = 48000;
    constexpr psynder::u32 kFrames = 4800;     // 100 ms
    constexpr psynder::f32 kBaseHz = 1000.0f;
    constexpr psynder::f32 kRatio  = 1.2f;
    std::vector<psynder::f32> buf(kFrames, 0.0f);
    doppler_render_sine(kBaseHz, kRatio, kSr, kFrames, buf.data());

    // count zero crossings (rising) in the 100 ms buffer; expected ≈
    // 1000 * 1.2 * 0.1 = 120.
    psynder::u32 zc = 0;
    for (psynder::u32 i = 1; i < kFrames; ++i) {
        if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) ++zc;
    }
    INFO("zero crossings (rising): " << zc);
    // tolerate ±5 crossings for boundary effects.
    REQUIRE(zc >= 115u);
    REQUIRE(zc <= 125u);
}

TEST_CASE("audio: Doppler sine renderer at ratio=1 matches base frequency", "[audio][doppler]") {
    constexpr psynder::u32 kSr     = 48000;
    constexpr psynder::u32 kFrames = 4800;
    constexpr psynder::f32 kBaseHz = 800.0f;
    std::vector<psynder::f32> buf(kFrames, 0.0f);
    doppler_render_sine(kBaseHz, /*ratio*/ 1.0f, kSr, kFrames, buf.data());

    psynder::u32 zc = 0;
    for (psynder::u32 i = 1; i < kFrames; ++i) {
        if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) ++zc;
    }
    // 800 Hz × 100 ms = 80 zero crossings
    REQUIRE(zc >= 77u);
    REQUIRE(zc <= 83u);
}
