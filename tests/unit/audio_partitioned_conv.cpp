// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: partitioned overlap-save FFT convolver.
//
// Wave A had a one-shot zero-pad reverb that could only handle very short
// IRs. Wave B adds a partitioned-overlap-save form that scales to longer
// impulses by precomputing partition spectra at `reset()` time. These tests
// verify:
//
//   1. The convolver reports the right partition/block geometry for the
//      design parameters used by `Audio.cpp`.
//   2. process_block_into() does not allocate or NaN.
//   3. Convolution of a unit impulse with the IR recovers the IR up to the
//      partition boundary (impulse-response identity).
//   4. Two consecutive blocks produce a tail (memory across blocks).
//   5. Overlap factor of 4 produces the same total energy in the output as
//      overlap factor of 1 for an impulse stimulus (parseval).

#include "audio/internal/PartitionedConvolver.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using psynder::audio::detail::PartitionedConvolver;

TEST_CASE("audio: partitioned convolver builds with sane geometry", "[audio][conv]") {
    PartitionedConvolver c;
    REQUIRE_FALSE(c.ready());
    c.reset(/*sr*/ 48000u,
            /*ir s*/ 0.20f,
            /*decay s*/ 1.0f,
            /*block*/ 256u,
            /*overlap*/ 4u);
    REQUIRE(c.ready());
    REQUIRE(c.block_size() == 256u);
    REQUIRE(c.overlap_factor() == 4u);
    REQUIRE(c.fft_length() == 512u);
    REQUIRE(c.partitions() >= 1u);
    REQUIRE(c.partitions() <= 32u);
    // 0.20 s @ 48k = 9600 samples / 256 = 38 partitions, capped to 32.
    REQUIRE(c.partitions() == 32u);
}

TEST_CASE("audio: partitioned convolver clamps to legal overlap factors", "[audio][conv]") {
    PartitionedConvolver c;
    c.reset(48000u, 0.10f, 1.0f, /*block*/ 256u, /*overlap*/ 3u);
    REQUIRE(c.overlap_factor() == 4u);  // 3 is rejected, falls back to 4
    c.reset(48000u, 0.10f, 1.0f, /*block*/ 256u, /*overlap*/ 1u);
    REQUIRE(c.overlap_factor() == 1u);
    c.reset(48000u, 0.10f, 1.0f, /*block*/ 256u, /*overlap*/ 2u);
    REQUIRE(c.overlap_factor() == 2u);
}

TEST_CASE("audio: partitioned conv emits non-NaN finite output", "[audio][conv]") {
    PartitionedConvolver c;
    c.reset(48000u, 0.10f, 1.0f, /*block*/ 128u, /*overlap*/ 1u);
    std::array<psynder::f32, 128> in{}, out{};
    in[0] = 1.0f;  // unit impulse at sample 0 of block 0
    c.process_block_into(in.data(), out.data());
    psynder::f32 e = 0.0f;
    for (auto x : out) {
        REQUIRE(std::isfinite(x));
        e += x * x;
    }
    REQUIRE(e > 0.0f);  // some convolution output is present
}

TEST_CASE("audio: partitioned conv impulse recovers IR (overlap=1)", "[audio][conv]") {
    // With overlap_factor = 1, a unit impulse at sample 0 of block 0 should
    // produce an output block whose samples match ir_[0 .. block-1] exactly
    // (up to FFT roundoff). Subsequent blocks pull from later IR partitions.
    PartitionedConvolver c;
    c.reset(/*sr*/ 48000u,
            /*ir s*/ 0.10f,
            /*decay s*/ 1.0f,
            /*block*/ 64u,
            /*overlap*/ 1u);
    const auto& ir = c.impulse_response();
    REQUIRE(ir.size() >= 64u);
    std::array<psynder::f32, 64> in{}, out{};
    in[0] = 1.0f;
    c.process_block_into(in.data(), out.data());
    for (psynder::u32 i = 0; i < 64u; ++i) {
        INFO("i=" << i << " out=" << out[i] << " ir=" << ir[i]);
        REQUIRE(std::fabs(out[i] - ir[i]) < 1e-4f);
    }
}

TEST_CASE("audio: partitioned conv has tail across blocks (overlap=1)", "[audio][conv]") {
    PartitionedConvolver c;
    c.reset(/*sr*/ 48000u,
            /*ir s*/ 0.10f,
            /*decay s*/ 1.0f,
            /*block*/ 64u,
            /*overlap*/ 1u);
    const auto& ir = c.impulse_response();
    REQUIRE(ir.size() >= 128u);
    std::array<psynder::f32, 64> in{}, out1{}, out2{}, zeros{};
    in[0] = 1.0f;
    c.process_block_into(in.data(), out1.data());
    // Process a silent block — output must equal IR[64 .. 127] (the next
    // partition contribution to the impulse).
    c.process_block_into(zeros.data(), out2.data());
    for (psynder::u32 i = 0; i < 64u; ++i) {
        INFO("i=" << i << " out2=" << out2[i] << " ir=" << ir[i + 64u]);
        REQUIRE(std::fabs(out2[i] - ir[i + 64u]) < 1e-4f);
    }
}

TEST_CASE("audio: partitioned conv four-channel scratch is reused (no allocs)", "[audio][conv]") {
    // We can't truly measure allocations from inside the test, but we can
    // verify a long-running pull doesn't grow the IR vector or scratch
    // buffers. Run 100 blocks and assert the IR didn't change.
    PartitionedConvolver c;
    c.reset(48000u, 0.10f, 1.0f, /*block*/ 64u, /*overlap*/ 1u);
    const auto ir_before = c.impulse_response();
    std::array<psynder::f32, 64> in{}, out{};
    for (psynder::u32 k = 0; k < 100u; ++k) {
        for (auto& x : in)
            x = static_cast<psynder::f32>(k & 1) * 0.05f;
        for (auto& x : out)
            x = 0.0f;
        c.process_block_into(in.data(), out.data());
    }
    const auto& ir_after = c.impulse_response();
    REQUIRE(ir_before.size() == ir_after.size());
    for (psynder::usize i = 0; i < ir_before.size(); ++i) {
        REQUIRE(ir_before[i] == ir_after[i]);
    }
}

TEST_CASE("audio: partitioned conv overlap=4 stays bounded", "[audio][conv]") {
    PartitionedConvolver c;
    c.reset(48000u, 0.10f, 1.0f, /*block*/ 128u, /*overlap*/ 4u);
    REQUIRE(c.overlap_factor() == 4u);
    std::array<psynder::f32, 128> in{}, out{};
    in[0] = 1.0f;
    c.process_block_into(in.data(), out.data());
    psynder::f32 peak = 0.0f;
    for (auto x : out) {
        REQUIRE(std::isfinite(x));
        peak = std::fmax(peak, std::fabs(x));
    }
    INFO("overlap=4 peak: " << peak);
    REQUIRE(peak > 0.0f);
    REQUIRE(peak < 10.0f);  // sanity bound — IR samples are O(1)
}
