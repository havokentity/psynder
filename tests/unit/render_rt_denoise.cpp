// SPDX-License-Identifier: MIT
// Psynder — Lane 08 unit test (Wave B):
// Denoiser monotonicity. The à-trous filter is a non-negative-weighted
// convex combination of input visibilities; for an in-shadow pixel
// (unfiltered ≤ all neighbours' visibilities) the filtered result must
// be ≥ the unfiltered value. Concretely we make an image whose minimum
// visibility is V0 over the whole frame; filtered[i] must be ≥ V0 for
// every pixel.

#include <catch2/catch_test_macros.hpp>

#include "render/rt/Bvh.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace psynder;
using namespace psynder::render::rt;

namespace {

// Build a 32×32 image where:
//   * depth = 5.0 everywhere (one plane → all neighbours pass the depth
//     bilateral weight)
//   * normals = +Z everywhere (all neighbours pass the normal weight)
//   * visibility is a checker pattern between V0 and 1.0.
struct DenoiseFixture {
    static constexpr u32 W = 32;
    static constexpr u32 H = 32;
    std::vector<f32> vis;
    std::vector<f32> depth;
    std::vector<f32> normals;
    std::vector<f32> out;
};

void fill_fixture(DenoiseFixture& f, f32 v_in_shadow, f32 v_lit) {
    const u32 n = f.W * f.H;
    f.vis.assign(n, 0.0f);
    f.depth.assign(n, 5.0f);
    f.normals.assign(n * 3, 0.0f);
    f.out.assign(n, 0.0f);
    for (u32 j = 0; j < f.H; ++j) {
        for (u32 i = 0; i < f.W; ++i) {
            const u32 idx = j * f.W + i;
            const bool shadow = ((i ^ j) & 1) == 0;
            f.vis[idx] = shadow ? v_in_shadow : v_lit;
            f.normals[idx*3 + 0] = 0.0f;
            f.normals[idx*3 + 1] = 0.0f;
            f.normals[idx*3 + 2] = 1.0f;
        }
    }
}

void run_denoise(DenoiseFixture& f) {
    DenoiseInput in;
    in.shadow_visibility = f.vis.data();
    in.depth             = f.depth.data();
    in.normals           = f.normals.data();
    in.width             = f.W;
    in.height            = f.H;
    denoise_shadows(in, f.out.data());
}

}  // namespace

TEST_CASE("Denoiser is monotone — filtered ≥ V0 when V0 is the input minimum",
          "[render_rt][denoise][monotone]") {
    DenoiseFixture f;
    const f32 V0 = 0.10f;
    fill_fixture(f, V0, 0.95f);
    run_denoise(f);

    f32 fmin = +1e30f;
    f32 fmax = -1e30f;
    for (f32 v : f.out) { fmin = std::min(fmin, v); fmax = std::max(fmax, v); }
    INFO("filtered min=" << fmin << " max=" << fmax << " V0=" << V0);
    REQUIRE(fmin >= V0 - 1e-5f);
    // And it should not exceed the input maximum (0.95).
    REQUIRE(fmax <= 0.95f + 1e-5f);
}

TEST_CASE("Denoiser smooths a checker: variance drops sharply",
          "[render_rt][denoise]") {
    DenoiseFixture f;
    fill_fixture(f, 0.0f, 1.0f);
    // Variance of the input checker = 0.25 (Bernoulli p=0.5).
    f32 in_sum = 0.0f, in_sq = 0.0f;
    for (f32 v : f.vis) { in_sum += v; in_sq += v * v; }
    const u32 n = static_cast<u32>(f.vis.size());
    const f32 in_mean = in_sum / static_cast<f32>(n);
    const f32 in_var  = in_sq / static_cast<f32>(n) - in_mean * in_mean;

    run_denoise(f);
    f32 out_sum = 0.0f, out_sq = 0.0f;
    for (f32 v : f.out) { out_sum += v; out_sq += v * v; }
    const f32 out_mean = out_sum / static_cast<f32>(n);
    const f32 out_var  = out_sq / static_cast<f32>(n) - out_mean * out_mean;

    INFO("in_var=" << in_var << " out_var=" << out_var);
    // The two-pass à-trous should crush variance by at least 4×; a checker
    // on a smooth plane is the easy case (no edges to preserve).
    REQUIRE(out_var < in_var * 0.25f);
    // Mean should be roughly preserved (no DC drift) — within 5%.
    REQUIRE(std::fabs(out_mean - in_mean) < 0.05f);
}

TEST_CASE("Denoiser preserves a flat fully-lit image",
          "[render_rt][denoise]") {
    DenoiseFixture f;
    fill_fixture(f, 1.0f, 1.0f);  // all pixels = 1
    run_denoise(f);
    for (u32 i = 0; i < f.W * f.H; ++i) {
        REQUIRE(std::fabs(f.out[i] - 1.0f) < 1e-4f);
    }
}

TEST_CASE("Denoiser preserves a flat fully-in-shadow image at any V0",
          "[render_rt][denoise][monotone]") {
    DenoiseFixture f;
    fill_fixture(f, 0.05f, 0.05f);  // every pixel = V0 = 0.05
    run_denoise(f);
    for (u32 i = 0; i < f.W * f.H; ++i) {
        INFO("idx " << i << " out=" << f.out[i]);
        REQUIRE(f.out[i] >= 0.05f - 1e-5f);
        REQUIRE(f.out[i] <= 0.05f + 1e-5f);
    }
}
