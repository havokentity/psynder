// SPDX-License-Identifier: MIT
// Psynder — internal types for the RmlUi binding. Lane 17 owns.
//
// Wave-A scope: a small in-house RML/RCSS subset parser drives load_document
// / show / hide and produces a triangle stream that flows into lane 07's
// rasterizer. When `PSYNDER_HAS_RMLUI` is defined (gated by the
// PSYNDER_VENDOR_RMLUI cmake option which FetchContent-pulls upstream
// RmlUi + FreeType), the same public API delegates to the real library and
// these structs become the RenderInterface staging buffers.
//
// Keeping the parser in-tree lets Wave-A land with green builds on every
// host without forcing every dev box through a multi-megabyte third-party
// fetch.  The interface (load_document/show/hide/render/update) is
// identical either way, so designers' `.rml` / `.rcss` files keep working
// as the backend swaps in.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psynder::ui::rml::detail {

// ─── Style ───────────────────────────────────────────────────────────────
//
// Wave-A subset: position (left/top), size (width/height), background
// color, text color, font size, display.  Enough to render a HUD panel
// + label rectangle and demonstrate the cascade for the unit test.

struct StyleBlock {
    // Sentinel-NaN means "unset, inherit".
    f32  left              = std::numeric_limits<f32>::quiet_NaN();
    f32  top               = std::numeric_limits<f32>::quiet_NaN();
    f32  width             = std::numeric_limits<f32>::quiet_NaN();
    f32  height            = std::numeric_limits<f32>::quiet_NaN();
    u32  background_color  = 0;       // RGBA, 0 == transparent / unset
    u32  text_color        = 0;       // RGBA, 0 == inherit
    f32  font_size         = std::numeric_limits<f32>::quiet_NaN();
    bool display_none      = false;

    // Has any field been touched?  Helps the cascade decide which child
    // overrides which parent property.
    bool any_set() const noexcept;
};

// One RCSS rule: selector + style.  Wave-A supports `tag`, `#id`,
// `.class` selectors (single component only).
struct Rule {
    enum class SelectorKind : u8 { Tag, Id, Class, Universal };
    SelectorKind kind = SelectorKind::Universal;
    std::string  name;     // e.g. "div", "hud-bar", "warning"
    StyleBlock   style{};
};

// ─── DOM ─────────────────────────────────────────────────────────────────
//
// Wave-A subset: tag, id, class list, text content, children, computed
// style.  Enough to validate parse and render a panel + a label.

struct Element {
    std::string              tag;        // "rml", "body", "div", "span", "img", "p" ...
    std::string              id;
    std::vector<std::string> classes;
    std::string              text;       // text content (for leaf text nodes)
    std::unordered_map<std::string, std::string> attributes;

    StyleBlock               inline_style{};   // from style="..."
    StyleBlock               computed_style{}; // after cascade
    std::vector<Element>     children;

    // Inline event handlers (onclick="lua:foo()", etc.) — the Lua adapter
    // resolves these at fire time.
    std::unordered_map<std::string, std::string> handlers;

    // Diagnostics
    u32  source_line = 0;
};

// ─── Document ────────────────────────────────────────────────────────────
//
// A loaded .rml/.rcss pair lives as a Document.  `root` holds the parsed
// DOM; `sheet` holds the parsed RCSS rules; `visible` toggles show()/hide().
//
// The owner stores documents by stable name (the name passed to
// load_document) so designers' Lua scripts can reference them.

struct Document {
    std::string         name;
    std::string         rml_virtual_path;
    std::string         rcss_virtual_path;
    Element             root;
    std::vector<Rule>   sheet;
    bool                visible           = false;
    bool                needs_reload      = false;
    u64                 reload_generation = 0;
};

// ─── Parser ──────────────────────────────────────────────────────────────
//
// Parses a tiny HTML/XML-flavoured subset.  Sufficient to validate the
// shipped test corpus and to keep designers' simple HUDs rendering before
// the vendored RmlUi switch is flipped on.

struct ParseResult {
    bool         ok = false;
    std::string  error;
    u32          error_line = 0;
};

ParseResult parse_rml(std::string_view src, Element& out_root);
ParseResult parse_rcss(std::string_view src, std::vector<Rule>& out_rules);

// Cascades sheet rules into element.computed_style for the whole tree.
void apply_cascade(Element& root, const std::vector<Rule>& sheet);

// ─── Renderer staging ────────────────────────────────────────────────────
//
// A document, once cascaded, fans out into "boxes": axis-aligned rectangles
// with a fill color.  The renderer turns each box into two triangles and
// submits them to the rasterizer.  Text is currently rendered as a
// background-coloured panel (FreeType glyph rasterization lands once
// PSYNDER_VENDOR_RMLUI=ON pulls FreeType).

struct LayoutBox {
    math::Vec2 origin{0.f, 0.f};
    math::Vec2 size  {0.f, 0.f};
    u32        rgba   = 0;
    bool       outline = false;
};

void layout(const Document& doc, math::Vec2 viewport, std::vector<LayoutBox>& out);

// ─── Lua dispatch backend ────────────────────────────────────────────────
//
// Function-pointer hook installed by lane 15 (or by the upstream RmlUi
// Lua plugin once `PSYNDER_VENDOR_RMLUI=ON`).  Until installed, handler
// dispatch logs the chunk and returns false — no link-time dependency on
// the script lane is required.
using LuaExecFn = bool (*)(std::string_view source, std::string_view name) noexcept;
void set_lua_backend(LuaExecFn backend) noexcept;

// Called by Rml.cpp for every inline-attribute handler that fires.  See
// LuaBinding.cpp for the implementation.
bool dispatch_handler(std::string_view event_name,
                      std::string_view handler_body);

// Bootstrapping shim — installs the `rml` Lua table when a backend is
// present.  Idempotent.
void register_lua_surface();

}  // namespace psynder::ui::rml::detail
