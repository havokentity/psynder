// SPDX-License-Identifier: MIT
// Psynder — internal chip-synth bridge used by the audio mixer.

#pragma once

#include "audio/Chiptune.h"

namespace psynder::audio::detail {

void chiptune_set_sample_rate(u32 sample_rate);
void chiptune_play(const ChipSong& song);
void chiptune_stop();
[[nodiscard]] bool chiptune_is_active();
void chiptune_render_into(f32* stereo, u32 frames) noexcept;

}  // namespace psynder::audio::detail
