// SPDX-License-Identifier: MIT
// Psynder - server-side input jitter buffer. Lane 14 (Wave C).
//
// ServerInputProcessor::process() applies each input the instant it arrives.
// Over a real link inputs arrive REORDERED and BURSTY (RTT jitter), so an
// immediate apply makes the authoritative motion stutter and the client's
// reconciliation chase a moving target. The standard fix (Source / Overwatch
// GDC "input buffer") is a small per-peer reorder + holdback buffer: arrivals
// are sorted by seq and released at a STEADY fixed-step cadence, one per server
// tick, in strict seq order. A short holdback (a couple of ticks) lets a
// briefly-late input slot into place before its turn instead of being applied
// out of order.
//
// Design (deterministic, fixed-capacity, alloc-free):
//   - A ring of kJitterCap slots indexed by seq % cap. `push` stores any
//     not-yet-released, in-window input (dedup by seq, drops stale/duplicate).
//   - `release(out)` is called once per server fixed step. It releases the
//     input whose seq == the next expected seq IF present. If that head is
//     missing it holds back, UNTIL the buffered lead exceeds the holdback
//     window, at which point it treats the hole as a lost packet and skips
//     forward to the next buffered seq (so a single drop can't wedge the
//     stream forever). The first release waits until `buffered >= holdback`
//     so the cushion is primed.
//   - No RNG, no wall-clock: release order is a pure function of arrival
//     contents + the fixed holdback, so the LoopbackBus test path reproduces
//     exactly.
//
// The buffer feeds ServerInputProcessor::process(): the server pops in-order
// inputs each step and processes them, instead of processing on arrival.

#pragma once

#include "Prediction.h"  // InputCmd
#include "core/Types.h"

#include <array>

namespace psynder::net {

// Reorder window capacity. Sized for the worst-case in-flight burst at 60Hz
// over a high-RTT link without exceeding the input ring; pooled, never grows.
inline constexpr u32 kJitterCap = 64;

// Default holdback in inputs: how much lead we accumulate before draining, and
// the max hole we tolerate before skipping a presumed-lost input. Two ticks is
// the classic small-cushion default.
inline constexpr u32 kJitterHoldback = 2;

class InputJitterBuffer {
   public:
    InputJitterBuffer() noexcept = default;
    explicit InputJitterBuffer(u32 holdback) noexcept
        : holdback_(holdback == 0 ? 1u : holdback) {}

    // Insert an arrival. Returns false for a stale (seq < next expected),
    // duplicate, or out-of-window (too-far-ahead) input - those are dropped.
    // The very first push seeds the expected seq to that input's seq so a
    // session that starts mid-stream still drains in order.
    bool push(const InputCmd& cmd) noexcept {
        if (cmd.seq == 0)
            return false;
        if (!seeded_) {
            next_seq_ = cmd.seq;
            seeded_ = true;
        } else if (!draining_ && cmd.seq < next_seq_) {
            // Head reordering before we have released anything: an earlier seq
            // arrived after a later one. Re-seed the expected head downward so
            // the genuinely-first input is not mistaken for stale. (Once we
            // start draining, a seq below the head IS stale and is dropped.)
            if (cmd.seq + kJitterCap > next_seq_) {  // still inside the window.
                next_seq_ = cmd.seq;
            } else {
                return false;
            }
        }
        if (cmd.seq < next_seq_)
            return false;  // already released / stale.
        if (cmd.seq >= next_seq_ + kJitterCap)
            return false;  // beyond the reorder window: drop (caller too far behind).
        const u32 slot = cmd.seq % kJitterCap;
        if (valid_[slot] && buf_[slot].seq == cmd.seq)
            return false;  // duplicate.
        buf_[slot] = cmd;
        valid_[slot] = true;
        ++buffered_;
        return true;
    }

    // Release the next in-seq input at the steady cadence (call once per server
    // fixed step). Returns true and writes `out` when an input is released;
    // false when the buffer is intentionally holding back (priming the cushion
    // or waiting on a not-yet-late head). Skips a presumed-lost head once the
    // buffered lead exceeds the holdback window.
    bool release(InputCmd& out) noexcept {
        if (!seeded_ || buffered_ == 0)
            return false;
        // Prime the cushion: don't drain until we hold at least `holdback`
        // inputs (or we've already started draining - tracked by draining_).
        if (!draining_ && buffered_ < holdback_)
            return false;
        draining_ = true;

        const u32 head_slot = next_seq_ % kJitterCap;
        if (valid_[head_slot] && buf_[head_slot].seq == next_seq_) {
            out = buf_[head_slot];
            valid_[head_slot] = false;
            --buffered_;
            ++next_seq_;
            return true;
        }
        // Head missing. Hold back unless the lead has grown past the holdback
        // window - then the head is presumed lost and we skip forward to the
        // earliest buffered seq so the stream keeps moving.
        if (buffered_ < holdback_)
            return false;
        // Find the earliest buffered seq > next_seq_ and jump to it.
        u32 scan = next_seq_ + 1;
        const u32 limit = next_seq_ + kJitterCap;
        while (scan < limit) {
            const u32 s = scan % kJitterCap;
            if (valid_[s] && buf_[s].seq == scan) {
                next_seq_ = scan;
                out = buf_[s];
                valid_[s] = false;
                --buffered_;
                ++next_seq_;
                return true;
            }
            ++scan;
        }
        return false;  // nothing releasable (shouldn't happen while buffered>0).
    }

    u32 buffered() const noexcept { return buffered_; }
    u32 next_expected() const noexcept { return next_seq_; }
    bool draining() const noexcept { return draining_; }

    void clear() noexcept {
        valid_.fill(false);
        buffered_ = 0;
        next_seq_ = 0;
        seeded_ = false;
        draining_ = false;
    }

   private:
    std::array<InputCmd, kJitterCap> buf_{};
    std::array<bool, kJitterCap> valid_{};
    u32 buffered_ = 0;
    u32 next_seq_ = 0;  // next seq to release.
    u32 holdback_ = kJitterHoldback;
    bool seeded_ = false;
    bool draining_ = false;
};

}  // namespace psynder::net
