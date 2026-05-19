// SPDX-License-Identifier: MIT
// Psynder — audio backend dispatcher + weak fallback implementations.
//
// The real WASAPI / CoreAudio / PipeWire / ALSA wiring lives in the platform
// lanes (21 win32, 22 linux, 23 macos). Until those land, this TU provides
// fallback implementations marked `[[gnu::weak]]` so any platform lane that
// supplies a strong symbol takes precedence at link time. Engine code calls
// the dispatcher entry points unconditionally, so Wave-A builds stay green
// regardless of which platform lane is merged.

#include "Backend.h"
#include "core/Log.h"

namespace psynder::audio {

namespace {
// Track the last-init'd callback so a null-device thread can keep the mixer
// alive (drives Engine::active_voice_count assertions even with no real HW).
MixerCallback g_null_cb   = nullptr;
void*         g_null_user = nullptr;
u32           g_null_sr   = 48000;
u32           g_null_frames = 512;
}  // namespace

// ─── Weak per-backend fallbacks ──────────────────────────────────────────
//
// Platform lanes provide strong overrides; the GCC/Clang `[[gnu::weak]]`
// attribute lets the linker prefer them when present. On MSVC we currently
// rely on the platform lanes shipping their own object — fine for Wave A
// because the orchestrator runs on macOS.
#if defined(__clang__) || defined(__GNUC__)
#   define PSY_WEAK __attribute__((weak))
#else
#   define PSY_WEAK
#endif

PSY_WEAK bool backend_init_wasapi(const DeviceDesc&, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_wasapi: lane 12 fallback (null device).");
    g_null_cb = cb; g_null_user = user;
    return true;
}
PSY_WEAK void backend_shutdown_wasapi() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

PSY_WEAK bool backend_init_coreaudio(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_coreaudio: lane 12 fallback (null device).");
    g_null_cb     = cb;
    g_null_user   = user;
    g_null_sr     = desc.sample_rate;
    g_null_frames = desc.buffer_frames;
    return true;
}
PSY_WEAK void backend_shutdown_coreaudio() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

PSY_WEAK bool backend_init_pipewire(const DeviceDesc&, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_pipewire: lane 12 fallback (null device).");
    g_null_cb = cb; g_null_user = user;
    return true;
}
PSY_WEAK void backend_shutdown_pipewire() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

PSY_WEAK bool backend_init_alsa(const DeviceDesc&, MixerCallback cb, void* user) noexcept {
    PSY_LOG_INFO("[audio] backend_init_alsa: lane 12 fallback (null device).");
    g_null_cb = cb; g_null_user = user;
    return true;
}
PSY_WEAK void backend_shutdown_alsa() noexcept {
    g_null_cb = nullptr; g_null_user = nullptr;
}

// ─── Dispatcher ──────────────────────────────────────────────────────────
bool backend_init(const DeviceDesc& desc, MixerCallback cb, void* user, Backend& chosen) noexcept {
    Backend b = desc.backend;
    if (b == Backend::Auto) {
#if defined(PSYNDER_PLATFORM_MACOS)
        b = Backend::CoreAudio;
#elif defined(PSYNDER_PLATFORM_WIN32)
        b = Backend::WASAPI;
#elif defined(PSYNDER_PLATFORM_LINUX)
        b = Backend::PipeWire;  // PipeWire-over-ALSA preferred per DESIGN
#else
        b = Backend::ALSA;
#endif
    }
    chosen = b;
    switch (b) {
        case Backend::WASAPI:    return backend_init_wasapi   (desc, cb, user);
        case Backend::CoreAudio: return backend_init_coreaudio(desc, cb, user);
        case Backend::PipeWire:  return backend_init_pipewire (desc, cb, user);
        case Backend::ALSA:      return backend_init_alsa     (desc, cb, user);
        case Backend::Auto:      return false;  // unreachable
    }
    return false;
}

void backend_shutdown(Backend chosen) noexcept {
    switch (chosen) {
        case Backend::WASAPI:    backend_shutdown_wasapi();    break;
        case Backend::CoreAudio: backend_shutdown_coreaudio(); break;
        case Backend::PipeWire:  backend_shutdown_pipewire();  break;
        case Backend::ALSA:      backend_shutdown_alsa();      break;
        case Backend::Auto:                                    break;
    }
}

}  // namespace psynder::audio
