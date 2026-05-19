// SPDX-License-Identifier: MIT
// Psynder — shadow-visibility denoiser. Lane 08 owns.
//
// Edge-aware à-trous wavelet filter, **2 passes** guided by depth + normal
// (DESIGN.md §8.2). The à-trous (Dammertz 2010 "Edge-Avoiding À-Trous
// Wavelet Transform for fast Global Illumination Filtering") is a separable
// stationary-kernel filter where successive passes use the *same* 5-tap
// Gaussian kernel but with *exponentially widening* stride between samples
// (step = 1, 2, 4, …). Two passes give us a roughly 9-tap effective radius
// while remaining edge-aware via depth + normal weights.
//
// Properties relied on by tests:
//   * Bilateral weights are all ≥ 0; output is a weighted convex
//     combination of input samples, so output ∈ [min(in), max(in)] over
//     the kernel window. In particular, where every sample has visibility
//     ≥ V0, the filtered output is also ≥ V0. For shadow-visibility in
//     [0, 1] this gives the monotonicity required by the unit test.
//   * Output never goes negative (inputs assumed in [0, 1]).

#include "Bvh.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace psynder::render::rt {

namespace {

PSY_FORCEINLINE
f32 gauss5(i32 d) noexcept {
    // 1/16, 4/16, 6/16, 4/16, 1/16 — discrete Gaussian.
    constexpr f32 w[5] = { 1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f };
    const i32 idx = std::clamp(d + 2, 0, 4);
    return w[idx];
}

// Bilateral weight from a depth delta.
// σ_z = 1/32 — about a third of a unit, matches typical world-space depth
// scales in shadow space. Falls off as e^{-|Δz| / σ_z}.
PSY_FORCEINLINE
f32 depth_weight(f32 dz) noexcept {
    return std::exp(-std::fabs(dz) * 32.0f);
}

// Bilateral weight from the angle between two normals. Uses (n·n')^4 to
// give a tight lobe — sharp creases stop the filter.
PSY_FORCEINLINE
f32 normal_weight(const f32* n0, const f32* n1) noexcept {
    const f32 d = n0[0]*n1[0] + n0[1]*n1[1] + n0[2]*n1[2];
    const f32 c = std::clamp(d, 0.0f, 1.0f);
    return c * c * c * c;
}

// One à-trous pass. `step` is the stride between samples in pixels — the
// classic à-trous schedule doubles step each pass (1, 2, 4, …). With a
// 5-tap base kernel this produces a 1+4*step radius per pass.
void atrous_pass(const f32* in, f32* out,
                 const f32* depth, const f32* normals,
                 u32 width, u32 height, i32 step)
{
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 idx = y * width + x;
            const f32 d0 = depth[idx];
            const f32 n0[3] = { normals[idx*3], normals[idx*3+1], normals[idx*3+2] };
            f32 sum_w = 0.0f;
            f32 sum_v = 0.0f;
            for (i32 dy = -2; dy <= 2; ++dy) {
                for (i32 dx = -2; dx <= 2; ++dx) {
                    const i32 sx = static_cast<i32>(x) + dx * step;
                    const i32 sy = static_cast<i32>(y) + dy * step;
                    if (sx < 0 || sx >= static_cast<i32>(width))  continue;
                    if (sy < 0 || sy >= static_cast<i32>(height)) continue;
                    const u32 sidx = static_cast<u32>(sy) * width + static_cast<u32>(sx);
                    const f32 d1 = depth[sidx];
                    const f32 n1[3] = { normals[sidx*3], normals[sidx*3+1], normals[sidx*3+2] };
                    const f32 wd  = depth_weight(d1 - d0);
                    const f32 wn  = normal_weight(n0, n1);
                    const f32 wg  = gauss5(dx) * gauss5(dy);
                    const f32 w   = wg * wd * wn;
                    sum_w += w;
                    sum_v += w * in[sidx];
                }
            }
            // Center pixel always has full weight (wg=6/16*6/16=36/256,
            // wd=1, wn=1), so sum_w > 0 always; the guard is defensive.
            out[idx] = (sum_w > 0.0f) ? (sum_v / sum_w) : in[idx];
            // Clamp to [0, max-in-kernel] is implicit because output is a
            // non-negative convex combination of non-negative inputs.
        }
    }
}

}  // namespace

void denoise_shadows(const DenoiseInput& in, f32* output_visibility) {
    if (!in.shadow_visibility || !in.depth || !in.normals || !output_visibility) return;
    if (in.width == 0 || in.height == 0) return;

    const usize n = static_cast<usize>(in.width) * in.height;
    // Two-pass à-trous: pass 1 step=1, pass 2 step=2 (classic schedule).
    // Use the output as a ping-pong target alongside a thread-local temp.
    static thread_local std::vector<f32> tmp;
    if (tmp.size() < n) tmp.resize(n);

    atrous_pass(in.shadow_visibility, tmp.data(),     in.depth, in.normals, in.width, in.height, 1);
    atrous_pass(tmp.data(),           output_visibility, in.depth, in.normals, in.width, in.height, 2);
}

}  // namespace psynder::render::rt
