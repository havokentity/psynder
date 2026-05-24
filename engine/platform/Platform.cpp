// SPDX-License-Identifier: MIT
// Psynder — platform shared shim. The actual window / input / clock impl
// lives in the platform-specific lane (win32, linux, macos). Provides a
// dispatching create_window() the samples call into.

#include "Platform.h"

#include <chrono>
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
