// SPDX-License-Identifier: MIT
// Psynder — Lane 22 (platform-linux) internal header. Shared between the
// dispatching LinuxPlatform.cpp and the Wayland / X11 / evdev / audio TUs.
//
// This header is internal to the lane: only files inside
// engine/platform/linux/ should include it. The public contract remains
// engine/platform/Platform.h (frozen).

#pragma once

#include "platform/Platform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace psynder::platform::linux_impl {

// ─── KeyCode lookup ──────────────────────────────────────────────────────
// Map a Linux evdev (input-event-codes.h) keycode or X11 keysym to our
// platform-agnostic KeyCode enum.  Returns KeyCode::Unknown on miss.
KeyCode key_from_evdev(uint32_t evdev_code) noexcept;
KeyCode key_from_xkb(uint32_t xkb_keysym) noexcept;

// Set/clear the key down/pressed state in the shared input ring. Called by
// the evdev / xkb event sources; queried by Input::key_down/pressed().
void input_push_key(KeyCode k, bool down) noexcept;
void input_push_mouse_motion(float dx, float dy, float abs_x, float abs_y) noexcept;
void input_push_mouse_button(int button /*0=L 1=R 2=M*/, bool down) noexcept;
void input_push_mouse_wheel(float delta) noexcept;
// Append a typed Unicode codepoint to this frame's text-entry buffer
// (Input::text_input()). Fed by Xutf8LookupString / xkb_state_key_get_utf32;
// C0 controls + DEL are filtered inside. Cleared by input_frame_advance().
void input_push_text(u32 codepoint) noexcept;
// Called once per frame from poll_events() — promotes "down" to "pressed"
// (first frame down) and clears edge bits.
void input_frame_advance() noexcept;

// ─── Window backend ──────────────────────────────────────────────────────
// Concrete window backends. Returns nullptr on failure; caller falls back to
// the next backend (Wayland → X11 → null).
Window* try_create_wayland_window(const WindowDesc& desc) noexcept;
Window* try_create_x11_window(const WindowDesc& desc) noexcept;

// ─── evdev source ────────────────────────────────────────────────────────
// Starts a background thread polling /dev/input/event* for gamepads.
// Idempotent. Stopped at process exit / destroy_window_impl.
void evdev_start() noexcept;
void evdev_stop() noexcept;

// ─── Audio backend ───────────────────────────────────────────────────────
// PipeWire-first + ALSA fallback. We don't ship an audio API yet (Lane 12
// owns engine/audio/Audio.h); the platform side only opens the device. The
// returned handle's destructor closes the device. Optional in Wave A —
// sample_00_clear doesn't use audio. We still open the device so cold-start
// latency is paid up front.
struct AudioDevice {
    virtual ~AudioDevice() = default;
    virtual const char* backend_name() const noexcept = 0;
    virtual bool ok() const noexcept = 0;
};
std::unique_ptr<AudioDevice> try_open_pipewire() noexcept;
std::unique_ptr<AudioDevice> try_open_alsa() noexcept;

}  // namespace psynder::platform::linux_impl
