// SPDX-License-Identifier: MIT
// Psynder — Lane 02 fixed-point Q24.8 tests.

#include "math/FxQ24_8.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace psynder;
using namespace psynder::math;

TEST_CASE("FxQ24_8 from_int round-trips", "[math][fxq248]") {
    FxQ24_8 q = FxQ24_8::from_int(42);
    REQUIRE(q.raw == 42 * 256);
    REQUIRE(q.to_int_trunc() == 42);
}

TEST_CASE("FxQ24_8 from_f32 carries sub-pixel precision", "[math][fxq248]") {
    // 1/256 should be exactly representable.
    FxQ24_8 q = FxQ24_8::from_f32(1.0f / 256.0f);
    REQUIRE(q.raw == 1);

    FxQ24_8 r = FxQ24_8::from_f32(0.5f);
    REQUIRE(r.raw == 128);

    REQUIRE(std::fabs(r.to_f32() - 0.5f) < 1e-6f);
}

TEST_CASE("FxQ24_8 add and sub are exact in raw", "[math][fxq248]") {
    FxQ24_8 a = FxQ24_8::from_f32(1.25f);
    FxQ24_8 b = FxQ24_8::from_f32(2.5f);
    REQUIRE((a + b).raw == FxQ24_8::from_f32(3.75f).raw);
    REQUIRE((b - a).raw == FxQ24_8::from_f32(1.25f).raw);
    REQUIRE((-a).raw    == -a.raw);
}

TEST_CASE("FxQ24_8 multiplication retains the 1-over-256 grid", "[math][fxq248]") {
    FxQ24_8 a = FxQ24_8::from_f32(2.0f);
    FxQ24_8 b = FxQ24_8::from_f32(3.5f);
    FxQ24_8 c = a * b;
    REQUIRE(std::fabs(c.to_f32() - 7.0f) < 1e-3f);
}

TEST_CASE("FxQ24_8 division round-trips reasonably", "[math][fxq248]") {
    FxQ24_8 a = FxQ24_8::from_f32(10.0f);
    FxQ24_8 b = FxQ24_8::from_f32(4.0f);
    FxQ24_8 c = a / b;
    REQUIRE(std::fabs(c.to_f32() - 2.5f) < 1e-3f);

    // Divide-by-zero returns 0 rather than UB.
    FxQ24_8 d = a / FxQ24_8::from_int(0);
    REQUIRE(d.raw == 0);
}

TEST_CASE("FxQ24_8 ordering matches integer semantics", "[math][fxq248]") {
    FxQ24_8 small = FxQ24_8::from_f32(1.5f);
    FxQ24_8 big   = FxQ24_8::from_f32(3.0f);
    REQUIRE(small <  big);
    REQUIRE(big   >  small);
    REQUIRE(small != big);
    REQUIRE(small == FxQ24_8::from_f32(1.5f));
}

TEST_CASE("FxQ24_8 round versus truncate near a tick", "[math][fxq248]") {
    // 0.4999/256 truncates down to 0; round-to-nearest snaps to 1/256.
    f32 just_below = 0.4f / 256.0f;
    f32 just_above = 0.6f / 256.0f;
    REQUIRE(FxQ24_8::from_f32(just_below).raw       == 0);
    REQUIRE(FxQ24_8::from_f32_round(just_above).raw == 1);

    f32 negative = -0.6f / 256.0f;
    REQUIRE(FxQ24_8::from_f32_round(negative).raw == -1);
}

TEST_CASE("FxQ24_8 free-function constructor matches static method", "[math][fxq248]") {
    constexpr FxQ24_8 a = fx(1.5f);
    REQUIRE(a.raw == FxQ24_8::from_f32(1.5f).raw);
}
