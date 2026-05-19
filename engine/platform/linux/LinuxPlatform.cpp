// SPDX-License-Identifier: MIT
// Psynder — Linux platform entry point. Lane 22.
//
// This TU is the dispatcher: it implements the platform::create_window_impl
// / destroy_window_impl / input() / FS helpers that Platform.cpp's shim
// forwards into. The actual window backends live in WaylandWindow.cpp and
// X11Window.cpp; the evdev input source lives in EvdevInput.cpp; the
// PipeWire / ALSA audio openers live in LinuxAudio.cpp.
//
// Wayland is tried first per DESIGN.md §11.2; on failure (no compositor,
// XDG_RUNTIME_DIR unset, server connect fails) we fall back to X11. If
// neither works we hand back a null window so the sample exits cleanly.
//
// Belt-and-braces: this whole TU is guarded by PSYNDER_PLATFORM_LINUX. The
// parent CMakeLists.txt only descends into engine/platform/linux/ under
// that flag, so on a Mac / Windows host this file is a no-op even if it
// somehow ended up in the build.

#ifdef PSYNDER_PLATFORM_LINUX

#include "platform/Platform.h"
#include "LinuxPlatform_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace psynder::platform {

// Forward declarations — defined in the backend TUs.
namespace linux_impl {
Window* try_create_wayland_window(const WindowDesc& desc) noexcept;
Window* try_create_x11_window    (const WindowDesc& desc) noexcept;

// Process-lifetime audio device handle. Initialized when the first window
// is created; torn down when the last window is destroyed. The dispatcher
// owns this; the AudioDevice interface is declared in the internal header.
std::unique_ptr<AudioDevice> g_audio;
}  // namespace linux_impl

// ─── Window factory ──────────────────────────────────────────────────────
// Try Wayland first (xdg-shell + viewporter), then X11 (XShm + XRender).
// XDG_RUNTIME_DIR + WAYLAND_DISPLAY presence is checked inside the Wayland
// backend; we don't second-guess the user's environment here.
Window* create_window_impl(const WindowDesc& desc) {
    Window* w = nullptr;

    // Allow explicit override via PSYNDER_PLATFORM=wayland|x11. Useful for
    // testing the fallback path on a working Wayland session.
    const char* force = std::getenv("PSYNDER_PLATFORM");
    if (force && std::strcmp(force, "x11") == 0) {
        w = linux_impl::try_create_x11_window(desc);
    } else if (force && std::strcmp(force, "wayland") == 0) {
        w = linux_impl::try_create_wayland_window(desc);
    } else {
        w = linux_impl::try_create_wayland_window(desc);
        if (!w) w = linux_impl::try_create_x11_window(desc);
    }

    if (w) {
        // Background-poll gamepads via evdev. Keyboard / mouse comes from
        // the display server's surface events; evdev is a sidecar.
        linux_impl::evdev_start();
        // Open the audio device early so cold-start latency doesn't bite
        // the first sound effect. PipeWire first, ALSA fallback.
        linux_impl::g_audio = linux_impl::try_open_pipewire();
        if (!linux_impl::g_audio) {
            linux_impl::g_audio = linux_impl::try_open_alsa();
        }
        if (!linux_impl::g_audio) {
            PSY_LOG_WARN("audio: neither PipeWire nor ALSA available");
        }
    }
    return w;
}

void destroy_window_impl(Window* w) {
    linux_impl::g_audio.reset();
    linux_impl::evdev_stop();
    delete w;   // virtual dtor cleans up the backend
}

