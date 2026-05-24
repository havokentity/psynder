// SPDX-License-Identifier: MIT
// Psynder — platform shared shim. The actual window / input / clock impl
// lives in the platform-specific lane (win32, linux, macos). Provides a
// dispatching create_window() the samples call into.

#include "Platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace psynder::platform {

// Each platform lane provides this factory.
Window* create_window_impl(const WindowDesc& desc);
void destroy_window_impl(Window* w);

#if defined(__APPLE__)
std::string mac_clipboard_text_impl();
void mac_set_clipboard_text_impl(std::string_view text);
#endif

namespace {
// The live window (single window per process, DESIGN §11). Lets display
// control reach the window without the caller holding the pointer.
Window* g_active_window = nullptr;
WindowDesc g_active_desc{};
// Set by the software console while it's open; suppresses the backend's
// default Escape-closes-the-window behaviour so the console owns Escape.
bool g_text_input_capturing = false;
#if !defined(__APPLE__)
std::string g_clipboard_fallback;
#endif
}  // namespace

Window* create_window(const WindowDesc& desc) {
    Window* w = create_window_impl(desc);
    g_active_window = w;
    g_active_desc = desc;
    return w;
}
void destroy_window(Window* w) {
    if (g_active_window == w)
        g_active_window = nullptr;
    destroy_window_impl(w);
}

void request_fullscreen(bool on) {
    if (g_active_window)
        g_active_window->set_fullscreen(on);
}
void toggle_fullscreen() {
    if (g_active_window)
        g_active_window->set_fullscreen(!g_active_window->is_fullscreen());
}
bool is_fullscreen() {
    return g_active_window != nullptr && g_active_window->is_fullscreen();
}
void request_window_size(u32 width, u32 height) {
    if (g_active_window)
        g_active_window->set_window_size(width, height);
}
u32 active_window_width() {
    return g_active_window ? g_active_window->window_width() : 0u;
}
u32 active_window_height() {
    return g_active_window ? g_active_window->window_height() : 0u;
}

MouseState mouse_to_framebuffer_space(const MouseState& mouse, u32 fb_w, u32 fb_h) {
    MouseState out = mouse;
    const u32 win_w_u = active_window_width();
    const u32 win_h_u = active_window_height();
    if (win_w_u == 0u || win_h_u == 0u || fb_w == 0u || fb_h == 0u)
        return out;

    const f64 win_w = static_cast<f64>(win_w_u);
    const f64 win_h = static_cast<f64>(win_h_u);
    const f64 fb_wd = static_cast<f64>(fb_w);
    const f64 fb_hd = static_cast<f64>(fb_h);

    f64 drawn_w = win_w;
    f64 drawn_h = win_h;
    f64 src_u0 = 0.0;
    f64 src_v0 = 0.0;
    f64 src_u1 = 1.0;
    f64 src_v1 = 1.0;

    if (g_active_desc.aspect_mode == AspectMode::Stretch) {
        // Full window; no aspect preservation.
    } else if (g_active_desc.scale_mode == ScaleMode::Integer) {
        const u32 nx = static_cast<u32>(std::max<f64>(1.0, std::floor(win_w / fb_wd)));
        const u32 ny = static_cast<u32>(std::max<f64>(1.0, std::floor(win_h / fb_hd)));
        const u32 n = std::max<u32>(1u, std::min(nx, ny));
        drawn_w = fb_wd * static_cast<f64>(n);
        drawn_h = fb_hd * static_cast<f64>(n);
    } else if (g_active_desc.aspect_mode == AspectMode::Crop) {
        const f64 a_win = win_w / win_h;
        const f64 a_fb = fb_wd / fb_hd;
        if (a_win > a_fb) {
            drawn_w = win_w;
            drawn_h = win_w / a_fb;
            const f64 over = (drawn_h - win_h) / drawn_h;
            src_v0 = 0.5 * over;
            src_v1 = 1.0 - 0.5 * over;
            drawn_h = win_h;
        } else {
            drawn_h = win_h;
            drawn_w = win_h * a_fb;
            const f64 over = (drawn_w - win_w) / drawn_w;
            src_u0 = 0.5 * over;
            src_u1 = 1.0 - 0.5 * over;
            drawn_w = win_w;
        }
    } else {
        const f64 a_win = win_w / win_h;
        const f64 a_fb = fb_wd / fb_hd;
        if (a_win > a_fb) {
            drawn_h = win_h;
            drawn_w = drawn_h * a_fb;
        } else {
            drawn_w = win_w;
            drawn_h = drawn_w / a_fb;
        }
    }

    const f64 rect_x = (win_w - drawn_w) * 0.5;
    const f64 rect_y = (win_h - drawn_h) * 0.5;
    const f64 u = src_u0 + ((static_cast<f64>(mouse.x) - rect_x) / drawn_w) * (src_u1 - src_u0);
    const f64 v = src_v0 + ((static_cast<f64>(mouse.y) - rect_y) / drawn_h) * (src_v1 - src_v0);
    out.x = static_cast<f32>(u * fb_wd);
    out.y = static_cast<f32>(v * fb_hd);
    out.dx = drawn_w > 0.0
                 ? static_cast<f32>((static_cast<f64>(mouse.dx) / drawn_w) * (src_u1 - src_u0) * fb_wd)
                 : mouse.dx;
    out.dy = drawn_h > 0.0
                 ? static_cast<f32>((static_cast<f64>(mouse.dy) / drawn_h) * (src_v1 - src_v0) * fb_hd)
                 : mouse.dy;
    return out;
}

void set_text_input_capturing(bool capturing) {
    g_text_input_capturing = capturing;
}
bool text_input_capturing() {
    return g_text_input_capturing;
}

std::string clipboard_text() {
#if defined(__APPLE__)
    return mac_clipboard_text_impl();
#else
    return g_clipboard_fallback;
#endif
}

void set_clipboard_text(std::string_view text) {
#if defined(__APPLE__)
    mac_set_clipboard_text_impl(text);
#else
    g_clipboard_fallback.assign(text.data(), text.size());
#endif
}

u64 Clock::ticks_now() {
    using clock = std::chrono::steady_clock;
    return static_cast<u64>(clock::now().time_since_epoch().count());
}
u64 Clock::ticks_per_second() {
    using clock = std::chrono::steady_clock;
    return clock::period::den / clock::period::num;
}
f64 Clock::seconds(u64 ticks) {
    return static_cast<f64>(ticks) / static_cast<f64>(ticks_per_second());
}

}  // namespace psynder::platform
