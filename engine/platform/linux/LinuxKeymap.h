// SPDX-License-Identifier: MIT
// Psynder — Linux keymap (header-only).
//
// Maps evdev keycodes (Linux uapi <linux/input-event-codes.h>) and XKB
// keysyms (libxkbcommon) to our platform-agnostic KeyCode enum.
//
// This header is intentionally dependency-free: it does not include any
// Linux / X11 / Wayland headers — it just contains the numeric constants
// inline. That way the same tables can be unit-tested on a Mac host where
// the actual platform library isn't built.

#pragma once

#include "platform/Platform.h"

#include <cstdint>

namespace psynder::platform::linux_impl {

// ─── evdev keycodes (subset, from <linux/input-event-codes.h>) ───────────
// These are stable kernel ABI numbers; safe to hardcode.
namespace evdev {
inline constexpr std::uint32_t KEY_ESC = 1;
inline constexpr std::uint32_t KEY_1 = 2;
inline constexpr std::uint32_t KEY_2 = 3;
inline constexpr std::uint32_t KEY_3 = 4;
inline constexpr std::uint32_t KEY_4 = 5;
inline constexpr std::uint32_t KEY_5 = 6;
inline constexpr std::uint32_t KEY_6 = 7;
inline constexpr std::uint32_t KEY_7 = 8;
inline constexpr std::uint32_t KEY_8 = 9;
inline constexpr std::uint32_t KEY_9 = 10;
inline constexpr std::uint32_t KEY_0 = 11;
inline constexpr std::uint32_t KEY_MINUS = 12;
inline constexpr std::uint32_t KEY_EQUAL = 13;
inline constexpr std::uint32_t KEY_BACKSPACE = 14;
inline constexpr std::uint32_t KEY_DELETE = 111;  // forward delete
inline constexpr std::uint32_t KEY_TAB = 15;
inline constexpr std::uint32_t KEY_Q = 16;
inline constexpr std::uint32_t KEY_W = 17;
inline constexpr std::uint32_t KEY_E = 18;
inline constexpr std::uint32_t KEY_R = 19;
inline constexpr std::uint32_t KEY_T = 20;
inline constexpr std::uint32_t KEY_Y = 21;
inline constexpr std::uint32_t KEY_U = 22;
inline constexpr std::uint32_t KEY_I = 23;
inline constexpr std::uint32_t KEY_O = 24;
inline constexpr std::uint32_t KEY_P = 25;
inline constexpr std::uint32_t KEY_ENTER = 28;
inline constexpr std::uint32_t KEY_LEFTCTRL = 29;
inline constexpr std::uint32_t KEY_A = 30;
inline constexpr std::uint32_t KEY_S = 31;
inline constexpr std::uint32_t KEY_D = 32;
inline constexpr std::uint32_t KEY_F = 33;
inline constexpr std::uint32_t KEY_G = 34;
inline constexpr std::uint32_t KEY_H = 35;
inline constexpr std::uint32_t KEY_J = 36;
inline constexpr std::uint32_t KEY_K = 37;
inline constexpr std::uint32_t KEY_L = 38;
inline constexpr std::uint32_t KEY_LEFTSHIFT = 42;
inline constexpr std::uint32_t KEY_Z = 44;
inline constexpr std::uint32_t KEY_X = 45;
inline constexpr std::uint32_t KEY_C = 46;
inline constexpr std::uint32_t KEY_V = 47;
inline constexpr std::uint32_t KEY_B = 48;
inline constexpr std::uint32_t KEY_N = 49;
inline constexpr std::uint32_t KEY_M = 50;
inline constexpr std::uint32_t KEY_GRAVE = 41;  // `~`
inline constexpr std::uint32_t KEY_RIGHTSHIFT = 54;
inline constexpr std::uint32_t KEY_LEFTALT = 56;
inline constexpr std::uint32_t KEY_SPACE = 57;
inline constexpr std::uint32_t KEY_F1 = 59;
inline constexpr std::uint32_t KEY_F2 = 60;
inline constexpr std::uint32_t KEY_F3 = 61;
inline constexpr std::uint32_t KEY_F4 = 62;
inline constexpr std::uint32_t KEY_F5 = 63;
inline constexpr std::uint32_t KEY_F6 = 64;
inline constexpr std::uint32_t KEY_F7 = 65;
inline constexpr std::uint32_t KEY_F8 = 66;
inline constexpr std::uint32_t KEY_F9 = 67;
inline constexpr std::uint32_t KEY_F10 = 68;
inline constexpr std::uint32_t KEY_UP = 103;
inline constexpr std::uint32_t KEY_LEFT = 105;
inline constexpr std::uint32_t KEY_RIGHT = 106;
inline constexpr std::uint32_t KEY_DOWN = 108;
inline constexpr std::uint32_t KEY_F11 = 87;
inline constexpr std::uint32_t KEY_F12 = 88;
inline constexpr std::uint32_t KEY_RIGHTCTRL = 97;
inline constexpr std::uint32_t KEY_RIGHTALT = 100;

// Mouse buttons live in the same event-code namespace.
inline constexpr std::uint32_t BTN_LEFT = 0x110;
inline constexpr std::uint32_t BTN_RIGHT = 0x111;
inline constexpr std::uint32_t BTN_MIDDLE = 0x112;
}  // namespace evdev

// ─── XKB keysyms (subset, from <xkbcommon/xkbcommon-keysyms.h>) ──────────
// These are X11 keysyms; stable ABI defined by xkbcommon.
namespace xkb {
inline constexpr std::uint32_t XK_Escape = 0xFF1B;
inline constexpr std::uint32_t XK_Return = 0xFF0D;
inline constexpr std::uint32_t XK_Tab = 0xFF09;
inline constexpr std::uint32_t XK_BackSpace = 0xFF08;
inline constexpr std::uint32_t XK_Delete = 0xFFFF;  // forward delete
inline constexpr std::uint32_t XK_space = 0x0020;
inline constexpr std::uint32_t XK_Left = 0xFF51;
inline constexpr std::uint32_t XK_Up = 0xFF52;
inline constexpr std::uint32_t XK_Right = 0xFF53;
inline constexpr std::uint32_t XK_Down = 0xFF54;
inline constexpr std::uint32_t XK_grave = 0x0060;  // `~`
inline constexpr std::uint32_t XK_F1 = 0xFFBE;
inline constexpr std::uint32_t XK_F2 = 0xFFBF;
inline constexpr std::uint32_t XK_F3 = 0xFFC0;
inline constexpr std::uint32_t XK_F4 = 0xFFC1;
inline constexpr std::uint32_t XK_F5 = 0xFFC2;
inline constexpr std::uint32_t XK_F6 = 0xFFC3;
inline constexpr std::uint32_t XK_F7 = 0xFFC4;
inline constexpr std::uint32_t XK_F8 = 0xFFC5;
inline constexpr std::uint32_t XK_F9 = 0xFFC6;
inline constexpr std::uint32_t XK_F10 = 0xFFC7;
inline constexpr std::uint32_t XK_F11 = 0xFFC8;
inline constexpr std::uint32_t XK_F12 = 0xFFC9;
inline constexpr std::uint32_t XK_Shift_L = 0xFFE1;
inline constexpr std::uint32_t XK_Shift_R = 0xFFE2;
inline constexpr std::uint32_t XK_Control_L = 0xFFE3;
inline constexpr std::uint32_t XK_Control_R = 0xFFE4;
inline constexpr std::uint32_t XK_Alt_L = 0xFFE9;
inline constexpr std::uint32_t XK_Alt_R = 0xFFEA;
inline constexpr std::uint32_t XK_a = 0x0061;
inline constexpr std::uint32_t XK_z = 0x007A;
}  // namespace xkb

// ─── evdev → KeyCode ─────────────────────────────────────────────────────
// constexpr so it folds at compile time; const correctness in the lookup.
constexpr KeyCode keycode_from_evdev(std::uint32_t c) noexcept {
    using namespace evdev;
    switch (c) {
        case KEY_ESC:
            return KeyCode::Escape;
        case KEY_ENTER:
            return KeyCode::Enter;
        case KEY_SPACE:
            return KeyCode::Space;
        case KEY_TAB:
            return KeyCode::Tab;
        case KEY_BACKSPACE:
            return KeyCode::Backspace;
        case KEY_DELETE:
            return KeyCode::Delete;
        case KEY_LEFT:
            return KeyCode::Left;
        case KEY_RIGHT:
            return KeyCode::Right;
        case KEY_UP:
            return KeyCode::Up;
        case KEY_DOWN:
            return KeyCode::Down;
        case KEY_GRAVE:
            return KeyCode::Tilde;
        case KEY_LEFTSHIFT:
            return KeyCode::LeftShift;
        case KEY_RIGHTSHIFT:
            return KeyCode::RightShift;
        case KEY_LEFTCTRL:
            return KeyCode::LeftCtrl;
        case KEY_RIGHTCTRL:
            return KeyCode::RightCtrl;
        case KEY_LEFTALT:
            return KeyCode::LeftAlt;
        case KEY_RIGHTALT:
            return KeyCode::RightAlt;
        case KEY_A:
            return KeyCode::A;
        case KEY_B:
            return KeyCode::B;
        case KEY_C:
            return KeyCode::C;
        case KEY_D:
            return KeyCode::D;
        case KEY_E:
            return KeyCode::E;
        case KEY_F:
            return KeyCode::F;
        case KEY_G:
            return KeyCode::G;
        case KEY_H:
            return KeyCode::H;
        case KEY_I:
            return KeyCode::I;
        case KEY_J:
            return KeyCode::J;
        case KEY_K:
            return KeyCode::K;
        case KEY_L:
            return KeyCode::L;
        case KEY_M:
            return KeyCode::M;
        case KEY_N:
            return KeyCode::N;
        case KEY_O:
            return KeyCode::O;
        case KEY_P:
            return KeyCode::P;
        case KEY_Q:
            return KeyCode::Q;
        case KEY_R:
            return KeyCode::R;
        case KEY_S:
            return KeyCode::S;
        case KEY_T:
            return KeyCode::T;
        case KEY_U:
            return KeyCode::U;
        case KEY_V:
            return KeyCode::V;
        case KEY_W:
            return KeyCode::W;
        case KEY_X:
            return KeyCode::X;
        case KEY_Y:
            return KeyCode::Y;
        case KEY_Z:
            return KeyCode::Z;
        case KEY_F1:
            return KeyCode::F1;
        case KEY_F2:
            return KeyCode::F2;
        case KEY_F3:
            return KeyCode::F3;
        case KEY_F4:
            return KeyCode::F4;
        case KEY_F5:
            return KeyCode::F5;
        case KEY_F6:
            return KeyCode::F6;
        case KEY_F7:
            return KeyCode::F7;
        case KEY_F8:
            return KeyCode::F8;
        case KEY_F9:
            return KeyCode::F9;
        case KEY_F10:
            return KeyCode::F10;
        case KEY_F11:
            return KeyCode::F11;
        case KEY_F12:
            return KeyCode::F12;
        default:
            return KeyCode::Unknown;
    }
}

// ─── XKB keysym → KeyCode ────────────────────────────────────────────────
constexpr KeyCode keycode_from_xkb(std::uint32_t k) noexcept {
    using namespace xkb;
    switch (k) {
        case XK_Escape:
            return KeyCode::Escape;
        case XK_Return:
            return KeyCode::Enter;
        case XK_Tab:
            return KeyCode::Tab;
        case XK_BackSpace:
            return KeyCode::Backspace;
        case XK_Delete:
            return KeyCode::Delete;
        case XK_space:
            return KeyCode::Space;
        case XK_Left:
            return KeyCode::Left;
        case XK_Right:
            return KeyCode::Right;
        case XK_Up:
            return KeyCode::Up;
        case XK_Down:
            return KeyCode::Down;
        case XK_grave:
            return KeyCode::Tilde;
        case XK_Shift_L:
            return KeyCode::LeftShift;
        case XK_Shift_R:
            return KeyCode::RightShift;
        case XK_Control_L:
            return KeyCode::LeftCtrl;
        case XK_Control_R:
            return KeyCode::RightCtrl;
        case XK_Alt_L:
            return KeyCode::LeftAlt;
        case XK_Alt_R:
            return KeyCode::RightAlt;
        case XK_F1:
            return KeyCode::F1;
        case XK_F2:
            return KeyCode::F2;
        case XK_F3:
            return KeyCode::F3;
        case XK_F4:
            return KeyCode::F4;
        case XK_F5:
            return KeyCode::F5;
        case XK_F6:
            return KeyCode::F6;
        case XK_F7:
            return KeyCode::F7;
        case XK_F8:
            return KeyCode::F8;
        case XK_F9:
            return KeyCode::F9;
        case XK_F10:
            return KeyCode::F10;
        case XK_F11:
            return KeyCode::F11;
        case XK_F12:
            return KeyCode::F12;
        default:
            // X11 keysyms for a-z are 0x61..0x7A (lowercase ASCII). Map them onto
            // our A..Z enum.
            if (k >= XK_a && k <= XK_z) {
                return static_cast<KeyCode>(static_cast<std::uint16_t>(KeyCode::A) + (k - XK_a));
            }
            return KeyCode::Unknown;
    }
}

// ─── Letterbox / aspect helpers ──────────────────────────────────────────
// Compute the destination rect for the framebuffer blit given a window size,
// a render (framebuffer) size, and the aspect mode chosen at startup. The
// rectangle is returned in window-pixel coordinates as { x, y, w, h }.
//
// Pure math, no platform deps — also unit-tested host-only.
struct BlitRect {
    int x = 0, y = 0;
    int w = 0, h = 0;
};

constexpr BlitRect compute_blit_rect(
    int window_w, int window_h, int render_w, int render_h, AspectMode mode, ScaleMode scale) noexcept {
    if (window_w <= 0 || window_h <= 0 || render_w <= 0 || render_h <= 0) {
        return BlitRect{};
    }

    if (mode == AspectMode::Stretch) {
        return BlitRect{0, 0, window_w, window_h};
    }

    // Integer scale snaps the framebuffer to whole multiples, letterboxed.
    if (scale == ScaleMode::Integer) {
        int sx = window_w / render_w;
        int sy = window_h / render_h;
        int s = (sx < sy) ? sx : sy;
        if (s < 1)
            s = 1;
        int w = render_w * s;
        int h = render_h * s;
        return BlitRect{
            (window_w - w) / 2,
            (window_h - h) / 2,
            w,
            h,
        };
    }

    // Crop: fill window, possibly clipping framebuffer edges. We expand
    // the destination so the *short* axis matches the window; the long
    // axis overflows by an amount the window will clip.
    if (mode == AspectMode::Crop) {
        // window aspect vs render aspect
        // dst is sized so min(dst_w/render_w, dst_h/render_h) == max
        long long sx_n = static_cast<long long>(window_w) * render_h;
        long long sy_n = static_cast<long long>(window_h) * render_w;
        int w, h;
        if (sx_n > sy_n) {
            // window wider than fb → fill width, overflow height
            w = window_w;
            h = static_cast<int>(sx_n / render_w);
        } else {
            h = window_h;
            w = static_cast<int>(sy_n / render_h);
        }
        return BlitRect{
            (window_w - w) / 2,
            (window_h - h) / 2,
            w,
            h,
        };
    }

    // Letterbox (default): preserve aspect, fit, bars on the off-axis.
    long long sx_n = static_cast<long long>(window_w) * render_h;
    long long sy_n = static_cast<long long>(window_h) * render_w;
    int w, h;
    if (sx_n < sy_n) {
        w = window_w;
        h = static_cast<int>(sx_n / render_w);
    } else {
        h = window_h;
        w = static_cast<int>(sy_n / render_h);
    }
    return BlitRect{
        (window_w - w) / 2,
        (window_h - h) / 2,
        w,
        h,
    };
}

}  // namespace psynder::platform::linux_impl
