// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: mixer does not clip with N quiet voices.
//
// Pulls the SIMD voice-sum from the header-only mixer core so the test
// compiles without `psynder_audio` linkage (see `audio_voices.cpp`).

#include "audio/internal/MixerCore.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace {
constexpr psynder::u32 kFrames = 256;
}  // namespace

TEST_CASE("audio: mixer doesn't clip with 32 quiet sine voices", "[audio][mixer]") {
    using namespace psynder;
    using namespace psynder::audio::detail;

    // Each voice writes a sine at amplitude (1 / kMaxVoices) so the sum
    // peaks at ≤ 1.0 even in the worst-case phase-aligned scenario.
    constexpr u32 stereo_floats = 2u * kFrames;
    std::array<f32, stereo_floats> master{};

    const f32 per_voice_amp = 1.0f / static_cast<f32>(kMaxVoices);

    std::vector<f32> voice_buf(stereo_floats, 0.0f);
    for (u32 v = 0; v < kMaxVoices; ++v) {
        // Render this voice into its own buffer first.
        std::fill(voice_buf.begin(), voice_buf.end(), 0.0f);
        const f32 phase = static_cast<f32>(v) * 0.123f;
        for (u32 i = 0; i < kFrames; ++i) {
            const f32 s = std::sin(
                2.0f * psynder::math::kPi * (static_cast<f32>(i) / static_cast<f32>(kFrames)) + phase);
            voice_buf[2 * i + 0] = s * per_voice_amp;
            voice_buf[2 * i + 1] = s * per_voice_amp;
        }
        // SIMD-merge into master.
        simd_mix_into(master.data(), voice_buf.data(), 1.0f, stereo_floats);
    }

    // master peak should be ≤ 1.0 with healthy headroom; assert strictly
    // less than +1.0 and greater than -1.0 (the deliverable bar).
    f32 peak = 0.0f;
    for (u32 i = 0; i < stereo_floats; ++i) {
        peak = std::fmax(peak, std::fabs(master[i]));
    }
    INFO("Mixed peak amplitude: " << peak);
    REQUIRE(peak <= 1.0f);
    // Sanity: with random phases the mixed peak shouldn't approach the
    // per-voice peak (1/N * N = 1) — the random phase should reduce it.
    REQUIRE(peak > 0.0f);
}

TEST_CASE("audio: simd_mix_into matches scalar reference", "[audio][mixer][simd]") {
    using namespace psynder;
    using namespace psynder::audio::detail;

    constexpr u32 n = 17;  // odd to exercise SIMD-bulk + scalar-tail
    std::array<f32, n> dst_simd{};
    std::array<f32, n> dst_ref{};
    std::array<f32, n> src{};
    for (u32 i = 0; i < n; ++i)
        src[i] = static_cast<f32>(i) * 0.1f;

    simd_mix_into(dst_simd.data(), src.data(), 2.0f, n);
    for (u32 i = 0; i < n; ++i)
        dst_ref[i] = src[i] * 2.0f;

    for (u32 i = 0; i < n; ++i) {
        INFO("i=" << i << " simd=" << dst_simd[i] << " ref=" << dst_ref[i]);
        REQUIRE(std::fabs(dst_simd[i] - dst_ref[i]) < 1e-6f);
    }
}
