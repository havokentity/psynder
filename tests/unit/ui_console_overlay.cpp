// SPDX-License-Identifier: MIT
// Psynder — software console overlay unit tests.
//
// Exercises the overlay's input pipeline end-to-end through a fake
// platform::Input: open on the tilde edge, type a cvar-set line, submit, and
// confirm the backend cvar changed. Also covers the toggle-frame backtick
// filter, Up-history recall, and a non-crashing draw into a real framebuffer.
//
// The overlay state is private (file-static in the lane), so we observe it
// indirectly via the console backend it drives — which is exactly the
// contract a host depends on.

#include "ui/console/ConsoleOverlay.h"

#include "core/console/Console.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "ui/imm/DebugHud.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;
using platform::Input;
using platform::KeyCode;
using platform::MouseState;

namespace {

// Fake input device. Per frame: queue typed text + edge-press keys, call the
// overlay, then advance_frame() to clear edges (held keys persist until
// release()). key_pressed() is a stable per-frame edge, matching the real
// backends.
class FakeInput final : public Input {
   public:
    bool key_down(KeyCode k) const override { return down_[idx(k)]; }
    bool key_pressed(KeyCode k) const override { return pressed_[idx(k)]; }
    const MouseState& mouse() const override { return mouse_; }
    std::span<const u32> text_input() const override { return text_; }

    void press(KeyCode k) {
        pressed_[idx(k)] = true;
        down_[idx(k)] = true;
    }
    void type(std::string_view s) {
        for (char c : s)
            text_.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
    }
    void type_cp(std::uint32_t cp) { text_.push_back(cp); }
    void set_mouse(f32 x, f32 y, bool left) {
        mouse_.x = x;
        mouse_.y = y;
        mouse_.left = left;
    }
    void set_wheel(f32 wheel) { mouse_.wheel = wheel; }

    // End-of-frame: drop edge flags + typed text; keep held keys down.
    void advance_frame() {
        for (auto& b : pressed_)
            b = false;
        text_.clear();
        mouse_.wheel = 0.0f;
    }
    void release_all() {
        for (auto& b : down_)
            b = false;
    }

   private:
    static usize idx(KeyCode k) { return static_cast<usize>(k); }
    static constexpr usize kKeys = static_cast<usize>(KeyCode::Count);
    mutable std::array<bool, kKeys> pressed_{};
    std::array<bool, kKeys> down_{};
    std::vector<std::uint32_t> text_{};
    MouseState mouse_{};
};

constexpr f32 kDt = 1.0f / 60.0f;

// Run one frame: overlay update with the fake's current scripted input, then
// clear the per-frame edges. Returns the capturing flag.
bool tick(FakeInput& in) {
    const bool cap = ui::console::update(in, kDt);
    in.advance_frame();
    return cap;
}

}  // namespace

render::Framebuffer make_console_test_fb(std::vector<std::uint32_t>& pixels) {
    pixels.assign(256 * 256, 0xFF000000u);
    render::Framebuffer fb{};
    fb.pixels = reinterpret_cast<std::uint8_t*>(pixels.data());
    fb.width = 256;
    fb.height = 256;
    fb.pitch = 256 * 4;
    fb.format = render::PixelFormat::RGBA8;
    return fb;
}

TEST_CASE("console: tilde toggles open and closed", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    FakeInput in;

    REQUIRE_FALSE(ui::console::is_open());

    in.press(KeyCode::Tilde);
    const bool cap_open = tick(in);
    REQUIRE(ui::console::is_open());
    REQUIRE(cap_open);  // opening frame captures input

    in.release_all();
    in.press(KeyCode::Tilde);
    tick(in);
    REQUIRE_FALSE(ui::console::is_open());

    ui::console::set_open(false);
}

