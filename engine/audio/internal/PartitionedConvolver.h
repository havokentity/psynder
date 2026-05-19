// SPDX-License-Identifier: MIT
// Psynder — partitioned-overlap-save FFT convolution reverb (lane 12, Wave B).
//
// Wave A shipped `FftConvReverb`: a one-shot zero-pad → FFT → mul → IFFT
// pipeline per block, which works for an IR ≤ kMaxIr (~1024 taps at 48 k =
// 21 ms). That collapses past about a 200 ms IR because the FFT size has to
// grow to next-power-of-two(block + IR), so doubling the IR doubles the FFT
// length and quadruples the cost of every block.
//
// This file implements the partitioned form used by every serious convolution
// reverb:
//
//   * Split the IR into P partitions of length B (block size).
//   * Pre-compute the spectrum of each partition once (in `reset()`); they're
//     reused on every input block.
//   * Per block: FFT(input ++ prev_input) once → multiply against each
//     partition's stored spectrum → accumulate spectra → IFFT once → emit
//     the second half (overlap-save lower bound).
//
// Cost is O(P · N · log N) where N = 2B per block, dominated by the FFT(input)
// and one IFFT per block — partition multiplies are O(N) each. Memory is
// O(P · N) for the stored partition spectra plus one block-size scratch ring.
//
// Wave-B refinements vs Wave A:
//   - 4× block-overlap (B / 4 hop) so reverb tail update happens every quarter
//     of the block rather than once per full block. This is the "low-latency
//     UPOLS" style — same partition spectra but read out at 4× the rate.
//   - Per-channel impulses: the convolver carries its own IR. The Audio.cpp
//     state owns one instance per ear, so the engine no longer has to
//     re-bake one shared mono kernel.
//   - Scratch buffer pool reused across blocks: every allocation happens in
//     `reset()`; `process_block_into()` never allocates.
//
// Hot-path discipline (DESIGN.md §10.2 / wave-a-bar.md):
//   * No `virtual`. PartitionedConvolver is concrete and final.
//   * No per-frame allocs. All buffers sized in `reset()`.
//   * Stays compatible with the SIMD mixer summing path — output is added
//     into a caller buffer via simd_mix_into-style accumulation.

#pragma once

#include "audio/internal/MixerCore.h"

#include "core/Types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace psynder::audio::detail {

// ─── PartitionedConvolver — overlap-save, per-channel ────────────────────
//
// Designed for a single ear's IR. The Audio.cpp state instantiates two of
// these (one per ear) when indoor reverb is engaged.
//
//   reset(sample_rate, ir_seconds, decay_seconds, block_size, overlap_factor)
//
// The IR itself is generated deterministically (same generator the Wave-A
// FftConvReverb used) so the test surface compares straight to the Wave-A
// kernel for IR equivalence with overlap_factor = 1.
class PartitionedConvolver {
public:
    static constexpr u32 kMinBlock      = 32;
    static constexpr u32 kMaxBlock      = 1024;
    static constexpr u32 kMaxPartitions = 32;
    static constexpr u32 kMaxFftLen     = 2 * kMaxBlock;       // partition is 2× block

