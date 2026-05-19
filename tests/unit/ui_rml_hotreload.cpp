// SPDX-License-Identifier: MIT
// Lane 17 — Wave-B integration test: hot-reload through the real asset
// VFS (lane 05).  Writes `.rml` + `.rcss` to a scratch directory, mounts
// the directory in the VFS, calls `Rml::load_document`, edits the file,
// fires the VFS watcher poll, calls `Rml::update`, asserts the DOM
// reflects the edit.
//
// This is the "would it actually work in the editor?" path that
// validates the wiring lane-05 → lane-17 end-to-end.  The smaller
// in-memory tests in `ui_rml_parse.cpp` cover the swap semantics
// (visibility preservation, atomic replacement); this test covers the
// "real file edits → DOM updates" promise designers care about.

#include "asset/Vfs.h"
#include "asset/VfsInternal.h"
#include "ui/rml/Rml.h"
#include "ui/rml/Rml_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace psynder::ui::rml::test_only {
const ::psynder::ui::rml::detail::Document* find_document(std::string_view name);
::psynder::u64                      reload_generation(std::string_view name);
}  // namespace psynder::ui::rml::test_only

namespace {

fs::path make_scratch_dir(const char* tag) {
    static int counter = 0;
    fs::path base = fs::temp_directory_path() / "psynder_ui_rml_test";
    fs::create_directories(base);
    fs::path d = base / (std::string(tag) + "_" + std::to_string(++counter));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_file(const fs::path& p, std::string_view bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct ScopedReset {
    ScopedReset()  {
        // Reset asset VFS + drop the watcher thread so prior tests'
        // mounts don't leak in.
        psynder::asset::internal::reset_for_tests();
    }
    ~ScopedReset() {
        psynder::ui::rml::shutdown();
        psynder::asset::internal::reset_for_tests();
    }
};

}  // namespace

TEST_CASE("ui_rml: VFS watch -> update() reloads DOM end-to-end",
          "[ui][rml][hotreload][vfs]") {
    ScopedReset reset;

    auto dir = make_scratch_dir("hotreload");

    constexpr std::string_view kRmlV1 = R"RML(<rml><body>
        <div id="hp" style="width: 100; height: 16; background-color: #ff0000"></div>
    </body></rml>)RML";

    constexpr std::string_view kRcssV1 = "* { color: #ffffff; }";

    write_file(dir / "hud.rml",  kRmlV1);
    write_file(dir / "hud.rcss", kRcssV1);
    REQUIRE(psynder::asset::Vfs::Get().mount_directory(dir.string()));

    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::load_document("hud.rml", "hud"));
    psynder::ui::rml::show("hud");

    // Seed the watcher baseline (first poll just records mtime).
    psynder::asset::internal::poll_watchers_now();

    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->root.children[0].children.size() == 1);
    REQUIRE(doc->root.children[0].children[0].id == "hp");
    REQUIRE(doc->root.children[0].children[0].computed_style.background_color
            == 0xFF0000FFu);

    const auto gen0 = psynder::ui::rml::test_only::reload_generation("hud");

    // Simulate the designer editing the .rml in their editor.  Sleep
    // briefly to make sure the mtime advances on coarser-resolution
    // filesystems before we re-poll.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    constexpr std::string_view kRmlV2 = R"RML(<rml><body>
        <div id="hp"   style="width: 200; height: 32; background-color: #00ff00"></div>
        <div id="ammo" style="width: 50;  height: 16; background-color: #ffff00"></div>
    </body></rml>)RML";

    write_file(dir / "hud.rml", kRmlV2);

    // Drive the watcher: the .rml mtime advanced, so poll fires the
    // watch callback which flips needs_reload=true on the document.
    psynder::asset::internal::poll_watchers_now();

    // update() drains the dirty bit and atomically swaps in the new
    // DOM.  Document identity (the map slot, addressable by name) is
    // preserved — render() between frames continues to find "hud".
    psynder::ui::rml::update(0.f);

    // Re-fetch — the document pointer is stable across reload.
    const auto* after = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(after == doc);

    REQUIRE(after->root.children[0].children.size() == 2);
    REQUIRE(after->root.children[0].children[0].id == "hp");
    REQUIRE(after->root.children[0].children[0].computed_style.background_color
            == 0x00FF00FFu);
    REQUIRE(after->root.children[0].children[1].id == "ammo");

    // Show/hide flag survived.
    REQUIRE(after->visible);

    // Reload generation bumped.
    const auto gen1 = psynder::ui::rml::test_only::reload_generation("hud");
    REQUIRE(gen1 > gen0);

    // .rcss edits trip the watcher too — modify only the stylesheet and
    // confirm the cascade re-applies.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_file(dir / "hud.rcss",
        "* { color: #ffffff; } #hp { background-color: #0000ff; }");
    psynder::asset::internal::poll_watchers_now();
    psynder::ui::rml::update(0.f);

    // Inline style still wins (designer intent), so this is more a
    // smoke check that the rcss reload re-ran the cascade without
    // crashing.  The reload_generation counter advances either way.
    const auto gen2 = psynder::ui::rml::test_only::reload_generation("hud");
    REQUIRE(gen2 > gen1);

    psynder::ui::rml::hide("hud");
    REQUIRE_FALSE(after->visible);
}

