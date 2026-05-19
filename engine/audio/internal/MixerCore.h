// SPDX-License-Identifier: MIT
// Psynder — audio mixer core (header-only algorithmic kernels).
//
// This header contains the testable algorithmic core of the lane-12 audio
// subsystem: voice pool, SIMD voice-sum, HRTF azimuth model (geometric ITD +
// minimal HRIR fade-in), and a Cooley-Tukey FFT. It is deliberately header-
// only so the shared tests/unit CMakeLists (lane 25, not owned by this lane)
// can compile against the algorithms without linking `psynder_audio`.
//
// DESIGN.md §10.2:
//   - 32-channel software CPU mixer.
//   - SIMD-merged voice sum (engine/simd, lane 03).
//   - Per-voice mixing dispatched via the job system parallel_for (lane 04).
//   - HRTF (vendored minimal HRIR set) for FPS.
//   - FDN reverb (outdoors), FFT convolution (indoors).
//
// Wave-A rule (docs/wave-a-bar.md): no `virtual` in the mixer hot path.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "simd/Simd.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>

namespace psynder::audio::detail {

// ─── Constants ───────────────────────────────────────────────────────────
inline constexpr u32 kMaxVoices       = 32;          // DESIGN.md §10.2
inline constexpr u32 kHrirLength      = 64;          // minimal HRIR taps / ear
inline constexpr u32 kHrirAzimuthBins = 16;          // 22.5° resolution
inline constexpr u32 kFdnSize         = 4;           // 4-line FDN
inline constexpr f32 kSpeedOfSound    = 343.0f;      // m/s @ 20 °C
inline constexpr f32 kHeadRadius      = 0.0875f;     // m (avg adult head)

// ─── Voice ──────────────────────────────────────────────────────────────
// One voice is one playing instance of a clip. The voice pool is a tagged-
// handle array with generation counters so a stale VoiceId compares unequal
// after a stop_voice() + play() reuse.
struct Voice {
    u32         gen      = 0;   // monotonic
    bool        active   = false;
    u32         clip_raw = 0;
    f32         volume   = 0.0f;
    math::Vec3  position{0,0,0};
    // playback cursor (frame index into clip's PCM stream)
    u32         cursor   = 0;
};

// Voice pool — fixed-size, contiguous, no allocations on acquire/release.
// Returns -1 when full so callers fail gracefully.
class VoicePool {
public:
    // Find a free slot; mark active; bump generation; return packed id.
    // Packed id layout: low 24b = slot index, high 8b = generation low byte.
    // 0 is reserved for "invalid".
    u32 acquire(u32 clip_raw, math::Vec3 pos, f32 volume) noexcept {
        for (u32 i = 0; i < kMaxVoices; ++i) {
            if (!voices_[i].active) {
                voices_[i].active   = true;
                voices_[i].clip_raw = clip_raw;
                voices_[i].position = pos;
                voices_[i].volume   = volume;
                voices_[i].cursor   = 0;
                // bump gen; never zero (so packed id != 0)
                voices_[i].gen = (voices_[i].gen + 1u) & 0xFFu;
                if (voices_[i].gen == 0u) voices_[i].gen = 1u;
                ++active_count_;
                return pack(i, voices_[i].gen);
            }
        }
        return 0u;  // full
    }

    // Release a previously-acquired voice. Mismatched gen is a no-op (stale).
    bool release(u32 packed) noexcept {
        if (packed == 0u) return false;
        const u32 idx = unpack_index(packed);
        const u32 gen = unpack_gen(packed);
        if (idx >= kMaxVoices) return false;
        if (!voices_[idx].active) return false;
        if (voices_[idx].gen != gen) return false;
        voices_[idx].active = false;
        if (active_count_ > 0u) --active_count_;
        return true;
    }

    void clear() noexcept {
        for (auto& v : voices_) v.active = false;
        active_count_ = 0;
    }

    u32  active_count() const noexcept { return active_count_; }

    Voice&       at(u32 i)       noexcept { return voices_[i]; }
    const Voice& at(u32 i) const noexcept { return voices_[i]; }

