// SPDX-License-Identifier: MIT
// Psynder - deterministic hash helper tests.

#include "core/HashHelpers.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;

TEST_CASE("hash helpers: 2D hashes are deterministic and seeded", "[core][hash]") {
    constexpr u32 a = hash_helpers::hash2_u32(12u, 34u, 56u);
    constexpr u32 b = hash_helpers::hash2_u32(12u, 34u, 56u);
    constexpr u32 c = hash_helpers::hash2_u32(12u, 34u, 57u);
    REQUIRE(a == b);
    REQUIRE(a != c);

    constexpr u32 murmur_a = hash_helpers::murmur_mix2_u32(12u, 34u, 56u);
    constexpr u32 murmur_b = hash_helpers::murmur_mix2_u32(12u, 34u, 56u);
    constexpr u32 murmur_c = hash_helpers::murmur_mix2_u32(12u, 35u, 56u);
    REQUIRE(murmur_a == murmur_b);
    REQUIRE(murmur_a != murmur_c);
}

TEST_CASE("hash helpers: unit conversions stay in range", "[core][hash]") {
    const f32 a = hash_helpers::hash2_unit24(1u, 2u, 3u);
    const f32 b = hash_helpers::hash2_unit32(1u, 2u, 3u);
    const f32 c = hash_helpers::murmur_mix2_unit32(1u, 2u, 3u);
    REQUIRE(a >= 0.0f);
    REQUIRE(a < 1.0f);
    REQUIRE(b >= 0.0f);
    REQUIRE(b < 1.0f);
    REQUIRE(c >= 0.0f);
    REQUIRE(c < 1.0f);
}
