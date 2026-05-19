// SPDX-License-Identifier: MIT
// Psynder — sliding-window reliability layer.
//
// The send side keeps a ring of in-flight reliable frames keyed by seq. When
// it sees an `ack_base + ack_bits` mask from the peer, it walks the ring and
// retires every acknowledged entry, leaving the rest queued for retransmit.
//
// The recv side keeps a "have-I-seen-this-seq?" bitmap, used both to dedupe
// replayed frames and to build the next outbound ack reply.
//
// Wave-A shipped a 32-bit selective-ACK bitmap. Wave-B (DESIGN.md §10.4)
// makes the window depth configurable: 32, 64, or 128 bits. The wider
// settings buy more in-flight pipelining and better tolerance of bursty
// loss patterns at the cost of more state per peer. Default stays 32 so
// the on-wire `FrameHeader.ack_bits` (a single u32) keeps its meaning
// unchanged when the Host runs in the default profile.
//
// The send-ring is sized to the maximum supported width (128) so all three
// modes share one storage layout; only the operational window depth
// (`window_bits_`) changes between modes.
//
// This file is INTERNAL to lane 14.

#pragma once

#include "Frame.h"
#include "core/Types.h"

#include <array>
#include <span>
#include <vector>

namespace psynder::net {

// Selective-ACK bitmap width. Selectable at Host startup; defaults to 32
// for ABI parity with Wave-A on-wire frames.
enum class WindowSize : u8 {
    Bits32  = 32,
    Bits64  = 64,
    Bits128 = 128,
};

// Maximum supported bitmap width. The send-ring is always sized to this so
// switching modes is just a runtime depth setting.
inline constexpr usize kMaxWindowBits = 128;

// Send-ring slot count. We size to 2× the widest selective-ACK window so a
// sender can keep one full window in flight while the next is being
// acknowledged. This matches the Wave-A ratio (64-slot ring vs. 32-bit
// bitmap).
inline constexpr usize kSendRingSlots = kMaxWindowBits * 2;   // 256

// Wave-A back-compat alias. Old call sites that read `kSendWindow` see the
// default-profile cap (32 selective-ACK bits ⇒ 64-slot send ring).
inline constexpr usize kSendWindow = 64;
inline constexpr usize kRecvWindow = 64;   // de-dupe history (Wave-A const).

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
    // Default ctor: 32-bit window (Wave-A back-compat).
    Reliability() noexcept;
    explicit Reliability(WindowSize sz) noexcept;

    // Inflight cap at construction time. Returns the operational window
    // depth this instance was built for.
    u32 window_bits() const noexcept { return window_bits_; }

    // Reserve `seq` as the next outbound reliable frame. Returns 0 if the
    // window is full (caller must back off). Otherwise returns the assigned
    // sequence number (1-based; 0 reserved as sentinel for "none").
    //
    // `payload` is copied so the caller can free / overwrite immediately.
    u32 enqueue(std::span<const u8> payload, u32 logical_tick) noexcept;

    // Apply an incoming ack mask. Walks the ring; entries whose seq is
    // present in the mask are marked acked and freed. Returns the number of
    // entries newly retired.
    //
    // 32-bit form: Wave-A wire layout (FrameHeader.ack_bits is one u32).
    u32 apply_ack(u32 ack_base, u32 ack_bits) noexcept;

    // Wide form: up to 128 bits of selective ACK. `bits_lo..bits_hi3`
    // represent the prior 128 sequences (bit i in bits_lo == ack_base-1-i,
    // bit i in bits_hi at i+32, etc.). Bits above this instance's
    // `window_bits()` are ignored, so callers in 32-bit mode get the
    // narrow-form behaviour even if they pass extra words.
    u32 apply_ack_wide(u32 ack_base,
                       u32 bits_lo, u32 bits_hi, u32 bits_hi2, u32 bits_hi3) noexcept;

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
    std::array<SendEntry, kSendRingSlots> ring_{};
    u32 next_seq_       = 1;          // 0 reserved; first outbound seq is 1.
    u32 oldest_unacked_ = 1;          // earliest sequence still potentially live.
    u32 inflight_       = 0;
    u32 window_bits_    = 32;         // operational selective-ACK width.
};

// Receiver — keeps a bitmap history of recent seqs for dedup + ack-bitmap
// generation. Internally always tracks 128 bits; only the lowest
// `window_bits()` are emitted to the wire / consumed from the wire.
class AckTracker {
public:
    AckTracker() noexcept = default;
    explicit AckTracker(WindowSize sz) noexcept
        : window_bits_(static_cast<u32>(sz)) {}

    u32 window_bits() const noexcept { return window_bits_; }

    // Record receipt of `seq`. Returns false if `seq` is a duplicate (the
    // caller should drop the payload but still pass through an ack reply).
    bool observe(u32 seq) noexcept;

    // Build the outbound ack pair (base + bitmask of the prior 32 seqs).
    // This is the Wave-A wire form: a single u32 worth of selective ACK.
    void snapshot(u32& out_ack_base, u32& out_ack_bits) const noexcept;

    // Wide snapshot — up to 128 bits of selective-ACK history. The upper
    // words above `window_bits()` are returned as zero. Callers that want
    // the narrow form should use `snapshot()`.
    void snapshot_wide(u32& out_ack_base,
                       u32& out_bits_lo,
                       u32& out_bits_hi,
                       u32& out_bits_hi2,
                       u32& out_bits_hi3) const noexcept;

    // For tests.
    u32 highest_seq() const noexcept { return highest_; }

private:
    u32 highest_     = 0;
    // 128-bit bitmap. bits_[0] bit i set ⇒ (highest_ - 1 - i) was observed.
    // bits_[1] continues from bit 64, etc.
    u64 bits_[2]     = {0, 0};
    u32 window_bits_ = 32;
};

}  // namespace psynder::net
