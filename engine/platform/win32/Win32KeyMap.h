// SPDX-License-Identifier: MIT
// Psynder — Win32 virtual-key code to engine KeyCode mapping.
//
// Header-only by design: the function body is a switch table that the
// optimizer inlines anyway, and keeping it in a header lets unit tests
// (which don't link psynder_platform_win32) exercise it directly.

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "Win32Common.h"
#include "platform/Platform.h"

namespace psynder::platform::win32 {

namespace detail {
constexpr u32 kLParamExtendedKey = (1u << 24);
}  // namespace detail

// Maps a Win32 VK_* code (after WM_KEYDOWN/UP `wParam`) plus the lParam
// extended-key bit so we can disambiguate Left vs Right modifiers, to a
// KeyCode value. Returns KeyCode::Unknown if there's no mapping.
inline KeyCode vk_to_keycode(u32 vk, u32 lparam_flags) noexcept {
    // Letters: VK_A..VK_Z map to ASCII 'A'..'Z' (0x41..0x5A).
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<KeyCode>(static_cast<u16>(KeyCode::A) + (vk - 'A'));
    }
    // Function keys F1..F12.
    if (vk >= VK_F1 && vk <= VK_F12) {
        return static_cast<KeyCode>(static_cast<u16>(KeyCode::F1) + (vk - VK_F1));
    }
    const bool extended = (lparam_flags & detail::kLParamExtendedKey) != 0;
    switch (vk) {
        case VK_ESCAPE:
            return KeyCode::Escape;
        case VK_RETURN:
            return KeyCode::Enter;
        case VK_SPACE:
            return KeyCode::Space;
        case VK_TAB:
            return KeyCode::Tab;
        case VK_BACK:
            return KeyCode::Backspace;
        case VK_DELETE:
            return KeyCode::Delete;
        case VK_HOME:
            return KeyCode::Home;
        case VK_END:
            return KeyCode::End;
        case VK_LEFT:
            return KeyCode::Left;
        case VK_RIGHT:
            return KeyCode::Right;
        case VK_UP:
            return KeyCode::Up;
        case VK_DOWN:
            return KeyCode::Down;
        case VK_OEM_3:
            return KeyCode::Tilde;  // US `~` key
        case VK_LSHIFT:
            return KeyCode::LeftShift;
        case VK_RSHIFT:
            return KeyCode::RightShift;
        case VK_SHIFT:
            return KeyCode::LeftShift;  // generic = treat as left
        case VK_LCONTROL:
            return KeyCode::LeftCtrl;
        case VK_RCONTROL:
            return KeyCode::RightCtrl;
        case VK_CONTROL:
            return extended ? KeyCode::RightCtrl : KeyCode::LeftCtrl;
        case VK_LMENU:
            return KeyCode::LeftAlt;
        case VK_RMENU:
            return KeyCode::RightAlt;
        case VK_MENU:
            return extended ? KeyCode::RightAlt : KeyCode::LeftAlt;
        case VK_LWIN:
            return KeyCode::LeftSuper;
        case VK_RWIN:
            return KeyCode::RightSuper;
        default:
            return KeyCode::Unknown;
    }
}

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