TEST_CASE("console: typing a cvar-set line and Enter runs it via the backend", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);

    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_test_val", "0", "overlay test cvar");
    REQUIRE(con.SetCVarOverride("con_test_val", "0"));

    FakeInput in;
    // Frame 1: open (the backtick char on this frame must be ignored).
    in.press(KeyCode::Tilde);
    in.type_cp(0x60);  // stray '`' from the toggle key
    REQUIRE(tick(in));
    REQUIRE(ui::console::is_open());

    // Frame 2: type the command.
    in.type("con_test_val 42");
    tick(in);

    // Frame 3: submit.
    in.press(KeyCode::Enter);
    tick(in);

    const psynder::console::CVar* v = con.FindCVar("con_test_val");
    REQUIRE(v != nullptr);
    REQUIRE(v->value == "42");  // line was not corrupted by the toggle backtick

    ui::console::set_open(false);
}

TEST_CASE("console: Up recalls the previous submitted line", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);

    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_hist_val", "0", "overlay history test cvar");
    con.SetCVarOverride("con_hist_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);

    // Submit "con_hist_val 7".
    in.type("con_hist_val 7");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_hist_val")->value == "7");

    // Reset the value behind the console's back, then recall + resubmit.
    con.SetCVarOverride("con_hist_val", "0");
    REQUIRE(con.FindCVar("con_hist_val")->value == "0");

    in.press(KeyCode::Up);  // recall "con_hist_val 7" into the prompt
    tick(in);
    in.press(KeyCode::Enter);  // resubmit it
    tick(in);
    REQUIRE(con.FindCVar("con_hist_val")->value == "7");

    ui::console::set_open(false);
}

TEST_CASE("console: r_debug_hud cvar drives the debug HUD mode", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    ui::imm::set_debug_hud_mode(ui::imm::DebugHudMode::Off);

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);  // open + register the built-in cvars/commands on first update

    in.type("r_debug_hud full");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(ui::imm::debug_hud_mode() == ui::imm::DebugHudMode::Full);

    in.type("r_debug_hud off");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(ui::imm::debug_hud_mode() == ui::imm::DebugHudMode::Off);

    ui::console::set_open(false);
}

TEST_CASE("console: r_resolution / r_fullscreen are registered with presets", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);  // first update registers the built-in cvars

    auto& con = psynder::console::Console::Get();
    const psynder::console::CVar* res = con.FindCVar("r_resolution");
    REQUIRE(res != nullptr);
    REQUIRE_FALSE(res->allowed_values.empty());
    // The preset list feeds the autocomplete; 1080p must be one of them.
    REQUIRE(std::find(res->allowed_values.begin(), res->allowed_values.end(), "1920x1080") !=
            res->allowed_values.end());

    const psynder::console::CVar* fsv = con.FindCVar("r_fullscreen");
    REQUIRE(fsv != nullptr);
    REQUIRE(fsv->allowed_values.size() == 2);  // {"0","1"}

    // Setting a preset updates the cvar (the platform call is a no-op with no
    // window open, but the value must take).
    in.type("r_resolution 1920x1080");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("r_resolution")->value == "1920x1080");

    ui::console::set_open(false);
}

TEST_CASE("console: Tab accepts the highlighted completion", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.SetCVarOverride("r_fullscreen", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);  // open + register built-ins
    in.type("r_full");
    tick(in);  // completion popup -> r_fullscreen
    in.press(KeyCode::Tab);
    tick(in);  // accept -> prompt becomes "r_fullscreen "
    in.type("1");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);  // submit "r_fullscreen 1"
    REQUIRE(con.FindCVar("r_fullscreen")->value == "1");

    con.SetCVarOverride("r_fullscreen", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: Escape clears the prompt and keeps the console open", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_esc_val", "0", "escape test cvar");
    con.SetCVarOverride("con_esc_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);  // open
    in.type("garbage text");
    tick(in);
    in.press(KeyCode::Escape);
    tick(in);  // clears the prompt, must NOT close
    REQUIRE(ui::console::is_open());

    // A cleared prompt means the next line isn't prefixed with the garbage.
    in.type("con_esc_val 5");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_esc_val")->value == "5");

    ui::console::set_open(false);
}

