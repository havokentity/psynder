// SPDX-License-Identifier: MIT
// Lane 17 — Wave-E unit tests for the DataBind setters.
//
// Covers the in-tree backend's `set_element_text` +
// `set_element_attribute` paths.  The vendored-RmlUi path (gated by
// `PSYNDER_HAS_RMLUI`) routes through real RmlUi elements + Context and
// is opt-in / untested in CI per the Wave-E brief — flipping
// `PSYNDER_VENDOR_RMLUI=ON` validates that the binding *compiles + links*
// (`psynder_ui_rml` against `rmlui_core`); the live-binding hook lands in
// a later wave alongside the full RenderInterface bring-up.

#include "ui/rml/DataBind.h"
#include "ui/rml/Rml.h"
#include "ui/rml/Rml_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string_view>
#include <vector>

namespace psynder::ui::rml::test_only {
bool inject_source(std::string_view name, std::string_view rml_src, std::string_view rcss_src);
const ::psynder::ui::rml::detail::Document* find_document(std::string_view name);
void render_layout(std::string_view name,
                   ::psynder::math::Vec2 viewport,
                   std::vector<::psynder::ui::rml::detail::LayoutBox>& out);
}  // namespace psynder::ui::rml::test_only

namespace {

// Pre-built fixture matching the sample-04 HUD shape: a small text run
// to bind a number into, plus a bar whose width comes from a
// per-element `style="width:N"` attribute.  Mirrors the structure the
// Wave-E sample uses so the test exercises the same code path.
constexpr std::string_view kHudRml = R"RML(<rml><body>
    <div id="root">
        <div id="speed-value" class="speed-number">--</div>
        <div id="gear-letter" class="gear">N</div>
        <div id="rpm-fill" class="bar bar-fill" style="width:0"></div>
        <div id="thr-fill" class="pedal-fill"
             style="top:200; height:0"></div>
    </div>
</body></rml>)RML";

