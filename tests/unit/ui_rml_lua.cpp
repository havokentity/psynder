// SPDX-License-Identifier: MIT
// Lane 17 — Wave-B integration test: lane 17's Lua handler binding talks
// to lane 15's `Vm::execute_repl`, end-to-end.
//
// Lane 17 doesn't link `psynder_script` from its own CMake (the in-tree
// fallback would otherwise drag every Wave-A consumer onto the Lua VM).
// The shared `psynder_unit` test binary DOES link both lanes — lane 15
// adds itself via cmake_language(DEFER ...).  So this file is the single
// place where we can write a test that exercises the full handler chain
// in one process.
//
// The chain we cover:
//
//   .rml: onclick="lua:engine.last_fired = event.target_id"
//        │
//        ▼
//   detail::dispatch_handler(payload)
//        │
//        ▼
//   detail::set_lua_backend(thunk)
//        │
//        ▼  (thunk just forwards to Vm::execute_repl)
//   psynder::script::Vm::execute_repl(chunk, out)
//
// Wave-B requirement: the handler body must see a structured `event`
// table containing kind / target_id / mouse_x / mouse_y / button.

#include "ui/rml/Rml.h"
#include "ui/rml/Rml_internal.h"
#include "script/Script.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

// Forward decls from Rml.cpp (lane 17's test-only injection surface).
namespace psynder::ui::rml::test_only {
bool inject_source(std::string_view name, std::string_view rml_src, std::string_view rcss_src);
bool fire_handler_with_payload(std::string_view name,
                               std::string_view element_id,
                               std::string_view event_name,
                               const ::psynder::ui::rml::detail::EventPayload& payload);
}  // namespace psynder::ui::rml::test_only

namespace {

// The bridge function-pointer thunk.  Lane 17 takes a `LuaExecFn` shape
// `bool(*)(string_view, string_view) noexcept`; lane 15's REPL takes
// `(string_view, std::string&) -> bool` — the call shapes are close
// enough that a `noexcept` wrapper does the trick.  In production this
// trampoline lives in whichever lane wires the two together (today: in
// editor-core or the engine's startup driver; both link both lanes).
bool exec_via_repl(std::string_view source, std::string_view /*name*/) noexcept {
    try {
        std::string out;
        return psynder::script::Vm::Get().execute_repl(source, out);
    } catch (...) {
        return false;
    }
}

class ScriptVmFixture {
   public:
    ScriptVmFixture() {
        REQUIRE(psynder::script::Vm::Get().start());
        psynder::ui::rml::detail::set_lua_backend(&exec_via_repl);
    }
    ~ScriptVmFixture() {
        psynder::ui::rml::detail::set_lua_backend(nullptr);
        psynder::script::Vm::Get().shutdown();
    }
};

}  // namespace

using namespace std::string_view_literals;

TEST_CASE("ui_rml: handler runs through lane-15 Vm::execute_repl", "[ui][rml][lua][integration]") {
    psynder::ui::rml::initialize();
    ScriptVmFixture vm_fixture;

    // Seed a tiny script-side table so the handler body has somewhere
    // to write the event payload it received.
    std::string out;
    REQUIRE(psynder::script::Vm::Get().execute_repl("engine = engine or {}; engine.last_fired = ''",
                                                    out));

    constexpr std::string_view kRml = R"RML(<rml><body>
      <button id="fire"
              onclick="lua:engine.last_fired = event.target_id;
                       engine.last_x = event.mouse_x;
                       engine.last_button = event.button">Fire</button>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("hud", kRml, ""sv));

    psynder::ui::rml::detail::EventPayload payload{};
    payload.kind = "click";
    payload.target_id = "fire";
    payload.mouse_x = 128.f;
    payload.mouse_y = 96.f;
    payload.button = 0;

    REQUIRE(psynder::ui::rml::test_only::fire_handler_with_payload("hud", "fire", "onclick", payload));

    // Verify the script side received the payload via the `event` table.
    REQUIRE(psynder::script::Vm::Get().execute_repl("engine.last_fired", out));
    REQUIRE(out == "fire");

    // Round-trip through math: assert numerical equality on the Lua
    // side rather than the stringification (%g elides the trailing
    // ".0" for whole-number coordinates, so the REPL prints "128"
    // even though the original payload was 128.f).
    REQUIRE(psynder::script::Vm::Get().execute_repl("engine.last_x == 128", out));
    REQUIRE(out == "true");

    REQUIRE(psynder::script::Vm::Get().execute_repl("engine.last_button", out));
    REQUIRE(out == "0");

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: lane-15 wiring tolerates handler bodies that error",
          "[ui][rml][lua][integration]") {
    psynder::ui::rml::initialize();
    ScriptVmFixture vm_fixture;

    constexpr std::string_view kRml = R"RML(<rml><body>
      <div id="oops" onclick="lua:nonexistent_function()"></div>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("err", kRml, ""sv));

    psynder::ui::rml::detail::EventPayload payload{};
    payload.kind = "click";
    payload.target_id = "oops";

    // The handler body references a nil global; execute_repl reports
    // the runtime error by returning false.  Our dispatch surface
    // forwards the bool faithfully so the caller can react (e.g.,
    // surface the error in the editor console).
    REQUIRE_FALSE(
        psynder::ui::rml::test_only::fire_handler_with_payload("err", "oops", "onclick", payload));

    psynder::ui::rml::shutdown();
}

TEST_CASE("ui_rml: handler with no `lua:` prefix is still treated as Lua",
          "[ui][rml][lua][integration]") {
    psynder::ui::rml::initialize();
    ScriptVmFixture vm_fixture;

    // The convention is `onclick="lua:..."` but the dispatcher's
    // strip_lua_prefix() leaves a missing prefix alone.  This keeps the
    // door open for an `onclick="js:..."` future without breaking the
    // common case.
    constexpr std::string_view kRml = R"RML(<rml><body>
      <div id="tag" onclick="engine.tagged = 'no-prefix'"></div>
    </body></rml>)RML"sv;

    REQUIRE(psynder::ui::rml::test_only::inject_source("np", kRml, ""sv));

    std::string out;
    REQUIRE(psynder::script::Vm::Get().execute_repl("engine = engine or {}; engine.tagged = ''", out));

    psynder::ui::rml::detail::EventPayload payload{};
    payload.kind = "click";
    payload.target_id = "tag";

    REQUIRE(psynder::ui::rml::test_only::fire_handler_with_payload("np", "tag", "onclick", payload));

    REQUIRE(psynder::script::Vm::Get().execute_repl("engine.tagged", out));
    REQUIRE(out == "no-prefix");

    psynder::ui::rml::shutdown();
}
