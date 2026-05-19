// SPDX-License-Identifier: MIT
// Unit tests for psynder::console completion engine.
//
// Covers ScoreMatch semantics so the web-console JS and the C++
// frontend agree on what "top match" means.

#include "core/console/Completion.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace cn = psynder::console;

TEST_CASE("ScoreMatch prefers prefix > substring > fuzzy",
          "[core][console][completion]") {
    using PairVec = std::vector<std::pair<std::size_t, std::size_t>>;
    PairVec spans;

    // PREFIX wins for "r_denoiser" against the prefix "r_den".
    int prefix = cn::ScoreMatch("r_denoiser", "r_den", &spans);
    REQUIRE(prefix >= 1000);

    // SUBSTRING ranks below PREFIX but above FUZZY.
    int substr = cn::ScoreMatch("psy_denoiser", "den", &spans);
    REQUIRE(substr >= 500);
    REQUIRE(substr < 1000);

    // FUZZY (chars-in-order with gaps) is the lowest tier.
    int fuzzy = cn::ScoreMatch("r_perfect_overlay", "pfov", &spans);
    REQUIRE(fuzzy >= 100);
    REQUIRE(fuzzy < 500);

    // Non-match returns 0.
    REQUIRE(cn::ScoreMatch("r_denoiser", "z", nullptr) == 0);
}

TEST_CASE("ScoreMatch is case-insensitive", "[core][console][completion]") {
    REQUIRE(cn::ScoreMatch("r_Denoiser", "DEN", nullptr) > 0);
    REQUIRE(cn::ScoreMatch("R_DENOISER", "den", nullptr) > 0);
}

TEST_CASE("Utf8SafeTruncate doesn't split codepoints",
          "[core][console][completion]") {
    // Mix of ASCII and a 3-byte UTF-8 codepoint (U+2248 ≈ in UTF-8 is
    // 0xE2 0x89 0x88).
    std::string s = "approx \xE2\x89\x88 99";
    // Truncate to a byte index that lands inside the codepoint; expect
    // the truncated result to NOT include a partial codepoint at the tail.
    const std::size_t before_len = s.size();
    cn::Utf8SafeTruncate(s, 8);            // boundary inside the 0xE2 0x89 0x88 run
    REQUIRE(s.size() <= 8);
    REQUIRE(s.size() < before_len);
    // The last byte should not be a UTF-8 continuation byte (10xxxxxx).
    if (!s.empty()) {
        unsigned char tail = static_cast<unsigned char>(s.back());
        REQUIRE((tail & 0xC0) != 0x80);
    }
}