constexpr std::string_view kHudRcss = R"RCSS(
    .speed-number { color: #ffffff; font-size: 48; }
    .bar-fill     { height: 12; background-color: #ffa030; }
    .pedal-fill   { width:  20; background-color: #30ff30; }
)RCSS";

const psynder::ui::rml::detail::Element* find_by_id(const psynder::ui::rml::detail::Element& root,
                                                    std::string_view id) {
    if (root.id == id)
        return &root;
    for (const auto& child : root.children) {
        if (const auto* hit = find_by_id(child, id))
            return hit;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("ui_rml: set_element_text updates DOM text + rendered output", "[ui][rml][databind]") {
    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", kHudRml, kHudRcss));

    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);
    const auto* el = find_by_id(doc->root, "speed-value");
    REQUIRE(el != nullptr);
    REQUIRE(el->text == "--");

    // No such doc → returns false, no crash.
    REQUIRE_FALSE(psynder::ui::rml::set_element_text("missing", "speed-value", "120"));
    // No such element → false.
    REQUIRE_FALSE(psynder::ui::rml::set_element_text("hud", "nope", "x"));

    // Happy path.
    REQUIRE(psynder::ui::rml::set_element_text("hud", "speed-value", "120"));
    REQUIRE(el->text == "120");

    // A second update overwrites cleanly.
    REQUIRE(psynder::ui::rml::set_element_text("hud", "speed-value", "97"));
    REQUIRE(el->text == "97");

    // Single-character text (mirrors the HUD gear letter).
    REQUIRE(psynder::ui::rml::set_element_text("hud", "gear-letter", "R"));
    const auto* gear = find_by_id(doc->root, "gear-letter");
    REQUIRE(gear != nullptr);
    REQUIRE(gear->text == "R");

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: set_element_attribute(style) re-cascades into layout", "[ui][rml][databind]") {
    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", kHudRml, kHudRcss));
    psynder::ui::rml::show("hud");

    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);

    // Baseline — the .rml seeds `width:0` on rpm-fill (RCSS supplies
    // height/colour).  Cascade should have folded the inline style into
    // computed_style.width.
    const auto* rpm = find_by_id(doc->root, "rpm-fill");
    REQUIRE(rpm != nullptr);
    REQUIRE(rpm->computed_style.width == 0.f);

    // Update the style — width should reflect the new value after the
    // setter triggers the cascade.
    REQUIRE(psynder::ui::rml::set_element_attribute("hud", "rpm-fill", "style", "width:240"));
    REQUIRE(rpm->computed_style.width == 240.f);
    REQUIRE(rpm->attributes.at("style") == "width:240");

    // The new width must reach layout: rpm-fill is sized 240×12 (height
    // from the .bar-fill RCSS rule), so it emits a 240-wide box.
    std::vector<psynder::ui::rml::detail::LayoutBox> boxes;
    psynder::ui::rml::test_only::render_layout("hud", psynder::math::Vec2{1280.f, 720.f}, boxes);
    bool found_box = false;
    for (const auto& b : boxes) {
        if (b.size.x == 240.f && b.size.y == 12.f) {
            found_box = true;
            break;
        }
    }
    REQUIRE(found_box);

    // Switch the style to a two-property declaration — the parser must
    // overwrite the old inline_style cleanly so values that disappeared
    // from the new declaration don't leak through.
    REQUIRE(
        psynder::ui::rml::set_element_attribute("hud", "thr-fill", "style", "top:30; height:170"));
    const auto* thr = find_by_id(doc->root, "thr-fill");
    REQUIRE(thr != nullptr);
    REQUIRE(thr->computed_style.top == 30.f);
    REQUIRE(thr->computed_style.height == 170.f);

    // Clearing the style attribute (empty value) drops both the
    // attribute and the inline-style override; the RCSS `.bar-fill`
    // rule has no width so computed_style.width falls back to the
    // sentinel NaN ("inherit / unset").
    REQUIRE(psynder::ui::rml::set_element_attribute("hud", "rpm-fill", "style", ""));
    REQUIRE(std::isnan(rpm->computed_style.width));
    REQUIRE(rpm->attributes.find("style") == rpm->attributes.end());

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: set_element_attribute(class) re-cascades inheritance", "[ui][rml][databind]") {
    constexpr std::string_view kRml = R"RML(<rml><body>
        <div id="box"></div>
    </body></rml>)RML";
    constexpr std::string_view kRcss = R"RCSS(
        .small { width: 40; height: 40; background-color: #ff0000; }
        .big   { width: 200; height: 80; background-color: #00ff00; }
    )RCSS";

    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::test_only::inject_source("doc", kRml, kRcss));

    const auto* doc = psynder::ui::rml::test_only::find_document("doc");
    REQUIRE(doc != nullptr);

    // Initial: no class, no width.
    const auto* box = find_by_id(doc->root, "box");
    REQUIRE(box != nullptr);
    REQUIRE(std::isnan(box->computed_style.width));

    REQUIRE(psynder::ui::rml::set_element_attribute("doc", "box", "class", "small"));
    REQUIRE(box->computed_style.width == 40.f);
    REQUIRE(box->computed_style.height == 40.f);

    REQUIRE(psynder::ui::rml::set_element_attribute("doc", "box", "class", "big"));
    REQUIRE(box->computed_style.width == 200.f);
    REQUIRE(box->computed_style.height == 80.f);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: set_element_attribute stores arbitrary attribute verbatim",
          "[ui][rml][databind]") {
    constexpr std::string_view kRml = R"RML(<rml><body>
        <div id="link"></div>
    </body></rml>)RML";

    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::test_only::inject_source("doc", kRml, ""));
    const auto* doc = psynder::ui::rml::test_only::find_document("doc");
    REQUIRE(doc != nullptr);

    REQUIRE(psynder::ui::rml::set_element_attribute("doc", "link", "href", "lua:nav.next()"));
    const auto* el = find_by_id(doc->root, "link");
    REQUIRE(el != nullptr);
    REQUIRE(el->attributes.at("href") == "lua:nav.next()");

    // Empty value clears the entry.
    REQUIRE(psynder::ui::rml::set_element_attribute("doc", "link", "href", ""));
    REQUIRE(el->attributes.find("href") == el->attributes.end());

    psynder::ui::rml::shutdown();
}
