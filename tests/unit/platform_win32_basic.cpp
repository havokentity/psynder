// SPDX-License-Identifier: MIT
// Psynder — Lane 21 / platform-win32 host-only unit tests.
//
// These tests only run when built on Windows; on Mac / Linux CI the
// translation unit compiles down to a single Catch2 SUCCEED so the unit
// binary still links cleanly.
//
// Note: the unit binary doesn't link psynder_platform_win32 (lane 25 owns
// tests/unit/CMakeLists.txt), so this test exercises only the header-only
// surface: the inline vk_to_keycode mapper and the inline Win32Input
// edge-trigger / accumulator state. The XInput poll inside begin_frame()
// stays in the lane's static lib and is verified by the user's PC pass.

#include <catch2/catch_test_macros.hpp>

#if defined(PSYNDER_PLATFORM_WIN32)

#include "platform/Platform.h"
#include "platform/win32/Win32Input.h"
#include "platform/win32/Win32KeyMap.h"

#include <windows.h>

using namespace psynder;
using namespace psynder::platform;
using namespace psynder::platform::win32;

TEST_CASE("vk_to_keycode covers the contracted KeyCode surface",
          "[platform-win32][keymap]")
{
    REQUIRE(vk_to_keycode('A', 0)               == KeyCode::A);
    REQUIRE(vk_to_keycode('Z', 0)               == KeyCode::Z);
    REQUIRE(vk_to_keycode(VK_ESCAPE, 0)         == KeyCode::Escape);
    REQUIRE(vk_to_keycode(VK_F1, 0)             == KeyCode::F1);
    REQUIRE(vk_to_keycode(VK_F12, 0)            == KeyCode::F12);
    REQUIRE(vk_to_keycode(VK_LSHIFT, 0)         == KeyCode::LeftShift);
    REQUIRE(vk_to_keycode(VK_RSHIFT, 0)         == KeyCode::RightShift);
    REQUIRE(vk_to_keycode(VK_OEM_3, 0)          == KeyCode::Tilde);
    REQUIRE(vk_to_keycode(VK_SPACE, 0)          == KeyCode::Space);
    // Extended-bit disambiguates the generic VK_CONTROL into L vs R.
    REQUIRE(vk_to_keycode(VK_CONTROL, 0)        == KeyCode::LeftCtrl);
    REQUIRE(vk_to_keycode(VK_CONTROL, 1u << 24) == KeyCode::RightCtrl);
    REQUIRE(vk_to_keycode(0xFFu, 0)             == KeyCode::Unknown);
}

TEST_CASE("Win32Input edge-triggered pressed clears each frame",
          "[platform-win32][input]")
{
    Win32Input in;

    // Synthesize a keydown — first observation should be both down + pressed.
    in.on_key(KeyCode::Space, true);
    REQUIRE(in.key_down(KeyCode::Space));
    REQUIRE(in.key_pressed(KeyCode::Space));

    // Without releasing, a fresh frame clears the rising-edge flag but
    // not the held-down state.
    in.reset_edge_state_for_test();
    REQUIRE(in.key_down(KeyCode::Space));
    REQUIRE_FALSE(in.key_pressed(KeyCode::Space));

    // Releasing clears held state too.
    in.on_key(KeyCode::Space, false);
    REQUIRE_FALSE(in.key_down(KeyCode::Space));
    REQUIRE_FALSE(in.key_pressed(KeyCode::Space));
}

TEST_CASE("Win32Input mouse wheel publishes accumulated ticks then resets",
          "[platform-win32][input]")
{
    Win32Input in;

    in.on_mouse_wheel(1.0f);
    in.on_mouse_wheel(0.5f);

    // Frame boundary publishes the accumulator into mouse().wheel.
    in.reset_edge_state_for_test();
    REQUIRE(in.mouse().wheel == 1.5f);

    // No new wheel events → next frame sees 0.
    in.reset_edge_state_for_test();
    REQUIRE(in.mouse().wheel == 0.0f);
}

TEST_CASE("Win32Input raw mouse delta accumulates between frames then clears",
          "[platform-win32][input]")
{
    Win32Input in;

    in.on_mouse_raw_delta(3.0f, -2.0f);
    in.on_mouse_raw_delta(1.0f,  1.0f);
    // Mid-frame, the deltas are already visible.
    REQUIRE(in.mouse().dx == 4.0f);
    REQUIRE(in.mouse().dy == -1.0f);

    // Frame boundary clears them for the next accumulation window.
    in.reset_edge_state_for_test();
    REQUIRE(in.mouse().dx == 0.0f);
    REQUIRE(in.mouse().dy == 0.0f);
}

TEST_CASE("Win32Input mouse buttons toggle correctly",
          "[platform-win32][input]")
{
    Win32Input in;
    REQUIRE_FALSE(in.mouse().left);
    in.on_mouse_button(0, true);
    REQUIRE(in.mouse().left);
    in.on_mouse_button(1, true);
    REQUIRE(in.mouse().right);
    in.on_mouse_button(2, true);
    REQUIRE(in.mouse().middle);
    in.on_mouse_button(0, false);
    REQUIRE_FALSE(in.mouse().left);
    REQUIRE(in.mouse().right);    // unaffected
}

#else  // !PSYNDER_PLATFORM_WIN32

// Anchor TU on non-Windows hosts. Tagged hidden so it doesn't pollute the
// host CI test counts.
TEST_CASE("platform-win32 tests are Windows-only", "[.platform-win32]") {
    SUCCEED("platform-win32 tests skipped on non-Windows host");
}

#endif  // PSYNDER_PLATFORM_WIN32
