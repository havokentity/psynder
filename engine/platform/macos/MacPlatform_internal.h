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

// ─── Filesystem helpers ──────────────────────────────────────────────────
// Live in MacPlatformFs.cpp so they can be tested without bringing AppKit
// into the test binary.
std::string fs_executable_path();
std::string fs_user_config_dir();
std::string fs_current_working_directory();
bool        fs_file_exists(std::string_view path);

}  // namespace psynder::platform::macos