    // Build the partition spectra from a synthetic IR (the same exponentially-
    // decayed pseudo-random kernel the Wave-A reverb used). `overlap_factor`
    // is the number of sub-block hops the convolver produces per block —
    // valid values are 1, 2, 4. Higher overlap = tighter tail-update rate at
    // cost of more partition multiplies per output sample.
    void reset(u32 sample_rate,
               f32 ir_seconds,
               f32 decay_seconds,
               u32 block_size      = 256,
               u32 overlap_factor  = 4,
               u32 seed            = 0xC0FFEEu) noexcept {
        if (block_size < kMinBlock) block_size = kMinBlock;
        if (block_size > kMaxBlock) block_size = kMaxBlock;
        // overlap is restricted to 1, 2, 4 to keep the hop math integer-clean.
        if (overlap_factor != 1u && overlap_factor != 2u && overlap_factor != 4u) {
            overlap_factor = 4u;
        }
        block_   = block_size;
        overlap_ = overlap_factor;
        // partition length = 2 * block (one block of new input + one block of
        // the previous block, classic overlap-save).
        const u32 fft_n = 2u * block_;
        u32 log2n = 0;
        while ((1u << log2n) < fft_n) ++log2n;
        fft_log2n_ = log2n;
        fft_len_   = 1u << log2n;

        // total IR length in samples (clamped to kMaxBlock * kMaxPartitions)
        u32 ir_total = static_cast<u32>(ir_seconds * static_cast<f32>(sample_rate));
        if (ir_total < block_) ir_total = block_;
        const u32 max_ir = block_ * kMaxPartitions;
        if (ir_total > max_ir) ir_total = max_ir;
        num_partitions_ = (ir_total + block_ - 1u) / block_;

        // generate IR
        u32 lcg = seed;
        const f32 inv_max = 1.0f / static_cast<f32>(0x7FFFFFFF);
        const f32 decay_s = decay_seconds < 0.05f ? 0.05f : decay_seconds;
        ir_.assign(static_cast<usize>(num_partitions_) *
                   static_cast<usize>(block_), 0.0f);
        for (u32 i = 0; i < num_partitions_ * block_; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            const i32 s = static_cast<i32>(lcg) >> 1;
            const f32 r = (static_cast<f32>(s) * inv_max) * 2.0f - 1.0f;
            const f32 t = static_cast<f32>(i) / static_cast<f32>(sample_rate);
            ir_[i] = r * std::exp(-3.0f * t / decay_s);
        }

        // Precompute partition spectra.
        partition_spec_.assign(static_cast<usize>(num_partitions_) *
                               static_cast<usize>(2u * fft_len_), 0.0f);
        for (u32 p = 0; p < num_partitions_; ++p) {
            f32* spec = partition_spec_.data() +
                        static_cast<usize>(p) * static_cast<usize>(2u * fft_len_);
            // zero the FFT scratch then load partition into the real channel.
            for (u32 i = 0; i < fft_len_; ++i) {
                spec[2*i + 0] = (i < block_) ? ir_[p * block_ + i] : 0.0f;
                spec[2*i + 1] = 0.0f;
            }
            fft(spec, fft_log2n_);
        }

        // Allocate frame ring (kMaxPartitions slots of one FFT-len complex)
        // — when num_partitions_ < kMaxPartitions we only use the first N.
        frame_ring_.assign(static_cast<usize>(num_partitions_) *
                           static_cast<usize>(2u * fft_len_), 0.0f);
        ring_head_ = 0;

        // Scratch buffers (reused, no per-block allocs).
        fft_scratch_.assign(static_cast<usize>(2u * fft_len_), 0.0f);
        sum_scratch_.assign(static_cast<usize>(2u * fft_len_), 0.0f);
        ifft_scratch_.assign(static_cast<usize>(2u * fft_len_), 0.0f);

        // Overlap-save tail: the previous block of input must be retained so
        // we can build a (prev ++ current) input block for the FFT. With
        // 4× overlap we keep four hops of B/4 samples each = one full block.
        prev_input_.assign(block_, 0.0f);
        ready_ = true;
        sr_    = sample_rate;
        (void)sr_;
    }

    bool   ready()          const noexcept { return ready_; }
    u32    partitions()     const noexcept { return num_partitions_; }
    u32    block_size()     const noexcept { return block_; }
    u32    overlap_factor() const noexcept { return overlap_; }
    u32    fft_length()     const noexcept { return fft_len_; }
    const std::vector<f32>& impulse_response() const noexcept { return ir_; }