TEST_CASE("console: fuzzy 'res' completes to r_resolution", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.SetCVarOverride("r_resolution", "1280x720");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("res");
    tick(in);  // fuzzy popup -> r_resolution at the top
    in.press(KeyCode::Tab);
    tick(in);  // accept -> prompt becomes "r_resolution "
    in.type("1920x1080");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("r_resolution")->value == "1920x1080");

    con.SetCVarOverride("r_resolution", "1280x720");
    ui::console::set_open(false);
}

TEST_CASE("console: mouse click accepts the hovered completion", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(true);
    auto& con = psynder::console::Console::Get();
    con.SetCVarOverride("r_resolution", "1280x720");

    std::vector<std::uint32_t> pixels;
    render::Framebuffer fb = make_console_test_fb(pixels);

    FakeInput in;
    for (int i = 0; i < 30; ++i) {
        ui::console::update(in, kDt);
        in.advance_frame();
        ui::console::draw(fb);
    }

    in.type("res");
    ui::console::update(in, kDt);
    in.advance_frame();
    ui::console::draw(fb);

    // 256px framebuffer, fully open: the visible popup top is around y=37 when
    // the fuzzy list is full, so y=44 clicks the first visible row.
    in.set_mouse(22.0f, 44.0f, true);
    tick(in);
    in.set_mouse(22.0f, 44.0f, false);
    tick(in);

    in.type("1920x1080");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("r_resolution")->value == "1920x1080");

    con.SetCVarOverride("r_resolution", "1280x720");
    ui::console::set_open(false);
}

TEST_CASE("console: Enter commits an active non-exact completion", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.SetCVarOverride("r_resolution", "1280x720");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("res");
    tick(in);  // fuzzy popup -> r_resolution at the top
    in.press(KeyCode::Enter);
    tick(in);  // commit completion, do not submit yet
    in.type("1920x1080");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("r_resolution")->value == "1920x1080");

    con.SetCVarOverride("r_resolution", "1280x720");
    ui::console::set_open(false);
}

TEST_CASE("console: 'res 1920x1080' smart-resolves and executes", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.SetCVarOverride("r_resolution", "1280x720");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("res 1920x1080");  // typed as the abbreviation, never expanded
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("r_resolution")->value == "1920x1080");

    con.SetCVarOverride("r_resolution", "1280x720");
    ui::console::set_open(false);
}

