// SPDX-License-Identifier: MIT
// Psynder — Lane 18 (Wave C) unit test: editor mode hot-key edge behaviour.
//
// Covers two scenarios that the Wave C `handle_input_frame()` watcher
// promises:
//
//   1. Tilde edge fires the toggle exactly once.
//   2. Holding Tilde across multiple frames does NOT keep toggling
//      (i.e. we read `key_pressed`, not `key_down`).
//
// We exercise the production function via a fake `platform::Input` that
// distinguishes "pressed this frame" (edge) from "held this frame"
// (level), matching the real platform layer's contract.

#include "editor/core/Editor.h"
#include "editor/core/HotKey.h"

#include "platform/Platform.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using psynder::platform::Input;
using psynder::platform::KeyCode;
using psynder::platform::MouseState;

namespace {

// Minimal mock that implements the `platform::Input` virtual interface.
// Tests poke `pressed_` for the edge frame and `down_` for held frames.
class FakeInput final : public Input {
   public:
    bool key_down(KeyCode k) const override { return down_[static_cast<u16>(k)]; }
    bool key_pressed(KeyCode k) const override { return pressed_[static_cast<u16>(k)]; }
    const MouseState& mouse() const override { return mouse_; }

    // Simulate one frame where `k` is on the leading edge (pressed) and
    // also held down — matches what the platform layer reports on the
    // first frame after a key transitions up→down.
    void press_edge(KeyCode k) noexcept {
        clear();
        pressed_[static_cast<u16>(k)] = true;
        down_[static_cast<u16>(k)] = true;
    }

    // Simulate one frame where `k` is being held but did NOT transition
    // this frame — only `key_down` reports true, `key_pressed` is false.
    void hold(KeyCode k) noexcept {
        clear();
        down_[static_cast<u16>(k)] = true;
    }

    // Simulate an idle frame with nothing pressed or held.
    void idle() noexcept { clear(); }

   private:
    void clear() noexcept {
        for (auto& b : pressed_)
            b = false;
        for (auto& b : down_)
            b = false;
    }

    static constexpr usize kKeys = static_cast<usize>(KeyCode::Count);
    bool pressed_[kKeys]{};
    bool down_[kKeys]{};
    MouseState mouse_{};
};

}  // namespace

TEST_CASE("editor hot-key: Tilde edge toggles mode exactly once", "[editor][hotkey]") {
    FakeInput in;

    // Reset to a known starting mode by toggling until we're at Play.
    while (editor::current_mode() != editor::Mode::Play) {
        editor::toggle_mode();
    }
    const editor::Mode start = editor::current_mode();
    REQUIRE(start == editor::Mode::Play);

    // Single press → exactly one flip → mode flips to Edit.
    in.press_edge(KeyCode::Tilde);
    editor::handle_input_frame(in);
    REQUIRE(editor::current_mode() == editor::Mode::Edit);

    // Idle frame after the edge — no further toggles.
    in.idle();
    editor::handle_input_frame(in);
    REQUIRE(editor::current_mode() == editor::Mode::Edit);

    // Restore Play so subsequent tests start from a clean slate.
    if (editor::current_mode() != start) {
        editor::toggle_mode();
    }
}

TEST_CASE("editor hot-key: holding Tilde does not retrigger the toggle", "[editor][hotkey]") {
    FakeInput in;

    // Force Play start.
    while (editor::current_mode() != editor::Mode::Play) {
        editor::toggle_mode();
    }
    REQUIRE(editor::current_mode() == editor::Mode::Play);

    // Frame 1: edge — flip to Edit.
    in.press_edge(KeyCode::Tilde);
    editor::handle_input_frame(in);
    REQUIRE(editor::current_mode() == editor::Mode::Edit);

    // Frames 2..N: key still down but no new edge. Mode must NOT flip.
    for (int i = 0; i < 5; ++i) {
        in.hold(KeyCode::Tilde);
        editor::handle_input_frame(in);
        REQUIRE(editor::current_mode() == editor::Mode::Edit);
    }

    // Release frame — also no edge, no flip.
    in.idle();
    editor::handle_input_frame(in);
    REQUIRE(editor::current_mode() == editor::Mode::Edit);

    // A SECOND edge later (e.g. player taps again) flips back to Play.
    in.press_edge(KeyCode::Tilde);
    editor::handle_input_frame(in);
    REQUIRE(editor::current_mode() == editor::Mode::Play);
}
