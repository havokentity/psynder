// SPDX-License-Identifier: MIT
// Lane 17 — unit coverage for the in-tree RML/RCSS subset parser, the
// document registry, show/hide visibility, the cascade, and the layout
// pass that feeds the rasterizer.
//
// The asset VFS is stubbed in Wave-A, so we inject the .rml + .rcss as
// in-memory strings via the `psynder::ui::rml::test_only` helper
// declared in Rml.cpp.  When PSYNDER_VENDOR_RMLUI flips ON the same
// invariants should hold against the upstream parser.

#include "core/Types.h"
#include "math/Math.h"
#include "ui/rml/Rml.h"
#include "ui/rml/Rml_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

// Forward decls — internal symbols exposed by Rml.cpp for tests only.
namespace psynder::ui::rml::test_only {
bool                                inject_source(std::string_view name,
                                                  std::string_view rml_src,
                                                  std::string_view rcss_src);
const ::psynder::ui::rml::detail::Document* find_document(std::string_view name);
::psynder::usize                    document_count();
void                                render_layout(std::string_view name,
                                                  ::psynder::math::Vec2 viewport,
                                                  std::vector<::psynder::ui::rml::detail::LayoutBox>& out);
bool                                fire_handler(std::string_view name,
                                                 std::string_view element_id,
                                                 std::string_view event_name);
}  // namespace psynder::ui::rml::test_only

using namespace std::string_view_literals;

TEST_CASE("ui_rml: initialize/shutdown", "[ui][rml]") {
    REQUIRE(psynder::ui::rml::initialize());
    // Idempotent.
    REQUIRE(psynder::ui::rml::initialize());
    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: trivial RML parses", "[ui][rml][parse]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"(<rml>
  <head><title>HUD</title></head>
  <body><div id="root">hello</div></body>
</rml>)"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("trivial", kRml, ""sv));

    const auto* doc = psynder::ui::rml::test_only::find_document("trivial");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->root.tag == "rml");

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: RCSS rules cascade", "[ui][rml][cascade]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"(<rml>
  <body>
    <div id="panel" class="hud">
      <span class="warning">Low health</span>
    </div>
  </body>