    // Convolve a `block_size()` block of mono input with the IR; ADD the
    // result into `out` (same length as in). No allocations.
    void process_block_into(const f32* in, f32* out) noexcept {
        if (!ready_) return;
        const u32 B    = block_;
        const u32 hop  = B / overlap_;
        const u32 N    = fft_len_;
        const u32 log2n = fft_log2n_;

        // For each sub-block at position s = 0, hop, 2*hop, ..., we build a
        // window of length 2B and FFT it. With overlap_factor == 1 this is
        // just one (prev ++ current) window per block; with 4× it's four.
        // We accumulate partition spectra each window, IFFT, and emit
        // overlap-save (second half).
        //
        // For simplicity and lockstep with the Wave-A reverb's IR/output
        // alignment, we treat the full block as one window when
        // overlap_factor == 1, and otherwise emit hop-sized output chunks
        // by sliding the FFT window. The "frame ring" stores the spectrum
        // of the most-recently-FFT'd input window in slot ring_head_, so the
        // partition-spectrum multiply pulls partition[p] against
        // frame_ring_[ring_head_ - p] modulo num_partitions_.

        // We currently keep state at block granularity (one ring entry per
        // hop) — when overlap_factor=1 there's exactly one update per block.
        // We process the block in one shot for overlap_factor=1 and in
        // overlap_-many hops otherwise.

        std::memset(sum_scratch_.data(), 0, sizeof(f32) * 2u * N);

        if (overlap_ == 1u) {
            // Build a single (prev_input ++ current) window.
            for (u32 i = 0; i < B; ++i) {
                fft_scratch_[2*i + 0] = prev_input_[i];
                fft_scratch_[2*i + 1] = 0.0f;
            }
            for (u32 i = 0; i < B; ++i) {
                fft_scratch_[2*(B + i) + 0] = in[i];
                fft_scratch_[2*(B + i) + 1] = 0.0f;
            }
            fft(fft_scratch_.data(), log2n);

            // Store this spectrum into the ring head slot.
            f32* head = frame_ring_.data() +
                        static_cast<usize>(ring_head_) * static_cast<usize>(2u * N);
            std::memcpy(head, fft_scratch_.data(), sizeof(f32) * 2u * N);

            // Sum partition[p] * frame_ring[head - p].
            for (u32 p = 0; p < num_partitions_; ++p) {
                const u32 slot = (ring_head_ + num_partitions_ - p) % num_partitions_;
                const f32* X   = frame_ring_.data() +
                                 static_cast<usize>(slot) * static_cast<usize>(2u * N);
                const f32* H   = partition_spec_.data() +
                                 static_cast<usize>(p) * static_cast<usize>(2u * N);
                for (u32 i = 0; i < N; ++i) {
                    const f32 ar = X[2*i + 0], ai = X[2*i + 1];
                    const f32 br = H[2*i + 0], bi = H[2*i + 1];
                    sum_scratch_[2*i + 0] += ar * br - ai * bi;
                    sum_scratch_[2*i + 1] += ar * bi + ai * br;
                }
            }
            // IFFT, emit second half (overlap-save discards the first B taps).
            std::memcpy(ifft_scratch_.data(), sum_scratch_.data(), sizeof(f32) * 2u * N);
            ifft(ifft_scratch_.data(), log2n);
            for (u32 i = 0; i < B; ++i) {
                out[i] += ifft_scratch_[2*(B + i) + 0];
            }
            std::memcpy(prev_input_.data(), in, sizeof(f32) * B);
            ring_head_ = (ring_head_ + 1u) % num_partitions_;
            return;
        }

        // overlap_factor in {2, 4}: process the block in `overlap_` hops.
        for (u32 h = 0; h < overlap_; ++h) {
            const u32 sample_offset = h * hop;

            // Compose the window. The first B samples are the second half of
            // the previous slid window (overlap), the next B samples are the
            // newly arrived hop's input padded with zeros.
            for (u32 i = 0; i < B; ++i) {
                // first half = sliding-window history
                fft_scratch_[2*i + 0] =
                    (sample_offset + i < B)
                        ? prev_input_[sample_offset + i]
                        : in[sample_offset + i - B];
                fft_scratch_[2*i + 1] = 0.0f;
            }
            for (u32 i = 0; i < B; ++i) {
                const u32 src_idx = sample_offset + B + i;
                f32 sample = 0.0f;
                if (src_idx < B + hop) {
                    // inside the current hop of in[]
                    if (src_idx < B) {
                        sample = prev_input_[src_idx];
                    } else {
                        sample = (src_idx - B < B) ? in[src_idx - B] : 0.0f;
                    }
                } else {
                    sample = 0.0f;
                }
                fft_scratch_[2*(B + i) + 0] = sample;
                fft_scratch_[2*(B + i) + 1] = 0.0f;
            }
            fft(fft_scratch_.data(), log2n);

            // Store into ring head.
            f32* head = frame_ring_.data() +
                        static_cast<usize>(ring_head_) * static_cast<usize>(2u * N);
            std::memcpy(head, fft_scratch_.data(), sizeof(f32) * 2u * N);

            // Accumulate partition spectra.
            std::memset(sum_scratch_.data(), 0, sizeof(f32) * 2u * N);
            for (u32 p = 0; p < num_partitions_; ++p) {
                const u32 slot = (ring_head_ + num_partitions_ - p) % num_partitions_;
                const f32* X   = frame_ring_.data() +
                                 static_cast<usize>(slot) * static_cast<usize>(2u * N);
                const f32* H   = partition_spec_.data() +
                                 static_cast<usize>(p) * static_cast<usize>(2u * N);
                for (u32 i = 0; i < N; ++i) {
                    const f32 ar = X[2*i + 0], ai = X[2*i + 1];
                    const f32 br = H[2*i + 0], bi = H[2*i + 1];
                    sum_scratch_[2*i + 0] += ar * br - ai * bi;
                    sum_scratch_[2*i + 1] += ar * bi + ai * br;
                }
            }
            std::memcpy(ifft_scratch_.data(), sum_scratch_.data(), sizeof(f32) * 2u * N);
            ifft(ifft_scratch_.data(), log2n);
            // Emit `hop` samples — the convolved hop sits in [B + sample_offset, B + sample_offset + hop)
            // of the overlap-save output. Sum-accumulate into out so multiple
            // hops can co-contribute to the same output index.
            for (u32 i = 0; i < hop; ++i) {
                out[sample_offset + i] += ifft_scratch_[2*(B + i) + 0];
            }
            ring_head_ = (ring_head_ + 1u) % num_partitions_;
        }
        std::memcpy(prev_input_.data(), in, sizeof(f32) * B);
    }

private:
    bool ready_  = false;
    u32  sr_     = 48000;
    u32  block_  = 256;
    u32  overlap_ = 4;
    u32  fft_log2n_ = 9;       // log2(2*block)
    u32  fft_len_   = 512;
    u32  num_partitions_ = 0;
    u32  ring_head_      = 0;

