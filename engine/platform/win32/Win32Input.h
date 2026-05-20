// SPDX-License-Identifier: MIT
// Psynder — Win32 input state (keyboard + raw mouse + XInput gamepads).
//
// State is owned by a process-wide singleton accessed via the Platform.h
// `input()` factory; the Win32Window writes into it from WindowProc.
//
// The class is header-defined so unit tests (which don't link
// psynder_platform_win32) can exercise the edge-trigger and accumulator
// behavior directly. XInput polling is the only out-of-line bit, and it
// only fires when the user calls begin_frame() in a real game frame —
// the test entry point doesn't need that path.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Common.h"
#include "platform/Platform.h"

#include <bitset>

namespace psynder::platform::win32 {

// Lightweight gamepad snapshot — populated from XInputGetState() during
// the window's poll_events(). One slot per XInput user index (0..3).
struct GamepadState {
    bool connected = false;
    u16 buttons = 0;     // XINPUT_GAMEPAD_* bitfield mirror
    i16 lx = 0, ly = 0;  // left thumb
    i16 rx = 0, ry = 0;  // right thumb
    u8 lt = 0, rt = 0;   // triggers (0..255)
};

class Win32Input final : public Input {
   public:
    Win32Input() = default;
    static constexpr usize kMaxGamepads = 4;

    // ── Public API (Platform.h Input contract) ──────────────────────────
    bool key_down(KeyCode k) const override {
        const auto idx = static_cast<usize>(k);
        return idx < kKeyBits && keys_down_.test(idx);
    }
    bool key_pressed(KeyCode k) const override {
        const auto idx = static_cast<usize>(k);
        return idx < kKeyBits && keys_pressed_this_frame_.test(idx);
    }
    const MouseState& mouse() const override { return mouse_; }

    // ── WindowProc hooks ────────────────────────────────────────────────
    void on_key(KeyCode k, bool down) {
        const auto idx = static_cast<usize>(k);
        if (idx >= kKeyBits)
            return;
        const bool was_down = keys_down_.test(idx);
        keys_down_.set(idx, down);
        // "pressed" is the rising edge — set when a key transitions up→down.
        if (down && !was_down)
            keys_pressed_this_frame_.set(idx);
    }
    void on_mouse_move(f32 x, f32 y) {
        mouse_.x = x;
        mouse_.y = y;
    }
    void on_mouse_raw_delta(f32 dx, f32 dy) {
        mouse_.dx += dx;
        mouse_.dy += dy;
    }
    void on_mouse_button(u32 button, bool down) {
        switch (button) {
            case 0:
                mouse_.left = down;
                break;
            case 1:
                mouse_.right = down;
                break;
            case 2:
                mouse_.middle = down;
                break;
            default:
                break;
        }
    }
    void on_mouse_wheel(f32 ticks) { mouse_wheel_accum_ += ticks; }

    // ── Frame book-keeping (called once per poll_events) ────────────────
    // Pulls XInput state and clears edge-flagged inputs from the prior frame.
    // Defined out-of-line so the XInput symbol stays in the Win32 lane
    // library (the test exercises the rest of the surface without it).
    void begin_frame();

    // Test/diagnostic accessors.
    const GamepadState& gamepad(usize slot) const noexcept {
        return pads_[slot < kMaxGamepads ? slot : 0];
    }

    // Shared bit of begin_frame() that doesn't touch XInput — split out so
    // the unit test (which lives outside psynder_platform_win32) can poke
    // edge-state behavior without pulling XInput.lib in.
    void reset_edge_state_for_test() {
        keys_pressed_this_frame_.reset();
        mouse_.wheel = mouse_wheel_accum_;
        mouse_wheel_accum_ = 0.0f;
        mouse_.dx = 0.0f;
        mouse_.dy = 0.0f;
    }

   private:
    // One bit per KeyCode. KeyCode::Count is well under 96, so bitset<256>
    // gives plenty of head-room even with future additions.
    static constexpr usize kKeyBits = 256;
    using KeyBits = std::bitset<kKeyBits>;

    KeyBits keys_down_{};
    KeyBits keys_pressed_this_frame_{};

    MouseState mouse_{};
    f32 mouse_wheel_accum_ = 0.0f;
    GamepadState pads_[kMaxGamepads]{};
};

// Singleton accessor — the Platform.h `input()` factory forwards here.
// Defined in Win32Input.cpp (so the static lives in the platform_win32 lane
// alongside begin_frame()'s XInput call).
Win32Input& input_singleton();

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
