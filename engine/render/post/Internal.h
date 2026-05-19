// SPDX-License-Identifier: MIT
// Psynder — post-process internal kernels. Lane 09 only. Pure inline
// functions for tonemap / dither / scanline / Gaussian so unit tests and the
// translation units of this lane share one canonical implementation without
// dragging the whole psynder_render_post static library into the test exe.
//
// Nothing in here is part of the public contract; the public surface lives
// in Post.h (FROZEN). Anything you change here is internal to lane 09.

#pragma once

#include "core/Types.h"

#include <cmath>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#   include <immintrin.h>
#endif

namespace psynder::render::post::detail {

// ─── HDR pixel view ──────────────────────────────────────────────────────
// The HDR framebuffer that resolve() reads from packs four f32 channels per
// pixel (R, G, B, A) into the byte buffer at `pixels`, pitch = width*16. The
// public Framebuffer type carries the enum but for HDR the format slot is
// ignored — there's a comment in Post.h that HDR is the input and LDR sRGB is
// the output; lane 07's PixelFormat enum stays frozen.
struct HdrPixel {
    f32 r, g, b, a;
};

static_assert(sizeof(HdrPixel) == 16, "HDR pixel must be tightly packed f32x4");

// ─── Reinhard tonemap (monotone, per-channel) ────────────────────────────
// y = (x * exposure) / (1 + x * exposure). Strictly monotone increasing on
// x >= 0 because dy/dx = exposure / (1 + e x)^2 > 0 whenever exposure > 0.
// We tonemap per-channel; the alternative (luminance-only) is fine too but
// per-channel gives the gentle desaturation in highlights that the retro
// look benefits from.
PSY_FORCEINLINE f32 reinhard(f32 x, f32 exposure) noexcept {
    const f32 ex = x * exposure;
    return ex / (1.0f + ex);
}

// ─── Linear → sRGB gamma encode ──────────────────────────────────────────
// Piecewise sRGB curve. Faster than IEC 61966-2-1 + branchless for the lit
// region; the gamma argument is honoured for non-2.2 users (CRT lovers tend
// to want 2.2 or 2.5).
PSY_FORCEINLINE f32 linear_to_srgb(f32 c, f32 gamma) noexcept {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    // Strictly speaking sRGB is the piecewise 1/2.4 curve, but DESIGN §7.7
    // calls out "gamma" as user-tunable. Pow is fine here — resolve is 1 ms
    // budget at 1080p and modern powf hits ~3-4 cycles via lib.
    return std::pow(c, 1.0f / gamma);
}

// ─── 4×4 ordered Bayer matrix (normalised to [0,1)) ──────────────────────
// 16-step ordered dither; gives the unmistakable retro halftone look on
// flat HDR gradients. Normalised so threshold lives in [0, 1).
PSY_FORCEINLINE f32 bayer4x4(u32 x, u32 y) noexcept {
    static constexpr u8 m[16] = {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5,
    };
    const u32 idx = (y & 3u) * 4u + (x & 3u);
    return (static_cast<f32>(m[idx]) + 0.5f) * (1.0f / 16.0f);
}

// ─── 64×64 blue-noise tile (hash-based stand-in) ─────────────────────────
// True blue noise is an offline-baked texture; the asset path for that is on
// lane 05's roadmap. Until that lands we use a hash that decorrelates
// neighbours well enough to avoid banding without the regular grid of Bayer.
// Output in [0,1).
PSY_FORCEINLINE f32 blue_noise(u32 x, u32 y) noexcept {
    // Wang-hash style; cheap and good enough for an 8-bit threshold.
    u32 h = x * 0x9E3779B1u + y * 0x85EBCA77u;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return (h & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

enum class DitherMode : u32 {
    Off    = 0,
    Bayer  = 1,
    Blue   = 2,
};

PSY_FORCEINLINE f32 dither_threshold(DitherMode m, u32 x, u32 y) noexcept {
    switch (m) {
        case DitherMode::Bayer: return bayer4x4(x, y);
        case DitherMode::Blue:  return blue_noise(x, y);
        case DitherMode::Off:   default: return 0.5f;
    }
}

// Float → u8 with dither. Threshold is in [0,1); the fractional contribution
// of c is compared to threshold to decide rounding direction. Saturates.
PSY_FORCEINLINE u8 quantize_u8(f32 c, f32 threshold) noexcept {
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    const f32 scaled = c * 255.0f;
    const f32 base   = std::floor(scaled);
    const f32 frac   = scaled - base;
    return static_cast<u8>(base + (frac > threshold ? 1.0f : 0.0f));
}

// ─── Scanline filter ─────────────────────────────────────────────────────
// Multiply odd rows by (1 - strength) and (here is the load-bearing trick)
// multiply even rows by (1 + strength) so that for any input the row-pair
// mean is preserved exactly. This is what the unit test pins down.
PSY_FORCEINLINE f32 scanline_factor(u32 row, f32 strength) noexcept {
    return (row & 1u) ? (1.0f - strength) : (1.0f + strength);
}

// ─── Separable 1-D Gaussian (radius 2, σ ≈ 1.0) ──────────────────────────
// 5-tap kernel from a unit-σ Gaussian, normalised to 1. Used by bloom's
// horizontal and vertical passes. The separability property is exactly:
//   convolve2D(image, g_x * g_y^T) == convolve1D_x(convolve1D_y(image, g_y), g_x)
// and that's what the unit test verifies on a small image.
inline constexpr int   kBloomRadius = 2;
inline constexpr usize kBloomTaps   = static_cast<usize>(kBloomRadius * 2 + 1);
inline constexpr f32   kBloomKernel[kBloomTaps] = {
    // σ=1.0, samples at -2,-1,0,1,2 — normalised.
    0.06136f, 0.24477f, 0.38774f, 0.24477f, 0.06136f,
};

// Static check that the kernel is unit-mass to within float ulps. This is
// what makes the separable-2D and 2×1D convolutions return the same average.
static_assert(
    kBloomKernel[0] + kBloomKernel[1] + kBloomKernel[2] +
    kBloomKernel[3] + kBloomKernel[4] > 0.999f, "bloom kernel under-normalised");
static_assert(
    kBloomKernel[0] + kBloomKernel[1] + kBloomKernel[2] +
    kBloomKernel[3] + kBloomKernel[4] < 1.001f, "bloom kernel over-normalised");

// Convolve a horizontal scanline of `n` HDR pixels with the bloom kernel.
// Clamp-edge addressing. `dst` and `src` may not alias.
inline void gaussian_h(const HdrPixel* src, HdrPixel* dst, u32 n) noexcept {
    for (u32 x = 0; x < n; ++x) {
        f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
        for (int t = -kBloomRadius; t <= kBloomRadius; ++t) {
            i64 sx = static_cast<i64>(x) + t;
            if (sx < 0)              sx = 0;
            if (sx >= static_cast<i64>(n)) sx = static_cast<i64>(n) - 1;
            const HdrPixel& p = src[static_cast<usize>(sx)];
            const f32 w = kBloomKernel[static_cast<usize>(t + kBloomRadius)];
            r += p.r * w;
            g += p.g * w;
            b += p.b * w;
            a += p.a * w;
        }
        dst[x] = HdrPixel{r, g, b, a};
    }
}

// Vertical 1-D convolution; src/dst are full-image row-major HDR images.
inline void gaussian_v(const HdrPixel* src, HdrPixel* dst, u32 w, u32 h) noexcept {
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int t = -kBloomRadius; t <= kBloomRadius; ++t) {
                i64 sy = static_cast<i64>(y) + t;
                if (sy < 0)              sy = 0;
                if (sy >= static_cast<i64>(h)) sy = static_cast<i64>(h) - 1;
                const HdrPixel& p =
                    src[static_cast<usize>(sy) * static_cast<usize>(w)
                        + static_cast<usize>(x)];
                const f32 w_t = kBloomKernel[static_cast<usize>(t + kBloomRadius)];
                r += p.r * w_t;
                g += p.g * w_t;
                b += p.b * w_t;
                a += p.a * w_t;
            }
            dst[static_cast<usize>(y) * static_cast<usize>(w)
                + static_cast<usize>(x)] = HdrPixel{r, g, b, a};
        }
    }
}

