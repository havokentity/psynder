// SPDX-License-Identifier: MIT
// Psynder — bloom pre-resolve pass.
//
// Operates on the HDR framebuffer (float4 per pixel) before resolve(). The
// downsample-blur-upsample pyramid is the standard 4-pass shape:
//
//   src HDR  ─┐
//             ├─ threshold > 1.0 → bright pixels written into a half-res mip
//             │  (mip 0). Below the threshold the mip pixel is zero.
//             ├─ each pass i: downsample to half, separable Gaussian blur.
//             ├─ four mips total at the default `passes = 4`.
//             └─ upsample-add back into src HDR with `intensity`.
//
// All scratch is heap-allocated via `new[]` for now — DESIGN §4 forbids
// per-frame OS allocs but a one-shot Bloom() call is a level-init context;
// when lane 04's frame allocator settles, Bloom() will take a FrameAllocator
// argument and Move::Move on instead. Marked TODO in the lane backlog.

#include "Post.h"
#include "Internal.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace psynder::render::post {

namespace {

using detail::HdrPixel;

// Box-2x2 downsample with bright-pass threshold. `thresh` is the luminance
// floor below which a sample contributes zero — keeps bloom out of the
// midtones where the look would just be a fuzzy smear.
void downsample_brightpass(
    const HdrPixel* src, u32 sw, u32 sh, HdrPixel* dst, u32 dw, u32 dh, f32 thresh) noexcept {
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            // 2x2 box, clamped
            f32 r = 0, g = 0, b = 0, a = 0;
            f32 wsum = 0.0f;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const u32 sx = std::min(x * 2u + static_cast<u32>(dx), sw - 1u);
                    const u32 sy = std::min(y * 2u + static_cast<u32>(dy), sh - 1u);
                    const HdrPixel& p = src[static_cast<usize>(sy) * sw + sx];
                    const f32 lum = 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b;
                    const f32 w = lum > thresh ? 1.0f : 0.0f;
                    r += p.r * w;
                    g += p.g * w;
                    b += p.b * w;
                    a += p.a * w;
                    wsum += 1.0f;
                }
            }
            wsum = wsum > 0.0f ? wsum : 1.0f;
            dst[static_cast<usize>(y) * dw + x] = HdrPixel{r / wsum, g / wsum, b / wsum, a / wsum};
        }
    }
}

// Plain 2x2 box downsample, no threshold (for mip n → mip n+1).
void downsample_box(const HdrPixel* src, u32 sw, u32 sh, HdrPixel* dst, u32 dw, u32 dh) noexcept {
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            f32 r = 0, g = 0, b = 0, a = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const u32 sx = std::min(x * 2u + static_cast<u32>(dx), sw - 1u);
                    const u32 sy = std::min(y * 2u + static_cast<u32>(dy), sh - 1u);
                    const HdrPixel& p = src[static_cast<usize>(sy) * sw + sx];
                    r += p.r;
                    g += p.g;
                    b += p.b;
                    a += p.a;
                }
            }
            dst[static_cast<usize>(y) * dw + x] = HdrPixel{r * 0.25f, g * 0.25f, b * 0.25f, a * 0.25f};
        }
    }
}

// Bilinear upsample-add: dst (full-res) += src (half-res) * intensity.
void upsample_add(const HdrPixel* src, u32 sw, u32 sh, HdrPixel* dst, u32 dw, u32 dh, f32 intensity) noexcept {
    for (u32 y = 0; y < dh; ++y) {
        const f32 fy = (static_cast<f32>(y) + 0.5f) * 0.5f - 0.5f;
        const i32 y0 = std::max(0, static_cast<i32>(std::floor(fy)));
        const i32 y1 = std::min(static_cast<i32>(sh) - 1, y0 + 1);
        const f32 wy = std::clamp(fy - static_cast<f32>(y0), 0.0f, 1.0f);
        for (u32 x = 0; x < dw; ++x) {
            const f32 fx = (static_cast<f32>(x) + 0.5f) * 0.5f - 0.5f;
            const i32 x0 = std::max(0, static_cast<i32>(std::floor(fx)));
            const i32 x1 = std::min(static_cast<i32>(sw) - 1, x0 + 1);
            const f32 wx = std::clamp(fx - static_cast<f32>(x0), 0.0f, 1.0f);

            const usize uy0 = static_cast<usize>(y0);
            const usize uy1 = static_cast<usize>(y1);
            const usize ux0 = static_cast<usize>(x0);
            const usize ux1 = static_cast<usize>(x1);
            const HdrPixel& p00 = src[uy0 * sw + ux0];
            const HdrPixel& p10 = src[uy0 * sw + ux1];
            const HdrPixel& p01 = src[uy1 * sw + ux0];
            const HdrPixel& p11 = src[uy1 * sw + ux1];

            const f32 r =
                (p00.r * (1 - wx) + p10.r * wx) * (1 - wy) + (p01.r * (1 - wx) + p11.r * wx) * wy;
            const f32 g =
                (p00.g * (1 - wx) + p10.g * wx) * (1 - wy) + (p01.g * (1 - wx) + p11.g * wx) * wy;
            const f32 b =
                (p00.b * (1 - wx) + p10.b * wx) * (1 - wy) + (p01.b * (1 - wx) + p11.b * wx) * wy;

            HdrPixel& d = dst[static_cast<usize>(y) * dw + x];
            d.r += r * intensity;
            d.g += g * intensity;
            d.b += b * intensity;
            // alpha untouched
        }
    }
}

}  // namespace

