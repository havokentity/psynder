// SPDX-License-Identifier: MIT
// Psynder — wire frame header for the reliable-UDP transport (Lane 14).
//
// Every datagram begins with a fixed-size FrameHeader. The header carries:
//   - magic + version: cheap sanity gate against accidental cross-protocol
//     traffic on the same UDP port.
//   - cipher_suite: reserved Wave-A. The header slot exists so we can add
//     authenticated encryption later (DESIGN.md §10.4) without breaking the
//     wire format. Value 0 means "plaintext, no MAC" (Wave-A default).
//   - channel: 0 == default, 1 == lockstep cmds, 2 == snapshot, 3 == reserved.
//   - flags: bit-packed (reliable, syn, fin, ack-only, …) — see FrameFlag.
//   - seq:    sender's per-channel monotonically increasing sequence number.
//   - ack_base + ack_bits: piggy-backed selective acknowledgement. `ack_base`
//     is the highest sequence the receiver has fully observed; `ack_bits`
//     is a 32-bit bitfield where bit `i` set means `ack_base - 1 - i` was
//     also received. This is the classic Gaffer-on-Games / Glenn Fiedler
//     "selective ack bitmask" scheme and lets a single reply ACK an entire
//     sliding-window worth of inflight frames.
//
// Header is little-endian on the wire (we're targeting little-endian
// x86-64 + Apple Silicon; the header serializer keeps an explicit pack so
// we can revisit if Wave-B brings big-endian targets).
//
// This header is INTERNAL to lane 14. The public surface is engine/net/Net.h.

#pragma once

#include "core/Types.h"

#include <span>

namespace psynder::net {

inline constexpr u32 kFrameMagic = 0x50534E44u;  // 'PSND'
inline constexpr u8 kFrameVersion = 0x01;

// Channels:
//   0 — default (mixed reliable / unreliable, app-defined).
//   1 — lockstep command stream (always reliable, ordered).
//   2 — snapshot stream (unreliable, latest-wins).
//   3 — reserved.
inline constexpr u8 kChannelDefault = 0;
inline constexpr u8 kChannelLockstep = 1;
inline constexpr u8 kChannelSnapshot = 2;

enum FrameFlag : u8 {
    kFlagReliable = 1u << 0,
    kFlagSyn = 1u << 1,
    kFlagFin = 1u << 2,
    kFlagAckOnly = 1u << 3,   // header carries no payload, just an ack.
    kFlagFragment = 1u << 4,  // payload is a fragment of a larger message.
};

// Wire layout — 24 bytes, packed. We serialize by hand so the on-wire layout
// is independent of host struct padding.
//
//   offset  size  field
//   ──────  ────  ─────────────────────────────────────────────
//      0     4    magic
//      4     1    version
//      5     1    cipher_suite      (reserved; 0 == plaintext)
//      6     1    channel
//      7     1    flags
//      8     4    seq
//     12     4    ack_base
//     16     4    ack_bits
//     20     2    payload_len
//     22     2    crc16             (header+payload, 0 for now; future MAC slot)
//   ──────  ────
//                 24 bytes.
struct FrameHeader {
    u32 magic = kFrameMagic;
    u8 version = kFrameVersion;
    u8 cipher_suite = 0;  // Reserved Wave-A; encryption lands later.
    u8 channel = kChannelDefault;
    u8 flags = 0;
    u32 seq = 0;
    u32 ack_base = 0;
    u32 ack_bits = 0;
    u16 payload_len = 0;
    u16 crc16 = 0;  // Reserved Wave-A; integrity slot.
};

inline constexpr usize kFrameHeaderBytes = 24;

// Serialize the header into out[0..24). Returns true on success (out must
// have room for kFrameHeaderBytes bytes).
bool encode_header(const FrameHeader& h, std::span<u8> out) noexcept;

// Deserialize 24 bytes into `h`. Returns false on magic / version / length
// failure. Does NOT validate semantic fields (channel, flags) — callers do.
bool decode_header(std::span<const u8> in, FrameHeader& h) noexcept;

}  // namespace psynder::net
