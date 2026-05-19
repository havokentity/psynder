// SPDX-License-Identifier: MIT
// Psynder — sliding-window reliability layer.
//
// The send side keeps a ring of in-flight reliable frames keyed by seq. When
// it sees an `ack_base + ack_bits` mask from the peer, it walks the ring and
// retires every acknowledged entry, leaving the rest queued for retransmit.
//
// The recv side keeps a 64-deep "have-I-seen-this-seq?" bitmap, used both to
// dedupe replayed frames and to build the next outbound `ack_base/ack_bits`.
//
// The window width (kSendWindow) is the number of frames a sender can have
// in flight before it must stop and wait for an ack. 64 was chosen to match
// the 32-bit ack-bitfield in FrameHeader plus its `ack_base` (33 frames of
// addressable history) with comfortable headroom for the sender's own ring.
//
// This file is INTERNAL to lane 14.

#pragma once

#include "Frame.h"
#include "core/Types.h"

#include <array>
#include <span>
#include <vector>

namespace psynder::net {

inline constexpr usize kSendWindow = 64;   // in-flight reliable frames
inline constexpr usize kRecvWindow = 64;   // de-dupe history

// Sender — keeps a fixed ring of in-flight reliable payloads keyed by seq.
struct SendEntry {
    u32              seq           = 0;
    bool             in_use        = false;
    bool             acked         = false;
    u32              tick_sent     = 0;   // for RTO; logical ticks
    u32              retransmits   = 0;
    std::vector<u8>  payload;             // header NOT included
};

class Reliability {
public:
    Reliability() noexcept;

    // Reserve `seq` as the next outbound reliable frame. Returns 0 if the
    // window is full (caller must back off). Otherwise returns the assigned
    // sequence number (1-based; 0 reserved as sentinel for "none").
    //
    // `payload` is copied so the caller can free / overwrite immediately.
    u32 enqueue(std::span<const u8> payload, u32 logical_tick) noexcept;

    // Apply an incoming ack mask. Walks the ring; entries whose seq is
    // present in the mask are marked acked and freed. Returns the number of
    // entries newly retired.
    u32 apply_ack(u32 ack_base, u32 ack_bits) noexcept;

    // Mark `seq` as just transmitted (for RTO bookkeeping).
    void mark_transmitted(u32 seq, u32 logical_tick) noexcept;

    // Returns the count of frames currently inflight (sent, not yet acked).
    u32 inflight_count() const noexcept;

    // Returns the count of frames awaiting retransmit at `now_tick` given
    // `rto_ticks` retransmission timeout.
    //
    // Out-parameter: indices into the internal ring where the entries live.
    u32 collect_retransmits(u32 now_tick, u32 rto_ticks,
                            std::vector<u32>& out_indices) noexcept;

    // Read access to a ring entry by index from collect_retransmits().
    const SendEntry& entry(u32 idx) const noexcept;
    SendEntry&       entry(u32 idx) noexcept;

    // Has any payload been queued at all?
    u32 next_seq() const noexcept { return next_seq_; }

private:
    std::array<SendEntry, kSendWindow> ring_;
    u32 next_seq_ = 1;          // 0 reserved; first outbound seq is 1.
    u32 oldest_unacked_ = 1;    // earliest sequence still potentially live.
    u32 inflight_ = 0;
};

// Receiver — keeps a 64-bit history of recent seqs for dedup + ack-bitmap
// generation.
class AckTracker {
public:
    AckTracker() noexcept = default;

    // Record receipt of `seq`. Returns false if `seq` is a duplicate (the
    // caller should drop the payload but still pass through an ack reply).
    bool observe(u32 seq) noexcept;

    // Build the outbound ack pair (base + bitmask of the prior 32 seqs).
    void snapshot(u32& out_ack_base, u32& out_ack_bits) const noexcept;

    // For tests.
    u32 highest_seq() const noexcept { return highest_; }

private:
    u32 highest_ = 0;
    u32 bitmask_ = 0;  // bit i set => (highest_ - 1 - i) was observed.
};

}  // namespace psynder::net
