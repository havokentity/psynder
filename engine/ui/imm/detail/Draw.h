// SPDX-License-Identifier: MIT
// Psynder — internal drawing primitives. Lane 16 (immediate-mode UI).
//
// Lives in a `detail` namespace because peer lanes call into us through
// the public Imm.h surface (or the additional overlay headers below the
// `imm` namespace). The helpers stay header-inline so the unit tests in
// `tests/unit/ui_imm_*.cpp` can drive them without linking the lane lib.

#pragma once

#include "Pixel.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <cmath>

namespace psynder::ui::imm::detail {

// ─── Filled axis-aligned rectangle ───────────────────────────────────────
inline void filled_rect(render::Framebuffer& fb,
                        math::Vec2 origin,
                        math::Vec2 size,
                        u32 colour) noexcept {
    if (!framebuffer_drawable(fb)) return;
    if (size.x <= 0.0f || size.y <= 0.0f) return;

    const auto fw = static_cast<i32>(fb.width);
    const auto fh = static_cast<i32>(fb.height);
    const auto x0 = iclamp(static_cast<i32>(std::floor(origin.x)),               0, fw);
    const auto y0 = iclamp(static_cast<i32>(std::floor(origin.y)),               0, fh);
    const auto x1 = iclamp(static_cast<i32>(std::floor(origin.x + size.x)),      0, fw);
    const auto y1 = iclamp(static_cast<i32>(std::floor(origin.y + size.y)),      0, fh);
    if (x1 <= x0 || y1 <= y0) return;

    for (i32 y = y0; y < y1; ++y) {
        u32* row = pixel_row(fb, static_cast<u32>(y));
        for (i32 x = x0; x < x1; ++x) {
            row[x] = colour;
        }
    }
}

// ─── 1-pixel rectangle outline ───────────────────────────────────────────
//
// The outline includes the four corners — pixel-perfect for the unit-test
// "rect_outline pixel correctness" coverage. Top/bottom rows span the full
// horizontal extent; left/right columns avoid double-plotting the corners.
inline void rect_outline(render::Framebuffer& fb,
                         math::Vec2 origin,
                         math::Vec2 size,
                         u32 colour) noexcept {
    if (!framebuffer_drawable(fb)) return;
    if (size.x <= 0.0f || size.y <= 0.0f) return;

    const auto x0_raw = static_cast<i32>(std::floor(origin.x));
    const auto y0_raw = static_cast<i32>(std::floor(origin.y));
    const auto x1_raw = static_cast<i32>(std::floor(origin.x + size.x)) - 1;
    const auto y1_raw = static_cast<i32>(std::floor(origin.y + size.y)) - 1;
    if (x1_raw < 0 || y1_raw < 0) return;
    if (x0_raw >= static_cast<i32>(fb.width)) return;
    if (y0_raw >= static_cast<i32>(fb.height)) return;
    if (x1_raw < x0_raw || y1_raw < y0_raw) return;

    const auto fw = static_cast<i32>(fb.width);
    const auto fh = static_cast<i32>(fb.height);
    const auto x0 = iclamp(x0_raw, 0, fw - 1);
    const auto y0 = iclamp(y0_raw, 0, fh - 1);
    const auto x1 = iclamp(x1_raw, 0, fw - 1);
    const auto y1 = iclamp(y1_raw, 0, fh - 1);

    // Top + bottom edges (cover the corners).
    if (y0_raw >= 0) {
        u32* row = pixel_row(fb, static_cast<u32>(y0));
        for (i32 x = x0; x <= x1; ++x) row[x] = colour;
    }
    if (y1_raw < fh && y1_raw != y0_raw) {
        u32* row = pixel_row(fb, static_cast<u32>(y1));
        for (i32 x = x0; x <= x1; ++x) row[x] = colour;
    }
    // Left + right edges — skip the corners we just drew.
    const i32 yt = std::max(y0_raw, 0);
    const i32 yb = std::min(y1_raw, fh - 1);
    const i32 inner_top = (y0_raw >= 0) ? yt + 1 : yt;
    const i32 inner_bot = (y1_raw < fh) ? yb - 1 : yb;
    if (inner_top <= inner_bot) {
        if (x0_raw >= 0) {
            for (i32 y = inner_top; y <= inner_bot; ++y) {
                pixel_row(fb, static_cast<u32>(y))[x0] = colour;
            }
        }
        if (x1_raw < fw && x1_raw != x0_raw) {
            for (i32 y = inner_top; y <= inner_bot; ++y) {
                pixel_row(fb, static_cast<u32>(y))[x1] = colour;
            }
        }
    }
}

// ─── Bresenham line ──────────────────────────────────────────────────────
inline void line(render::Framebuffer& fb,
                 math::Vec2 a,
                 math::Vec2 b,
                 u32 colour) noexcept {
    if (!framebuffer_drawable(fb)) return;
    i32 x0 = static_cast<i32>(std::lround(a.x));
    i32 y0 = static_cast<i32>(std::lround(a.y));
    const i32 x1 = static_cast<i32>(std::lround(b.x));
    const i32 y1 = static_cast<i32>(std::lround(b.y));
    const i32 dx =  std::abs(x1 - x0);
    const i32 dy = -std::abs(y1 - y0);
    const i32 sx = x0 < x1 ? 1 : -1;
    const i32 sy = y0 < y1 ? 1 : -1;
    i32 err = dx + dy;
    while (true) {
        plot(fb, x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        const i32 e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ─── Anti-aliasing-free midpoint circle (used by brush previews) ─────────
//
// Filled = false → 1-pixel outline; filled = true → solid disk. Both keep
// the standard Bresenham symmetry so the result is bit-stable.
inline void circle(render::Framebuffer& fb,
                   math::Vec2 centre,
                   f32 radius,
                   u32 colour,
                   bool filled = false) noexcept {
    if (!framebuffer_drawable(fb)) return;
    if (radius <= 0.0f) return;
    const i32 cx = static_cast<i32>(std::lround(centre.x));
    const i32 cy = static_cast<i32>(std::lround(centre.y));
    i32 x = static_cast<i32>(std::lround(radius));
    i32 y = 0;
    i32 err = 1 - x;
    auto hspan = [&](i32 xa, i32 xb, i32 yy) {
        for (i32 xx = xa; xx <= xb; ++xx) plot(fb, xx, yy, colour);
    };
    while (x >= y) {
        if (filled) {
            hspan(cx - x, cx + x, cy + y);
            hspan(cx - x, cx + x, cy - y);
            hspan(cx - y, cx + y, cy + x);
            hspan(cx - y, cx + y, cy - x);
        } else {
            plot(fb, cx + x, cy + y, colour);
            plot(fb, cx - x, cy + y, colour);
            plot(fb, cx + x, cy - y, colour);
            plot(fb, cx - x, cy - y, colour);
            plot(fb, cx + y, cy + x, colour);
            plot(fb, cx - y, cy + x, colour);
            plot(fb, cx + y, cy - x, colour);
            plot(fb, cx - y, cy - x, colour);
        }
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
}

// Translucent disk via the alpha blender — for brush previews where the
// terrain underneath needs to remain visible.
inline void blended_disk(render::Framebuffer& fb,
                         math::Vec2 centre,
                         f32 radius,
                         u32 colour) noexcept {
    if (!framebuffer_drawable(fb)) return;
    if (radius <= 0.0f) return;
    const i32 cx = static_cast<i32>(std::lround(centre.x));
    const i32 cy = static_cast<i32>(std::lround(centre.y));
    const i32 r  = static_cast<i32>(std::lround(radius));
    const i32 r2 = r * r;
    for (i32 dy = -r; dy <= r; ++dy) {
        const i32 dy2 = dy * dy;
        for (i32 dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy2 <= r2) {
                plot_blend(fb, cx + dx, cy + dy, colour);
            }
        }
    }
}

}  // namespace psynder::ui::imm::detail
