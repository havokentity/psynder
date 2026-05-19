// SPDX-License-Identifier: MIT
// Psynder — per-element data-binding setters for the RmlUi binding. Lane 17.
//
// Wave-E goal: replace the per-frame source-rebuild hack the Wave-D HUD
// uses (`test_only::reload_with_source`) with a real, public setter
// surface that mirrors RmlUi's data-binding API.  Designers updating a
// HUD field per-frame should never need to round-trip through the .rml
// parser — they should poke the element and let layout + render see the
// change.
//
// Two-backend story:
//
//   * In-tree (default): the setters walk the DOM the in-tree parser
//     built, touch `text` / `attributes` / `inline_style` / `classes`
//     directly, and re-cascade the document so `style="..."` overrides
//     reach `computed_style` in time for the next render() pass.
//
//   * Vendored RmlUi (`PSYNDER_VENDOR_RMLUI=ON`, gates `PSYNDER_HAS_RMLUI=1`):
//     the same calls route through `Rml::ElementDocument::GetElementById()`
//     plus `Element::SetInnerRML` / `SetAttribute`.  Same observable
//     behaviour, real library underneath.
//
// The function shapes match what an `Rml::DataModelConstructor` would
// expose for a single primitive field, deliberately scoped narrow so the
// in-tree subset can fulfil them without growing a full reactive
// binding system.

#pragma once

#include "core/Types.h"

#include <string_view>

namespace psynder::ui::rml {

// Set the text content of the element identified by `element_id` inside
// the document named `doc_name`.  Returns true on success; false if the
// document or element doesn't exist.
//
// The text becomes the element's only text run (the in-tree subset
// stores one collapsed string per element).  Children are preserved.
bool set_element_text(std::string_view doc_name,
                      std::string_view element_id,
                      std::string_view value) noexcept;

// Set (or replace) an attribute on the element identified by `element_id`
// inside `doc_name`.  Returns true on success.  Empty `value` clears the
// attribute if it was present.
//
// Special-cased attributes:
//   * `style` — re-parses the declarations into `inline_style` and re-
//     cascades the document so `computed_style` reflects the new
//     overrides before the next render.
//   * `class` — splits whitespace-separated tokens into `classes` and
//     re-cascades.
//
// All other attribute names are stored verbatim in the element's
// attribute map.
bool set_element_attribute(std::string_view doc_name,
                           std::string_view element_id,
                           std::string_view attr_name,
                           std::string_view value) noexcept;

}  // namespace psynder::ui::rml
