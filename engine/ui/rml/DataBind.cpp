// SPDX-License-Identifier: MIT
// Psynder — public per-element setters for the RmlUi binding. Lane 17.
//
// Two-backend implementation behind a single API:
//
//   * In-tree (default): the setters walk the DOM the in-tree parser
//     built (see Parser.cpp / Layout.cpp), update the targeted field,
//     and re-cascade the document so style overrides reach
//     `computed_style` in time for the next render() tick.  No reparse
//     of the .rml/.rcss is required — only the cascade.
//
//   * Vendored RmlUi (`PSYNDER_HAS_RMLUI=1`): same call paths route
//     through `Rml::ElementDocument::GetElementById()` plus the
//     element's `SetInnerRML` / `SetAttribute` methods.  The vendor
//     branch is gated by `PSYNDER_VENDOR_RMLUI=ON` and the same option
//     drives the FetchContent pull of upstream RmlUi + FreeType (see
//     CMakeLists.txt).
//
// The setters are deliberately scoped to the two primitives we needed
// to retire `test_only::reload_with_source` from sample_04: a text
// setter for elements like `<div id="speed-value">120</div>`, and an
// attribute setter for the `style="width:N"` / `style="top:N; height:N"`
// overrides the HUD uses to grow pedal bars + the rpm fill rectangle.

#include "DataBind.h"
#include "Rml_internal.h"

#include "core/Log.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#if defined(PSYNDER_HAS_RMLUI)
#  include <RmlUi/Core.h>
#  include <RmlUi/Core/Context.h>
#  include <RmlUi/Core/Element.h>
#  include <RmlUi/Core/ElementDocument.h>
#endif