// ─── Input state ─────────────────────────────────────────────────────────
// Lock-free-ish input state. We hold an array of key states (down + edge)
// indexed by KeyCode, plus the mouse state. Keys are updated from whichever
// thread is reading evdev / xkb (the window backend's event pump). Reads
// from the main thread query the snapshot.
//
// We use a mutex rather than per-key atomics because input volume is tiny
// (max a few events per ms) and the read pattern is "all keys, once per
// frame" — the cache contention of striped atomics would dwarf a quick
// mutex acquire. Public API stays trivially copyable.
namespace linux_impl {

namespace {

struct InputState {
    // [0] = down this frame, [1] = down last frame.
    std::array<std::uint8_t, static_cast<usize>(KeyCode::Count)> down{};
    std::array<std::uint8_t, static_cast<usize>(KeyCode::Count)> prev{};
    MouseState mouse{};
    float      mouse_dx_accum = 0.f;
    float      mouse_dy_accum = 0.f;
    float      wheel_accum    = 0.f;
};
std::mutex  g_input_mu;
InputState  g_input_state;

}  // namespace

void input_push_key(KeyCode k, bool down) noexcept {
    if (k == KeyCode::Unknown || k >= KeyCode::Count) return;
    std::lock_guard lock(g_input_mu);
    g_input_state.down[static_cast<usize>(k)] = down ? 1u : 0u;
}

void input_push_mouse_motion(float dx, float dy, float abs_x, float abs_y) noexcept {
    std::lock_guard lock(g_input_mu);
    g_input_state.mouse_dx_accum += dx;
    g_input_state.mouse_dy_accum += dy;
    g_input_state.mouse.x = abs_x;
    g_input_state.mouse.y = abs_y;
}

void input_push_mouse_button(int button, bool down) noexcept {
    std::lock_guard lock(g_input_mu);
    switch (button) {
    case 0: g_input_state.mouse.left   = down; break;
    case 1: g_input_state.mouse.right  = down; break;
    case 2: g_input_state.mouse.middle = down; break;
    default: break;
    }
}

void input_push_mouse_wheel(float delta) noexcept {
    std::lock_guard lock(g_input_mu);
    g_input_state.wheel_accum += delta;
}

void input_frame_advance() noexcept {
    std::lock_guard lock(g_input_mu);
    // Snapshot the per-frame deltas into the mouse state and clear them.
    g_input_state.mouse.dx    = g_input_state.mouse_dx_accum;
    g_input_state.mouse.dy    = g_input_state.mouse_dy_accum;
    g_input_state.mouse.wheel = g_input_state.wheel_accum;
    g_input_state.mouse_dx_accum = 0.f;
    g_input_state.mouse_dy_accum = 0.f;
    g_input_state.wheel_accum    = 0.f;
    // Promote down→prev so key_pressed() can fire the rising edge once.
    g_input_state.prev = g_input_state.down;
}

namespace {

class LinuxInput final : public Input {
public:
    bool key_down(KeyCode k) const override {
        if (k == KeyCode::Unknown || k >= KeyCode::Count) return false;
        std::lock_guard lock(g_input_mu);
        return g_input_state.down[static_cast<usize>(k)] != 0u;
    }
    bool key_pressed(KeyCode k) const override {
        if (k == KeyCode::Unknown || k >= KeyCode::Count) return false;
        std::lock_guard lock(g_input_mu);
        return g_input_state.down[static_cast<usize>(k)] != 0u
            && g_input_state.prev[static_cast<usize>(k)] == 0u;
    }
    const MouseState& mouse() const override {
        // Return a thread-local snapshot. The shared state may be written
        // from the evdev worker thread; copying under the lock once per
        // call gives the caller a stable view that survives until the
        // next mouse() invocation on this thread.
        thread_local MouseState snap{};
        std::lock_guard lock(g_input_mu);
        snap = g_input_state.mouse;
        return snap;
    }
};

LinuxInput g_input;

}  // namespace
}  // namespace linux_impl

Input* input() { return &linux_impl::g_input; }

// ─── Process / FS helpers ────────────────────────────────────────────────
std::string executable_path() {
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string{buf};
}

std::string user_config_dir() {
    // XDG_CONFIG_HOME, then $HOME/.config, then "". freedesktop.org spec.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string{xdg} + "/psynder";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string{home} + "/.config/psynder";
    }
    return {};
}

std::string current_working_directory() {
    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf)) == nullptr) return {};
    return std::string{buf};
}

bool file_exists(std::string_view path) {
    if (path.empty()) return false;
    // string_view may not be NUL-terminated; copy into a small buffer.
    std::string p{path};
    struct ::stat st {};
    return ::stat(p.c_str(), &st) == 0;
}

}  // namespace psynder::platform

#endif  // PSYNDER_PLATFORM_LINUX