void apply_bloom(Framebuffer& fb, const BloomParams& params) {
    if (!fb.pixels || fb.width == 0 || fb.height == 0)
        return;

    // Public param: passes; clamp to [1, 6] to avoid degenerate sizes.
    const int passes = std::clamp(params.passes, 1, 6);
    const u32 w0 = fb.width;
    const u32 h0 = fb.height;

    // Compute the mip chain dimensions. Stop when either dimension drops to
    // 1 — going further yields no useful blur.
    struct Mip {
        u32 w, h;
        usize off;
    };
    std::vector<Mip> mips;
    mips.reserve(static_cast<usize>(passes));

    u32 mw = std::max(w0 / 2u, 1u);
    u32 mh = std::max(h0 / 2u, 1u);
    usize total = 0;
    for (int i = 0; i < passes; ++i) {
        mips.push_back({mw, mh, total});
        total += static_cast<usize>(mw) * mh;
        if (mw <= 1 || mh <= 1)
            break;
        mw = std::max(mw / 2u, 1u);
        mh = std::max(mh / 2u, 1u);
    }

    // Two interleaved buffers for the H-then-V Gaussian.
    auto buf_a = std::make_unique<HdrPixel[]>(total);
    auto buf_b = std::make_unique<HdrPixel[]>(total);

    // src HDR view
    auto* hdr = reinterpret_cast<HdrPixel*>(fb.pixels);
    const usize src_pitch_pix = fb.pitch ? (fb.pitch / sizeof(HdrPixel)) : w0;

    // Build a contiguous copy of the src into a tightly-packed buffer so the
    // pyramid code can assume row pitch == width. (The HDR framebuffer is
    // expected to be tight already but we accept any pitch.)
    std::vector<HdrPixel> src_tight(static_cast<usize>(w0) * h0);
    for (u32 y = 0; y < h0; ++y) {
        std::memcpy(src_tight.data() + static_cast<usize>(y) * w0,
                    hdr + static_cast<usize>(y) * src_pitch_pix,
                    static_cast<usize>(w0) * sizeof(HdrPixel));
    }

    // Mip 0: bright-pass downsample from src.
    {
        const auto& m = mips[0];
        downsample_brightpass(src_tight.data(), w0, h0, buf_a.get() + m.off, m.w, m.h, params.threshold);
        // Separable blur into buf_b at the same offset, using a per-row
        // scratch row in buf_b.
        // For correctness we use the gaussian_separable helper which needs a
        // full scratch image of size w*h. Allocate just for this mip.
        std::vector<HdrPixel> scratch(static_cast<usize>(m.w) * m.h);
        detail::gaussian_separable(buf_a.get() + m.off, buf_b.get() + m.off, scratch.data(), m.w, m.h);
    }
    // Mips 1..N: downsample from previous mip's blurred buf_b, blur into
    // current mip's buf_b. We use buf_a as scratch.
    for (usize i = 1; i < mips.size(); ++i) {
        const auto& prev = mips[i - 1];
        const auto& cur = mips[i];
        downsample_box(buf_b.get() + prev.off, prev.w, prev.h, buf_a.get() + cur.off, cur.w, cur.h);
        std::vector<HdrPixel> scratch(static_cast<usize>(cur.w) * cur.h);
        detail::gaussian_separable(buf_a.get() + cur.off,
                                   buf_b.get() + cur.off,
                                   scratch.data(),
                                   cur.w,
                                   cur.h);
    }

    // Composite each mip's blurred buf_b back up into the HDR src, halving
    // intensity at each pyramid step so coarser blur contributes less weight.
    f32 step_intensity = params.intensity;
    for (usize i = 0; i < mips.size(); ++i) {
        const auto& m = mips[i];
        upsample_add(buf_b.get() + m.off, m.w, m.h, src_tight.data(), w0, h0, step_intensity);
        step_intensity *= 0.5f;
    }

    // Write the composited HDR back into the framebuffer (respecting pitch).
    for (u32 y = 0; y < h0; ++y) {
        std::memcpy(hdr + static_cast<usize>(y) * src_pitch_pix,
                    src_tight.data() + static_cast<usize>(y) * w0,
                    static_cast<usize>(w0) * sizeof(HdrPixel));
    }
}

}  // namespace psynder::render::post
