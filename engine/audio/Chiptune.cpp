// SPDX-License-Identifier: MIT
// Psynder — lightweight chip-synth playback for boot jingles and music cues.

#include "Chiptune.h"

#include "internal/ChiptuneSynth.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string_view>
#include <vector>

namespace psynder::audio {

namespace {

struct PlaybackNote {
    ChipNote note{};
    u64 start_sample = 0;
    u64 end_sample = 0;
    f32 frequency_hz = 440.0f;
};

struct Playback {
    std::mutex mu{};
    std::vector<PlaybackNote> notes{};
    u64 cursor_sample = 0;
    u64 total_samples = 0;
    u32 sample_rate = 48000;
    f32 master_volume = 0.22f;
    bool active = false;
    bool loop = false;
};

struct PhraseNote {
    u32 start_tick = 0;
    u32 duration_ticks = 1;
    i8 semitone = 0;
    u8 velocity = 200;
    u8 channel = 0;
    i8 pan = 0;
    ChipWave wave = ChipWave::Pulse50;
};

struct Phrase {
    std::string_view name{};
    u8 root_midi = 60;
    std::span<const PhraseNote> notes{};
};

Playback& playback() {
    static Playback p;
    return p;
}

f32 midi_to_frequency(u8 midi_note) noexcept {
    return 440.0f * std::pow(2.0f, (static_cast<f32>(midi_note) - 69.0f) / 12.0f);
}

f32 wave_sample(ChipWave wave, f32 phase, u64 local_sample, u8 midi_note, u8 channel) noexcept {
    phase -= std::floor(phase);
    switch (wave) {
        case ChipWave::Pulse12:
            return phase < 0.125f ? 1.0f : -1.0f;
        case ChipWave::Pulse25:
            return phase < 0.25f ? 1.0f : -1.0f;
        case ChipWave::Pulse50:
            return phase < 0.5f ? 1.0f : -1.0f;
        case ChipWave::Triangle:
            return phase < 0.5f ? -1.0f + phase * 4.0f : 3.0f - phase * 4.0f;
        case ChipWave::Noise: {
            u32 x = static_cast<u32>(local_sample);
            x ^= static_cast<u32>(midi_note) * 0x9E3779B9u;
            x ^= static_cast<u32>(channel) * 0x85EBCA6Bu;
            x ^= x >> 16u;
            x *= 0x7FEB352Du;
            x ^= x >> 15u;
            x *= 0x846CA68Bu;
            x ^= x >> 16u;
            return (x & 0xFFFFu) / 32767.5f - 1.0f;
        }
    }
    return 0.0f;
}

f32 envelope(u64 local_sample, u64 duration_samples, u32 sample_rate) noexcept {
    if (duration_samples == 0u)
        return 0.0f;
    const u64 attack = std::min<u64>(std::max<u64>(1u, sample_rate / 800u), duration_samples / 4u);
    const u64 release = std::min<u64>(std::max<u64>(1u, sample_rate / 140u), duration_samples / 3u);
    f32 env = 1.0f;
    if (attack > 0u && local_sample < attack)
        env = static_cast<f32>(local_sample) / static_cast<f32>(attack);
    if (release > 0u && local_sample + release >= duration_samples) {
        const u64 remaining = duration_samples - local_sample;
        env = std::min(env, static_cast<f32>(remaining) / static_cast<f32>(release));
    }
    return std::clamp(env, 0.0f, 1.0f);
}

bool equal_ascii_ci(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size())
        return false;
    for (usize i = 0; i < lhs.size(); ++i) {
        char a = lhs[i];
        char b = rhs[i];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

u8 clamp_midi(i32 note) noexcept {
    return static_cast<u8>(std::clamp(note, 0, 127));
}

void stamp_phrase(std::vector<ChipNote>& out,
                  const Phrase& phrase,
                  u32 start_tick,
                  i8 transpose = 0,
                  i8 pan_offset = 0,
                  u8 velocity_scale = 255) {
    out.reserve(out.size() + phrase.notes.size());
    for (const PhraseNote& src : phrase.notes) {
        const i32 note = static_cast<i32>(phrase.root_midi) + static_cast<i32>(src.semitone) +
                         static_cast<i32>(transpose);
        const i32 pan = static_cast<i32>(src.pan) + static_cast<i32>(pan_offset);
        const u32 velocity =
            (static_cast<u32>(src.velocity) * static_cast<u32>(velocity_scale) + 127u) / 255u;
        out.push_back(ChipNote{
            .start_tick = start_tick + src.start_tick,
            .duration_ticks = src.duration_ticks,
            .midi_note = clamp_midi(note),
            .velocity = static_cast<u8>(std::min<u32>(velocity, 255u)),
            .channel = src.channel,
            .pan = static_cast<i8>(std::clamp(pan, -127, 127)),
            .wave = src.wave,
        });
    }
}

void stamp_chord(std::vector<ChipNote>& out,
                 u32 start_tick,
                 u32 duration_ticks,
                 std::span<const i8> semitones,
                 u8 root_midi,
                 u8 velocity,
                 u8 channel,
                 ChipWave wave,
                 i8 spread) {
    if (semitones.empty())
        return;
    const i32 half = static_cast<i32>(semitones.size() - 1u) / 2;
    for (usize i = 0; i < semitones.size(); ++i) {
        const i32 pan = static_cast<i32>(i) - half;
        out.push_back(ChipNote{
            .start_tick = start_tick,
            .duration_ticks = duration_ticks,
            .midi_note = clamp_midi(static_cast<i32>(root_midi) + static_cast<i32>(semitones[i])),
            .velocity = velocity,
            .channel = channel,
            .pan = static_cast<i8>(std::clamp(pan * static_cast<i32>(spread), -127, 127)),
            .wave = wave,
        });
    }
}

std::span<const PhraseNote> as_phrase_notes(const PhraseNote* data, usize count) noexcept {
    return std::span<const PhraseNote>{data, count};
}

Phrase make_phrase(std::string_view name, u8 root, const PhraseNote* data, usize count) noexcept {
    return Phrase{.name = name, .root_midi = root, .notes = as_phrase_notes(data, count)};
}

std::vector<ChipNote> build_psy_arcade_boot_notes() {
    static constexpr PhraseNote kBassLift[]{
        {.start_tick = 0,
         .duration_ticks = 8,
         .semitone = 0,
         .velocity = 100,
         .channel = 0,
         .pan = -8,
         .wave = ChipWave::Triangle},
        {.start_tick = 8,
         .duration_ticks = 8,
         .semitone = 5,
         .velocity = 96,
         .channel = 0,
         .pan = 8,
         .wave = ChipWave::Triangle},
        {.start_tick = 16,
         .duration_ticks = 8,
         .semitone = 7,
         .velocity = 104,
         .channel = 0,
         .pan = -6,
         .wave = ChipWave::Triangle},
        {.start_tick = 24,
         .duration_ticks = 10,
         .semitone = 12,
         .velocity = 112,
         .channel = 0,
         .pan = 6,
         .wave = ChipWave::Triangle},
    };
    static constexpr PhraseNote kPulseSteps[]{
        {.start_tick = 0,
         .duration_ticks = 3,
         .semitone = 12,
         .velocity = 86,
         .channel = 1,
         .pan = -38,
         .wave = ChipWave::Pulse25},
        {.start_tick = 4,
         .duration_ticks = 3,
         .semitone = 16,
         .velocity = 82,
         .channel = 1,
         .pan = 34,
         .wave = ChipWave::Pulse25},
        {.start_tick = 8,
         .duration_ticks = 3,
         .semitone = 17,
         .velocity = 90,
         .channel = 1,
         .pan = -30,
         .wave = ChipWave::Pulse25},
        {.start_tick = 12,
         .duration_ticks = 3,
         .semitone = 21,
         .velocity = 86,
         .channel = 1,
         .pan = 28,
         .wave = ChipWave::Pulse25},
        {.start_tick = 16,
         .duration_ticks = 3,
         .semitone = 19,
         .velocity = 92,
         .channel = 1,
         .pan = -24,
         .wave = ChipWave::Pulse25},
        {.start_tick = 20,
         .duration_ticks = 3,
         .semitone = 23,
         .velocity = 90,
         .channel = 1,
         .pan = 24,
         .wave = ChipWave::Pulse25},
        {.start_tick = 24,
         .duration_ticks = 3,
         .semitone = 24,
         .velocity = 102,
         .channel = 1,
         .pan = -18,
         .wave = ChipWave::Pulse25},
        {.start_tick = 28,
         .duration_ticks = 5,
         .semitone = 31,
         .velocity = 116,
         .channel = 1,
         .pan = 16,
         .wave = ChipWave::Pulse50},
    };
    static constexpr PhraseNote kLogoHook[]{
        {.start_tick = 3,
         .duration_ticks = 5,
         .semitone = 28,
         .velocity = 124,
         .channel = 2,
         .pan = 12,
         .wave = ChipWave::Pulse12},
        {.start_tick = 7,
         .duration_ticks = 4,
         .semitone = 31,
         .velocity = 130,
         .channel = 2,
         .pan = -16,
         .wave = ChipWave::Pulse12},
        {.start_tick = 11,
         .duration_ticks = 5,
         .semitone = 36,
         .velocity = 140,
         .channel = 2,
         .pan = 16,
         .wave = ChipWave::Pulse12},
        {.start_tick = 15,
         .duration_ticks = 4,
         .semitone = 33,
         .velocity = 126,
         .channel = 2,
         .pan = -20,
         .wave = ChipWave::Pulse12},
        {.start_tick = 19,
         .duration_ticks = 5,
         .semitone = 40,
         .velocity = 144,
         .channel = 2,
         .pan = 22,
         .wave = ChipWave::Pulse12},
        {.start_tick = 23,
         .duration_ticks = 4,
         .semitone = 43,
         .velocity = 150,
         .channel = 2,
         .pan = -22,
         .wave = ChipWave::Pulse12},
        {.start_tick = 27,
         .duration_ticks = 7,
         .semitone = 48,
         .velocity = 160,
         .channel = 2,
         .pan = 0,
         .wave = ChipWave::Pulse25},
    };
    static constexpr PhraseNote kShimmer[]{
        {.start_tick = 1,
         .duration_ticks = 1,
         .semitone = 36,
         .velocity = 58,
         .channel = 3,
         .pan = -50,
         .wave = ChipWave::Pulse12},
        {.start_tick = 2,
         .duration_ticks = 1,
         .semitone = 40,
         .velocity = 60,
         .channel = 3,
         .pan = 50,
         .wave = ChipWave::Pulse12},
        {.start_tick = 5,
         .duration_ticks = 1,
         .semitone = 43,
         .velocity = 62,
         .channel = 3,
         .pan = -44,
         .wave = ChipWave::Pulse12},
        {.start_tick = 6,
         .duration_ticks = 1,
         .semitone = 48,
         .velocity = 58,
         .channel = 3,
         .pan = 44,
         .wave = ChipWave::Pulse12},
        {.start_tick = 9,
         .duration_ticks = 1,
         .semitone = 40,
         .velocity = 62,
         .channel = 3,
         .pan = -38,
         .wave = ChipWave::Pulse12},
        {.start_tick = 10,
         .duration_ticks = 1,
         .semitone = 43,
         .velocity = 64,
         .channel = 3,
         .pan = 38,
         .wave = ChipWave::Pulse12},
        {.start_tick = 13,
         .duration_ticks = 1,
         .semitone = 47,
         .velocity = 68,
         .channel = 3,
         .pan = -34,
         .wave = ChipWave::Pulse12},
        {.start_tick = 14,
         .duration_ticks = 1,
         .semitone = 52,
         .velocity = 62,
         .channel = 3,
         .pan = 34,
         .wave = ChipWave::Pulse12},
        {.start_tick = 17,
         .duration_ticks = 1,
         .semitone = 43,
         .velocity = 64,
         .channel = 3,
         .pan = -30,
         .wave = ChipWave::Pulse12},
        {.start_tick = 18,
         .duration_ticks = 1,
         .semitone = 48,
         .velocity = 66,
         .channel = 3,
         .pan = 30,
         .wave = ChipWave::Pulse12},
        {.start_tick = 21,
         .duration_ticks = 1,
         .semitone = 52,
         .velocity = 70,
         .channel = 3,
         .pan = -26,
         .wave = ChipWave::Pulse12},
        {.start_tick = 22,
         .duration_ticks = 1,
         .semitone = 55,
         .velocity = 66,
         .channel = 3,
         .pan = 26,
         .wave = ChipWave::Pulse12},
    };
    static constexpr PhraseNote kPerc[]{
        {.start_tick = 0,
         .duration_ticks = 1,
         .semitone = 7,
         .velocity = 26,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 2,
         .duration_ticks = 1,
         .semitone = 22,
         .velocity = 18,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 4,
         .duration_ticks = 1,
         .semitone = 14,
         .velocity = 28,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 6,
         .duration_ticks = 1,
         .semitone = 26,
         .velocity = 20,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 8,
         .duration_ticks = 1,
         .semitone = 7,
         .velocity = 30,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 12,
         .duration_ticks = 1,
         .semitone = 14,
         .velocity = 32,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 16,
         .duration_ticks = 2,
         .semitone = 10,
         .velocity = 26,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 24,
         .duration_ticks = 2,
         .semitone = 14,
         .velocity = 34,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 28,
         .duration_ticks = 3,
         .semitone = 42,
         .velocity = 30,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
    };

    std::vector<ChipNote> notes;
    stamp_phrase(notes, make_phrase("bass-lift", 48, kBassLift, std::size(kBassLift)), 0);
    stamp_phrase(notes, make_phrase("pulse-steps", 48, kPulseSteps, std::size(kPulseSteps)), 0);
    stamp_phrase(notes, make_phrase("logo-hook", 48, kLogoHook, std::size(kLogoHook)), 0);
    stamp_phrase(notes, make_phrase("arpeggio-shimmer", 48, kShimmer, std::size(kShimmer)), 0);
    stamp_phrase(notes, make_phrase("soft-noise-perc", 48, kPerc, std::size(kPerc)), 0);
    static constexpr i8 kFinalMajor[]{40, 43, 36};
    stamp_chord(notes,
                30,
                2,
                std::span<const i8>{kFinalMajor, std::size(kFinalMajor)},
                48,
                118,
                5,
                ChipWave::Pulse25,
                18);
    static constexpr i8 kFinalResolve[]{36, 43, 48};
    stamp_chord(notes,
                32,
                6,
                std::span<const i8>{kFinalResolve, std::size(kFinalResolve)},
                48,
                116,
                5,
                ChipWave::Triangle,
                14);
    return notes;
}

std::vector<ChipNote> build_mysterious_boot_notes() {
    static constexpr PhraseNote kDrone[]{
        {.start_tick = 0,
         .duration_ticks = 16,
         .semitone = 0,
         .velocity = 74,
         .channel = 0,
         .pan = -14,
         .wave = ChipWave::Triangle},
        {.start_tick = 16,
         .duration_ticks = 16,
         .semitone = 7,
         .velocity = 76,
         .channel = 0,
         .pan = 14,
         .wave = ChipWave::Triangle},
        {.start_tick = 32,
         .duration_ticks = 12,
         .semitone = 12,
         .velocity = 88,
         .channel = 0,
         .pan = 0,
         .wave = ChipWave::Triangle},
    };
    static constexpr PhraseNote kMinorSpark[]{
        {.start_tick = 4,
         .duration_ticks = 2,
         .semitone = 24,
         .velocity = 70,
         .channel = 1,
         .pan = -52,
         .wave = ChipWave::Pulse12},
        {.start_tick = 6,
         .duration_ticks = 2,
         .semitone = 31,
         .velocity = 78,
         .channel = 1,
         .pan = 52,
         .wave = ChipWave::Pulse12},
        {.start_tick = 10,
         .duration_ticks = 3,
         .semitone = 27,
         .velocity = 82,
         .channel = 1,
         .pan = -34,
         .wave = ChipWave::Pulse25},
        {.start_tick = 14,
         .duration_ticks = 2,
         .semitone = 34,
         .velocity = 74,
         .channel = 1,
         .pan = 34,
         .wave = ChipWave::Pulse12},
        {.start_tick = 20,
         .duration_ticks = 2,
         .semitone = 31,
         .velocity = 86,
         .channel = 1,
         .pan = -28,
         .wave = ChipWave::Pulse12},
        {.start_tick = 22,
         .duration_ticks = 2,
         .semitone = 36,
         .velocity = 92,
         .channel = 1,
         .pan = 28,
         .wave = ChipWave::Pulse12},
        {.start_tick = 28,
         .duration_ticks = 6,
         .semitone = 39,
         .velocity = 102,
         .channel = 1,
         .pan = 0,
         .wave = ChipWave::Pulse25},
    };
    static constexpr PhraseNote kPulseWake[]{
        {.start_tick = 1,
         .duration_ticks = 1,
         .semitone = 12,
         .velocity = 34,
         .channel = 2,
         .pan = -40,
         .wave = ChipWave::Pulse12},
        {.start_tick = 5,
         .duration_ticks = 1,
         .semitone = 19,
         .velocity = 38,
         .channel = 2,
         .pan = 40,
         .wave = ChipWave::Pulse12},
        {.start_tick = 9,
         .duration_ticks = 1,
         .semitone = 24,
         .velocity = 44,
         .channel = 2,
         .pan = -32,
         .wave = ChipWave::Pulse12},
        {.start_tick = 13,
         .duration_ticks = 1,
         .semitone = 31,
         .velocity = 46,
         .channel = 2,
         .pan = 32,
         .wave = ChipWave::Pulse12},
        {.start_tick = 17,
         .duration_ticks = 1,
         .semitone = 27,
         .velocity = 48,
         .channel = 2,
         .pan = -24,
         .wave = ChipWave::Pulse12},
        {.start_tick = 21,
         .duration_ticks = 1,
         .semitone = 34,
         .velocity = 52,
         .channel = 2,
         .pan = 24,
         .wave = ChipWave::Pulse12},
        {.start_tick = 25,
         .duration_ticks = 1,
         .semitone = 39,
         .velocity = 56,
         .channel = 2,
         .pan = -16,
         .wave = ChipWave::Pulse12},
        {.start_tick = 29,
         .duration_ticks = 1,
         .semitone = 43,
         .velocity = 60,
         .channel = 2,
         .pan = 16,
         .wave = ChipWave::Pulse12},
    };
    std::vector<ChipNote> notes;
    stamp_phrase(notes, make_phrase("dawn-drone", 45, kDrone, std::size(kDrone)), 0);
    stamp_phrase(notes, make_phrase("minor-spark", 45, kMinorSpark, std::size(kMinorSpark)), 0);
    stamp_phrase(notes, make_phrase("pulse-wake", 45, kPulseWake, std::size(kPulseWake)), 0);
    static constexpr i8 kResolve[]{24, 31, 36, 39};
    stamp_chord(notes,
                34,
                8,
                std::span<const i8>{kResolve, std::size(kResolve)},
                45,
                84,
                3,
                ChipWave::Triangle,
                12);
    return notes;
}

std::vector<ChipNote> build_uplift_boot_notes() {
    static constexpr PhraseNote kAscend[]{
        {.start_tick = 0,
         .duration_ticks = 4,
         .semitone = 0,
         .velocity = 96,
         .channel = 0,
         .pan = -12,
         .wave = ChipWave::Triangle},
        {.start_tick = 4,
         .duration_ticks = 4,
         .semitone = 4,
         .velocity = 98,
         .channel = 0,
         .pan = 12,
         .wave = ChipWave::Triangle},
        {.start_tick = 8,
         .duration_ticks = 4,
         .semitone = 7,
         .velocity = 102,
         .channel = 0,
         .pan = -10,
         .wave = ChipWave::Triangle},
        {.start_tick = 12,
         .duration_ticks = 6,
         .semitone = 12,
         .velocity = 112,
         .channel = 0,
         .pan = 10,
         .wave = ChipWave::Triangle},
    };
    static constexpr PhraseNote kLead[]{
        {.start_tick = 2,
         .duration_ticks = 3,
         .semitone = 24,
         .velocity = 120,
         .channel = 1,
         .pan = -22,
         .wave = ChipWave::Pulse25},
        {.start_tick = 5,
         .duration_ticks = 3,
         .semitone = 28,
         .velocity = 126,
         .channel = 1,
         .pan = 22,
         .wave = ChipWave::Pulse25},
        {.start_tick = 8,
         .duration_ticks = 3,
         .semitone = 31,
         .velocity = 134,
         .channel = 1,
         .pan = -18,
         .wave = ChipWave::Pulse12},
        {.start_tick = 11,
         .duration_ticks = 3,
         .semitone = 35,
         .velocity = 130,
         .channel = 1,
         .pan = 18,
         .wave = ChipWave::Pulse12},
        {.start_tick = 14,
         .duration_ticks = 6,
         .semitone = 36,
         .velocity = 148,
         .channel = 1,
         .pan = 0,
         .wave = ChipWave::Pulse25},
    };
    static constexpr PhraseNote kSparkle[]{
        {.start_tick = 1,
         .duration_ticks = 1,
         .semitone = 36,
         .velocity = 54,
         .channel = 2,
         .pan = -54,
         .wave = ChipWave::Pulse12},
        {.start_tick = 3,
         .duration_ticks = 1,
         .semitone = 40,
         .velocity = 54,
         .channel = 2,
         .pan = 54,
         .wave = ChipWave::Pulse12},
        {.start_tick = 7,
         .duration_ticks = 1,
         .semitone = 43,
         .velocity = 58,
         .channel = 2,
         .pan = -42,
         .wave = ChipWave::Pulse12},
        {.start_tick = 9,
         .duration_ticks = 1,
         .semitone = 47,
         .velocity = 58,
         .channel = 2,
         .pan = 42,
         .wave = ChipWave::Pulse12},
        {.start_tick = 13,
         .duration_ticks = 1,
         .semitone = 48,
         .velocity = 66,
         .channel = 2,
         .pan = -30,
         .wave = ChipWave::Pulse12},
        {.start_tick = 15,
         .duration_ticks = 1,
         .semitone = 52,
         .velocity = 68,
         .channel = 2,
         .pan = 30,
         .wave = ChipWave::Pulse12},
    };
    std::vector<ChipNote> notes;
    stamp_phrase(notes, make_phrase("major-ascend", 48, kAscend, std::size(kAscend)), 0);
    stamp_phrase(notes, make_phrase("uplift-lead", 48, kLead, std::size(kLead)), 0);
    stamp_phrase(notes, make_phrase("sparkle", 48, kSparkle, std::size(kSparkle)), 0);
    static constexpr i8 kResolve[]{24, 28, 31, 36};
    stamp_chord(notes,
                18,
                6,
                std::span<const i8>{kResolve, std::size(kResolve)},
                48,
                104,
                3,
                ChipWave::Triangle,
                10);
    return notes;
}

const ChipSong& make_song(ChipTunePreset preset) {
    switch (preset) {
        case ChipTunePreset::PsyArcadeBootMysterious: {
            static const std::vector<ChipNote> kNotes = build_mysterious_boot_notes();
            static const ChipSong kSong{
                .bpm = 132u,
                .ticks_per_beat = 4u,
                .master_volume = 0.13f,
                .loop = false,
                .notes = std::span<const ChipNote>{kNotes.data(), kNotes.size()},
            };
            return kSong;
        }
        case ChipTunePreset::PsyArcadeBootUplift: {
            static const std::vector<ChipNote> kNotes = build_uplift_boot_notes();
            static const ChipSong kSong{
                .bpm = 152u,
                .ticks_per_beat = 4u,
                .master_volume = 0.12f,
                .loop = false,
                .notes = std::span<const ChipNote>{kNotes.data(), kNotes.size()},
            };
            return kSong;
        }
        case ChipTunePreset::PsyArcadeBoot:
        default: {
            static const std::vector<ChipNote> kNotes = build_psy_arcade_boot_notes();
            static const ChipSong kSong{
                .bpm = 148u,
                .ticks_per_beat = 4u,
                .master_volume = 0.12f,
                .loop = false,
                .notes = std::span<const ChipNote>{kNotes.data(), kNotes.size()},
            };
            return kSong;
        }
    }
}

}  // namespace

namespace detail {

void chiptune_set_sample_rate(u32 sample_rate) {
    Playback& p = playback();
    std::lock_guard<std::mutex> lk(p.mu);
    p.sample_rate = sample_rate == 0u ? 48000u : sample_rate;
}

void chiptune_play(const ChipSong& song) {
    Playback& p = playback();
    std::lock_guard<std::mutex> lk(p.mu);
    p.notes.clear();
    p.cursor_sample = 0;
    p.total_samples = 0;
    p.active = false;
    p.loop = song.loop;
    p.master_volume = std::clamp(song.master_volume, 0.0f, 1.0f);

    const u32 bpm = song.bpm == 0u ? 120u : song.bpm;
    const u32 ticks_per_beat = song.ticks_per_beat == 0u ? 4u : song.ticks_per_beat;
    const f64 ticks_per_second = (static_cast<f64>(bpm) / 60.0) * static_cast<f64>(ticks_per_beat);
    const f64 samples_per_tick = static_cast<f64>(p.sample_rate) / ticks_per_second;
    p.notes.reserve(song.notes.size());
    for (const ChipNote& note : song.notes) {
        if (note.duration_ticks == 0u || note.velocity == 0u)
            continue;
        const u64 start =
            static_cast<u64>(std::llround(static_cast<f64>(note.start_tick) * samples_per_tick));
        const u64 end = static_cast<u64>(
            std::llround(static_cast<f64>(note.start_tick + note.duration_ticks) * samples_per_tick));
        if (end <= start)
            continue;
        p.notes.push_back(PlaybackNote{
            .note = note,
            .start_sample = start,
            .end_sample = end,
            .frequency_hz = midi_to_frequency(note.midi_note),
        });
        p.total_samples = std::max(p.total_samples, end);
    }
    p.active = !p.notes.empty();
}

void chiptune_stop() {
    Playback& p = playback();
    std::lock_guard<std::mutex> lk(p.mu);
    p.active = false;
    p.cursor_sample = 0;
}

bool chiptune_is_active() {
    Playback& p = playback();
    std::lock_guard<std::mutex> lk(p.mu);
    return p.active;
}

void chiptune_render_into(f32* stereo, u32 frames) noexcept {
    Playback& p = playback();
    // Real-time audio thread: MUST NOT block. chiptune_play() holds p.mu while
    // it reserves/push_backs the note vector (heap allocation) — blocking on
    // that here would stall the audio callback (priority inversion, glitch).
    // try_lock instead: if play/stop is mid-mutation we skip this one block
    // (outputs the silence already in `stereo`), which is imperceptible.
    std::unique_lock<std::mutex> lk(p.mu, std::try_to_lock);
    if (!lk.owns_lock())
        return;
    if (!p.active || p.notes.empty() || p.sample_rate == 0u)
        return;

    for (u32 frame = 0; frame < frames; ++frame) {
        if (p.total_samples == 0u || p.cursor_sample >= p.total_samples) {
            if (p.loop && p.total_samples > 0u) {
                p.cursor_sample %= p.total_samples;
            } else {
                p.active = false;
                return;
            }
        }

        const u64 sample = p.cursor_sample;
        f32 left = 0.0f;
        f32 right = 0.0f;
        for (const PlaybackNote& event : p.notes) {
            if (sample < event.start_sample || sample >= event.end_sample)
                continue;
            const u64 local_sample = sample - event.start_sample;
            const u64 duration = event.end_sample - event.start_sample;
            const f32 phase = static_cast<f32>(
                (static_cast<f64>(local_sample) * static_cast<f64>(event.frequency_hz)) /
                static_cast<f64>(p.sample_rate));
            const f32 env = envelope(local_sample, duration, p.sample_rate);
            const f32 velocity = static_cast<f32>(event.note.velocity) / 255.0f;
            const f32 mono =
                wave_sample(event.note.wave, phase, local_sample, event.note.midi_note, event.note.channel) *
                velocity * env * p.master_volume;
            const f32 pan = std::clamp(static_cast<f32>(event.note.pan) / 127.0f, -1.0f, 1.0f);
            left += mono * (pan <= 0.0f ? 1.0f : 1.0f - pan);
            right += mono * (pan >= 0.0f ? 1.0f : 1.0f + pan);
        }
        stereo[frame * 2u + 0u] += left;
        stereo[frame * 2u + 1u] += right;
        ++p.cursor_sample;
    }
}

}  // namespace detail

void play_chiptune(const ChipSong& song) {
    detail::chiptune_play(song);
}

void stop_chiptune() {
    detail::chiptune_stop();
}

bool chiptune_active() {
    return detail::chiptune_is_active();
}

const char* chiptune_preset_name(ChipTunePreset preset) noexcept {
    switch (preset) {
        case ChipTunePreset::PsyArcadeBoot:
            return "psyarcade_boot";
        case ChipTunePreset::PsyArcadeBootMysterious:
            return "psyarcade_boot_mysterious";
        case ChipTunePreset::PsyArcadeBootUplift:
            return "psyarcade_boot_uplift";
    }
    return "psyarcade_boot";
}

ChipTunePreset chiptune_preset_from_name(std::string_view name, ChipTunePreset fallback) noexcept {
    if (equal_ascii_ci(name, "psyarcade_boot") || equal_ascii_ci(name, "boot") ||
        equal_ascii_ci(name, "default"))
        return ChipTunePreset::PsyArcadeBoot;
    if (equal_ascii_ci(name, "psyarcade_boot_mysterious") || equal_ascii_ci(name, "mysterious") ||
        equal_ascii_ci(name, "mystery"))
        return ChipTunePreset::PsyArcadeBootMysterious;
    if (equal_ascii_ci(name, "psyarcade_boot_uplift") || equal_ascii_ci(name, "uplift") ||
        equal_ascii_ci(name, "arcade"))
        return ChipTunePreset::PsyArcadeBootUplift;
    return fallback;
}

const ChipSong& chiptune_preset_song(ChipTunePreset preset) {
    return make_song(preset);
}

const ChipSong& boot_chime_song() {
    return chiptune_preset_song(ChipTunePreset::PsyArcadeBoot);
}

}  // namespace psynder::audio