namespace psynder::ui::rml {

#if defined(PSYNDER_HAS_RMLUI)

// ─── Vendored RmlUi backend ──────────────────────────────────────────────
//
// `find_mutable_document` (in-tree DOM accessor) is irrelevant in the
// vendored path — we resolve documents through RmlUi's own registry.
// The engine init path (`initialize()`) is responsible for stashing a
// pointer to the active context; lane 17's vendor-bringup stores it in
// a TU-local pointer the setters read here.

namespace detail_vendor {

// First-mile stub: returns whatever pointer the vendored bring-up
// stored in the TU-local global below.  The Wave-E vendor verification
// only confirms `psynder_ui_rml` *links* against rmlui_core; a future
// wave wires `initialize()` to allocate the `Rml::Context*` after
// `Rml::Initialise()` succeeds and call `set_active_context(ctx)` here.
// Keeping the indirection in this TU means lane 17 owns the entire
// vendor bring-up surface without touching the frozen public header.
namespace {
::Rml::Context* g_active_context = nullptr;
}

::Rml::Context* active_context() noexcept { return g_active_context; }
[[maybe_unused]] void set_active_context(::Rml::Context* ctx) noexcept {
    g_active_context = ctx;
}

}  // namespace detail_vendor

namespace {

::Rml::Element* resolve_element(std::string_view doc_name,
                                std::string_view element_id) noexcept {
    auto* ctx = detail_vendor::active_context();
    if (!ctx) return nullptr;
    auto* doc = ctx->GetDocument(::Rml::String(doc_name.data(), doc_name.size()));
    if (!doc) return nullptr;
    return doc->GetElementById(::Rml::String(element_id.data(), element_id.size()));
}

}  // namespace

bool set_element_text(std::string_view doc_name,
                      std::string_view element_id,
                      std::string_view value) noexcept {
    auto* el = resolve_element(doc_name, element_id);
    if (!el) return false;
    el->SetInnerRML(::Rml::String(value.data(), value.size()));
    return true;
}

bool set_element_attribute(std::string_view doc_name,
                           std::string_view element_id,
                           std::string_view attr_name,
                           std::string_view value) noexcept {
    auto* el = resolve_element(doc_name, element_id);
    if (!el) return false;
    if (value.empty()) {
        el->RemoveAttribute(::Rml::String(attr_name.data(), attr_name.size()));
    } else {
        el->SetAttribute(::Rml::String(attr_name.data(), attr_name.size()),
                         ::Rml::String(value.data(),     value.size()));
    }
    return true;
}

#else

// ─── In-tree DOM backend ─────────────────────────────────────────────────

namespace {

// Depth-first search for an element with `id` in the document rooted at
// `root`.  Returns nullptr if the id isn't present.  The in-tree subset
// guarantees ids are unique by parser construction — we honour that by
// returning the first match.
detail::Element* find_by_id(detail::Element& root,
                            std::string_view id) noexcept {
    if (root.id == id) return &root;
    for (auto& child : root.children) {
        if (auto* hit = find_by_id(child, id)) return hit;
    }
    return nullptr;
}

// Split a class="a b c" string into tokens — matches the parser's own
// `parse_classes` (Parser.cpp) behaviour without round-tripping through
// the .rml parser.
void split_classes(std::string_view raw, std::vector<std::string>& out) {
    out.clear();
    usize start = 0;
    for (usize i = 0; i <= raw.size(); ++i) {
        if (i == raw.size()
            || std::isspace(static_cast<unsigned char>(raw[i]))) {
            if (i > start) out.emplace_back(raw.substr(start, i - start));
            start = i + 1;
        }
    }
}

}  // namespace

bool set_element_text(std::string_view doc_name,
                      std::string_view element_id,
                      std::string_view value) noexcept {
    auto* doc = detail::find_mutable_document(doc_name);
    if (!doc) return false;
    auto* el = find_by_id(doc->root, element_id);
    if (!el) return false;
    // The in-tree subset stores a single collapsed text run per
    // element.  Replacing it wholesale matches what RmlUi's
    // SetInnerRML does for a text-only child — no children are
    // emitted, no cascade re-run is necessary (text doesn't affect
    // box sizing in the Wave-A layout pass).
    el->text.assign(value.data(), value.size());
    return true;
}

bool set_element_attribute(std::string_view doc_name,
                           std::string_view element_id,
                           std::string_view attr_name,
                           std::string_view value) noexcept {
    auto* doc = detail::find_mutable_document(doc_name);
    if (!doc) return false;
    auto* el = find_by_id(doc->root, element_id);
    if (!el) return false;

    // Normalise the attribute name for the two special cases that
    // touch the style cascade.  Everything else is a verbatim map
    // entry — the parser keeps attribute names lower-cased and we
    // honour that here.
    std::string key(attr_name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    bool needs_recascade = false;

    if (key == "style") {
        // Re-derive the inline_style from scratch so the new value
        // wins cleanly (clearing properties that disappeared from the
        // old declaration string).  The cascade re-merge below stamps
        // the result into `computed_style`.
        el->inline_style = detail::StyleBlock{};
        if (!value.empty()) {
            detail::apply_declarations(value, el->inline_style);
        }
        if (value.empty()) {
            el->attributes.erase(key);
        } else {
            el->attributes[key].assign(value.data(), value.size());
        }
        needs_recascade = true;
    } else if (key == "class") {
        split_classes(value, el->classes);
        if (value.empty()) {
            el->attributes.erase(key);
        } else {
            el->attributes[key].assign(value.data(), value.size());
        }
        needs_recascade = true;
    } else if (key == "id") {
        // Renaming an element by setting `id="..."` is exotic enough
        // that we still allow it but skip the cascade because the
        // computed style is keyed by the element pointer, not the id.
        el->id.assign(value.data(), value.size());
        if (value.empty()) {
            el->attributes.erase(key);
        } else {
            el->attributes[key].assign(value.data(), value.size());
        }
    } else {
        if (value.empty()) {
            el->attributes.erase(key);
        } else {
            el->attributes[key].assign(value.data(), value.size());
        }
    }

    if (needs_recascade) {
        // The cascade walks the whole tree; for a Wave-A HUD with ~16
        // elements this is sub-microsecond and lets descendant
        // inheritance (text-color, font-size) keep working when a
        // class change flips a parent rule.
        detail::apply_cascade(doc->root, doc->sheet);
    }
    return true;
}

#endif  // PSYNDER_HAS_RMLUI

}  // namespace psynder::ui::rml