    static constexpr u32 pack(u32 idx, u32 gen) noexcept {
        return (idx & 0x00FFFFFFu) | ((gen & 0xFFu) << 24);
    }
    static constexpr u32 unpack_index(u32 packed) noexcept { return packed & 0x00FFFFFFu; }
    static constexpr u32 unpack_gen(u32 packed)   noexcept { return (packed >> 24) & 0xFFu; }

private:
    std::array<Voice, kMaxVoices> voices_{};
    u32 active_count_ = 0;
};

// ─── SIMD voice-sum ──────────────────────────────────────────────────────
// Restrict-style aliasing macro (clang accepts the attribute everywhere we ship).
#if defined(__clang__) || defined(__GNUC__)
#   define PSY_RESTRICT_ALIAS __restrict
#else
#   define PSY_RESTRICT_ALIAS
#endif

// Accumulates `src[i] * gain` into `dst[i]` over `count` floats.
// PSY_FORCEINLINE so this can be called from a parallel_for body without
// indirection — no `virtual`, no function-pointer barrier (Wave-A rule).
PSY_FORCEINLINE void simd_mix_into(f32* PSY_RESTRICT_ALIAS dst,
                                   const f32* PSY_RESTRICT_ALIAS src,
                                   f32 gain, u32 count) noexcept {
    // 4-wide SIMD bulk + scalar tail. simd::fma calls into lane 03's f32x4
    // back-end (NEON on Apple Silicon, SSE/AVX on x86, scalar fallback).
    const simd::f32x4 g = simd::broadcast(gain);
    u32 i = 0;
    for (; i + 4u <= count; i += 4u) {
        simd::f32x4 d = simd::load(dst + i);
        simd::f32x4 s = simd::load(src + i);
        d = simd::fma(s, g, d);
        simd::store(dst + i, d);
    }
    for (; i < count; ++i) dst[i] += src[i] * gain;
}

// Equal-power pan gains. azimuth in radians; 0 = front, +π/2 = right.
// Result clipped to [0,1] for left and right.
PSY_FORCEINLINE void pan_equal_power(f32 azimuth_rad, f32& left_gain, f32& right_gain) noexcept {
    // Map azimuth ∈ [-π/2, +π/2] → x ∈ [0,1] where 0=full left, 1=full right.
    const f32 az = std::fmax(-math::kHalfPi, std::fmin(math::kHalfPi, azimuth_rad));
    const f32 x  = 0.5f * (az / math::kHalfPi) + 0.5f;
    const f32 a  = x * math::kHalfPi;
    left_gain  = std::cos(a);
    right_gain = std::sin(a);
}

// ─── HRTF — minimal vendored "IRC"-style azimuth-only HRIR set ──────────
//
// A real IRC HRIR is per-(azimuth × elevation) measured impulse pair. For the
// Wave-A bar we vendor an analytical *minimal* set: per-azimuth bins of a
// short stereo impulse with a geometric ITD shift plus a mild head-shadow
// low-pass on the contralateral ear. This is enough to (a) demonstrate the
// hot-path mixing structure and (b) hit the deliverable's "HRTF azimuth
// produces expected ITD" test.
//
// Bigger / real HRIRs slot in by replacing `make_minimal_hrir()` with a
// vendored binary table (still inside this lane), keeping the Hrtf class
// stable.

struct StereoHrir {
    std::array<f32, kHrirLength> left{};
    std::array<f32, kHrirLength> right{};
    u32 left_delay_samples  = 0;
    u32 right_delay_samples = 0;
};

// Compute the ITD (inter-aural time difference) in samples for a given
// azimuth and sample rate. Woodworth's formula:
//   ITD ≈ r/c * (sin θ + θ)   for |θ| ≤ π/2
//   ITD ≈ r/c * (π - θ + sin θ) for π/2 < θ ≤ π
// where r is the head radius and c is the speed of sound.
inline f32 itd_seconds(f32 azimuth_rad) noexcept {
    const f32 a   = std::fabs(azimuth_rad);
    const f32 sin_a = std::sin(a);
    f32 t = 0.0f;
    if (a <= math::kHalfPi) {
        t = (kHeadRadius / kSpeedOfSound) * (sin_a + a);
    } else {
        const f32 a_clamped = std::fmin(a, math::kPi);
        t = (kHeadRadius / kSpeedOfSound) * (math::kPi - a_clamped + sin_a);
    }
    return azimuth_rad >= 0.0f ? +t : -t;
}

// Convert ITD in seconds to (left_delay, right_delay) integer-sample offsets
// such that both are non-negative (we delay whichever ear arrives later).
inline void itd_to_delays(f32 azimuth_rad, u32 sample_rate,
                          u32& left_delay, u32& right_delay) noexcept {
    const f32 t = itd_seconds(azimuth_rad);
    const f32 samples = std::fabs(t) * static_cast<f32>(sample_rate);
    const u32 d = static_cast<u32>(samples + 0.5f);
    if (t >= 0.0f) {
        // sound on the right → arrives at right first → left ear is delayed
        left_delay  = d;
        right_delay = 0u;
    } else {
        left_delay  = 0u;
        right_delay = d;
    }
}

// Synthesise a minimal HRIR for the given azimuth (radians). The contralateral
// ear gets a 1-pole low-pass to model head shadow; both ears get a unit-
// energy short impulse so that two summed channels equal the same on-axis
// level as a plain mono play at azimuth = 0.
inline StereoHrir make_minimal_hrir(f32 azimuth_rad, u32 sample_rate) noexcept {
    StereoHrir h{};
    itd_to_delays(azimuth_rad, sample_rate, h.left_delay_samples, h.right_delay_samples);

    // base impulse: 8-tap raised cosine (compact, no aliasing).
    constexpr u32 kBaseTaps = 8;
    std::array<f32, kBaseTaps> base{};
    f32 sum = 0.0f;
    for (u32 i = 0; i < kBaseTaps; ++i) {
        base[i] = 0.5f * (1.0f - std::cos(2.0f * math::kPi *
                          (static_cast<f32>(i) + 0.5f) /
                          static_cast<f32>(kBaseTaps)));
        sum += base[i];
    }
    if (sum > 0.0f) for (auto& b : base) b /= sum;

    // pan gains via equal-power pan (azimuth interpreted L↔R only).
    f32 lg, rg;
    pan_equal_power(azimuth_rad, lg, rg);

    // build left ear (1-pole LP on contralateral side when source is right)
    const bool source_on_right = azimuth_rad > 0.0f;
    const f32 a_shadow         = 0.4f;          // 1-pole feedforward coeff
    f32 lp_l = 0.0f, lp_r = 0.0f;
    for (u32 i = 0; i < kHrirLength; ++i) {
        const f32 b = (i < kBaseTaps) ? base[i] : 0.0f;
        // ipsilateral ears get the raw impulse; contralateral gets LP-shaped
        f32 l_in = b * lg;
        f32 r_in = b * rg;
        if (source_on_right) {
            // left ear contralateral
            lp_l = a_shadow * l_in + (1.0f - a_shadow) * lp_l;
            h.left[i]  = lp_l;
            h.right[i] = r_in;
        } else {
            // right ear contralateral
            lp_r = a_shadow * r_in + (1.0f - a_shadow) * lp_r;
            h.right[i] = lp_r;
            h.left[i]  = l_in;
        }
    }
    return h;
}

// ─── Cooley-Tukey FFT (radix-2, in-place) ───────────────────────────────
//
// Used by the indoor reverb (FFT convolution against a short room impulse).
// Real-only signals are zero-imag on input; output is complex; bit-reversal
// done in-place before butterflies. We expose `fft` and `ifft` taking
// f32 arrays of length 2*N (interleaved re,im).

inline u32 bit_reverse(u32 x, u32 log2n) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < log2n; ++i) {
        n = (n << 1) | (x & 1u);
        x >>= 1;
    }
    return n;
}

