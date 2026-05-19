// SPDX-License-Identifier: MIT
// Lane 09 — bloom kernel separability.
//
// The Gaussian kernel used by apply_bloom() is built as the outer product of
// a 1-D 5-tap kernel g (radius=2, σ≈1.0). Separability is the property:
//
//   (g ⊗ g) * I  ==  g_v * (g_h * I)
//
// where '*' is 2-D convolution, '⊗' is outer product, g_h/g_v are 1-D
// convolutions along the corresponding axis. The lane's apply_bloom relies
// on this so it can replace an O(w*h*k*k) dense convolution with two
// O(w*h*k) passes.
//
// The test convolves a small synthetic HDR image with (a) the dense 2-D
// kernel and (b) the separable H-then-V pipeline, then compares pixels.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "render/post/Internal.h"

#include <random>
#include <vector>

using namespace psynder;  // for u8/u32/usize/f32
using namespace psynder::render::post::detail;

namespace {

std::vector<HdrPixel> make_random_hdr(u32 w, u32 h, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 2.0f);
    std::vector<HdrPixel> img(static_cast<usize>(w) * h);
    for (auto& p : img) {
        p.r = dist(rng);
        p.g = dist(rng);
        p.b = dist(rng);
        p.a = 1.0f;
    }
    return img;
}

}  // namespace

TEST_CASE("render_post: bloom kernel is separable (dense == h * v)",
          "[render_post][bloom]")
{
    const u32 W = 24;
    const u32 H = 18;
    const auto src = make_random_hdr(W, H, /*seed*/ 0xC0FFEEu);

    std::vector<HdrPixel> dense(static_cast<usize>(W) * H);
    std::vector<HdrPixel> sep(static_cast<usize>(W) * H);
    std::vector<HdrPixel> scratch(static_cast<usize>(W) * H);

    gaussian_dense(src.data(), dense.data(), W, H);
    gaussian_separable(src.data(), sep.data(), scratch.data(), W, H);

    // Element-wise equality within float roundoff. The two paths add the
    // same products in a different order, so a few ULPs of slack is right.
    for (usize i = 0; i < dense.size(); ++i) {
        REQUIRE(dense[i].r == Catch::Approx(sep[i].r).margin(1e-5f));
        REQUIRE(dense[i].g == Catch::Approx(sep[i].g).margin(1e-5f));
        REQUIRE(dense[i].b == Catch::Approx(sep[i].b).margin(1e-5f));
    }
}

TEST_CASE("render_post: bloom kernel preserves DC on flat image",
          "[render_post][bloom]")
{
    // A constant-color image should round-trip unchanged because the kernel
    // sums to 1 and the clamp-edge addressing is a no-op when every sample
    // is the same value.
    const u32 W = 16, H = 16;
    std::vector<HdrPixel> src(static_cast<usize>(W) * H, HdrPixel{0.3f, 0.6f, 0.9f, 1.0f});
    std::vector<HdrPixel> dst(src.size());
    std::vector<HdrPixel> scratch(src.size());

    gaussian_separable(src.data(), dst.data(), scratch.data(), W, H);

    for (const auto& p : dst) {
        REQUIRE(p.r == Catch::Approx(0.3f).margin(1e-5f));
        REQUIRE(p.g == Catch::Approx(0.6f).margin(1e-5f));
        REQUIRE(p.b == Catch::Approx(0.9f).margin(1e-5f));
    }
}

TEST_CASE("render_post: 1-D Gaussian kernel sums to one",
          "[render_post][bloom]")
{
    float s = 0.0f;
    for (usize i = 0; i < kBloomTaps; ++i) s += kBloomKernel[i];
    REQUIRE(s == Catch::Approx(1.0f).margin(1e-3f));
}
