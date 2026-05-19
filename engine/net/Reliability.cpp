// SPDX-License-Identifier: MIT
// Psynder — sliding-window reliability impl. Lane 14 internal.

#include "Reliability.h"

namespace psynder::net {

namespace {

// Wrap-aware "is `seq` strictly between (low, high]" — we treat seq as a
// modular counter and assume the ring never spans more than 2^31 sequences.
PSY_FORCEINLINE bool seq_greater(u32 a, u32 b) noexcept {
    return i32(a - b) > 0;
}

}  // namespace

// ─── Reliability (send side) ──────────────────────────────────────────────

Reliability::Reliability() noexcept = default;

u32 Reliability::enqueue(std::span<const u8> payload, u32 logical_tick) noexcept {
    if (inflight_ >= kSendWindow) {
        return 0;  // window full
    }
    u32 seq  = next_seq_++;
    u32 slot = seq % kSendWindow;
    SendEntry& e = ring_[slot];
    e.seq         = seq;
    e.in_use      = true;
    e.acked       = false;
    e.tick_sent   = logical_tick;
    e.retransmits = 0;
    e.payload.assign(payload.begin(), payload.end());
    ++inflight_;
    return seq;
}

u32 Reliability::apply_ack(u32 ack_base, u32 ack_bits) noexcept {
    if (ack_base == 0) return 0;
    u32 retired = 0;

    // Mark slot for ack_base itself.
    {
        u32 slot = ack_base % kSendWindow;
        SendEntry& e = ring_[slot];
        if (e.in_use && e.seq == ack_base && !e.acked) {
            e.acked  = true;
            e.in_use = false;
            e.payload.clear();
            e.payload.shrink_to_fit();
            --inflight_;
            ++retired;
        }
    }

    // Walk the 32-bit selective bitmap, lowest bit == ack_base - 1.
    for (u32 i = 0; i < 32; ++i) {
        if ((ack_bits & (1u << i)) == 0) continue;
        u32 s = ack_base - 1 - i;
        if (s == 0) continue;            // 0 reserved
        u32 slot = s % kSendWindow;
        SendEntry& e = ring_[slot];
        if (e.in_use && e.seq == s && !e.acked) {
            e.acked  = true;
            e.in_use = false;
            e.payload.clear();
            e.payload.shrink_to_fit();
            --inflight_;
            ++retired;
        }
    }

    // Advance oldest_unacked_ past anything that just retired.
    while (oldest_unacked_ < next_seq_) {
        u32 slot = oldest_unacked_ % kSendWindow;
        if (ring_[slot].in_use && ring_[slot].seq == oldest_unacked_) break;
        ++oldest_unacked_;
    }
    return retired;
}

void Reliability::mark_transmitted(u32 seq, u32 logical_tick) noexcept {
    u32 slot = seq % kSendWindow;
    SendEntry& e = ring_[slot];
    if (e.in_use && e.seq == seq) {
        e.tick_sent = logical_tick;
        ++e.retransmits;
    }
}

u32 Reliability::inflight_count() const noexcept {
    return inflight_;
}

u32 Reliability::collect_retransmits(u32 now_tick, u32 rto_ticks,
                                     std::vector<u32>& out_indices) noexcept {
    out_indices.clear();
    // Walk from oldest_unacked_ up to next_seq_ - 1; entries older than
    // rto_ticks ticks are candidates.
    u32 count = 0;
    for (u32 s = oldest_unacked_; seq_greater(next_seq_, s); ++s) {
        u32 slot = s % kSendWindow;
        const SendEntry& e = ring_[slot];
        if (!e.in_use || e.seq != s || e.acked) continue;
        if (now_tick - e.tick_sent >= rto_ticks) {
            out_indices.push_back(slot);
            ++count;
        }
    }
    return count;
}

const SendEntry& Reliability::entry(u32 idx) const noexcept { return ring_[idx]; }
SendEntry&       Reliability::entry(u32 idx) noexcept       { return ring_[idx]; }

// ─── AckTracker (recv side) ───────────────────────────────────────────────

bool AckTracker::observe(u32 seq) noexcept {
    if (seq == 0) return false;          // 0 is the sentinel; never valid.
    if (seq == highest_) return false;   // duplicate of newest.
    if (seq_greater(seq, highest_)) {
        // New peak. Shift bitmap left by the delta, then set bit 0 (which
        // represents the previous `highest_`).
        u32 delta = seq - highest_;
        if (delta >= 32) {
            bitmask_ = 0;
        } else {
            bitmask_ = (bitmask_ << delta);
            if (highest_ != 0) bitmask_ |= (1u << (delta - 1));
        }
        highest_ = seq;
        return true;
    }
    // seq < highest_: check the bitmap.
    u32 diff = highest_ - seq;
    if (diff > 32) {
        // Below the 32-bit selective-ACK window. We can't track its
        // observation in the bitmap, but we MUST tell the caller it's
        // "new" if we haven't seen it — the caller's higher-layer dedup
        // (in-order delivery cursor + ooo buffer) will reject true
        // duplicates, and accepting "new but ancient" lets retransmitted
        // hole-fillers reach the OOO buffer when the gap is far behind
        // the current peak.
        return true;
    }
    u32 bit = (1u << (diff - 1));
    if (bitmask_ & bit) return false;    // duplicate inside window.
    bitmask_ |= bit;
    return true;
}

void AckTracker::snapshot(u32& out_ack_base, u32& out_ack_bits) const noexcept {
    out_ack_base = highest_;
    out_ack_bits = bitmask_;
}

}  // namespace psynder::net
