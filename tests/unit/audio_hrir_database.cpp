// SPDX-License-Identifier: MIT
// Psynder — lane 12 unit test: Wave-B HrirDatabase (2 × 12 cells, 256 taps).
//
// Wave-A's `make_minimal_hrir` produced an analytical 64-tap pair. Wave B
// replaces it with a pre-built table of `kHrirAzBins × kHrirElBins` cells of
// 256 taps per ear, looked up via bilinear (az, el) blend + frequency-domain
// crossfade. These tests verify:
//
//   1. table dimensions match the design (2 × 12 × 256).
//   2. on-axis cells are symmetric L↔R.
//   3. lookup at a measured (az, el) reproduces the underlying cell up to
//      a small frequency-blend deviation (the FFT-crossfade introduces a
//      small magnitude perturbation but preserves overall energy).
//   4. lookup is finite and energy-bounded for the full 2π × 2-elevation grid.

#include "audio/internal/HrirDatabase.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

using psynder::audio::detail::HrirDatabase;
using psynder::audio::detail::kHrirAzBins;
using psynder::audio::detail::kHrirElBins;
using psynder::audio::detail::kHrirTaps;
using psynder::math::kTwoPi;
using psynder::math::kPi;

TEST_CASE("audio: HRIR database dimensions match Wave-B design", "[audio][hrtf][db]") {
    REQUIRE(HrirDatabase::azimuth_bins()   == 12u);
    REQUIRE(HrirDatabase::elevation_bins() == 2u);
    REQUIRE(HrirDatabase::taps()           == 256u);
}

TEST_CASE("audio: HRIR database builds and reports ready", "[audio][hrtf][db]") {
    HrirDatabase db;
    REQUIRE_FALSE(db.ready());
    db.build(48000u);
    REQUIRE(db.ready());
}

TEST_CASE("audio: HRIR cells are L/R-asymmetric off-axis", "[audio][hrtf][db]") {
    // For an azimuth bin clearly to one side (bin 3 ≈ -π + 3 * 30° = -π + π/2 → -90°),
    // the ipsilateral ear should carry strictly more peak energy than the
    // contralateral ear.
    HrirDatabase db;
    db.build(48000u);
    // bin 9 corresponds to azimuth = -π + 9 * (2π/12) = π/2 (right side)
    const auto& cell = db.cell(/*az_bin*/ 9u, /*el_bin*/ 0u);
    psynder::f32 lpeak = 0.0f, rpeak = 0.0f;
    for (psynder::u32 i = 0; i < kHrirTaps; ++i) {
        lpeak = std::fmax(lpeak, std::fabs(cell.left[i]));
        rpeak = std::fmax(rpeak, std::fabs(cell.right[i]));
    }
    INFO("L peak: " << lpeak << ", R peak: " << rpeak);
    REQUIRE(rpeak > 0.0f);
    REQUIRE(lpeak > 0.0f);
    // ipsilateral (right) > contralateral (left)
    REQUIRE(rpeak > lpeak * 1.05f);
}

TEST_CASE("audio: HRIR on-axis cell is L/R-symmetric in peak energy", "[audio][hrtf][db]") {
    HrirDatabase db;
    db.build(48000u);
    // bin 6 = -π + 6*(2π/12) = 0 (front-on)
    const auto& cell = db.cell(/*az_bin*/ 6u, /*el_bin*/ 0u);
    psynder::f32 le = 0.0f, re = 0.0f;
    for (psynder::u32 i = 0; i < kHrirTaps; ++i) {
        le += cell.left[i]  * cell.left[i];
        re += cell.right[i] * cell.right[i];
    }
    INFO("L energy: " << le << ", R energy: " << re);
    REQUIRE(le > 0.0f);
    REQUIRE(re > 0.0f);
    REQUIRE(std::fabs(le - re) / std::fmax(le, re) < 0.05f);
}

TEST_CASE("audio: HRIR lookup at on-axis returns finite, non-NaN taps", "[audio][hrtf][db]") {
    HrirDatabase db;
    db.build(48000u);
    psynder::f32 L[kHrirTaps]{}, R[kHrirTaps]{};
    db.lookup(/*az*/ 0.0f, /*el*/ 0.0f, L, R);
    for (psynder::u32 i = 0; i < kHrirTaps; ++i) {
        REQUIRE(std::isfinite(L[i]));
        REQUIRE(std::isfinite(R[i]));
    }
    // total energy should be non-zero (the burst is in the table).
    psynder::f32 e = 0.0f;
    for (psynder::u32 i = 0; i < kHrirTaps; ++i) e += L[i] * L[i] + R[i] * R[i];
    REQUIRE(e > 0.0f);
}

TEST_CASE("audio: HRIR lookup sweeps full grid without NaN or overflow", "[audio][hrtf][db]") {
    HrirDatabase db;
    db.build(48000u);
    psynder::f32 L[kHrirTaps]{}, R[kHrirTaps]{};
    constexpr psynder::u32 kAzSweep = 36;   // 10° steps
    constexpr psynder::u32 kElSweep = 5;
    for (psynder::u32 a = 0; a < kAzSweep; ++a) {
        const psynder::f32 az = -kPi + (kTwoPi * static_cast<psynder::f32>(a) /
                                        static_cast<psynder::f32>(kAzSweep));
        for (psynder::u32 e = 0; e < kElSweep; ++e) {
            const psynder::f32 el = 0.6f * static_cast<psynder::f32>(e) /
                                    static_cast<psynder::f32>(kElSweep - 1u);
            db.lookup(az, el, L, R);
            psynder::f32 peak = 0.0f;
            for (psynder::u32 i = 0; i < kHrirTaps; ++i) {
                REQUIRE(std::isfinite(L[i]));
                REQUIRE(std::isfinite(R[i]));
                peak = std::fmax(peak, std::fmax(std::fabs(L[i]), std::fabs(R[i])));
            }
            INFO("az=" << az << " el=" << el << " peak=" << peak);
            // The pinna-burst peak is < 1.0 by construction; the freq-domain
            // crossfade preserves magnitude, so the result must stay well
            // bounded (allow some headroom for the FFT round-trip).
            REQUIRE(peak < 2.0f);
        }
    }
}

TEST_CASE("audio: HRIR lookup near an azimuth bin reproduces that bin", "[audio][hrtf][db]") {
    // When we look up exactly at a bin centre, the frequency-domain crossfade
    // weight t is zero, so the result must match the precomputed cell up to
    // a small FFT round-trip error.
    HrirDatabase db;
    db.build(48000u);
    constexpr psynder::u32 kBin = 7;       // arbitrary
    const psynder::f32 az = -kPi + (kTwoPi * static_cast<psynder::f32>(kBin) /
                                    static_cast<psynder::f32>(kHrirAzBins));
    psynder::f32 L[kHrirTaps]{}, R[kHrirTaps]{};
    db.lookup(az, /*el*/ 0.0f, L, R);
    const auto& cell = db.cell(kBin, 0u);
    // Compare cumulative energy — the FFT round-trip preserves total energy
    // exactly up to roundoff.
    psynder::f32 e_ref = 0.0f, e_act = 0.0f;
    for (psynder::u32 i = 0; i < kHrirTaps; ++i) {
        e_ref += cell.left[i]  * cell.left[i]  + cell.right[i] * cell.right[i];
        e_act += L[i] * L[i] + R[i] * R[i];
    }
    INFO("e_ref=" << e_ref << " e_act=" << e_act);
    REQUIRE(e_ref > 0.0f);
    REQUIRE(std::fabs(e_act - e_ref) / e_ref < 0.05f);
}
