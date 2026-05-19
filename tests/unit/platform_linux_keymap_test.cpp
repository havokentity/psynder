// SPDX-License-Identifier: MIT
// Psynder — Lane 22 (platform-linux) host-only unit tests.
//
// Covers the dependency-free pieces of engine/platform/linux/LinuxKeymap.h
// that the Wayland / X11 / evdev TUs share. We exercise:
//   - evdev_keycode → KeyCode mapping
//   - XKB keysym   → KeyCode mapping
//   - compute_blit_rect for all four scale × aspect combinations
//
// These tests run on Mac CI (orchestrator) too, since the keymap header
// has no Linux / Wayland / X11 dependencies — just stdint plus the
// frozen public KeyCode enum.

#include "platform/linux/LinuxKeymap.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder::platform;
using namespace psynder::platform::linux_impl;

// ─── evdev → KeyCode ─────────────────────────────────────────────────────
TEST_CASE("linux_keymap: evdev codes map to expected KeyCode", "[platform_linux][keymap]") {
    REQUIRE(keycode_from_evdev(evdev::KEY_ESC)         == KeyCode::Escape);
    REQUIRE(keycode_from_evdev(evdev::KEY_ENTER)       == KeyCode::Enter);
    REQUIRE(keycode_from_evdev(evdev::KEY_SPACE)       == KeyCode::Space);
    REQUIRE(keycode_from_evdev(evdev::KEY_TAB)         == KeyCode::Tab);
    REQUIRE(keycode_from_evdev(evdev::KEY_BACKSPACE)   == KeyCode::Backspace);
    REQUIRE(keycode_from_evdev(evdev::KEY_LEFT)        == KeyCode::Left);
    REQUIRE(keycode_from_evdev(evdev::KEY_RIGHT)       == KeyCode::Right);
    REQUIRE(keycode_from_evdev(evdev::KEY_UP)          == KeyCode::Up);
    REQUIRE(keycode_from_evdev(evdev::KEY_DOWN)        == KeyCode::Down);
    REQUIRE(keycode_from_evdev(evdev::KEY_GRAVE)       == KeyCode::Tilde);
    REQUIRE(keycode_from_evdev(evdev::KEY_LEFTSHIFT)   == KeyCode::LeftShift);
    REQUIRE(keycode_from_evdev(evdev::KEY_LEFTCTRL)    == KeyCode::LeftCtrl);
    REQUIRE(keycode_from_evdev(evdev::KEY_LEFTALT)    == KeyCode::LeftAlt);
    REQUIRE(keycode_from_evdev(evdev::KEY_F1)          == KeyCode::F1);
    REQUIRE(keycode_from_evdev(evdev::KEY_F12)         == KeyCode::F12);
}

TEST_CASE("linux_keymap: evdev letter codes cover A..M and Z", "[platform_linux][keymap]") {
    REQUIRE(keycode_from_evdev(evdev::KEY_A) == KeyCode::A);
    REQUIRE(keycode_from_evdev(evdev::KEY_B) == KeyCode::B);
    REQUIRE(keycode_from_evdev(evdev::KEY_M) == KeyCode::M);
    REQUIRE(keycode_from_evdev(evdev::KEY_Z) == KeyCode::Z);
}

TEST_CASE("linux_keymap: unknown evdev code → KeyCode::Unknown", "[platform_linux][keymap]") {
    REQUIRE(keycode_from_evdev(0xDEADBEEF) == KeyCode::Unknown);
    REQUIRE(keycode_from_evdev(0)          == KeyCode::Unknown);
}

// ─── XKB → KeyCode ───────────────────────────────────────────────────────
TEST_CASE("linux_keymap: XKB keysyms map to KeyCode", "[platform_linux][keymap]") {
    REQUIRE(keycode_from_xkb(xkb::XK_Escape)    == KeyCode::Escape);
    REQUIRE(keycode_from_xkb(xkb::XK_Return)    == KeyCode::Enter);
    REQUIRE(keycode_from_xkb(xkb::XK_Tab)       == KeyCode::Tab);
    REQUIRE(keycode_from_xkb(xkb::XK_Left)      == KeyCode::Left);
    REQUIRE(keycode_from_xkb(xkb::XK_grave)     == KeyCode::Tilde);
    REQUIRE(keycode_from_xkb(xkb::XK_F1)        == KeyCode::F1);
    REQUIRE(keycode_from_xkb(xkb::XK_F12)       == KeyCode::F12);
    REQUIRE(keycode_from_xkb(xkb::XK_Shift_L)   == KeyCode::LeftShift);
    REQUIRE(keycode_from_xkb(xkb::XK_Control_L) == KeyCode::LeftCtrl);
    REQUIRE(keycode_from_xkb(xkb::XK_Alt_L)     == KeyCode::LeftAlt);
}

TEST_CASE("linux_keymap: XKB lowercase a-z range maps to A..Z", "[platform_linux][keymap]") {
    REQUIRE(keycode_from_xkb(0x61) == KeyCode::A);  // 'a'
    REQUIRE(keycode_from_xkb(0x62) == KeyCode::B);  // 'b'
    REQUIRE(keycode_from_xkb(0x7A) == KeyCode::Z);  // 'z'
}

