// SPDX-License-Identifier: MIT
// Psynder — lane 25 unit test. Covers the golden-image comparator that
// underwrites every render-regression check in the repo. If this test
// passes and the comparator says "0% mismatch", the rest of the engine
// can trust that signal.

#include "../golden/image_diff.h"

#include <catch2/catch_test_macros.hpp>

using psynder::u32;
using psynder::u8;
using psynder::usize;
using psynder::testing::compare_images;
using psynder::testing::CompareResult;
using psynder::testing::Image;

namespace {

Image solid(u32 w, u32 h, u8 r, u8 g, u8 b) {
    Image im{};
    im.width = w;
    im.height = h;
    im.rgb.assign(static_cast<usize>(w) * h * 3, 0);
    for (usize i = 0, n = static_cast<usize>(w) * h; i < n; ++i) {
        im.rgb[i * 3 + 0] = r;
        im.rgb[i * 3 + 1] = g;
        im.rgb[i * 3 + 2] = b;
    }
    return im;
}

}  // namespace

TEST_CASE("samples_tests: identical images report zero mismatch", "[lane25][golden]") {
    const Image a = solid(8, 8, 100, 150, 200);
    const Image b = solid(8, 8, 100, 150, 200);
    const CompareResult r = compare_images(a, b);
    REQUIRE(r.sizes_match);
    REQUIRE(r.mismatch_count == 0);
    REQUIRE(r.total_pixels == 64);
    REQUIRE(r.mismatch_pct() == 0.0);
}

TEST_CASE("samples_tests: tiny per-channel delta is within tolerance", "[lane25][golden]") {
    // Single-channel difference of 4 is below the default 8-eps gate.
    Image a = solid(4, 4, 100, 150, 200);
    Image b = solid(4, 4, 104, 150, 200);
    const CompareResult r = compare_images(a, b);
    REQUIRE(r.sizes_match);
    REQUIRE(r.mismatch_count == 0);  // delta=4 < 8 => match
}

TEST_CASE("samples_tests: any channel above eps counts as a mismatch", "[lane25][golden]") {
    Image a = solid(4, 4, 100, 150, 200);
    Image b = solid(4, 4, 100, 150, 220);  // blue jumps by 20, > eps
    const CompareResult r = compare_images(a, b);
    REQUIRE(r.sizes_match);
    REQUIRE(r.mismatch_count == 16);
    REQUIRE(r.mismatch_pct() == 100.0);
}

TEST_CASE("samples_tests: partial mismatch reports exact percentage", "[lane25][golden]") {
    // 100-pixel image, mutate exactly 1 pixel beyond eps => 1.0%.
    Image a = solid(10, 10, 50, 50, 50);
    Image b = a;
    b.rgb[3 * 3 + 0] = 200;  // pixel index 3, red jump
    const CompareResult r = compare_images(a, b);
    REQUIRE(r.sizes_match);
    REQUIRE(r.mismatch_count == 1);
    REQUIRE(r.mismatch_pct() == 1.0);
}

TEST_CASE("samples_tests: differing sizes refuse to compare", "[lane25][golden]") {
    const Image a = solid(8, 8, 0, 0, 0);
    const Image b = solid(7, 8, 0, 0, 0);
    const CompareResult r = compare_images(a, b);
    REQUIRE_FALSE(r.sizes_match);
    REQUIRE(r.total_pixels == 0);
    REQUIRE(r.mismatch_pct() == 0.0);
}

TEST_CASE("samples_tests: tolerance bar honours DESIGN.md §14 0.5%", "[lane25][golden]") {
    // 200x200 = 40000 pixels. 0.5% = 200 allowed mismatches.
    Image a = solid(200, 200, 64, 96, 128);
    Image b = a;
    for (usize i = 0; i < 200; ++i) {
        b.rgb[i * 3 + 0] = 255;
    }
    const CompareResult r = compare_images(a, b);
    REQUIRE(r.sizes_match);
    REQUIRE(r.mismatch_count == 200);
    REQUIRE(r.mismatch_pct() <= 0.5);
}
