// SPDX-License-Identifier: MIT
// Psynder — Wave-A layout pass for the in-tree RML subset. Lane 17 owns.
//
// Walks the cascaded DOM and produces a flat list of axis-aligned
// `LayoutBox` rectangles to draw.  Wave-A semantics:
//
//   - The root element fills the viewport.
//   - Children flow top-to-bottom by default, taking their declared
//     width/height, or a portion of the parent box otherwise.
//   - `left` / `top` on an element switches it to absolute positioning
//     relative to the parent's origin.
//   - `display: none` skips the element and its subtree.
//
// Wave-A is consciously narrow: enough to render a HUD-style panel + a
// label-style child with text overlay, and to feed the rasterizer in a
// shape that lane 07 can consume the moment it lights up.  Full
// box-model + flex/grid is what flips the PSYNDER_VENDOR_RMLUI switch on
// down the road.

#include "Rml_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace psynder::ui::rml::detail {

namespace {

inline bool finite(f32 v) noexcept {
    return !std::isnan(v) && std::isfinite(v);
}

void layout_node(const Element& el, math::Vec2 origin, math::Vec2 avail, std::vector<LayoutBox>& out) {
    if (el.computed_style.display_none)
        return;

    const auto& cs = el.computed_style;

    // Resolve position
    math::Vec2 pos = origin;
    if (finite(cs.left))
        pos.x = origin.x + cs.left;
    if (finite(cs.top))
        pos.y = origin.y + cs.top;

    // Resolve size
    math::Vec2 size = avail;
    if (finite(cs.width))
        size.x = cs.width;
    if (finite(cs.height))
        size.y = cs.height;

    // Clamp to non-negative
    if (size.x < 0.f)
        size.x = 0.f;
    if (size.y < 0.f)
        size.y = 0.f;

    // Emit the background box if any color is set (root with no bg is
    // skipped so the UI compositor doesn't paint the viewport black).
    if (cs.background_color != 0) {
        LayoutBox box;
        box.origin = pos;
        box.size = size;
        box.rgba = cs.background_color;
        out.emplace_back(box);
    }

    // Lay out children top-to-bottom inside the box.
    math::Vec2 cursor = pos;
    for (const auto& child : el.children) {
        math::Vec2 child_avail{size.x, size.y};
        layout_node(child, cursor, child_avail, out);

        // Advance the cursor only for non-absolutely-positioned children.
        if (!finite(child.computed_style.top)) {
            f32 advance = finite(child.computed_style.height) ? child.computed_style.height : 0.f;
            cursor.y += advance;
        }
    }
}

}  // namespace

void layout(const Document& doc, math::Vec2 viewport, std::vector<LayoutBox>& out) {
    out.clear();
    if (!doc.visible)
        return;
    layout_node(doc.root, math::Vec2{0.f, 0.f}, viewport, out);
}

}  // namespace psynder::ui::rml::detail