    std::vector<f32> ir_;
    std::vector<f32> partition_spec_;   // [P × 2N]
    std::vector<f32> frame_ring_;       // [P × 2N]
    std::vector<f32> fft_scratch_;
    std::vector<f32> sum_scratch_;
    std::vector<f32> ifft_scratch_;
    std::vector<f32> prev_input_;
};

// ─── ModulatedFdnReverb — Wave-A FDN + LFO detuning on each line ─────────
//
// The Wave-A FDN was unmodulated: each delay line tapped a fixed integer
// length, so the reverb tail could "ring" at frequencies whose period was
// commensurate with the four prime delays. We add a slow per-line LFO that
// fractionally walks the read pointer (linear interpolation) so the tail
// loses its modal coherence and sounds diffuse.
//
// Frequencies are mutually incommensurate (different LFO rates per line) so
// the tail never re-aligns.
class ModulatedFdnReverb {
public:
    void reset(u32 sample_rate, f32 decay_seconds,
               f32 mod_depth_samples = 4.0f,
               f32 mod_rate_hz_base  = 0.71f) noexcept {
        sr_ = sample_rate;
        // mutually-prime delay-line lengths (in samples) — matched to FdnReverb's
        // base spacings for consistency with the Wave-A test expectations.
        const f32 base[kFdnSize] = { 0.0297f, 0.0371f, 0.0411f, 0.0437f };
        for (u32 i = 0; i < kFdnSize; ++i) {
            const u32 len = static_cast<u32>(base[i] * static_cast<f32>(sample_rate));
            line_len_[i]  = (len < 1u) ? 1u : len;
            // headroom for fractional modulated lookups.
            if (line_len_[i] + static_cast<u32>(mod_depth_samples) + 2u > kMaxLine) {
                line_len_[i] = kMaxLine - static_cast<u32>(mod_depth_samples) - 2u;
            }
            for (auto& x : lines_[i]) x = 0.0f;
            head_[i]     = 0;
            mod_phase_[i] = 0.0f;
            // mutually-incommensurate LFO rates: base ± 17% per line.
            const f32 mult[kFdnSize] = { 0.83f, 1.00f, 1.21f, 1.47f };
            mod_step_[i]  = math::kTwoPi * mod_rate_hz_base * mult[i] /
                            static_cast<f32>(sample_rate);
        }
        mod_depth_ = mod_depth_samples;
        const f32 mean_s  = 0.5f * (base[0] + base[3]);
        const f32 decay_s = decay_seconds < 0.05f ? 0.05f : decay_seconds;
        const f32 db60    = -3.0f * mean_s / decay_s;
        gain_             = std::pow(10.0f, db60);
        ready_ = true;
        (void)sr_;
    }