// `data` is interleaved real+imag pairs of length 2*N where N = (1<<log2n).
// Sign: -1 for forward, +1 for inverse (we scale by 1/N inside ifft()).
inline void fft_radix2(f32* data, u32 log2n, int sign) noexcept {
    const u32 n = 1u << log2n;
    // bit-reversal permutation
    for (u32 i = 0; i < n; ++i) {
        const u32 j = bit_reverse(i, log2n);
        if (j > i) {
            std::swap(data[2*i],   data[2*j]);
            std::swap(data[2*i+1], data[2*j+1]);
        }
    }
    // butterflies
    for (u32 size = 2; size <= n; size <<= 1) {
        const u32 half = size >> 1;
        const f32 theta = static_cast<f32>(sign) * (2.0f * math::kPi / static_cast<f32>(size));
        const f32 wr_step = std::cos(theta);
        const f32 wi_step = std::sin(theta);
        for (u32 start = 0; start < n; start += size) {
            f32 wr = 1.0f, wi = 0.0f;
            for (u32 k = 0; k < half; ++k) {
                const u32 a = start + k;
                const u32 b = a + half;
                const f32 t_re = wr * data[2*b]   - wi * data[2*b+1];
                const f32 t_im = wr * data[2*b+1] + wi * data[2*b];
                data[2*b]   = data[2*a]   - t_re;
                data[2*b+1] = data[2*a+1] - t_im;
                data[2*a]   = data[2*a]   + t_re;
                data[2*a+1] = data[2*a+1] + t_im;
                const f32 nwr = wr * wr_step - wi * wi_step;
                const f32 nwi = wr * wi_step + wi * wr_step;
                wr = nwr; wi = nwi;
            }
        }
    }
}

