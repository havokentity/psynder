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

#include <catch2/catch_test_macros.hpp>

#include <array>
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
// release()). key_pressed() is read-and-clear, matching the real backends.
class FakeInput final : public Input {
   public:
    bool key_down(KeyCode k) const override { return down_[idx(k)]; }
    bool key_pressed(KeyCode k) const override {
        const bool v = pressed_[idx(k)];
        pressed_[idx(k)] = false;  // read-and-clear edge
        return v;
    }
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

    // End-of-frame: drop edge flags + typed text; keep held keys down.
    void advance_frame() {
        for (auto& b : pressed_)
            b = false;
        text_.clear();
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

TEST_CASE("console: draw into a framebuffer paints the panel without crashing", "[ui][console]") {
    ui::console::reset();
    ui::console::set_open(true);

    std::vector<std::uint32_t> pixels(256 * 256, 0xFF000000u);
    render::Framebuffer fb{};
    fb.pixels = reinterpret_cast<std::uint8_t*>(pixels.data());
    fb.width = 256;
    fb.height = 256;
    fb.pitch = 256 * 4;
    fb.format = render::PixelFormat::RGBA8;

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