TEST_CASE("console: forward Delete removes the char at the caret", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.SetCVarOverride("r_fullscreen", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("Xr_fullscreen 1");  // leading junk char to forward-delete
    tick(in);
    // Walk the caret to the very start (clamps at 0).
    for (int i = 0; i < 20; ++i) {
        in.press(KeyCode::Left);
        tick(in);
    }
    in.press(KeyCode::Delete);  // removes the 'X' AT the caret
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("r_fullscreen")->value == "1");

    con.SetCVarOverride("r_fullscreen", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: Home and End move the prompt caret", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_home_end_val", "0", "home/end test cvar");
    con.SetCVarOverride("con_home_end_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type(" 4");
    tick(in);
    in.press(KeyCode::Home);
    tick(in);
    in.type("con_home_end_val");
    tick(in);
    in.press(KeyCode::End);
    tick(in);
    in.type("2");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_home_end_val")->value == "42");

    con.SetCVarOverride("con_home_end_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: Shift Home selects typed prompt text", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_shift_home_val", "0", "shift home test cvar");
    con.SetCVarOverride("con_shift_home_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("bad");
    tick(in);
    in.press(KeyCode::LeftShift);
    in.press(KeyCode::Home);
    tick(in);
    in.release_all();
    in.type("con_shift_home_val 9");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_shift_home_val")->value == "9");

    con.SetCVarOverride("con_shift_home_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: Shift Home survives earlier Home edge reads", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_shift_home_edge_val", "0", "shift home edge test cvar");
    con.SetCVarOverride("con_shift_home_edge_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("bad");
    tick(in);
    in.press(KeyCode::LeftShift);
    in.press(KeyCode::Home);
    REQUIRE(in.key_pressed(KeyCode::Home));
    tick(in);
    in.release_all();
    in.type("con_shift_home_edge_val 5");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_shift_home_edge_val")->value == "5");

    con.SetCVarOverride("con_shift_home_edge_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: delayed Shift after Home still extends selection", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_shift_home_late_val", "0", "shift home late test cvar");
    con.SetCVarOverride("con_shift_home_late_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("bad");
    tick(in);
    in.press(KeyCode::Home);
    tick(in);
    in.release_all();
    in.press(KeyCode::LeftShift);
    in.type("con_shift_home_late_val 7");
    tick(in);
    in.release_all();
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_shift_home_late_val")->value == "7");

    con.SetCVarOverride("con_shift_home_late_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: shortcut Left and Right jump to prompt ends", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_shortcut_edges_val", "0", "shortcut edge test cvar");
    con.SetCVarOverride("con_shortcut_edges_val", "0");

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type(" 4");
    tick(in);
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::Left);
    tick(in);
    in.release_all();
    in.type("con_shortcut_edges_val");
    tick(in);
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::Right);
    tick(in);
    in.release_all();
    in.type("2");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_shortcut_edges_val")->value == "42");

    con.SetCVarOverride("con_shortcut_edges_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: paste works at the beginning and can paste again", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_paste_repeat_val", "0", "paste repeat test cvar");
    con.SetCVarOverride("con_paste_repeat_val", "0");

    const std::string old_clipboard = platform::clipboard_text();

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type(" 4");
    tick(in);
    in.press(KeyCode::Home);
    tick(in);
    platform::set_clipboard_text("con_paste_repeat_val");
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::V);
    tick(in);
    in.release_all();
    in.press(KeyCode::End);
    tick(in);
    platform::set_clipboard_text("2");
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::V);
    tick(in);
    in.release_all();
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_paste_repeat_val")->value == "42");

    platform::set_clipboard_text(old_clipboard);
    con.SetCVarOverride("con_paste_repeat_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: same clipboard text can paste repeatedly", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_paste_same_val", "0", "same paste test cvar");
    con.SetCVarOverride("con_paste_same_val", "0");

    const std::string old_clipboard = platform::clipboard_text();

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("con_paste_same_val ");
    tick(in);
    platform::set_clipboard_text("1");
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::V);
    tick(in);
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::V);
    tick(in);
    in.release_all();
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_paste_same_val")->value == "11");

    platform::set_clipboard_text(old_clipboard);
    con.SetCVarOverride("con_paste_same_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: mouse hover selects popup row before Enter", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(true);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_hover_val_a", "0", "hover test cvar a");
    con.RegisterCVar("con_hover_val_b", "0", "hover test cvar b");
    con.SetCVarOverride("con_hover_val_a", "0");
    con.SetCVarOverride("con_hover_val_b", "0");

    std::vector<std::uint32_t> pixels;
    render::Framebuffer fb = make_console_test_fb(pixels);

    FakeInput in;
    for (int i = 0; i < 30; ++i) {
        ui::console::update(in, kDt);
        in.advance_frame();
        ui::console::draw(fb);
    }

    in.type("con_hover_val_");
    ui::console::update(in, kDt);
    in.advance_frame();
    ui::console::draw(fb);

    in.set_mouse(24.0f, 114.0f, false);  // second popup row in the 256px test framebuffer
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    in.type("7");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_hover_val_a")->value == "0");
    REQUIRE(con.FindCVar("con_hover_val_b")->value == "7");

    con.SetCVarOverride("con_hover_val_a", "0");
    con.SetCVarOverride("con_hover_val_b", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: completion arrows clamp inside the visible popup", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    for (int i = 0; i < 12; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "con_arrow_vis_%02d", i);
        con.RegisterCVar(name, "0", "arrow visible clamp test cvar");
        con.SetCVarOverride(name, "0");
    }

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("con_arrow_vis_");
    tick(in);
    for (int i = 0; i < 20; ++i) {
        in.press(KeyCode::Down);
        tick(in);
        in.release_all();
    }
    in.press(KeyCode::Enter);
    tick(in);
    in.type("5");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);

    REQUIRE(con.FindCVar("con_arrow_vis_07")->value == "5");
    REQUIRE(con.FindCVar("con_arrow_vis_08")->value == "0");

    for (int i = 0; i < 12; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "con_arrow_vis_%02d", i);
        con.SetCVarOverride(name, "0");
    }
    ui::console::set_open(false);
}

TEST_CASE("console: wheel scrolls completion popup rows", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(false);
    auto& con = psynder::console::Console::Get();
    for (int i = 0; i < 12; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "con_wheel_vis_%02d", i);
        con.RegisterCVar(name, "0", "wheel visible popup test cvar");
        con.SetCVarOverride(name, "0");
    }

    FakeInput in;
    in.press(KeyCode::Tilde);
    tick(in);
    in.type("con_wheel_vis_");
    tick(in);
    in.set_wheel(-1.0f);
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);
    in.type("6");
    tick(in);
    in.press(KeyCode::Enter);
    tick(in);

    REQUIRE(con.FindCVar("con_wheel_vis_00")->value == "0");
    REQUIRE(con.FindCVar("con_wheel_vis_01")->value == "6");

    for (int i = 0; i < 12; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "con_wheel_vis_%02d", i);
        con.SetCVarOverride(name, "0");
    }
    ui::console::set_open(false);
}