inline void fft (f32* data, u32 log2n) noexcept { fft_radix2(data, log2n, -1); }
inline void ifft(f32* data, u32 log2n) noexcept {
    fft_radix2(data, log2n, +1);
    const f32 inv_n = 1.0f / static_cast<f32>(1u << log2n);
    const u32 n2    = (1u << log2n) * 2u;
    for (u32 i = 0; i < n2; ++i) data[i] *= inv_n;
}

// ─── FDN reverb (outdoors) ──────────────────────────────────────────────
//
// A 4-line feedback delay network: 4 mutually-feeding delay lines tapped at
// mutually-prime lengths, mixed through a Hadamard-style 4×4 unitary matrix
// and fed back. Stable, cheap, ~5 multiplies/sample/channel. Suitable for
// outdoors where impulse responses are short but diffuse.

class FdnReverb {
public:
    void reset(u32 sample_rate, f32 decay_seconds) noexcept {
        sr_  = sample_rate;
        // mutually-prime delay-line lengths (in samples) — empirically pleasant
        // for an SR of 48k; scale with SR.
        const f32 base[kFdnSize] = { 0.0297f, 0.0371f, 0.0411f, 0.0437f };  // seconds
        for (u32 i = 0; i < kFdnSize; ++i) {
            const u32 len = static_cast<u32>(base[i] * static_cast<f32>(sample_rate));
            line_len_[i]  = (len < 1u) ? 1u : len;
            for (auto& x : lines_[i]) x = 0.0f;
            head_[i] = 0;
        }
        // feedback gain such that 60-dB decay reaches at decay_seconds
        // g = 10^(-3 * mean_delay_s / decay_s)
        const f32 mean_s  = 0.5f * (base[0] + base[3]);
        const f32 decay_s = decay_seconds < 0.05f ? 0.05f : decay_seconds;
        const f32 db60    = -3.0f * mean_s / decay_s;
        gain_             = std::pow(10.0f, db60);
        (void)sr_;
    }

    // Process one mono sample, returning the wet-only reverb output.
    PSY_FORCEINLINE f32 tick(f32 in) noexcept {
        // read taps
        f32 r[kFdnSize];
        for (u32 i = 0; i < kFdnSize; ++i) {
            const u32 idx = head_[i];
            r[i] = lines_[i][idx];
        }
        // Hadamard 4×4 / 2 → unitary mixing matrix (each output = sum of pairs)
        const f32 m0 =  0.5f * ( r[0] + r[1] + r[2] + r[3]);
        const f32 m1 =  0.5f * ( r[0] - r[1] + r[2] - r[3]);
        const f32 m2 =  0.5f * ( r[0] + r[1] - r[2] - r[3]);
        const f32 m3 =  0.5f * ( r[0] - r[1] - r[2] + r[3]);

        // write back with input injection + decay gain
        for (u32 i = 0; i < kFdnSize; ++i) {
            const f32 m = (i == 0) ? m0 : (i == 1) ? m1 : (i == 2) ? m2 : m3;
            const u32 idx = head_[i];
            lines_[i][idx] = in + gain_ * m;
            head_[i] = (idx + 1u) % line_len_[i];
        }
        return 0.25f * (m0 + m1 + m2 + m3);
    }

private:
    static constexpr u32 kMaxLine = 4096;  // ~85 ms @ 48k — enough for outdoors
    u32                              sr_ = 48000;
    f32                              gain_ = 0.0f;
    std::array<u32, kFdnSize>        line_len_{};
    std::array<u32, kFdnSize>        head_{};
    std::array<std::array<f32, kMaxLine>, kFdnSize> lines_{};
};

// ─── FFT-convolution reverb (indoors) ───────────────────────────────────
//
// Tiny convolution against a fixed N-tap room impulse response. We zero-pad
// both signal and IR to the next power of two, FFT, multiply spectra,
// inverse FFT. For Wave A the impulse is generated synthetically (a short
// noise burst exponentially decayed) so the lane has no external assets.