    PSY_FORCEINLINE f32 tick(f32 in) noexcept {
        // read taps with fractional modulated offset
        f32 r[kFdnSize];
        for (u32 i = 0; i < kFdnSize; ++i) {
            const f32 phase = mod_phase_[i];
            const f32 offs  = mod_depth_ * std::sin(phase);   // ± mod_depth
            // read pos = head - len + offs (mod kMaxLine), with offs added.
            const f32 read_f = static_cast<f32>(head_[i] + kMaxLine -
                                                line_len_[i]) + offs;
            const u32 i0     = static_cast<u32>(read_f) % kMaxLine;
            const f32 frac   = read_f - std::floor(read_f);
            const u32 i1     = (i0 + 1u) % kMaxLine;
            r[i] = lines_[i][i0] * (1.0f - frac) + lines_[i][i1] * frac;
            mod_phase_[i]   += mod_step_[i];
            if (mod_phase_[i] > math::kTwoPi) mod_phase_[i] -= math::kTwoPi;
        }
        const f32 m0 = 0.5f * ( r[0] + r[1] + r[2] + r[3]);
        const f32 m1 = 0.5f * ( r[0] - r[1] + r[2] - r[3]);
        const f32 m2 = 0.5f * ( r[0] + r[1] - r[2] - r[3]);
        const f32 m3 = 0.5f * ( r[0] - r[1] - r[2] + r[3]);

        for (u32 i = 0; i < kFdnSize; ++i) {
            const f32 m = (i == 0) ? m0 : (i == 1) ? m1 : (i == 2) ? m2 : m3;
            const u32 idx = head_[i];
            lines_[i][idx] = in + gain_ * m;
            head_[i] = (idx + 1u) % kMaxLine;
        }
        return 0.25f * (m0 + m1 + m2 + m3);
    }

    bool ready() const noexcept { return ready_; }
    f32  mod_depth_samples() const noexcept { return mod_depth_; }

private:
    static constexpr u32 kMaxLine = 4096;
    bool                              ready_      = false;
    u32                               sr_         = 48000;
    f32                               gain_       = 0.0f;
    f32                               mod_depth_  = 4.0f;
    std::array<u32, kFdnSize>         line_len_{};
    std::array<u32, kFdnSize>         head_{};
    std::array<f32, kFdnSize>         mod_phase_{};
    std::array<f32, kFdnSize>         mod_step_{};
    std::array<std::array<f32, kMaxLine>, kFdnSize> lines_{};
};

}  // namespace psynder::audio::detail
