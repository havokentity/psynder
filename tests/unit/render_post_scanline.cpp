// SPDX-License-Identifier: MIT
// Lane 09 — scanline preserves average luminance.
//
// The lane's scanline factor schedule is:
//   even row → (1 + strength)
//   odd  row → (1 - strength)
//
// So for any pair (even, odd) of consecutive rows the arithmetic mean of
// the post-scanline luminance equals the pre-scanline mean exactly. When
// the image has even height the per-image mean is preserved.
//
// Test strategy: synthesise a small HDR image, apply scanline_factor row-
// by-row, verify the image mean matches the pre-scanline mean within a
// tight float tolerance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "render/post/Internal.h"

#include <vector>

using namespace psynder;  // for u32/usize/f32
using namespace psynder::render::post::detail;

namespace {

float image_mean_luminance(const std::vector<HdrPixel>& img) {
    double sum = 0.0;
    for (const auto& p : img) {
        sum += 0.2126 * static_cast<double>(p.r)
             + 0.7152 * static_cast<double>(p.g)
             + 0.0722 * static_cast<double>(p.b);
    }
    return static_cast<float>(sum / static_cast<double>(img.size()));
}

// The invariant "image mean preserved" requires mean(even_rows) ==
// mean(odd_rows). The cleanest such pattern is one that varies along x but
// is identical across y (vertical stripes) — then every row has the same
// mean. The test image below uses that pattern so the invariant pins the
// kernel exactly, not the choice of test data.
std::vector<HdrPixel> make_test_image(u32 w, u32 h) {
    std::vector<HdrPixel> img(static_cast<usize>(w) * h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const float t = static_cast<float>(x) * 0.0625f;
            img[static_cast<usize>(y) * w + x] = HdrPixel{
                0.30f + 0.20f * std::sin(t),
                0.50f + 0.10f * std::cos(t * 1.3f),
                0.40f + 0.15f * std::sin(t * 0.7f + 1.0f),
                1.0f,
            };
        }
    }
    return img;
}

void apply_scanline_in_place(std::vector<HdrPixel>& img, u32 w, u32 h, float strength) {
    for (u32 y = 0; y < h; ++y) {
        const float f = scanline_factor(y, strength);
        for (u32 x = 0; x < w; ++x) {
            auto& p = img[static_cast<usize>(y) * w + x];
            p.r *= f;
            p.g *= f;
            p.b *= f;
        }
    }
}

}  // namespace

TEST_CASE("render_post: scanline preserves average luminance (even height)",
          "[render_post][scanline]")
{
    const u32 W = 32;
    const u32 H = 32;  // even — invariant holds exactly

    for (float strength : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        auto img = make_test_image(W, H);
        const float pre = image_mean_luminance(img);
        apply_scanline_in_place(img, W, H, strength);
        const float post = image_mean_luminance(img);
        // Tolerance: 32x32 image with ~0.5 luminance values, float math
        // accumulates ~1e-7 per add, so 1e-4 is loose enough.
        REQUIRE(post == Catch::Approx(pre).margin(1e-4f));
    }
}

TEST_CASE("render_post: scanline at strength=0 is identity",
          "[render_post][scanline]")
{
    const u32 W = 16, H = 16;
    auto a = make_test_image(W, H);
    auto b = a;
    apply_scanline_in_place(b, W, H, 0.0f);
    for (usize i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].r == b[i].r);
        REQUIRE(a[i].g == b[i].g);
        REQUIRE(a[i].b == b[i].b);
    }
}

TEST_CASE("render_post: scanline_factor schedule matches design",
          "[render_post][scanline]")
{
    const float s = 0.3f;
    // Even rows brightened, odd rows darkened
    REQUIRE(scanline_factor(0u, s) == Catch::Approx(1.0f + s));
    REQUIRE(scanline_factor(1u, s) == Catch::Approx(1.0f - s));
    REQUIRE(scanline_factor(2u, s) == Catch::Approx(1.0f + s));
    REQUIRE(scanline_factor(3u, s) == Catch::Approx(1.0f - s));
    // Pair mean is 1
    REQUIRE(0.5f * (scanline_factor(0u, s) + scanline_factor(1u, s))
            == Catch::Approx(1.0f));
}

TEST_CASE("render_post: scanline per-row-pair invariant on uniform pairs",
          "[render_post][scanline]")
{
    // When consecutive even/odd rows have the same content, the per-pair
    // mean (and hence image mean) is preserved exactly — this is the
    // foundation of the average-luminance theorem the test above relies on.
    const u32 W = 8, H = 8;
    std::vector<HdrPixel> img(static_cast<usize>(W) * H);
    // Fill all 8 rows with the same content (so even and odd rows match).
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            img[static_cast<usize>(y) * W + x] = HdrPixel{
                static_cast<float>(x) * 0.1f,
                static_cast<float>(x) * 0.07f,
                static_cast<float>(x) * 0.05f,
                1.0f,
            };
        }
    }
    const float pre = image_mean_luminance(img);
    apply_scanline_in_place(img, W, H, 0.4f);
    const float post = image_mean_luminance(img);
    // Per-pair invariant: 0.5*((1+s)*p + (1-s)*p) = p. Exact in float.
    REQUIRE(post == Catch::Approx(pre).margin(1e-6f));
}
