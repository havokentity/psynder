// SPDX-License-Identifier: MIT
// Psynder — internal pixel-level helpers for the immediate-mode UI.
// Lane 16. Not a public contract — peer lanes go through Imm.h / Overlay.h.

#pragma once

#include "core/Types.h"
#include "render/Framebuffer.h"

namespace psynder::ui::imm::detail {

// Pack an RGBA8 pixel: R in the LOW byte, then G, B, A. On little-endian
// memory the bytes land as [R, G, B, A] — the standard RGBA8 layout the
// framebuffer, `clear_framebuffer`, and the platform present all use, so imm
// overlays composite in true colour. (The old encoding put R in the HIGH byte
// — 0xRRGGBBAA — which channel-swapped against the framebuffer and rendered,
// e.g., a dark-blue fill as bright red. Always pack via this helper; never
// hand-write 0xRRGGBBAA literals.)
inline constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 0xFFu) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

// Bytes-per-pixel for the pixel formats Lane 16 can draw into. We only
// promise RGBA8 / BGRA8 because the perf graph + gizmo work needs an
// addressable 32-bit pixel; paletted/RGB565 drop to a no-op draw.
inline u32 bytes_per_pixel(render::PixelFormat fmt) noexcept {
    switch (fmt) {
        case render::PixelFormat::RGBA8:
        case render::PixelFormat::BGRA8:
            return 4U;
        case render::PixelFormat::RGB565:
            return 2U;
        case render::PixelFormat::Paletted8:
            return 1U;
    }
    return 0U;
}

inline bool framebuffer_drawable(const render::Framebuffer& fb) noexcept {
    return fb.pixels != nullptr && fb.width > 0U && fb.height > 0U &&
           (fb.format == render::PixelFormat::RGBA8 || fb.format == render::PixelFormat::BGRA8);
}

// Address a 32-bit pixel without UB. Assumes `framebuffer_drawable`.
inline u32* pixel_row(const render::Framebuffer& fb, u32 y) noexcept {
    u8* row = fb.pixels + static_cast<usize>(fb.pitch) * y;
    return reinterpret_cast<u32*>(row);
}

inline void plot(const render::Framebuffer& fb, i32 x, i32 y, u32 colour) noexcept {
    if (x < 0 || y < 0)
        return;
    if (static_cast<u32>(x) >= fb.width || static_cast<u32>(y) >= fb.height)
        return;
    pixel_row(fb, static_cast<u32>(y))[static_cast<u32>(x)] = colour;
}

// Source-over alpha blend. Channels match the rgba() packing above: R is the
// low byte, A the high byte. Used by the brush preview + selection-highlight.
inline void plot_blend(const render::Framebuffer& fb, i32 x, i32 y, u32 colour) noexcept {
    if (x < 0 || y < 0)
        return;
    if (static_cast<u32>(x) >= fb.width || static_cast<u32>(y) >= fb.height)
        return;
    const u32 src = colour;
    const u32 src_a = (src >> 24) & 0xFFu;
    if (src_a == 0U)
        return;
    u32& dst = pixel_row(fb, static_cast<u32>(y))[static_cast<u32>(x)];
    if (src_a == 0xFFu) {
        dst = src;
        return;
    }
    const u32 dst_r = dst & 0xFFu;
    const u32 dst_g = (dst >> 8) & 0xFFu;
    const u32 dst_b = (dst >> 16) & 0xFFu;
    const u32 dst_a = (dst >> 24) & 0xFFu;
    const u32 src_r = src & 0xFFu;
    const u32 src_g = (src >> 8) & 0xFFu;
    const u32 src_b = (src >> 16) & 0xFFu;
    const u32 inv = 255U - src_a;
    const u32 out_r = (src_r * src_a + dst_r * inv) / 255U;
    const u32 out_g = (src_g * src_a + dst_g * inv) / 255U;
    const u32 out_b = (src_b * src_a + dst_b * inv) / 255U;
    const u32 out_a = src_a + (dst_a * inv) / 255U;
    dst = out_r | (out_g << 8) | (out_b << 16) | (out_a << 24);
}

// Clamp a [lo,hi] integer to an i32. Centralized so the overflow-safe
// clamps in the draw routines don't accumulate sign-conversion warnings.
inline i32 iclamp(i32 v, i32 lo, i32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace psynder::ui::imm::detail
