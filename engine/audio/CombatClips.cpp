// SPDX-License-Identifier: MIT
// Psynder -- procedural combat clip synthesis (gunshot / impact / death).
// See CombatClips.h. Everything here is a pure function of its inputs: no RNG
// keyed on the clock, no global state, no heap. The single entropy source is
// the per-sound LCG `seed`, so a clip reproduces bit-for-bit run to run.

#include "audio/CombatClips.h"

#include <algorithm>
#include <cmath>

namespace psynder::audio {

namespace {

// 32-bit linear congruential generator (Numerical Recipes constants). A noisy
// crack/thud needs broadband content; this gives deterministic white noise in
// [-1, 1] with a state we thread by reference so the whole clip shares a tail.
struct Lcg {
    u32 state;
    f32 next() noexcept {
        state = state * 1664525u + 1013904223u;
        // Map the top 16 bits to [-1, 1] (stable across platforms -- pure int).
        const u32 top = state >> 16;
        return static_cast<f32>(top) / 32767.5f - 1.0f;
    }
};

f32 clampf(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// One stateful clip generator. Both synthesize_combat_clip() and
// combat_clip_energy() drive this so there is exactly ONE sample-producing
// path -- no risk of the energy estimate diverging from the rendered PCM.
// `desc` must already be sanitized; `total` is the full clip length.
struct ClipVoice {
    f32 sr;
    f32 attack_samples;
    f32 decay_samples;
    bool descending;
    f32 tone_hz;
    f32 noise_mix;
    f32 peak;
    u32 total;
    Lcg rng;
    f32 phase = 0.0f;
    u32 i = 0u;

    static constexpr f32 kTwoPi = 6.28318530718f;

    explicit ClipVoice(const CombatClipDesc& d, u32 total_samples) noexcept
        : sr(static_cast<f32>(d.sample_rate)),
          attack_samples(std::max(1.0f, d.attack_s * static_cast<f32>(d.sample_rate))),
          decay_samples(std::max(1.0f, d.decay_s * static_cast<f32>(d.sample_rate))),
          descending(d.duration_s > 0.30f),  // long clips sweep their tone down.
          tone_hz(d.tone_hz),
          noise_mix(d.noise_mix),
          peak(d.peak),
          total(total_samples),
          rng{d.seed == 0u ? 1u : d.seed} {}

    // Produce the next sample in [-peak, peak]. Advances the generator.
    f32 next() noexcept {
        const f32 t = static_cast<f32>(i);
        // Attack (linear in) * decay (exponential out): smooth, never negative,
        // tail well below the noise floor by clip end.
        f32 env = 1.0f;
        if (t < attack_samples)
            env = t / attack_samples;
        env *= std::exp(-t / decay_samples);
        // Body oscillator (sine). Death sweeps the frequency down to ~40%.
        f32 freq = tone_hz;
        if (descending) {
            const f32 frac = (total > 1u) ? t / static_cast<f32>(total - 1u) : 0.0f;
            freq = tone_hz * (1.0f - 0.6f * frac);
        }
        phase += kTwoPi * freq / sr;
        if (phase > kTwoPi)
            phase -= kTwoPi;
        const f32 tone = std::sin(phase);
        const f32 noise = rng.next();
        const f32 mixed = (1.0f - noise_mix) * tone + noise_mix * noise;
        ++i;
        return clampf(peak * env * mixed, -1.0f, 1.0f);
    }
};

// Sanitize a descriptor into safe ranges so a bad host value can never produce
// NaN / a zero-length clip. Pure.
CombatClipDesc sanitize(const CombatClipDesc& in) noexcept {
    CombatClipDesc d = in;
    if (d.sample_rate == 0u)
        d.sample_rate = 48000u;
    if (!(d.duration_s > 0.0f) || !std::isfinite(d.duration_s))
        d.duration_s = 0.05f;
    d.duration_s = clampf(d.duration_s, 1.0f / static_cast<f32>(d.sample_rate), 4.0f);
    if (!std::isfinite(d.tone_hz) || d.tone_hz < 0.0f)
        d.tone_hz = 0.0f;
    d.noise_mix = clampf(std::isfinite(d.noise_mix) ? d.noise_mix : 0.0f, 0.0f, 1.0f);
    d.attack_s = clampf(std::isfinite(d.attack_s) ? d.attack_s : 0.0f, 0.0f, d.duration_s);
    if (!(d.decay_s > 0.0f) || !std::isfinite(d.decay_s))
        d.decay_s = d.duration_s * 0.5f;
    d.peak = clampf(std::isfinite(d.peak) ? d.peak : 0.0f, 0.0f, 1.0f);
    return d;
}

}  // namespace

CombatClipDesc combat_clip_desc(CombatSound sound, u32 sample_rate) noexcept {
    CombatClipDesc d{};
    d.sample_rate = (sample_rate == 0u) ? 48000u : sample_rate;
    switch (sound) {
        case CombatSound::Gunshot:
            // Loud, short, noise-dominated crack with a hint of low body.
            d.duration_s = 0.16f;
            d.tone_hz = 140.0f;
            d.noise_mix = 0.92f;
            d.attack_s = 0.0006f;
            d.decay_s = 0.07f;
            d.peak = 0.95f;
            d.seed = 0x9E3779B9u;
            break;
        case CombatSound::Impact:
            // Dull, even shorter thud -- more body, less crack.
            d.duration_s = 0.10f;
            d.tone_hz = 95.0f;
            d.noise_mix = 0.55f;
            d.attack_s = 0.0008f;
            d.decay_s = 0.045f;
            d.peak = 0.7f;
            d.seed = 0x85EBCA6Bu;
            break;
        case CombatSound::Death:
        case CombatSound::Count:
        default:
            // A longer, tonal, descending groan: mostly tone, slow decay.
            d.duration_s = 0.55f;
            d.tone_hz = 180.0f;
            d.noise_mix = 0.30f;
            d.attack_s = 0.004f;
            d.decay_s = 0.40f;
            d.peak = 0.8f;
            d.seed = 0xC2B2AE35u;
            break;
    }
    return d;
}

u32 combat_clip_sample_count(const CombatClipDesc& desc) noexcept {
    const CombatClipDesc d = sanitize(desc);
    const f32 n = d.duration_s * static_cast<f32>(d.sample_rate);
    const u32 count = static_cast<u32>(n);
    return count == 0u ? 1u : count;
}

u32 synthesize_combat_clip(const CombatClipDesc& desc, std::span<f32> out) noexcept {
    const CombatClipDesc d = sanitize(desc);
    const u32 total = combat_clip_sample_count(d);
    const u32 n = static_cast<u32>(std::min<usize>(out.size(), static_cast<usize>(total)));
    ClipVoice voice{d, total};
    for (u32 i = 0; i < n; ++i)
        out[i] = voice.next();
    return n;
}

f32 combat_clip_energy(const CombatClipDesc& desc) noexcept {
    const CombatClipDesc d = sanitize(desc);
    const u32 n = combat_clip_sample_count(d);
    if (n == 0u)
        return 0.0f;
    // O(n), alloc-free single pass over the SAME ClipVoice the renderer uses, so
    // the energy figure can never drift from the produced PCM.
    ClipVoice voice{d, n};
    f64 sum_sq = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const f32 s = voice.next();
        sum_sq += static_cast<f64>(s) * static_cast<f64>(s);
    }
    return static_cast<f32>(std::sqrt(sum_sq / static_cast<f64>(n)));
}

CombatClipTable register_combat_clips() noexcept {
    CombatClipTable t{};
    // Stable, deterministic identity tokens: ordinal + 1 (0 stays the invalid
    // handle). The mixer ignores clip sample data today, so these only need to
    // be distinct, non-zero, and reproducible.
    for (u32 i = 0; i < static_cast<u32>(CombatSound::Count); ++i)
        t.ids[i] = ClipId{i + 1u};
    return t;
}

}  // namespace psynder::audio
