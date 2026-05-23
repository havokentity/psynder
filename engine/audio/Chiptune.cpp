// SPDX-License-Identifier: MIT
// Psynder — lightweight chip-synth playback for boot jingles and music cues.

#include "Chiptune.h"

#include "internal/ChiptuneSynth.h"

#include <algorithm>
#include <cmath>
#include <mutex>
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
    std::lock_guard<std::mutex> lk(p.mu);
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

const ChipSong& boot_chime_song() {
    static constexpr ChipNote kNotes[]{
        // Sub-bass pedal: slow minor movement, triangle keeps it console-like but cinematic.
        {.start_tick = 0,
         .duration_ticks = 8,
         .midi_note = 36,
         .velocity = 130,
         .channel = 0,
         .pan = -10,
         .wave = ChipWave::Triangle},
        {.start_tick = 8,
         .duration_ticks = 8,
         .midi_note = 43,
         .velocity = 120,
         .channel = 0,
         .pan = 12,
         .wave = ChipWave::Triangle},
        {.start_tick = 16,
         .duration_ticks = 8,
         .midi_note = 39,
         .velocity = 128,
         .channel = 0,
         .pan = -8,
         .wave = ChipWave::Triangle},
        {.start_tick = 24,
         .duration_ticks = 10,
         .midi_note = 46,
         .velocity = 138,
         .channel = 0,
         .pan = 10,
         .wave = ChipWave::Triangle},

        // Low pulse ostinato.
        {.start_tick = 0,
         .duration_ticks = 3,
         .midi_note = 48,
         .velocity = 115,
         .channel = 1,
         .pan = -36,
         .wave = ChipWave::Pulse25},
        {.start_tick = 4,
         .duration_ticks = 3,
         .midi_note = 55,
         .velocity = 105,
         .channel = 1,
         .pan = 36,
         .wave = ChipWave::Pulse25},
        {.start_tick = 8,
         .duration_ticks = 3,
         .midi_note = 51,
         .velocity = 120,
         .channel = 1,
         .pan = -30,
         .wave = ChipWave::Pulse25},
        {.start_tick = 12,
         .duration_ticks = 3,
         .midi_note = 58,
         .velocity = 110,
         .channel = 1,
         .pan = 30,
         .wave = ChipWave::Pulse25},
        {.start_tick = 16,
         .duration_ticks = 3,
         .midi_note = 51,
         .velocity = 122,
         .channel = 1,
         .pan = -26,
         .wave = ChipWave::Pulse25},
        {.start_tick = 20,
         .duration_ticks = 3,
         .midi_note = 55,
         .velocity = 112,
         .channel = 1,
         .pan = 26,
         .wave = ChipWave::Pulse25},
        {.start_tick = 24,
         .duration_ticks = 3,
         .midi_note = 53,
         .velocity = 130,
         .channel = 1,
         .pan = -22,
         .wave = ChipWave::Pulse25},
        {.start_tick = 28,
         .duration_ticks = 5,
         .midi_note = 62,
         .velocity = 140,
         .channel = 1,
         .pan = 18,
         .wave = ChipWave::Pulse50},

        // Suspended glassy lead.
        {.start_tick = 3,
         .duration_ticks = 5,
         .midi_note = 72,
         .velocity = 110,
         .channel = 2,
         .pan = 12,
         .wave = ChipWave::Pulse12},
        {.start_tick = 7,
         .duration_ticks = 4,
         .midi_note = 75,
         .velocity = 118,
         .channel = 2,
         .pan = -16,
         .wave = ChipWave::Pulse12},
        {.start_tick = 11,
         .duration_ticks = 5,
         .midi_note = 79,
         .velocity = 125,
         .channel = 2,
         .pan = 16,
         .wave = ChipWave::Pulse12},
        {.start_tick = 15,
         .duration_ticks = 4,
         .midi_note = 82,
         .velocity = 118,
         .channel = 2,
         .pan = -20,
         .wave = ChipWave::Pulse12},
        {.start_tick = 19,
         .duration_ticks = 5,
         .midi_note = 78,
         .velocity = 120,
         .channel = 2,
         .pan = 22,
         .wave = ChipWave::Pulse12},
        {.start_tick = 23,
         .duration_ticks = 4,
         .midi_note = 87,
         .velocity = 128,
         .channel = 2,
         .pan = -22,
         .wave = ChipWave::Pulse12},
        {.start_tick = 27,
         .duration_ticks = 7,
         .midi_note = 84,
         .velocity = 135,
         .channel = 2,
         .pan = 0,
         .wave = ChipWave::Pulse25},

        // Fast arpeggio shimmer, moving across stereo.
        {.start_tick = 1,
         .duration_ticks = 1,
         .midi_note = 84,
         .velocity = 70,
         .channel = 3,
         .pan = -50,
         .wave = ChipWave::Pulse12},
        {.start_tick = 2,
         .duration_ticks = 1,
         .midi_note = 87,
         .velocity = 72,
         .channel = 3,
         .pan = 50,
         .wave = ChipWave::Pulse12},
        {.start_tick = 5,
         .duration_ticks = 1,
         .midi_note = 91,
         .velocity = 76,
         .channel = 3,
         .pan = -44,
         .wave = ChipWave::Pulse12},
        {.start_tick = 6,
         .duration_ticks = 1,
         .midi_note = 94,
         .velocity = 70,
         .channel = 3,
         .pan = 44,
         .wave = ChipWave::Pulse12},
        {.start_tick = 9,
         .duration_ticks = 1,
         .midi_note = 87,
         .velocity = 76,
         .channel = 3,
         .pan = -38,
         .wave = ChipWave::Pulse12},
        {.start_tick = 10,
         .duration_ticks = 1,
         .midi_note = 91,
         .velocity = 78,
         .channel = 3,
         .pan = 38,
         .wave = ChipWave::Pulse12},
        {.start_tick = 13,
         .duration_ticks = 1,
         .midi_note = 94,
         .velocity = 82,
         .channel = 3,
         .pan = -34,
         .wave = ChipWave::Pulse12},
        {.start_tick = 14,
         .duration_ticks = 1,
         .midi_note = 99,
         .velocity = 76,
         .channel = 3,
         .pan = 34,
         .wave = ChipWave::Pulse12},
        {.start_tick = 17,
         .duration_ticks = 1,
         .midi_note = 82,
         .velocity = 78,
         .channel = 3,
         .pan = -30,
         .wave = ChipWave::Pulse12},
        {.start_tick = 18,
         .duration_ticks = 1,
         .midi_note = 87,
         .velocity = 80,
         .channel = 3,
         .pan = 30,
         .wave = ChipWave::Pulse12},
        {.start_tick = 21,
         .duration_ticks = 1,
         .midi_note = 91,
         .velocity = 84,
         .channel = 3,
         .pan = -26,
         .wave = ChipWave::Pulse12},
        {.start_tick = 22,
         .duration_ticks = 1,
         .midi_note = 94,
         .velocity = 80,
         .channel = 3,
         .pan = 26,
         .wave = ChipWave::Pulse12},

        // Noise sweeps and hits for bootup machinery.
        {.start_tick = 0,
         .duration_ticks = 1,
         .midi_note = 55,
         .velocity = 75,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 2,
         .duration_ticks = 1,
         .midi_note = 70,
         .velocity = 50,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 4,
         .duration_ticks = 1,
         .midi_note = 62,
         .velocity = 82,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 6,
         .duration_ticks = 1,
         .midi_note = 74,
         .velocity = 58,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 8,
         .duration_ticks = 1,
         .midi_note = 55,
         .velocity = 86,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 10,
         .duration_ticks = 1,
         .midi_note = 70,
         .velocity = 52,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 12,
         .duration_ticks = 1,
         .midi_note = 62,
         .velocity = 90,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 14,
         .duration_ticks = 1,
         .midi_note = 82,
         .velocity = 68,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 16,
         .duration_ticks = 2,
         .midi_note = 58,
         .velocity = 76,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 20,
         .duration_ticks = 2,
         .midi_note = 76,
         .velocity = 64,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 24,
         .duration_ticks = 2,
         .midi_note = 62,
         .velocity = 98,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},
        {.start_tick = 28,
         .duration_ticks = 3,
         .midi_note = 90,
         .velocity = 90,
         .channel = 4,
         .pan = 0,
         .wave = ChipWave::Noise},

        // Final resolve, still slightly eerie.
        {.start_tick = 30,
         .duration_ticks = 2,
         .midi_note = 79,
         .velocity = 140,
         .channel = 5,
         .pan = -16,
         .wave = ChipWave::Pulse25},
        {.start_tick = 30,
         .duration_ticks = 2,
         .midi_note = 84,
         .velocity = 125,
         .channel = 5,
         .pan = 16,
         .wave = ChipWave::Pulse12},
        {.start_tick = 32,
         .duration_ticks = 6,
         .midi_note = 72,
         .velocity = 150,
         .channel = 5,
         .pan = 0,
         .wave = ChipWave::Triangle},
    };
    static const ChipSong kSong{
        .bpm = 108u,
        .ticks_per_beat = 4u,
        .master_volume = 0.14f,
        .loop = false,
        .notes = std::span<const ChipNote>{kNotes, sizeof(kNotes) / sizeof(kNotes[0])},
    };
    return kSong;
}

}  // namespace psynder::audio