TEST_CASE("console: mouse selection supports copy and paste replacement", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(true);
    auto& con = psynder::console::Console::Get();
    con.RegisterCVar("con_clip_val", "0", "clipboard test cvar");
    con.SetCVarOverride("con_clip_val", "0");

    const std::string old_clipboard = platform::clipboard_text();
    std::vector<std::uint32_t> pixels;
    render::Framebuffer fb = make_console_test_fb(pixels);

    FakeInput in;
    for (int i = 0; i < 30; ++i) {
        ui::console::update(in, kDt);
        in.advance_frame();
        ui::console::draw(fb);
    }

    in.type("con_clip_val bad");
    ui::console::update(in, kDt);
    in.advance_frame();
    ui::console::draw(fb);

    // The prompt text starts at x=18 in the 256px test framebuffer. Select the
    // trailing "bad" token by dragging from char 13 to char 16.
    in.set_mouse(96.0f, 127.0f, true);
    tick(in);
    in.set_mouse(114.0f, 127.0f, true);
    tick(in);
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::C);
    tick(in);
    REQUIRE(platform::clipboard_text() == "bad");

    platform::set_clipboard_text("7");
    in.release_all();
    in.press(KeyCode::LeftCtrl);
    in.press(KeyCode::V);
    tick(in);
    in.release_all();
    in.press(KeyCode::Enter);
    tick(in);
    REQUIRE(con.FindCVar("con_clip_val")->value == "7");

    platform::set_clipboard_text(old_clipboard);
    con.SetCVarOverride("con_clip_val", "0");
    ui::console::set_open(false);
}

TEST_CASE("console: draw into a framebuffer paints the panel without crashing", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(true);

    std::vector<std::uint32_t> pixels;
    render::Framebuffer fb = make_console_test_fb(pixels);

    // Advance the slide animation to fully open, drawing each step.
    FakeInput in;
    for (int i = 0; i < 30; ++i) {
        ui::console::update(in, kDt);
        in.advance_frame();
        ui::console::draw(fb);
    }

    // The top rows should now contain non-clear (panel) pixels.
    bool painted = false;
    for (usize i = 0; i < static_cast<usize>(256 * 8); ++i) {
        if (pixels[i] != 0xFF000000u) {
            painted = true;
            break;
        }
    }
    REQUIRE(painted);

    ui::console::set_open(false);
}
