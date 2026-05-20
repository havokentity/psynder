// SPDX-License-Identifier: MIT
// Psynder — scanline filter. Retro CRT-style darkening on every other row.
//
// The trick (and what render_post_scanline.cpp pins down): even rows are
// MULTIPLIED by (1 + strength), odd rows by (1 - strength). The arithmetic
// mean of any (even, odd) row pair is then exactly the mean of the original
// pair — so the per-image average luminance is preserved when height is
// even. (For odd height the last row picks up the "even" factor; the test
// uses even height to keep the invariant clean.)
//
// We support HDR float4 input (the typical call point — apply_scanline
// happens after bloom, before resolve), and LDR RGBA8/BGRA8 input (for
// post-resolve applications, e.g. when the user wants the scanline visible
// in screenshots saved straight from the LDR target).

#include "Post.h"
#include "Internal.h"

#include <algorithm>
#include <cstdint>

namespace psynder::render::post {

namespace {

PSY_FORCEINLINE u8 clamp_u8(f32 c) noexcept {
    return c <= 0.0f ? u8{0} : c >= 255.0f ? u8{255} : static_cast<u8>(c + 0.5f);
}

void apply_scanline_hdr(Framebuffer& fb, f32 strength) noexcept {
    auto* pixels = reinterpret_cast<detail::HdrPixel*>(fb.pixels);
    const usize pitch_pix = fb.pitch ? (fb.pitch / sizeof(detail::HdrPixel)) : fb.width;
    for (u32 y = 0; y < fb.height; ++y) {
        const f32 factor = detail::scanline_factor(y, strength);
        auto* row = pixels + static_cast<usize>(y) * pitch_pix;
        for (u32 x = 0; x < fb.width; ++x) {
            row[x].r *= factor;
            row[x].g *= factor;
            row[x].b *= factor;
            // alpha untouched
        }
    }
}

void apply_scanline_rgba8(Framebuffer& fb, f32 strength) noexcept {
    // Per-row factor; multiply each byte channel and clamp.
    for (u32 y = 0; y < fb.height; ++y) {
        const f32 factor = detail::scanline_factor(y, strength);
        auto* row = reinterpret_cast<u8*>(fb.pixels + static_cast<usize>(y) * fb.pitch);
        for (u32 x = 0; x < fb.width; ++x) {
            u8* p = row + x * 4u;
            p[0] = clamp_u8(static_cast<f32>(p[0]) * factor);
            p[1] = clamp_u8(static_cast<f32>(p[1]) * factor);
            p[2] = clamp_u8(static_cast<f32>(p[2]) * factor);
            // p[3] (alpha) untouched
        }
    }
}

}  // namespace

void apply_scanline(Framebuffer& fb, const ScanlineParams& params) {
    if (!params.enabled)
        return;
    if (!fb.pixels || fb.width == 0 || fb.height == 0)
        return;

    // strength clamped — at strength=1.0 odd rows go black and even rows
    // double; >1 would invert. The cvar is clamped in Cvars.cpp on change.
    const f32 strength = std::clamp(params.strength, 0.0f, 1.0f);
    if (strength == 0.0f)
        return;

    // Lane convention: an HDR framebuffer uses the same `Framebuffer` struct
    // but with `pitch >= width * 16` (4 floats per pixel). LDR uses `pitch
    // >= width * 4` (RGBA8 / BGRA8). The format slot is honoured for the
    // explicit LDR formats; anything else with a pitch big enough for HDR
    // is treated as HDR.
    const usize row_bytes_hdr = static_cast<usize>(fb.width) * sizeof(detail::HdrPixel);
    const bool is_hdr = fb.pitch >= row_bytes_hdr;

    if (is_hdr) {
        apply_scanline_hdr(fb, strength);
        return;
    }
    if (fb.format == PixelFormat::RGBA8 || fb.format == PixelFormat::BGRA8) {
        apply_scanline_rgba8(fb, strength);
    }
    // RGB565 / Paletted8 are not useful scanline targets; skip silently.
}

}  // namespace psynder::render::post