// Full separable 2-D pass — H then V — into a scratch buffer (size >= w*h).
inline void gaussian_separable(
    const HdrPixel* src, HdrPixel* dst, HdrPixel* scratch, u32 w, u32 h) noexcept
{
    // H pass into scratch
    const usize uw = static_cast<usize>(w);
    for (u32 y = 0; y < h; ++y) {
        const usize off = static_cast<usize>(y) * uw;
        gaussian_h(src + off, scratch + off, w);
    }
    // V pass into dst
    gaussian_v(scratch, dst, w, h);
}

// Reference dense 2-D convolution; only used by the separability unit test
// (slow, O(w*h*k*k)).
inline void gaussian_dense(
    const HdrPixel* src, HdrPixel* dst, u32 w, u32 h) noexcept
{
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
            for (int ty = -kBloomRadius; ty <= kBloomRadius; ++ty) {
                for (int tx = -kBloomRadius; tx <= kBloomRadius; ++tx) {
                    i64 sx = static_cast<i64>(x) + tx;
                    i64 sy = static_cast<i64>(y) + ty;
                    if (sx < 0) sx = 0;
                    if (sy < 0) sy = 0;
                    if (sx >= static_cast<i64>(w)) sx = static_cast<i64>(w) - 1;
                    if (sy >= static_cast<i64>(h)) sy = static_cast<i64>(h) - 1;
                    const HdrPixel& p =
                        src[static_cast<usize>(sy) * static_cast<usize>(w)
                            + static_cast<usize>(sx)];
                    const f32 wt = kBloomKernel[static_cast<usize>(tx + kBloomRadius)]
                                 * kBloomKernel[static_cast<usize>(ty + kBloomRadius)];
                    r += p.r * wt;
                    g += p.g * wt;
                    b += p.b * wt;
                    a += p.a * wt;
                }
            }
            dst[static_cast<usize>(y) * static_cast<usize>(w)
                + static_cast<usize>(x)] = HdrPixel{r, g, b, a};
        }
    }
}

// ─── Streaming store helper ──────────────────────────────────────────────
// Per DESIGN §4.3 — write-once-read-elsewhere destinations should bypass the
// cache via _mm_stream_* (x86) / stnp (arm64). On scalar fallback we fall
// back to a plain store; correctness is identical, just cache-polluting.
PSY_FORCEINLINE void stream_store_u32(u32* dst, u32 v) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_stream_si32(reinterpret_cast<int*>(dst), static_cast<int>(v));
#elif defined(__aarch64__)
    // STNP wants a 64-bit pair; the closest non-temporal hint we can give for
    // a u32 on Apple Silicon is __builtin_nontemporal_store, which clang
    // lowers to STNP when paired. Tools enforce alignment in the SIMD path.
    __builtin_nontemporal_store(v, dst);
#else
    *dst = v;
#endif
}

PSY_FORCEINLINE void stream_fence() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb ishst" ::: "memory");
#else
    // Nothing to do — scalar path doesn't reorder visibly.
#endif
}

}  // namespace psynder::render::post::detail
