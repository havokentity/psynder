// SPDX-License-Identifier: MIT
// Psynder — small chip-synth music lane for boot jingles and lightweight cues.

#pragma once

#include "core/Types.h"

#include <span>
#include <string_view>

namespace psynder::audio {

enum class ChipWave : u8 {
    Pulse12,
    Pulse25,
    Pulse50,
    Triangle,
    Noise,
};

struct ChipNote {
    u32 start_tick = 0;
    u32 duration_ticks = 1;
    u8 midi_note = 69;
    u8 velocity = 200;
    u8 channel = 0;
    i8 pan = 0;  // -127 = left, 0 = centre, 127 = right.
    ChipWave wave = ChipWave::Pulse50;
};

struct ChipSong {
    u32 bpm = 120;
    u32 ticks_per_beat = 4;
    f32 master_volume = 0.22f;
    bool loop = false;
    std::span<const ChipNote> notes{};
};

enum class ChipTunePreset : u8 {
    PsyArcadeBoot,
    PsyArcadeBootMysterious,
    PsyArcadeBootUplift,
};

void play_chiptune(const ChipSong& song);
void stop_chiptune();
[[nodiscard]] bool chiptune_active();

[[nodiscard]] const char* chiptune_preset_name(ChipTunePreset preset) noexcept;
[[nodiscard]] ChipTunePreset chiptune_preset_from_name(
    std::string_view name, ChipTunePreset fallback = ChipTunePreset::PsyArcadeBoot) noexcept;
[[nodiscard]] const ChipSong& chiptune_preset_song(ChipTunePreset preset);
[[nodiscard]] const ChipSong& boot_chime_song();

}  // namespace psynder::audio
