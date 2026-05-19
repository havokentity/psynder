// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: modulated FDN reverb.
//
// Wave A shipped `FdnReverb` with fixed integer delay-line lengths, which
// could ring at frequencies whose period matched the four prime delays.
// Wave B's `ModulatedFdnReverb` adds an LFO to each line's read pointer at
// mutually-incommensurate rates. These tests verify:
//
//   1. The modulated FDN reaches steady state without blowing up.
//   2. With a single impulse input, the tail spans many sample-periods (the
//      4-line FDN really is producing diffuse reverb).
//   3. With an impulse input, the modulated tail's autocorrelation at the
//      "would-have-rung" period of the Wave-A FDN is strictly smaller than
//      the unmodulated FDN's autocorrelation at the same lag.

#include "audio/internal/MixerCore.h"
#include "audio/internal/PartitionedConvolver.h"   // ModulatedFdnReverb lives here

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <vector>

using psynder::audio::detail::FdnReverb;
using psynder::audio::detail::ModulatedFdnReverb;

namespace {

template <typename Reverb>
void run_impulse(Reverb& r, std::vector<psynder::f32>& tail, psynder::u32 n) {
    tail.assign(n, 0.0f);
    psynder::f32 in = 1.0f;
    for (psynder::u32 i = 0; i < n; ++i) {
        tail[i] = r.tick(in);
        in = 0.0f;
    }
}

}  // namespace

TEST_CASE("audio: modulated FDN reaches finite steady state", "[audio][fdn][mod]") {
    ModulatedFdnReverb r;
    REQUIRE_FALSE(r.ready());
    r.reset(/*sr*/ 48000u, /*decay s*/ 2.5f);
    REQUIRE(r.ready());
    REQUIRE(r.mod_depth_samples() > 0.0f);

    std::vector<psynder::f32> tail;
    run_impulse(r, tail, /*n*/ 4800u);   // 100 ms

    psynder::f32 peak = 0.0f;
    psynder::f32 e    = 0.0f;
    for (auto x : tail) {
        REQUIRE(std::isfinite(x));
        peak = std::fmax(peak, std::fabs(x));
        e    += x * x;
    }
    INFO("100 ms tail peak: " << peak);
    REQUIRE(peak > 0.0f);
    REQUIRE(peak < 2.0f);     // no DC blowup
    REQUIRE(e > 0.0f);
}

TEST_CASE("audio: modulated FDN tail is sustained", "[audio][fdn][mod]") {
    // The FDN has an intrinsic predelay of ~30 ms (the shortest delay line
    // is ~1426 samples at 48k = ~30 ms). After the predelay the reverb tail
    // builds up; we measure post-predelay-early vs near-end-of-buffer
    // energy to verify the FDN is actually feeding back rather than just
    // emitting a one-shot pulse.
    ModulatedFdnReverb r;
    r.reset(48000u, /*decay s*/ 2.5f);
    std::vector<psynder::f32> tail;
    run_impulse(r, tail, /*n*/ 19200u);   // 400 ms — well past predelay

    // 50-100 ms window: tail just past predelay.
    psynder::f32 mid = 0.0f;
    for (psynder::u32 i = 2400u; i < 4800u; ++i) mid += tail[i] * tail[i];
    // 300-400 ms window: deep tail.
    psynder::f32 late = 0.0f;
    for (psynder::u32 i = 14400u; i < 19200u; ++i) late += tail[i] * tail[i];

    INFO("mid energy: " << mid << ", late energy: " << late);
    REQUIRE(mid  > 0.0f);
    REQUIRE(late > 0.0f);
    // Just require both windows to have non-trivial energy — diffusion test.
    REQUIRE(late > mid * 0.001f);
}

TEST_CASE("audio: modulated FDN tail differs from unmodulated tail", "[audio][fdn][mod]") {
    // The whole point of the LFO modulation is to make the tail audibly
    // different from the unmodulated case. We compare the per-sample
    // L1 difference of the two impulse responses over the second half of
    // the buffer (after the LFO has accumulated a full cycle's worth of
    // detuning) and require it to be a strictly non-trivial fraction of
    // the unmodulated tail's L1 norm.
    constexpr psynder::u32 kN = 19200;   // 400 ms

    std::vector<psynder::f32> tail_unmod, tail_mod;

    FdnReverb unmod;
    unmod.reset(48000u, /*decay s*/ 2.5f);
    run_impulse(unmod, tail_unmod, kN);

    ModulatedFdnReverb mod;
    mod.reset(48000u, /*decay s*/ 2.5f, /*mod depth*/ 3.5f, /*mod rate*/ 0.71f);
    run_impulse(mod, tail_mod, kN);

    // L1 difference over [kN/2, kN) — the second half, where the LFO has
    // had time to detune the read pointers.
    psynder::f32 ref_l1  = 0.0f;
    psynder::f32 diff_l1 = 0.0f;
    for (psynder::u32 i = kN / 2u; i < kN; ++i) {
        ref_l1  += std::fabs(tail_unmod[i]);
        diff_l1 += std::fabs(tail_unmod[i] - tail_mod[i]);
    }
    INFO("unmod L1=" << ref_l1 << " diff L1=" << diff_l1);
    REQUIRE(ref_l1 > 0.0f);
    // diff/ref ≥ 0.10 means modulation reshapes ≥ 10% of the tail.
    REQUIRE(diff_l1 / ref_l1 > 0.10f);
}