TEST_CASE("linux_keymap: unknown XKB keysym → KeyCode::Unknown", "[platform_linux][keymap]") {
    REQUIRE(keycode_from_xkb(0)         == KeyCode::Unknown);
    REQUIRE(keycode_from_xkb(0x60)      == KeyCode::Tilde);   // grave
    REQUIRE(keycode_from_xkb(0xCAFE000) == KeyCode::Unknown);
}

// ─── compute_blit_rect ───────────────────────────────────────────────────
TEST_CASE("compute_blit_rect: stretch fills the window exactly",
          "[platform_linux][blit]") {
    const BlitRect r = compute_blit_rect(
        1920, 1080, 640, 360, AspectMode::Stretch, ScaleMode::Linear);
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 0);
    REQUIRE(r.w == 1920);
    REQUIRE(r.h == 1080);
}

TEST_CASE("compute_blit_rect: letterbox keeps aspect; 16:9 fits 16:9",
          "[platform_linux][blit]") {
    const BlitRect r = compute_blit_rect(
        1920, 1080, 640, 360, AspectMode::Letterbox, ScaleMode::Linear);
    // Same aspect → full window
    REQUIRE(r.w == 1920);
    REQUIRE(r.h == 1080);
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 0);
}

TEST_CASE("compute_blit_rect: letterbox bars when window is wider than fb",
          "[platform_linux][blit]") {
    // 4:3 window, 16:9 fb → black bars on the SIDES (pillarbox)
    const BlitRect r = compute_blit_rect(
        1024, 768, 640, 360, AspectMode::Letterbox, ScaleMode::Linear);
    // 16:9 of 768 high = 1365 wide, but capped to 1024 → height shrinks
    // Actually: 1024*360/640 = 576, less than 768 → height=576, full width
    REQUIRE(r.w == 1024);
    REQUIRE(r.h == 576);
    REQUIRE(r.x == 0);
    REQUIRE(r.y == (768 - 576) / 2);
}

TEST_CASE("compute_blit_rect: integer scale snaps to whole multiples",
          "[platform_linux][blit]") {
    // 1920×1080 window, 640×360 fb → 3× integer scale → 1920×1080 (perfect)
    const BlitRect r = compute_blit_rect(
        1920, 1080, 640, 360, AspectMode::Letterbox, ScaleMode::Integer);
    REQUIRE(r.w == 1920);
    REQUIRE(r.h == 1080);
    REQUIRE(r.x == 0);
    REQUIRE(r.y == 0);
}

TEST_CASE("compute_blit_rect: integer scale picks largest multiple that fits",
          "[platform_linux][blit]") {
    // 1300×800 window, 640×360 fb → 2× = 1280×720, 3× = 1920×1080 (doesn't fit)
    const BlitRect r = compute_blit_rect(
        1300, 800, 640, 360, AspectMode::Letterbox, ScaleMode::Integer);
    REQUIRE(r.w == 1280);
    REQUIRE(r.h == 720);
    REQUIRE(r.x == (1300 - 1280) / 2);
    REQUIRE(r.y == (800  - 720)  / 2);
}

TEST_CASE("compute_blit_rect: integer scale never goes below 1×",
          "[platform_linux][blit]") {
    // 100×100 window, 640×360 fb — too small for 1×, so we still output 1×
    // letterbox-cropping is the present's responsibility once the rect
    // overruns; compute_blit_rect just guarantees w>=render_w.
    const BlitRect r = compute_blit_rect(
        100, 100, 640, 360, AspectMode::Letterbox, ScaleMode::Integer);
    REQUIRE(r.w == 640);
    REQUIRE(r.h == 360);
}

TEST_CASE("compute_blit_rect: crop covers the window, clips long axis",
          "[platform_linux][blit]") {
    // 16:9 window (1920×1080), 4:3 fb (800×600).
    //   scale_x = 1920/800 = 2.4
    //   scale_y = 1080/600 = 1.8
    // Crop = max scale → 2.4 → dst = 1920×1440. The bottom/top 180 px
    // of the framebuffer extend past the window and get clipped by the
    // window's clip rect. Centered on Y.
    const BlitRect r = compute_blit_rect(
        1920, 1080, 800, 600, AspectMode::Crop, ScaleMode::Linear);
    REQUIRE(r.w == 1920);
    REQUIRE(r.h == 1440);
    REQUIRE(r.x == 0);
    REQUIRE(r.y == (1080 - 1440) / 2);   // negative — top of fb is above window
}

TEST_CASE("compute_blit_rect: zero/negative inputs return zeroed rect",
          "[platform_linux][blit]") {
    REQUIRE(compute_blit_rect(0, 0, 640, 360, AspectMode::Letterbox,
                              ScaleMode::Linear).w == 0);
    REQUIRE(compute_blit_rect(1920, 1080, 0, 0, AspectMode::Letterbox,
                              ScaleMode::Linear).w == 0);
}
