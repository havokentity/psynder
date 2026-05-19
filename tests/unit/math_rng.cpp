// SPDX-License-Identifier: MIT
// Psynder — Lane 02 RNG and stratified-sampler tests.

#include "math/Rng.h"
#include "math/Sampler.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::math;

TEST_CASE("Rng is deterministic for a given seed", "[math][rng]") {
    Rng a; a.seed(12345);
    Rng b; b.seed(12345);
    for (int i = 0; i < 32; ++i) {
        REQUIRE(a.next_u32() == b.next_u32());
    }
}

TEST_CASE("Rng next_f32 lives in the unit interval", "[math][rng]") {
    Rng r; r.seed(7);
    for (int i = 0; i < 1000; ++i) {
        f32 x = r.next_f32();
        REQUIRE(x >= 0.0f);
        REQUIRE(x <  1.0f);
    }
}

TEST_CASE("Rng different streams diverge", "[math][rng]") {
    Rng a; a.seed(99, 1);
    Rng b; b.seed(99, 2);
    int differences = 0;
    for (int i = 0; i < 32; ++i) {
        if (a.next_u32() != b.next_u32()) ++differences;
    }
    REQUIRE(differences > 16);  // streams should look statistically distinct
}

TEST_CASE("StratifiedSampler2D respects stratum boundaries", "[math][sampler]") {
    StratifiedSampler2D s;
    s.rng.seed(42);
    s.reset(4, 4);

    Sample2D samples[16];
    s.fill(samples, 16);

    // Sample i should fall in the cell (i % 4, i / 4) of the 4×4 grid.
    for (u32 i = 0; i < 16; ++i) {
        u32 ix = i % 4;
        u32 iy = i / 4;
        f32 u_lo = static_cast<f32>(ix) * 0.25f;
        f32 u_hi = u_lo + 0.25f;
        f32 v_lo = static_cast<f32>(iy) * 0.25f;
        f32 v_hi = v_lo + 0.25f;
        REQUIRE(samples[i].u >= u_lo);
        REQUIRE(samples[i].u <  u_hi);
        REQUIRE(samples[i].v >= v_lo);
        REQUIRE(samples[i].v <  v_hi);
    }
}

TEST_CASE("StratifiedSampler2D 1x1 still produces a valid sample", "[math][sampler]") {
    StratifiedSampler2D s;
    s.rng.seed(99);
    s.reset(1, 1);
    Sample2D x = s.next();
    REQUIRE(x.u >= 0.0f);
    REQUIRE(x.u <  1.0f);
    REQUIRE(x.v >= 0.0f);
    REQUIRE(x.v <  1.0f);
}

TEST_CASE("StratifiedSampler2D guards against zero strata", "[math][sampler]") {
    StratifiedSampler2D s;
    s.rng.seed(1);
    s.reset(0, 0);
    REQUIRE(s.count() == 1);
    Sample2D x = s.next();
    REQUIRE(x.u >= 0.0f);
}

TEST_CASE("StratifiedSampler2D wraps cursor when full", "[math][sampler]") {
    StratifiedSampler2D s;
    s.rng.seed(3);
    s.reset(2, 2);
    s.next(); s.next(); s.next(); s.next();
    REQUIRE(s.cursor == 0);  // wrapped back around
}
