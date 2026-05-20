// SPDX-License-Identifier: MIT
// Psynder — sliding-window reliability impl. Lane 14 internal.
//
// Wave-B (DESIGN.md §10.4) extends the original 32-bit selective-ACK to a
// runtime-configurable 32 / 64 / 128-bit mode. Internally the receiver
// keeps a 128-bit history; the operational `window_bits_` controls how
// many of those bits are emitted on the wire and consumed from incoming
// ACK replies. Default is 32, identical to Wave-A.

#include "Reliability.h"

#include <array>
#include <vector>

namespace psynder::net {

namespace {

// Wrap-aware "is `seq` strictly between (low, high]" — we treat seq as a
// modular counter and assume the ring never spans more than 2^31 sequences.
PSY_FORCEINLINE bool seq_greater(u32 a, u32 b) noexcept {
    return i32(a - b) > 0;
}

// Retire one entry by seq if it's present and unacked. Returns 1 on retire.
PSY_FORCEINLINE u32 try_retire(std::array<SendEntry, kSendRingSlots>& ring, u32 seq, u32& inflight) noexcept {
    u32 slot = seq % kSendRingSlots;
    SendEntry& e = ring[slot];
    if (e.in_use && e.seq == seq && !e.acked) {
        e.acked = true;
        e.in_use = false;
        e.payload.clear();
        e.payload.shrink_to_fit();
        --inflight;
        return 1;
    }
    return 0;
}

}  // namespace

// ─── Reliability (send side) ──────────────────────────────────────────────

Reliability::Reliability() noexcept : Reliability(WindowSize::Bits32) {}

Reliability::Reliability(WindowSize sz) noexcept : window_bits_(static_cast<u32>(sz)) {}

u32 Reliability::enqueue(std::span<const u8> payload, u32 logical_tick) noexcept {
    // Operational inflight cap = 2× the selective-ACK width (matches the
    // Wave-A 64-slot ring ↔ 32-bit ack ratio).
    const u32 cap = window_bits_ * 2;
    if (inflight_ >= cap) {
        return 0;  // window full
    }
    u32 seq = next_seq_++;
    u32 slot = seq % kSendRingSlots;
    SendEntry& e = ring_[slot];
    e.seq = seq;
    e.in_use = true;
    e.acked = false;
    e.tick_sent = logical_tick;
    e.retransmits = 0;
    e.payload.assign(payload.begin(), payload.end());
    ++inflight_;
    return seq;
}

u32 Reliability::apply_ack(u32 ack_base, u32 ack_bits) noexcept {
    return apply_ack_wide(ack_base, ack_bits, 0, 0, 0);
}

u32 Reliability::apply_ack_wide(u32 ack_base, u32 bits_lo, u32 bits_hi, u32 bits_hi2, u32 bits_hi3) noexcept {
    if (ack_base == 0)
        return 0;
    u32 retired = 0;

    // Mark slot for ack_base itself.
    retired += try_retire(ring_, ack_base, inflight_);

    // Pack the selective-ACK bits into one 128-bit value and walk it.
    // Bit i (0-indexed) corresponds to seq = ack_base - 1 - i.
    const u32 words[4] = {bits_lo, bits_hi, bits_hi2, bits_hi3};
    // Mask off any bits beyond this instance's operational window so a
    // 32-bit Host can't accidentally consume 64/128-bit extension words.
    const u32 max_i = window_bits_;

    for (u32 i = 0; i < max_i; ++i) {
        const u32 word = i >> 5;  // i / 32
        const u32 shift = i & 31u;
        if ((words[word] & (1u << shift)) == 0)
            continue;
        if (i + 1 > ack_base)
            continue;  // 0 reserved
        const u32 s = ack_base - 1 - i;
        if (s == 0)
            continue;
        retired += try_retire(ring_, s, inflight_);
    }

    // Advance oldest_unacked_ past anything that just retired.
    while (oldest_unacked_ < next_seq_) {
        u32 slot = oldest_unacked_ % kSendRingSlots;
        if (ring_[slot].in_use && ring_[slot].seq == oldest_unacked_)
            break;
        ++oldest_unacked_;
    }
    return retired;
}

void Reliability::mark_transmitted(u32 seq, u32 logical_tick) noexcept {
    u32 slot = seq % kSendRingSlots;
    SendEntry& e = ring_[slot];
    if (e.in_use && e.seq == seq) {
        e.tick_sent = logical_tick;
        ++e.retransmits;
    }
}

u32 Reliability::inflight_count() const noexcept {
    return inflight_;
}

u32 Reliability::collect_retransmits(u32 now_tick, u32 rto_ticks, std::vector<u32>& out_indices) noexcept {
    out_indices.clear();
    // Walk from oldest_unacked_ up to next_seq_ - 1; entries older than
    // rto_ticks ticks are candidates.
    u32 count = 0;
    for (u32 s = oldest_unacked_; seq_greater(next_seq_, s); ++s) {
        u32 slot = s % kSendRingSlots;
        const SendEntry& e = ring_[slot];
        if (!e.in_use || e.seq != s || e.acked)
            continue;
        if (now_tick - e.tick_sent >= rto_ticks) {
            out_indices.push_back(slot);
            ++count;
        }
    }
    return count;
}

const SendEntry& Reliability::entry(u32 idx) const noexcept {
    return ring_[idx];
}
SendEntry& Reliability::entry(u32 idx) noexcept {
    return ring_[idx];
}

// ─── AckTracker (recv side) ───────────────────────────────────────────────

bool AckTracker::observe(u32 seq) noexcept {
    if (seq == 0)
        return false;  // 0 is the sentinel; never valid.
    if (seq == highest_)
        return false;  // duplicate of newest.
    if (seq_greater(seq, highest_)) {
        // New peak. Shift the 128-bit bitmap left by `delta`, then set the
        // bit representing the previous `highest_`.
        u32 delta = seq - highest_;
        if (delta >= 128) {
            bits_[0] = 0;
            bits_[1] = 0;
        } else if (delta >= 64) {
            // Shift across the word boundary; bits in lo > 0 fall off.
            const u32 sh = delta - 64;
            bits_[1] = (sh == 0) ? bits_[0] : (bits_[0] << sh);
            bits_[0] = 0;
        } else {
            // delta in [1, 63]
            if (delta == 0) {
                // unreachable (handled above), kept for clarity.
            } else {
                // Pull the bits that will cross from lo into hi.
                const u64 carry = (delta == 64) ? bits_[0] : (bits_[0] >> (64 - delta));
                bits_[1] = (bits_[1] << delta) | carry;
                bits_[0] = bits_[0] << delta;
            }
        }
        // Bit 0 (in bits_[0]) represents (new highest_ - 1) == old highest_.
        if (highest_ != 0) {
            const u32 i = delta - 1;
            if (i < 64) {
                bits_[0] |= (u64(1) << i);
            } else {
                bits_[1] |= (u64(1) << (i - 64));
            }
        }
        highest_ = seq;
        return true;
    }
    // seq < highest_: check the bitmap.
    u32 diff = highest_ - seq;
    if (diff > 128) {
        // Below the 128-bit tracking window. We can't dedupe via the
        // bitmap; the caller's higher-layer in-order delivery cursor
        // (HostImpl::handle_reliable_in_) will reject true duplicates,
        // and accepting "new but ancient" lets retransmitted
        // hole-fillers reach the OOO buffer when the gap is far behind
        // the current peak.
        return true;
    }
    const u32 i = diff - 1;
    u64 bit;
    if (i < 64) {
        bit = u64(1) << i;
        if (bits_[0] & bit)
            return false;
        bits_[0] |= bit;
    } else {
        bit = u64(1) << (i - 64);
        if (bits_[1] & bit)
            return false;
        bits_[1] |= bit;
    }
    return true;
}

void AckTracker::snapshot(u32& out_ack_base, u32& out_ack_bits) const noexcept {
    out_ack_base = highest_;
    // Wave-A wire form: low 32 bits only.
    out_ack_bits = u32(bits_[0] & 0xFFFFFFFFu);
}

void AckTracker::snapshot_wide(u32& out_ack_base,
                               u32& out_bits_lo,
                               u32& out_bits_hi,
                               u32& out_bits_hi2,
                               u32& out_bits_hi3) const noexcept {
    out_ack_base = highest_;
    // Emit only as many bits as our operational window covers; words above
    // the active width are zero, so a 32-bit receiver fed our snapshot
    // gets the same wire-form ACK either way.
    out_bits_lo = (window_bits_ >= 32) ? u32(bits_[0] & 0xFFFFFFFFu) : 0u;
    out_bits_hi = (window_bits_ >= 64) ? u32((bits_[0] >> 32) & 0xFFFFFFFFu) : 0u;
    out_bits_hi2 = (window_bits_ >= 96) ? u32(bits_[1] & 0xFFFFFFFFu) : 0u;
    out_bits_hi3 = (window_bits_ >= 128) ? u32((bits_[1] >> 32) & 0xFFFFFFFFu) : 0u;
}

}  // namespace psynder::net
