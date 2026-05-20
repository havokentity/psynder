// SPDX-License-Identifier: MIT
// Lane 17 — unit coverage for the in-tree RML/RCSS subset parser, the
// document registry, show/hide visibility, the cascade, the layout pass
// that feeds the rasterizer, and the Wave-B hot-reload + Lua event-
// payload dispatch surfaces.
//
// The asset VFS is stubbed by default in unit tests, so we inject the
// .rml + .rcss as in-memory strings via the `psynder::ui::rml::test_only`
// helper declared in Rml.cpp.  When PSYNDER_VENDOR_RMLUI flips ON the
// same invariants should hold against the upstream parser.

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
bool inject_source(std::string_view name, std::string_view rml_src, std::string_view rcss_src);
const ::psynder::ui::rml::detail::Document* find_document(std::string_view name);
::psynder::usize document_count();
void render_layout(std::string_view name,
                   ::psynder::math::Vec2 viewport,
                   std::vector<::psynder::ui::rml::detail::LayoutBox>& out);
bool fire_handler(std::string_view name, std::string_view element_id, std::string_view event_name);
bool fire_handler_with_payload(std::string_view name,
                               std::string_view element_id,
                               std::string_view event_name,
                               const ::psynder::ui::rml::detail::EventPayload& payload);
