// SPDX-License-Identifier: MIT
// Psynder — Lane 16 (immediate-mode UI). ALLOCATOR HEATMAP — Wave B.
//
// Renders the per-tag memory heatmap surface exposed by Lane 01's
// `engine/core/alloc/Heatmap.h` as a stack of horizontal bars. Each bar
// shows `current / budget` fill, coloured green / yellow / red by
// utilisation:
//
//   • green   (< 0.50)
//   • yellow  (0.50 .. 0.85)
//   • red     (> 0.85)
//
// Plus a thin outline at `peak / budget` so the editor can see how close
// each tag came to its budget over the lifetime of the snapshot.
//
// Header-only inline so unit tests can drive it without linking the lane
// library. A `Tag::Misc` row with `budget == 0` is rendered as a
// background-only bar (no fill), matching the convention from the
// existing `core_heatmap.cpp` Catch2 tests.

#pragma once

#include "Imm.h"
#include "Overlay.h"

#include "core/Types.h"
#include "core/alloc/Heatmap.h"
#include "math/Math.h"

namespace psynder::ui::imm {

namespace heatmap_detail {

// Pack RGBA bytes the same way Pixel.h's `rgba()` does.
inline constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 0xFFu) noexcept {
    return (static_cast<u32>(r) << 24)
         | (static_cast<u32>(g) << 16)
         | (static_cast<u32>(b) << 8)
         | static_cast<u32>(a);
}

inline constexpr u32 kColourGreen  = rgba(0x40, 0xC0, 0x40);
inline constexpr u32 kColourYellow = rgba(0xE0, 0xC0, 0x40);
inline constexpr u32 kColourRed    = rgba(0xE0, 0x40, 0x40);
inline constexpr u32 kColourPeak   = rgba(0xFF, 0xFF, 0xFF);
inline constexpr u32 kColourBg     = rgba(0x20, 0x20, 0x20, 0xC0);
inline constexpr u32 kColourFrame  = rgba(0x80, 0x80, 0x80);
inline constexpr u32 kColourEmpty  = rgba(0x40, 0x40, 0x40, 0xC0);

inline u32 ratio_colour(f32 ratio) noexcept {
    if (ratio > 0.85f) return kColourRed;
    if (ratio > 0.50f) return kColourYellow;
    return kColourGreen;
}

}  // namespace heatmap_detail

// ─── alloc_heatmap ───────────────────────────────────────────────────────
//
// Renders the per-tag allocator heatmap into the rectangle defined by
// `origin` and `size`. Reads `mem::tag_stats()` for the snapshot — every
// row is drawn even if its budget is zero (so the editor sees which tags
// are unbudgeted at a glance). Returns the number of rows actually
// rendered, so callers can sanity-check the snapshot.
inline u32 alloc_heatmap(math::Vec2 origin, math::Vec2 size) noexcept {
    namespace hd = heatmap_detail;

    if (size.x <= 4.0f || size.y <= 4.0f) return 0;

    // Background frame.
    imm::filled_rect(origin, size, hd::kColourBg);
    imm::rect_outline(origin, size, hd::kColourFrame);

    const auto stats = mem::tag_stats();
    const u32 row_count = static_cast<u32>(stats.size());
    if (row_count == 0) return 0;

    // Layout: each row is a horizontal bar with 2px top/bottom padding.
    const f32 row_h = (size.y - 2.0f) / static_cast<f32>(row_count);
    if (row_h <= 0.5f) return 0;

    for (u32 i = 0; i < row_count; ++i) {
        const mem::TagStat& s = stats[i];
        const math::Vec2 row_origin{
            origin.x + 1.0f,
            origin.y + 1.0f + static_cast<f32>(i) * row_h,
        };
        const math::Vec2 row_size{
            size.x - 2.0f,
            row_h - 1.0f,
        };
        if (row_size.y <= 0.5f) continue;

        // Empty-budget rows: draw a slim "no budget" marker bar.
        if (s.budget == 0) {
            imm::filled_rect(row_origin, row_size, hd::kColourEmpty);
            continue;
        }

        const f32 cur_ratio = static_cast<f32>(s.current)
                            / static_cast<f32>(s.budget);
        const f32 cur_clamped = cur_ratio > 1.0f ? 1.0f
                                : cur_ratio < 0.0f ? 0.0f : cur_ratio;
        const f32 peak_ratio = static_cast<f32>(s.peak)
                             / static_cast<f32>(s.budget);
        const f32 peak_clamped = peak_ratio > 1.0f ? 1.0f
                                 : peak_ratio < 0.0f ? 0.0f : peak_ratio;

        // Fill bar.
        const math::Vec2 fill_size{
            row_size.x * cur_clamped,
            row_size.y,
        };
        imm::filled_rect(row_origin, fill_size, hd::ratio_colour(cur_clamped));

        // Peak outline — single vertical line at peak ratio.
        const f32 peak_x = row_origin.x + row_size.x * peak_clamped;
        imm::line(math::Vec2{ peak_x, row_origin.y },
                  math::Vec2{ peak_x, row_origin.y + row_size.y },
                  hd::kColourPeak);

        // Subtle row frame so adjacent bars are distinguishable.
        imm::rect_outline(row_origin, row_size, hd::kColourFrame);
    }
    return row_count;
}

}  // namespace psynder::ui::imm
