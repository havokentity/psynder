// SPDX-License-Identifier: MIT
// Psynder — internal widget logic + hit-test helpers. Lane 16.
//
// Pure functions; no global state lives here. The button() public API
// drives these via the Context singleton.

#pragma once

#include "Context.h"
#include "Draw.h"
#include "Font.h"
#include "Pixel.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <string_view>

namespace psynder::ui::imm::detail {

// Axis-aligned rectangle hit-test. `pos` is the top-left corner; `size`
// must be positive. Edges are inclusive on the min side and exclusive on
// the max side — matches the half-open convention `filled_rect()` uses
// when it rasterizes the rect, so a click on the rightmost pixel still
// lands inside the visible region.
inline bool hit_test(math::Vec2 pos, math::Vec2 size, math::Vec2 point) noexcept {
    if (size.x <= 0.0f || size.y <= 0.0f)
        return false;
    return point.x >= pos.x && point.x < pos.x + size.x && point.y >= pos.y &&
           point.y < pos.y + size.y;
}

// Default colour palette (matches DESIGN.md's debug-overlay convention —
// dark slate panels with cyan accents). Channel order = RRGGBBAA per
// Pixel.h documentation.
struct Theme {
    u32 button_normal = rgba(0x28, 0x2C, 0x34);
    u32 button_hot = rgba(0x3E, 0x44, 0x4E);
    u32 button_active = rgba(0x5A, 0xC8, 0xFA);
    u32 button_border = rgba(0x16, 0x18, 0x1D);
    u32 label_text = rgba(0xE6, 0xE6, 0xE6);
    u32 graph_bg = rgba(0x10, 0x12, 0x16, 0xC0);
    u32 graph_axis = rgba(0x52, 0x57, 0x65);
    u32 graph_line = rgba(0x86, 0xE1, 0xFA);
    u32 selection = rgba(0xFF, 0xE8, 0x4D);
    u32 gizmo_x = rgba(0xF0, 0x4D, 0x4D);
    u32 gizmo_y = rgba(0x55, 0xC0, 0x55);
    u32 gizmo_z = rgba(0x4D, 0x7B, 0xF0);
    u32 brush_outline = rgba(0xFF, 0xFF, 0xFF, 0xC0);
    u32 brush_fill = rgba(0xFF, 0xFF, 0xFF, 0x40);
};

inline const Theme& theme() noexcept {
    static const Theme t{};
    return t;
}

// Returns true on "this frame, the user just released the mouse inside
// the button" — same trigger semantics Dear ImGui uses. Mutates the
// context's hot/active IDs so a chain of buttons gives consistent input
// routing (only one can be hot or active at a time).
inline bool button_logic(Context& ctx, math::Vec2 pos, math::Vec2 size, std::string_view label) noexcept {
    const u64 id = widget_id(pos, size, label.data(), label.size());
    const bool over = hit_test(pos, size, ctx.input.mouse);
    if (over)
        ctx.hot_id = id;

    bool triggered = false;
    if (ctx.active_id == id) {
        if (ctx.input.just_released()) {
            if (over)
                triggered = true;
            ctx.active_id = 0;
        }
    } else if (over && ctx.input.just_pressed()) {
        ctx.active_id = id;
    }
    return triggered;
}

// Pick the right fill colour for the current widget state.
inline u32 button_fill_colour(const Context& ctx, u64 id) noexcept {
    const Theme& th = theme();
    if (ctx.active_id == id)
        return th.button_active;
    if (ctx.hot_id == id)
        return th.button_hot;
    return th.button_normal;
}

// Render a button skin into the framebuffer at `pos` with `size`. The
// label is centred vertically; horizontally it's left-padded so common
// short labels stay readable when widgets are stacked tightly.
inline void draw_button(render::Framebuffer& fb,
                        math::Vec2 pos,
                        math::Vec2 size,
                        std::string_view label,
                        u32 fill_colour) noexcept {
    filled_rect(fb, pos, size, fill_colour);
    rect_outline(fb, pos, size, theme().button_border);
    if (!label.empty()) {
        const i32 text_w = static_cast<i32>(text_width(label));
        const i32 text_h = static_cast<i32>(kGlyphHeight);
        const i32 tx = static_cast<i32>(pos.x) + (static_cast<i32>(size.x) - text_w) / 2;
        const i32 ty = static_cast<i32>(pos.y) + (static_cast<i32>(size.y) - text_h) / 2;
        draw_text(fb, tx, ty, label, theme().label_text);
    }
}

}  // namespace psynder::ui::imm::detail
