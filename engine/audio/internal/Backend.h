// SPDX-License-Identifier: MIT
// Psynder — audio backend dispatcher. Lane 12 declares the platform-init
// entry points; the platform lanes (21 / 22 / 23) implement them. Until
// those lanes wire the OS-specific code in, the dispatcher's default path
// runs a null device (no audio out, but engine start/stop still succeeds).
//
// DESIGN.md §10.2 backends: WASAPI / CoreAudio / PipeWire / ALSA.

#pragma once

#include "audio/Audio.h"
#include "core/Types.h"

namespace psynder::audio {

// One mixer pull callback. Backends invoke this from their device callback;
// the mixer fills `out_stereo_interleaved` with `frames` of stereo audio.
using MixerCallback = void (*)(f32* out_stereo_interleaved, u32 frames, void* user);

// Per-backend init/shutdown. Each returns true on success.
// The platform lanes provide the real implementations; lane 12 ships weak
// no-op fallbacks so engine code that calls these functions still links.
bool backend_init_wasapi(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept;
bool backend_init_coreaudio(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept;
bool backend_init_pipewire(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept;
bool backend_init_alsa(const DeviceDesc& desc, MixerCallback cb, void* user) noexcept;

void backend_shutdown_wasapi() noexcept;
void backend_shutdown_coreaudio() noexcept;
void backend_shutdown_pipewire() noexcept;
void backend_shutdown_alsa() noexcept;

// Dispatcher used by Engine::start / Engine::stop. Picks the right backend
// for the current platform when `desc.backend == Auto`.
bool backend_init(const DeviceDesc& desc, MixerCallback cb, void* user, Backend& chosen) noexcept;
void backend_shutdown(Backend chosen) noexcept;

}  // namespace psynder::audio
