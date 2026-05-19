// SPDX-License-Identifier: MIT
// Psynder — Win32 input out-of-line bits (the XInput poll + the singleton).
// Everything else lives inline in Win32Input.h so unit tests that don't
// link psynder_platform_win32 can exercise the edge-trigger and
// accumulator behaviour.

#include "Win32Input.h"

#if defined(PSYNDER_PLATFORM_WIN32)

namespace psynder::platform::win32 {

namespace {
// Process-wide singleton. We keep it function-local-static to avoid the
// "static initialization order fiasco" — first caller wins.
Win32Input& instance() {
    static Win32Input g_input{};
    return g_input;
}
}  // namespace

Win32Input& input_singleton() { return instance(); }

void Win32Input::begin_frame() {
    reset_edge_state_for_test();

    // ── XInput poll ────────────────────────────────────────────────────
    for (DWORD i = 0; i < kMaxGamepads; ++i) {
        XINPUT_STATE state{};
        const DWORD rc = ::XInputGetState(i, &state);
        if (rc == ERROR_SUCCESS) {
            pads_[i].connected = true;
            pads_[i].buttons   = state.Gamepad.wButtons;
            pads_[i].lx        = state.Gamepad.sThumbLX;
            pads_[i].ly        = state.Gamepad.sThumbLY;
            pads_[i].rx        = state.Gamepad.sThumbRX;
            pads_[i].ry        = state.Gamepad.sThumbRY;
            pads_[i].lt        = state.Gamepad.bLeftTrigger;
            pads_[i].rt        = state.Gamepad.bRightTrigger;
        } else {
            pads_[i] = {};
        }
    }
}

}  // namespace psynder::platform::win32

// Bridge from Platform.h's free function `input()` to the Win32 singleton.
// Declared in Platform.h, defined here so the Win32 lane owns it.
namespace psynder::platform {
Input* input() { return &win32::input_singleton(); }
}  // namespace psynder::platform

#endif  // PSYNDER_PLATFORM_WIN32
