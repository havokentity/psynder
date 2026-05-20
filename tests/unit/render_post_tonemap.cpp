// SPDX-License-Identifier: MIT
// Lane 09 — tonemap monotonicity.
//
// Reinhard is strictly increasing on [0, +inf) for any exposure > 0. The
// resolve() pass relies on this so that bright pixels never become darker
// than less-bright pixels after the round-trip. Quantization to u8 collapses
// adjacent values to the same byte, so the test asserts "non-decreasing
// after quantize" — which is the strongest invariant we can pin to the
// u8 output.
//
// This test includes the internal header directly so it doesn't have to
// link against psynder_render_post; the lane-shared unit-test target's
// CMakeLists.txt is owned by lane 25 and not in this lane's set.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "render/post/Internal.h"

#include <vector>

using namespace psynder;  // for u8/u32/usize/f32
using namespace psynder::render::post::detail;

TEST_CASE("render_post: reinhard is strictly monotone in 0..32", "[render_post][tonemap]") {
    // 0..32 covers the typical HDR range (a 32x overshoot of white is
    // already deep into bloom territory).
    constexpr int kSamples = 1024;
    constexpr float kMaxInput = 32.0f;
    const float exposure = 1.0f;

    float prev = reinhard(0.0f, exposure);
    for (int i = 1; i < kSamples; ++i) {
        const float x = static_cast<float>(i) / (kSamples - 1) * kMaxInput;
        const float y = reinhard(x, exposure);
        REQUIRE(y > prev);
        prev = y;
    }
}

TEST_CASE("render_post: reinhard exposure scales midpoint correctly", "[render_post][tonemap]") {
    // y = ex / (1 + ex). y = 0.5 when ex = 1, i.e. x = 1/exposure.
    for (float e : {0.5f, 1.0f, 2.0f, 4.0f}) {
        const float x = 1.0f / e;
        const float y = reinhard(x, e);
        REQUIRE(y == Catch::Approx(0.5f).margin(1e-6f));
    }
}

TEST_CASE("render_post: u8 quantization is non-decreasing after monotone tonemap",
          "[render_post][tonemap]") {
    // Round-trip: x in [0,32] -> reinhard -> sRGB encode -> quantize.
    // The composition is monotone in x; the byte output therefore must be
    // non-decreasing. This is what resolve() relies on at the pixel level.
    constexpr int kSamples = 4096;
    constexpr float kMaxIn = 32.0f;
    constexpr float kGamma = 2.2f;
    constexpr float kExposure = 1.0f;

    u8 prev = 0;
    for (int i = 0; i < kSamples; ++i) {
        const float x = static_cast<float>(i) / (kSamples - 1) * kMaxIn;
        const float t = reinhard(x, kExposure);
        const float s = linear_to_srgb(t, kGamma);
        const u8 q = quantize_u8(s, 0.5f);  // dither off → mid threshold
        REQUIRE(q >= prev);
        prev = q;
    }
    // The brightest input maps near (but not all the way to) white because
    // Reinhard is asymptotic — y = ex/(1+ex) → 1 only as x → ∞. At x=32 the
    // u8 result should be in the high 240s.
    {
        const float t = reinhard(kMaxIn, kExposure);
        const float s = linear_to_srgb(t, kGamma);
        const u8 q = quantize_u8(s, 0.5f);
        REQUIRE(q >= 248);
        REQUIRE(q <= 255);
    }
    // True saturating input (x=1e9) must land at 255.
    {
        const float t = reinhard(1.0e9f, kExposure);
        const float s = linear_to_srgb(t, kGamma);
        const u8 q = quantize_u8(s, 0.5f);
        REQUIRE(q == 255);
    }
    // The black input must land at 0.
    {
        const float t = reinhard(0.0f, kExposure);
        const float s = linear_to_srgb(t, kGamma);
        const u8 q = quantize_u8(s, 0.5f);
        REQUIRE(q == 0);
    }
}

TEST_CASE("render_post: dither modes produce thresholds in 0..1", "[render_post][dither]") {
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const float b = bayer4x4(x, y);
            REQUIRE(b >= 0.0f);
            REQUIRE(b < 1.0f);
            const float bn = blue_noise(x, y);
            REQUIRE(bn >= 0.0f);
            REQUIRE(bn < 1.0f);
        }
    }
}

TEST_CASE("render_post: bayer 4x4 is a permutation of 0..16 / 16", "[render_post][dither]") {
    std::vector<bool> seen(16, false);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const auto idx = static_cast<int>(bayer4x4(x, y) * 16.0f);
            REQUIRE(idx >= 0);
            REQUIRE(idx < 16);
            REQUIRE(!seen[static_cast<size_t>(idx)]);
            seen[static_cast<size_t>(idx)] = true;
        }
    }
}
