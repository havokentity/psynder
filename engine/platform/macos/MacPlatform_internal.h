// SPDX-License-Identifier: MIT
// Psynder — lane 23 / macOS platform internal header. Shared declarations
// used across the lane's translation units (the Objective-C++ AppKit/Metal
// driver and the C++ filesystem helpers). Not part of the public API.

#pragma once

#include "core/Types.h"
#include "platform/Platform.h"

#include <string>

namespace psynder::platform::macos {

// ─── Audio backend (CoreAudio AUHAL) ─────────────────────────────────────
// Lane 12 (audio) will call this from its mixer's start() once it lands.
// Until then it's exercised by the smoke binary and by the lane 23 unit
// tests. The hook receives a render callback that writes interleaved
// f32 samples into `out` for `frame_count` frames at `sample_rate` Hz with
// `channel_count` channels.
using AudioRenderCallback = void (*)(void* user, f32* out, u32 frame_count,
                                     u32 channel_count, u32 sample_rate);

struct AudioDeviceDesc {
    u32 sample_rate     = 48000;
    u32 channels        = 2;
    u32 buffer_frames   = 512;
};

bool audio_start(const AudioDeviceDesc& desc, AudioRenderCallback cb, void* user);
void audio_stop();
bool audio_running();
u32  audio_actual_sample_rate();
u32  audio_actual_channels();

// ─── Gamepad enumeration (GameController.framework) ──────────────────────
// Arms connect/disconnect notifications and seeds the count with any
// already-paired controllers. Idempotent.
void gamepad_arm();
u32  gamepad_count();

// ─── HID Force Feedback (IOKit / ForceFeedback.framework) ────────────────
// Wave-B: minimal support for a constant-force effect. The Wave-C NFS sample
// (lane 25 sample_04) drives the wheel with a constant-force descriptor
// that scales linearly with the front-tyre lateral slip angle. The lane
// 23 deliverable here is the *descriptor*: a transport-agnostic POD that
// callers fill in and we marshal into a `FFEFFECT` at submission time. The
// real hardware path goes through `ffb_submit_constant_force`; on hosts
// without a force-feedback device it is a no-op that still validates the
// descriptor so unit tests (and the smoke binary) keep running.
//
// Magnitudes follow the DirectInput convention (also adopted by Apple's
// ForceFeedback framework): integer range [-10'000, +10'000]. The
// descriptor itself is in normalized [-1, +1] floats so the engine doesn't
// have to memorise the DirectInput convention; we clamp + scale on submit.
struct FfbConstantForce {
    f32 magnitude     = 0.0f;   // [-1, +1], 0 = no force
    f32 direction_deg = 0.0f;   // 0 = +X axis, 90 = +Y axis (planar)
    u32 duration_us   = 0u;     // 0 = infinite (FF_INFINITE)
    u32 sample_period_us = 0u;  // 0 = device default
    u32 start_delay_us   = 0u;  // 0 = play immediately
    f32 gain          = 1.0f;   // [0, 1] global gain
};

// Build the descriptor's clamped + scaled integer fields. Returns the
// magnitude clamped to the DirectInput [-10000, 10000] integer range and
// the planar (X, Y) direction unit vector multiplied by 10000 (the FF
// convention for a 2-axis Cartesian direction). Pure function — used by
// unit tests so it can run without hardware.
struct FfbConstantForceWire {
    i32 magnitude;       // [-10000, +10000]
    i32 direction_x;     // [-10000, +10000]
    i32 direction_y;     // [-10000, +10000]
    u32 duration;        // microseconds, 0xFFFFFFFF == FF_INFINITE
    u32 sample_period;
    u32 start_delay;
    u32 gain;            // [0, 10000]
};
FfbConstantForceWire ffb_build_constant_force(const FfbConstantForce& d) noexcept;

// Submit a constant-force effect to the first available device. Returns
// true if the effect was queued on real hardware; false if no device was
// found / available. The descriptor is validated either way.
bool ffb_submit_constant_force(const FfbConstantForce& d);

// Release any in-flight FFB effects + close device handles. Idempotent.
void ffb_shutdown();

// ─── Filesystem helpers ──────────────────────────────────────────────────
// Live in MacPlatformFs.cpp so they can be tested without bringing AppKit
// into the test binary.
std::string fs_executable_path();
std::string fs_user_config_dir();
std::string fs_current_working_directory();
bool        fs_file_exists(std::string_view path);

}  // namespace psynder::platform::macos
