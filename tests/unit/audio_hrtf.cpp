// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: HRTF azimuth produces the expected ITD.
//
// We verify the Woodworth ITD formula directly off the algorithmic core
// from `audio/internal/MixerCore.h`. This is the deliverable's load-bearing
// HRTF property — a sound at +90° azimuth must arrive at the right ear
// roughly head-radius / speed-of-sound seconds before the left ear.

#include "audio/internal/MixerCore.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

TEST_CASE("audio: HRTF ITD is zero on-axis", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    const psynder::f32 itd = itd_seconds(0.0f);
    REQUIRE(std::fabs(itd) < 1e-6f);
}

TEST_CASE("audio: HRTF ITD at +90° matches Woodworth formula", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // Woodworth: ITD = r/c * (sin θ + θ)
    // At θ = π/2 → ITD = r/c * (1 + π/2) ≈ 0.0875 / 343 * 2.5708 ≈ 6.56e-4 s
    const psynder::f32 expected = (kHeadRadius / kSpeedOfSound) * (1.0f + psynder::math::kHalfPi);
    const psynder::f32 itd = itd_seconds(psynder::math::kHalfPi);
    INFO("expected ITD: " << expected << ", actual: " << itd);
    REQUIRE(itd > 0.0f);  // right-of-listener → positive
    REQUIRE(std::fabs(itd - expected) < 1e-5f);
}

TEST_CASE("audio: HRTF ITD is anti-symmetric across the median plane", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    for (psynder::f32 az : {0.25f, 0.7f, 1.2f, psynder::math::kHalfPi}) {
        const psynder::f32 right = itd_seconds(+az);
        const psynder::f32 left = itd_seconds(-az);
        INFO("azimuth=" << az);
        REQUIRE(std::fabs(right + left) < 1e-6f);
    }
}

TEST_CASE("audio: HRTF integer delays at 48 kHz at +90° give ~32 samples", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    // 6.56e-4 s * 48000 ≈ 31.5 → rounds to 32 samples.
    psynder::u32 left = 0, right = 0;
    itd_to_delays(psynder::math::kHalfPi, /*sr*/ 48000u, left, right);
    INFO("left delay: " << left << ", right delay: " << right);
    REQUIRE(right == 0u);  // ipsilateral arrives immediately
    REQUIRE(left >= 30u);
    REQUIRE(left <= 34u);

    // And the mirrored case
    itd_to_delays(-psynder::math::kHalfPi, /*sr*/ 48000u, left, right);
    REQUIRE(left == 0u);
    REQUIRE(right >= 30u);
    REQUIRE(right <= 34u);
}

TEST_CASE("audio: minimal HRIR has correct delay slot wiring", "[audio][hrtf]") {
    using namespace psynder::audio::detail;
    const StereoHrir h = make_minimal_hrir(+psynder::math::kHalfPi, /*sr*/ 48000u);
    // right is ipsilateral → no delay; left is contralateral → delayed
    REQUIRE(h.right_delay_samples == 0u);
    REQUIRE(h.left_delay_samples >= 30u);
    REQUIRE(h.left_delay_samples <= 34u);
}
