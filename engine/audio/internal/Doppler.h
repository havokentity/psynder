// SPDX-License-Identifier: MIT
// Psynder — Doppler pitch shift for moving sources (lane 12, Wave B).
//
// Relative-velocity-derived per-voice pitch ratio. The classical Doppler
// formula for a stationary listener and moving source is:
//
//   f_obs = f_src * c / (c - v_los)
//
// where v_los is the source velocity component along the listener→source
// direction (positive = approaching). When both listener and source move:
//
//   f_obs = f_src * (c + v_listener_los) / (c + v_source_los)
//
// where v_listener_los is the listener's velocity component along the
// source→listener direction (positive = approaching the source), and
// v_source_los is the source's velocity component along the source→listener
// direction (positive = receding from the listener).
//
// We cap the resulting pitch ratio to [0.5, 1.5]: outside that range the
// audible sample-rate aliasing becomes audible (no anti-imaging filter in
// this very simple implementation), and physically a 50% pitch shift already
// corresponds to a closing speed of about Mach 0.3.
//
// The shift is applied at the voice-rendering stage by walking the playback
// cursor with a fractional increment (linear interpolation between the two
// nearest source samples). For the Wave-A placeholder synth voices that
// just generate a sine wave, the same fractional cursor model lifts cleanly
// into the integer-cursor advance the existing `render_voice_into_stereo`
// uses — we expose a stand-alone helper that the test surface can verify
// without dragging in the rest of the mixer state.
//
// Hot-path discipline: no virtual, no allocs. Pure header-only kernels.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>

namespace psynder::audio::detail {

inline constexpr f32 kDopplerMinRatio = 0.5f;  // never shift below 0.5×
inline constexpr f32 kDopplerMaxRatio = 1.5f;  // never shift above 1.5×

// Compute the Doppler pitch ratio for a source/listener pair. All vectors
// are in world space. Returns 1.0 (no shift) when source and listener are
// coincident or the line-of-sight is degenerate.
//
// Convention: `to_listener` direction is computed inside, so callers pass
// the raw positions, not relative vectors.
inline f32 doppler_ratio(math::Vec3 listener_pos,
                         math::Vec3 listener_vel,
                         math::Vec3 source_pos,
                         math::Vec3 source_vel,
                         f32        speed_of_sound = 343.0f) noexcept {
    const math::Vec3 to_listener_v{
        listener_pos.x - source_pos.x,
        listener_pos.y - source_pos.y,
        listener_pos.z - source_pos.z
    };
    const f32 len_sq = to_listener_v.x * to_listener_v.x +
                       to_listener_v.y * to_listener_v.y +
                       to_listener_v.z * to_listener_v.z;
    if (len_sq < 1e-8f) return 1.0f;
    const f32 inv_len = 1.0f / std::sqrt(len_sq);
    // Unit vector from source toward listener.
    const f32 sx = to_listener_v.x * inv_len;
    const f32 sy = to_listener_v.y * inv_len;
    const f32 sz = to_listener_v.z * inv_len;

    // The unit vector (sx,sy,sz) points from source toward listener.
    //
    //   v_listener_toward_source = -dot(listener_vel, source→listener)
    //     positive when listener moves toward source (closes the gap)
    //   v_source_toward_listener =  dot(source_vel,   source→listener)
    //     positive when source moves toward listener (closes the gap)
    //
    // Classical Doppler:
    //   f_obs = f_src * (c + v_listener_toward_source) /
    //                   (c - v_source_toward_listener)
    // Both "toward" terms shrink the gap → both raise the observed pitch.
    const f32 v_listener_toward_source = -(listener_vel.x * sx +
                                           listener_vel.y * sy +
                                           listener_vel.z * sz);
    const f32 v_source_toward_listener =  (source_vel.x   * sx +
                                           source_vel.y   * sy +
                                           source_vel.z   * sz);

    const f32 num = speed_of_sound + v_listener_toward_source;
    const f32 den = speed_of_sound - v_source_toward_listener;
    // Guard against transonic denominators (source closing at near-Mach speed).
    if (den <= 1.0f) return kDopplerMaxRatio;
    const f32 ratio = num / den;
    return std::clamp(ratio, kDopplerMinRatio, kDopplerMaxRatio);
}

// Apply a fixed pitch ratio to a fractional cursor. Returns the linear-
// interpolated source sample at position `cursor`. The caller advances the
// cursor by `ratio` per output sample.
//
// Templated on the source array so the same helper drives both a placeholder
// sine-table voice and a real clip PCM stream (when wave-B PCM streaming
// lands).
template <typename SampleSource>
PSY_FORCEINLINE f32 doppler_sample(const SampleSource& src,
                                   f64 cursor) noexcept {
    const i64 i0 = static_cast<i64>(std::floor(cursor));
    const f64 frac = cursor - static_cast<f64>(i0);
    const f32 a = src(static_cast<u32>(i0));
    const f32 b = src(static_cast<u32>(i0 + 1));
    return static_cast<f32>(static_cast<f64>(a) * (1.0 - frac) +
                            static_cast<f64>(b) * frac);
}

// Convenience: emit `frames` samples of a sine source at `base_freq` shifted
// by `ratio`, written to a mono output buffer. Used by the unit test to
// verify both the ratio computation and the linear-interp resampler in one
// shot.
inline void doppler_render_sine(f32 base_freq, f32 ratio, u32 sample_rate,
                                u32 frames, f32* out_mono,
                                f64 start_cursor = 0.0) noexcept {
    const f64 phase_step = static_cast<f64>(ratio) *
                           static_cast<f64>(base_freq) /
                           static_cast<f64>(sample_rate);
    f64 cursor = start_cursor;
    const f64 two_pi = static_cast<f64>(math::kTwoPi);
    for (u32 i = 0; i < frames; ++i) {
        out_mono[i] = static_cast<f32>(std::sin(two_pi * cursor));
        cursor += phase_step;
    }
}

}  // namespace psynder::audio::detail
