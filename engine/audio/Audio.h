// SPDX-License-Identifier: MIT
// Psynder — software audio mixer + reverb + HRTF. Lane 12 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>

namespace psynder::audio {

struct VoiceTag {};
using VoiceId = Handle<VoiceTag>;

struct ClipTag {};
using ClipId = Handle<ClipTag>;

enum class Backend : u8 { Auto, WASAPI, CoreAudio, PipeWire, ALSA };

struct DeviceDesc {
    Backend backend       = Backend::Auto;
    u32     sample_rate   = 48000;
    u32     channels      = 2;
    u32     buffer_frames = 512;
};

class Engine {
public:
    static Engine& Get();

    bool start(const DeviceDesc& desc);
    void stop();

    VoiceId play(ClipId clip, math::Vec3 position, f32 volume = 1.0f);
    void    stop_voice(VoiceId voice);
    void    set_listener(math::Vec3 eye, math::Vec3 forward, math::Vec3 up);

    u32  active_voice_count() const;
};

}  // namespace psynder::audio
