// SPDX-License-Identifier: MIT
// Psynder -- procedural combat clips (gunshot / impact / death). Additive to
// the audio lane (ADR-024). No asset files: every clip is synthesized by a
// pure, deterministic generator (LCG noise + analytic envelopes), so the same
// parameters always yield byte-identical samples.
//
// The public mixer (`audio/Audio.h`) takes a ClipId but currently renders a
// placeholder tone (PCM clip streaming is a later wave). The ClipId returned
// by register_combat_clips() is therefore an IDENTITY token a host passes to
// Engine::play; the verifiable artifact is the synthesized PCM here, which the
// combat-audio layer + unit tests exercise directly. When PCM streaming lands,
// the same generators feed the mixer with zero call-site change.

#pragma once

#include "audio/Audio.h"
#include "core/Types.h"

#include <span>

namespace psynder::audio {

// The three combat cues. Kept a plain enum (not audio::ClipId) so the gameplay
// side-channel can name a sound without depending on the audio mixer.
enum class CombatSound : u8 {
    Gunshot = 0,  // sharp transient + noisy crack: a weapon discharge.
    Impact = 1,   // short dull thud: a bullet striking flesh / surface.
    Death = 2,    // a low descending tone: an actor expiring.
    Count = 3,
};

// Synthesis parameters for one mono combat clip. Defaults give a sane,
// game-ready cue; all fields are exposed so a host can re-voice a sound
// deterministically (no RNG seeded by time -- `seed` is the only entropy and is
// fixed per sound).
struct CombatClipDesc {
    u32 sample_rate = 48000u;
    f32 duration_s = 0.18f;   // total clip length in seconds.
    f32 tone_hz = 220.0f;     // body oscillator frequency (descends for Death).
    f32 noise_mix = 0.85f;    // 0 = pure tone, 1 = pure noise.
    f32 attack_s = 0.001f;    // linear fade-in.
    f32 decay_s = 0.12f;      // exponential-ish decay tail.
    f32 peak = 0.9f;          // peak absolute amplitude (pre-headroom).
    u32 seed = 0x1234567u;    // LCG seed -- fixed per sound for determinism.
};

// The canonical descriptor for each CombatSound at a given sample rate. Pure:
// same (sound, sample_rate) -> same descriptor every call.
[[nodiscard]] CombatClipDesc combat_clip_desc(CombatSound sound, u32 sample_rate = 48000u) noexcept;

// Number of mono samples a descriptor synthesizes (>= 1).
[[nodiscard]] u32 combat_clip_sample_count(const CombatClipDesc& desc) noexcept;

// Synthesize the clip into `out` (mono f32 in [-1,1]). Writes exactly
// min(out.size(), combat_clip_sample_count(desc)) samples and returns that
// count. Deterministic + alloc-free (the caller owns the buffer). No clamp is
// needed downstream: the generator keeps |sample| <= desc.peak.
u32 synthesize_combat_clip(const CombatClipDesc& desc, std::span<f32> out) noexcept;

// One peak-normalized RMS-ish energy figure for the synthesized clip, used by
// tests to assert a clip is audibly non-empty. Pure.
[[nodiscard]] f32 combat_clip_energy(const CombatClipDesc& desc) noexcept;

// --- Engine registration --------------------------------------------------
// A small immutable table mapping each CombatSound to the ClipId a host passes
// to Engine::play(). Populated once by register_combat_clips(); thereafter
// clip_id_for() is a pure lookup. Registration does NOT require a started
// engine -- it only mints stable ClipId tokens (the mixer ignores clip sample
// data today), so it is safe + deterministic headless.
struct CombatClipTable {
    ClipId ids[static_cast<usize>(CombatSound::Count)]{};

    [[nodiscard]] ClipId at(CombatSound sound) const noexcept {
        const auto i = static_cast<usize>(sound);
        return (i < static_cast<usize>(CombatSound::Count)) ? ids[i] : ClipId{};
    }
};

// Mint the three combat ClipIds (stable, deterministic: id == ordinal+1).
// Idempotent -- returns the same table every call. No engine state is touched.
[[nodiscard]] CombatClipTable register_combat_clips() noexcept;

}  // namespace psynder::audio