</rml>)"sv;

    constexpr std::string_view kRcss = R"(
        /* universal default */
        * { color: #ffffff; }
        body { font-size: 16; }
        .hud { background-color: rgb(40, 40, 40); width: 320; height: 64; }
        #panel { left: 8; top: 12; }
        .warning { color: #ff4040; }
    )"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", kRml, kRcss));

    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->sheet.size() >= 5);

    // body → div#panel.hud → span.warning
    REQUIRE(doc->root.tag == "rml");
    REQUIRE(doc->root.children.size() >= 1);
    const auto& body = doc->root.children[0];
    REQUIRE(body.tag == "body");
    REQUIRE(body.children.size() >= 1);
    const auto& panel = body.children[0];
    REQUIRE(panel.tag == "div");
    REQUIRE(panel.id == "panel");
    REQUIRE(panel.classes.size() == 1);
    REQUIRE(panel.classes[0] == "hud");

    // .hud rule applied
    REQUIRE(panel.computed_style.background_color != 0);
    REQUIRE(panel.computed_style.width == 320.0f);
    REQUIRE(panel.computed_style.height == 64.0f);

    // #panel rule applied
    REQUIRE(panel.computed_style.left == 8.0f);
    REQUIRE(panel.computed_style.top == 12.0f);

    // Inherited font-size
    REQUIRE(panel.computed_style.font_size == 16.0f);

    // Warning span: id has no rule, class .warning sets red text.
    REQUIRE(panel.children.size() == 1);
    const auto& warn = panel.children[0];
    REQUIRE(warn.tag == "span");
    REQUIRE(warn.classes.size() == 1);
    REQUIRE(warn.classes[0] == "warning");
    REQUIRE(warn.computed_style.text_color == 0xFF4040FFu);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: inline-style attribute wins over RCSS", "[ui][rml][cascade]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"(<rml><body>
        <div id="bar" style="background-color: #00ff00; width: 100; height: 16;"></div>
    </body></rml>)"sv;
    constexpr std::string_view kRcss = R"(
        #bar { background-color: #ff0000; width: 999; }
    )"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("hud2", kRml, kRcss));
    const auto* doc = psynder::ui::rml::test_only::find_document("hud2");
    REQUIRE(doc != nullptr);

    const auto& bar = doc->root.children[0].children[0];
    REQUIRE(bar.id == "bar");
    REQUIRE(bar.computed_style.background_color == 0x00FF00FFu);  // inline wins
    REQUIRE(bar.computed_style.width            == 100.0f);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: show / hide gates layout emission", "[ui][rml][layout]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"(<rml><body>
      <div id="root" style="width: 200; height: 50; background-color: #112233"></div>
    </body></rml>)"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("vis", kRml, ""sv));

    std::vector<psynder::ui::rml::detail::LayoutBox> boxes;
    psynder::math::Vec2 vp{ 640.f, 480.f };

    // Hidden by default — no boxes emitted.
    psynder::ui::rml::test_only::render_layout("vis", vp, boxes);
    REQUIRE(boxes.empty());

    psynder::ui::rml::show("vis");
    psynder::ui::rml::test_only::render_layout("vis", vp, boxes);
    REQUIRE(!boxes.empty());

    // One of the boxes carries the explicit color we set.
    bool found_color = false;
    for (const auto& b : boxes) {
        if (b.rgba == 0x112233FFu) { found_color = true; break; }
    }
    REQUIRE(found_color);

    psynder::ui::rml::hide("vis");
    psynder::ui::rml::test_only::render_layout("vis", vp, boxes);
    REQUIRE(boxes.empty());

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: handler attributes captured and dispatchable", "[ui][rml][lua]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"RML(<rml><body>
      <button id="quit" onclick="lua:engine.quit()">Quit</button>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("menu", kRml, ""sv));

    const auto* doc = psynder::ui::rml::test_only::find_document("menu");
    REQUIRE(doc != nullptr);
    const auto& body = doc->root.children[0];
    const auto& btn = body.children[0];
    REQUIRE(btn.tag == "button");
    REQUIRE(btn.id == "quit");
    auto it = btn.handlers.find("onclick");
    REQUIRE(it != btn.handlers.end());
    REQUIRE(it->second == "lua:engine.quit()");

    // Dispatching with no backend returns false — the call itself must
    // succeed (the binding is wired) but the default no-op acknowledges
    // nothing happened.
    REQUIRE_FALSE(psynder::ui::rml::test_only::fire_handler(
        "menu", "quit", "onclick"));

    // Install a tiny lambda-backed backend that captures the chunk —
    // proves the lane 15 hook point works end-to-end.
    static int        s_calls = 0;
    static std::string s_last_chunk;
    static std::string s_last_name;
    psynder::ui::rml::detail::set_lua_backend(
        [](std::string_view src, std::string_view name) noexcept -> bool {
            ++s_calls;
            s_last_chunk.assign(src);
            s_last_name.assign(name);
            return true;
        });

    REQUIRE(psynder::ui::rml::test_only::fire_handler(
        "menu", "quit", "onclick"));
    REQUIRE(s_calls == 1);
    REQUIRE(s_last_chunk == "engine.quit()");        // "lua:" prefix stripped
    REQUIRE(s_last_name  == "rml:onclick");

    // Unhook so other tests don't see the captured-backend.
    psynder::ui::rml::detail::set_lua_backend(nullptr);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: malformed input reported, not crashed", "[ui][rml][parse]") {
    psynder::ui::rml::initialize();

    // Missing close tag.
    REQUIRE_FALSE(psynder::ui::rml::test_only::inject_source(
        "bad1", "<rml><body><div>"sv, ""sv));
    // Empty source.
    REQUIRE_FALSE(psynder::ui::rml::test_only::inject_source(
        "bad2", ""sv, ""sv));

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: load_document via VFS path (stubbed VFS returns empty)",
          "[ui][rml][vfs]") {
    psynder::ui::rml::initialize();
    // The asset VFS is a Wave-A stub returning empty Blob.  The call
    // must succeed at registering the document anyway, so a subsequent
    // hot-reload (or test-injection) can populate it.
    const bool ok = psynder::ui::rml::load_document("hud/main.rml", "main");
    (void)ok;  // false expected with stub VFS; not a failure.
    REQUIRE(psynder::ui::rml::test_only::document_count() >= 1);
    psynder::ui::rml::shutdown();
}
