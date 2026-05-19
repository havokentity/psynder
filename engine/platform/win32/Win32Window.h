// SPDX-License-Identifier: MIT
// Psynder — Win32 Window implementation (CreateWindowEx + WndProc + DXGI present).

#pragma once

#if defined(PSYNDER_PLATFORM_WIN32)

#include "platform/Platform.h"
#include "Win32Common.h"
#include "Win32Present.h"

namespace psynder::platform::win32 {

class Win32Window final : public Window {
public:
    explicit Win32Window(const WindowDesc& desc);
    ~Win32Window() override;

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    // ── Public API (Platform.h Window contract) ─────────────────────────
    void poll_events() override;
    bool should_close() const override                { return should_close_; }
    void present(const render::Framebuffer& fb) override;
    void set_title(std::string_view title) override;
    u32  window_width()  const override               { return window_w_; }
    u32  window_height() const override               { return window_h_; }

    // True iff the underlying HWND + DXGI surface created successfully.
    bool valid() const noexcept { return hwnd_ != nullptr; }

    // The class-shared WndProc dispatches into Win32Window::wnd_proc().
    // Public so register_window_class() (a free function in the anonymous
    // namespace of Win32Window.cpp) can take its address when filling
    // the WNDCLASSEXW.lpfnWndProc slot. The user-facing API surface is
    // still the virtual `Window` contract — this is an MSVC requirement.
    static LRESULT CALLBACK static_wnd_proc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    LRESULT wnd_proc(UINT msg, WPARAM wparam, LPARAM lparam);

    // Raw-input registration — one mouse device with RIDEV_NOLEGACY off
    // (we still want the legacy WM_MOUSEMOVE for cursor position).
    bool register_raw_input();

    // Handle WM_INPUT raw-mouse delta packet → forward to Win32Input.
    void on_wm_input(LPARAM lparam);

    // ── State ───────────────────────────────────────────────────────────
    HWND        hwnd_         = nullptr;
    HINSTANCE   hinstance_    = nullptr;
    std::wstring class_name_;

    WindowDesc  desc_;
    u32         window_w_     = 0;
    u32         window_h_     = 0;

    bool        should_close_ = false;
    bool        size_pending_ = false;
    Win32Present present_;
};

// Factory entry — keeps the `new`/`delete` symmetry with `destroy_window_impl`.
Win32Window* create_window_native(const WindowDesc& desc);

}  // namespace psynder::platform::win32

#endif  // PSYNDER_PLATFORM_WIN32