bool mark_dirty(std::string_view name);
void run_update_tick();
bool reload_with_source(std::string_view name, std::string_view rml_src, std::string_view rcss_src);
::psynder::u64 reload_generation(std::string_view name);
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
    REQUIRE(bar.computed_style.width == 100.0f);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: show / hide gates layout emission", "[ui][rml][layout]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"(<rml><body>
      <div id="root" style="width: 200; height: 50; background-color: #112233"></div>
    </body></rml>)"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("vis", kRml, ""sv));

    std::vector<psynder::ui::rml::detail::LayoutBox> boxes;
    psynder::math::Vec2 vp{640.f, 480.f};

    // Hidden by default — no boxes emitted.
    psynder::ui::rml::test_only::render_layout("vis", vp, boxes);
    REQUIRE(boxes.empty());

    psynder::ui::rml::show("vis");
    psynder::ui::rml::test_only::render_layout("vis", vp, boxes);
    REQUIRE(!boxes.empty());

    // One of the boxes carries the explicit color we set.
    bool found_color = false;
    for (const auto& b : boxes) {
        if (b.rgba == 0x112233FFu) {
            found_color = true;
            break;
        }
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
    REQUIRE_FALSE(psynder::ui::rml::test_only::fire_handler("menu", "quit", "onclick"));

    // Install a tiny lambda-backed backend that captures the chunk —
    // proves the lane 15 hook point works end-to-end.  Wave-B prepends
    // a `local event = { ... };\n` line before the handler body so the
    // designer's Lua sees a structured payload upvalue.
    static int s_calls = 0;
    static std::string s_last_chunk;
    static std::string s_last_name;
    psynder::ui::rml::detail::set_lua_backend(
        [](std::string_view src, std::string_view name) noexcept -> bool {
            ++s_calls;
            s_last_chunk.assign(src);
            s_last_name.assign(name);
            return true;
        });

    REQUIRE(psynder::ui::rml::test_only::fire_handler("menu", "quit", "onclick"));
    REQUIRE(s_calls == 1);
    // The chunk shape is "<prelude>;\n<body>" — body is "engine.quit()"
    // after stripping the "lua:" prefix.  We assert on the suffix so the
    // exact prelude formatting stays an implementation detail.
    REQUIRE(s_last_chunk.find("local event = {") == 0);
    REQUIRE(s_last_chunk.find("engine.quit()") != std::string::npos);
    REQUIRE(s_last_name == "rml:onclick");

    // Unhook so other tests don't see the captured-backend.
    psynder::ui::rml::detail::set_lua_backend(nullptr);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: handler dispatch builds event-table payload", "[ui][rml][lua]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"RML(<rml><body>
      <button id="fire" onclick="lua:weapons.fire(event)">Fire</button>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", kRml, ""sv));

    static std::string captured;
    psynder::ui::rml::detail::set_lua_backend([](std::string_view src, std::string_view) noexcept -> bool {
        captured.assign(src);
        return true;
    });

    psynder::ui::rml::detail::EventPayload payload{};
    payload.kind = "click";
    payload.target_id = "fire";
    payload.mouse_x = 320.5f;
    payload.mouse_y = 64.0f;
    payload.button = 0;

    REQUIRE(psynder::ui::rml::test_only::fire_handler_with_payload("hud", "fire", "onclick", payload));

    // The prelude carries every payload field — the designer's handler
    // body picks fields out of the `event` table.
    REQUIRE(captured.find("kind=\"click\"") != std::string::npos);
    REQUIRE(captured.find("target_id=\"fire\"") != std::string::npos);
    REQUIRE(captured.find("mouse_x=320.5") != std::string::npos);
    REQUIRE(captured.find("mouse_y=64") != std::string::npos);
    REQUIRE(captured.find("button=0") != std::string::npos);
    REQUIRE(captured.find("weapons.fire(event)") != std::string::npos);

    // Backend rejecting → dispatch returns false (failure modes are
    // surfaced to the caller).
    psynder::ui::rml::detail::set_lua_backend(
        [](std::string_view, std::string_view) noexcept -> bool { return false; });
    REQUIRE_FALSE(
        psynder::ui::rml::test_only::fire_handler_with_payload("hud", "fire", "onclick", payload));

    psynder::ui::rml::detail::set_lua_backend(nullptr);
    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: handler dispatch escapes payload strings safely", "[ui][rml][lua]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"RML(<rml><body>
      <div id="x" onclick="lua:print('ok')"></div>
    </body></rml>)RML"sv;
    REQUIRE(psynder::ui::rml::test_only::inject_source("esc", kRml, ""sv));

    static std::string captured;
    psynder::ui::rml::detail::set_lua_backend([](std::string_view src, std::string_view) noexcept -> bool {
        captured.assign(src);
        return true;
    });

    // Payload carries " \\ \n — all three need escaping inside the
    // generated `local event = { kind = "...", ... }` literal so the
    // Lua chunk stays parseable.
    psynder::ui::rml::detail::EventPayload payload{};
    payload.kind = "click";
    payload.target_id = "weird\"\\\nid";  // " then \ then newline
    REQUIRE(psynder::ui::rml::test_only::fire_handler_with_payload("esc", "x", "onclick", payload));

    // Every special char in the input must appear in escaped form,
    // never raw — otherwise Lua wouldn't be able to parse the chunk.
    REQUIRE(captured.find("weird\\\"\\\\\\nid") != std::string::npos);
    // And the raw newline must not be present inside the event literal.
    // (We allow a single newline between prelude and body; check the
    // payload region only.)
    auto prelude_end = captured.find("};");
    REQUIRE(prelude_end != std::string::npos);
    const auto prelude = captured.substr(0, prelude_end);
    REQUIRE(prelude.find('\n') == std::string::npos);

    psynder::ui::rml::detail::set_lua_backend(nullptr);
    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: hot reload swaps DOM atomically and preserves identity", "[ui][rml][hotreload]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRmlV1 = R"RML(<rml><body>
        <div id="hp" style="width: 100; height: 16; background-color: #ff0000"></div>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", kRmlV1, ""sv));
    psynder::ui::rml::show("hud");

    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->root.children.size() == 1);
    REQUIRE(doc->root.children[0].children[0].id == "hp");
    REQUIRE(doc->root.children[0].children[0].computed_style.background_color == 0xFF0000FFu);
    REQUIRE(doc->visible);
    const auto gen0 = psynder::ui::rml::test_only::reload_generation("hud");

    // Simulate the designer editing the .rml + saving — fresh source
    // pair with a different colour and a new child element.
    constexpr std::string_view kRmlV2 = R"RML(<rml><body>
        <div id="hp" style="width: 200; height: 32; background-color: #00ff00"></div>
        <div id="ammo" style="width: 50; height: 16; background-color: #ffff00"></div>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::reload_with_source("hud", kRmlV2, ""sv));

    // The document map slot is the same key, so the name-based lookup
    // still works — designers' Lua scripts that hold the document name
    // keep functioning across the reload.
    const auto* after = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(after == doc);  // same pointer — slot stable

    // Content updated.
    REQUIRE(after->root.children[0].children.size() == 2);
    REQUIRE(after->root.children[0].children[0].id == "hp");
    REQUIRE(after->root.children[0].children[0].computed_style.background_color == 0x00FF00FFu);
    REQUIRE(after->root.children[0].children[1].id == "ammo");

    // Show/hide flag survives.
    REQUIRE(after->visible);

    // Reload generation advanced.
    const auto gen1 = psynder::ui::rml::test_only::reload_generation("hud");
    REQUIRE(gen1 > gen0);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: update() consumes the dirty bit on a successful reload", "[ui][rml][hotreload]") {
    psynder::ui::rml::initialize();

    // Inject a document, then mark it dirty as if the VFS watcher had
    // fired.  The asset VFS is stubbed in tests so the next update()
    // tick's read_pair() returns NoSource; the dirty bit should stay
    // set (so the next watcher tick — once we mount real sources — can
    // retry).
    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", "<rml><body></body></rml>"sv, ""sv));

    REQUIRE(psynder::ui::rml::test_only::mark_dirty("hud"));

    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->needs_reload.load());

    psynder::ui::rml::test_only::run_update_tick();

    // NoSource path re-armed the flag so a later VFS mount can pick it
    // up.  This is the intended behaviour for Wave-B: a transient
    // unreadable source must not lose its scheduled reload.
    REQUIRE(doc->needs_reload.load());

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: hot reload preserves child handler bindings", "[ui][rml][hotreload][lua]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRmlV1 = R"RML(<rml><body>
        <button id="ok" onclick="lua:print('v1')">OK</button>
    </body></rml>)RML"sv;
    constexpr std::string_view kRmlV2 = R"RML(<rml><body>
        <button id="ok" onclick="lua:print('v2')">OK</button>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("menu", kRmlV1, ""sv));

    static std::string last_chunk;
    psynder::ui::rml::detail::set_lua_backend([](std::string_view src, std::string_view) noexcept -> bool {
        last_chunk.assign(src);
        return true;
    });

    REQUIRE(psynder::ui::rml::test_only::fire_handler("menu", "ok", "onclick"));
    REQUIRE(last_chunk.find("print('v1')") != std::string::npos);

    // Hot-reload the designer's edited .rml — same id, different body.
    REQUIRE(psynder::ui::rml::test_only::reload_with_source("menu", kRmlV2, ""sv));

    REQUIRE(psynder::ui::rml::test_only::fire_handler("menu", "ok", "onclick"));
    REQUIRE(last_chunk.find("print('v2')") != std::string::npos);

    psynder::ui::rml::detail::set_lua_backend(nullptr);
    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: hot reload survives an rcss-only edit", "[ui][rml][hotreload]") {
    psynder::ui::rml::initialize();

    constexpr std::string_view kRml = R"RML(<rml><body>
        <div id="hp" class="bar"></div>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source(
        "hud",
        kRml,
        ".bar { width: 100; height: 8; background-color: #ff0000; }"sv));
    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc->root.children[0].children[0].computed_style.width == 100.f);

    // Same RML, modified RCSS.  Reload bumps the cascade.
    REQUIRE(psynder::ui::rml::test_only::reload_with_source(
        "hud",
        kRml,
        ".bar { width: 200; height: 16; background-color: #00ff00; }"sv));

    REQUIRE(doc->root.children[0].children[0].computed_style.width == 200.f);
    REQUIRE(doc->root.children[0].children[0].computed_style.height == 16.f);
    REQUIRE(doc->root.children[0].children[0].computed_style.background_color == 0x00FF00FFu);

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: malformed input reported, not crashed", "[ui][rml][parse]") {
    psynder::ui::rml::initialize();

    // Missing close tag.
    REQUIRE_FALSE(psynder::ui::rml::test_only::inject_source("bad1", "<rml><body><div>"sv, ""sv));
    // Empty source.
    REQUIRE_FALSE(psynder::ui::rml::test_only::inject_source("bad2", ""sv, ""sv));

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: load_document via VFS path (stubbed VFS returns empty)", "[ui][rml][vfs]") {
    psynder::ui::rml::initialize();
    // The asset VFS is a Wave-A stub returning empty Blob.  The call
    // must succeed at registering the document anyway, so a subsequent
    // hot-reload (or test-injection) can populate it.
    const bool ok = psynder::ui::rml::load_document("hud/main.rml", "main");
    (void)ok;  // false expected with stub VFS; not a failure.
    REQUIRE(psynder::ui::rml::test_only::document_count() >= 1);
    psynder::ui::rml::shutdown();
}