TEST_CASE("ui_rml: sample 03 HUD asset parses and lays out",
          "[ui][rml][sample][vfs]") {
    // The shipped HUD lives under samples/03_quake_room/assets/.  We
    // resolve relative to the source dir so the test runs from any
    // build directory and on any host.
    ScopedReset reset;

    const fs::path samples_dir = fs::path(PSYNDER_SOURCE_DIR)
        / "samples" / "03_quake_room" / "assets";
    REQUIRE(fs::is_regular_file(samples_dir / "hud.rml"));
    REQUIRE(fs::is_regular_file(samples_dir / "hud.rcss"));

    REQUIRE(psynder::asset::Vfs::Get().mount_directory(samples_dir.string()));
    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::load_document("hud.rml", "hud"));
    psynder::ui::rml::show("hud");

    // Sanity-check the structure designers committed: hud-root contains
    // the per-region panels, and the pause menu starts hidden.
    const auto* doc = psynder::ui::rml::test_only::find_document("hud");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->root.tag == "rml");

    // Walk to find the hud-root element by id (the parser uses
    // depth-first, so we scan the whole tree).
    const psynder::ui::rml::detail::Element* root_div   = nullptr;
    const psynder::ui::rml::detail::Element* pause_menu = nullptr;
    const psynder::ui::rml::detail::Element* quit_btn   = nullptr;
    std::vector<const psynder::ui::rml::detail::Element*> stack{ &doc->root };
    while (!stack.empty()) {
        const auto* el = stack.back();
        stack.pop_back();
        if (el->id == "hud-root")   root_div   = el;
        if (el->id == "pause-menu") pause_menu = el;
        if (el->id == "quit")       quit_btn   = el;
        for (const auto& c : el->children) stack.push_back(&c);
    }
    REQUIRE(root_div   != nullptr);
    REQUIRE(pause_menu != nullptr);
    REQUIRE(quit_btn   != nullptr);

    // Pause menu carries `.hidden` so display:none cascades onto it.
    REQUIRE(pause_menu->computed_style.display_none);

    // Quit button has its inline-attribute handler set.
    auto h = quit_btn->handlers.find("onclick");
    REQUIRE(h != quit_btn->handlers.end());
    REQUIRE(h->second == "lua:engine.quit()");

    // Layout against a sample 1280x720 viewport.  We don't assert exact
    // box counts — the parser/layout subset evolves with vendoring —
    // but rendering must produce *some* boxes for the visible chrome.
    std::vector<psynder::ui::rml::detail::LayoutBox> boxes;
    psynder::ui::rml::detail::layout(*doc,
        psynder::math::Vec2{1280.f, 720.f}, boxes);
    REQUIRE(!boxes.empty());
}

TEST_CASE("ui_rml: load_document watches both .rml and .rcss",
          "[ui][rml][hotreload][vfs]") {
    ScopedReset reset;

    auto dir = make_scratch_dir("watch_pair");
    write_file(dir / "panel.rml",
        "<rml><body><div id=\"x\"></div></body></rml>");
    write_file(dir / "panel.rcss",
        "#x { width: 64; height: 64; background-color: #112233; }");
    REQUIRE(psynder::asset::Vfs::Get().mount_directory(dir.string()));

    REQUIRE(psynder::ui::rml::initialize());
    REQUIRE(psynder::ui::rml::load_document("panel.rml", "panel"));

    psynder::asset::internal::poll_watchers_now();   // baseline
    const auto gen0 = psynder::ui::rml::test_only::reload_generation("panel");

    // Editing the .rcss only must still drive a reload — proves the
    // watch covers the companion stylesheet, not just the .rml.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_file(dir / "panel.rcss",
        "#x { width: 128; height: 96; background-color: #445566; }");
    psynder::asset::internal::poll_watchers_now();
    psynder::ui::rml::update(0.f);

    const auto* doc = psynder::ui::rml::test_only::find_document("panel");
    REQUIRE(doc != nullptr);
    REQUIRE(doc->root.children[0].children[0].computed_style.width  == 128.f);
    REQUIRE(doc->root.children[0].children[0].computed_style.height == 96.f);
    REQUIRE(doc->root.children[0].children[0].computed_style.background_color
            == 0x445566FFu);

    const auto gen1 = psynder::ui::rml::test_only::reload_generation("panel");
    REQUIRE(gen1 > gen0);
}