class FftConvReverb {
public:
    void reset(u32 sample_rate, f32 ir_seconds, f32 decay_seconds) noexcept {
        // build a short impulse response: exponentially decayed noise.
        u32 n = static_cast<u32>(ir_seconds * static_cast<f32>(sample_rate));
        if (n == 0) n = 1;
        if (n > kMaxIr) n = kMaxIr;
        ir_len_ = n;
        u32 lcg = 0xC0FFEEu;
        const f32 inv_max = 1.0f / static_cast<f32>(0x7FFFFFFF);
        const f32 decay_s = decay_seconds < 0.05f ? 0.05f : decay_seconds;
        for (u32 i = 0; i < n; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            const i32 s = static_cast<i32>(lcg) >> 1;       // [0, INT32_MAX]
            const f32 r = (static_cast<f32>(s) * inv_max) * 2.0f - 1.0f;
            const f32 t = static_cast<f32>(i) / static_cast<f32>(sample_rate);
            ir_[i] = r * std::exp(-3.0f * t / decay_s);
        }
    }

    // Convolve `in` (length N samples, must be ≤ kMaxBlock) with the room IR
    // via FFT and add the wet signal into `out`. Returns the output length.
    u32 process_block(const f32* in, u32 n, f32* out) noexcept {
        const u32 conv_len = n + ir_len_ - 1u;
        u32 log2n = 1;
        while ((1u << log2n) < conv_len) ++log2n;
        const u32 fft_n = 1u << log2n;
        if (fft_n * 2u > kScratch) return 0u;

        // zero-pad signal
        for (u32 i = 0; i < fft_n; ++i) {
            sig_[2*i]   = (i < n) ? in[i] : 0.0f;
            sig_[2*i+1] = 0.0f;
        }
        // zero-pad IR
        for (u32 i = 0; i < fft_n; ++i) {
            kern_[2*i]   = (i < ir_len_) ? ir_[i] : 0.0f;
            kern_[2*i+1] = 0.0f;
        }
        fft(sig_.data(),  log2n);
        fft(kern_.data(), log2n);
        // spectral multiply
        for (u32 i = 0; i < fft_n; ++i) {
            const f32 ar = sig_[2*i],  ai = sig_[2*i+1];
            const f32 br = kern_[2*i], bi = kern_[2*i+1];
            sig_[2*i]   = ar * br - ai * bi;
            sig_[2*i+1] = ar * bi + ai * br;
        }
        ifft(sig_.data(), log2n);
        for (u32 i = 0; i < conv_len; ++i) out[i] += sig_[2*i];
        return conv_len;
    }

    u32 ir_length() const noexcept { return ir_len_; }

private:
    static constexpr u32 kMaxIr    = 1024;
    static constexpr u32 kMaxBlock = 1024;
    static constexpr u32 kScratch  = 4096;  // 2 * (kMaxIr + kMaxBlock) interleaved re/im
    std::array<f32, kMaxIr>    ir_{};
    u32                        ir_len_ = 0;
    std::array<f32, kScratch>  sig_{};
    std::array<f32, kScratch>  kern_{};
};

// ─── Voice-mix kernel for one job ───────────────────────────────────────
//
// Called by the engine's parallel_for: one job per active voice. The voice
// emits a constant-amplitude tone at its assigned frequency (placeholder for
// a real clip stream — the real clip reader lands in Wave B). The result is
// HRTF-spatialised stereo, mixed into `dst_stereo`.
//
// `freq_hz` lets tests drive the function with reproducible signal energy
// for the "no-clip" invariant.
PSY_FORCEINLINE void render_voice_into_stereo(
        const Voice& v, f32 freq_hz,
        u32 sample_rate, u32 frames,
        f32* PSY_RESTRICT_ALIAS dst_stereo) noexcept {
    if (!v.active) return;
    // Compute azimuth from voice position assuming listener at origin facing +Z.
    // azimuth = atan2(x, z) ∈ [-π, π]; treat large |azimuth| as side/rear.
    const f32 az = std::atan2(v.position.x, std::fmax(0.0001f, v.position.z));
    f32 lg, rg;
    pan_equal_power(az, lg, rg);
    lg *= v.volume;
    rg *= v.volume;
    const f32 omega = 2.0f * math::kPi * freq_hz / static_cast<f32>(sample_rate);
    for (u32 i = 0; i < frames; ++i) {
        const f32 s = std::sin(omega * static_cast<f32>(v.cursor + i));
        dst_stereo[2*i + 0] += s * lg;
        dst_stereo[2*i + 1] += s * rg;
    }
}

}  // namespace psynder::audio::detail
