// SPDX-License-Identifier: MIT
// Psynder - connection handshake + lobby / peer-slot table. Lane 14 (Wave C).
//
// The replication + prediction layers assume a peer is already admitted and
// owns a controlled net_id. This module is what gets a peer to that state and
// tears it back down, using the reserved SYN / FIN FrameHeader flags:
//
//   ADMISSION  - a connecting client sends a SYN frame. The server's Lobby
//                allocates a fixed-capacity peer slot, assigns the peer a net
//                player index + a controlled net_id (the entity that peer has
//                authority over), and replies SYN|ACK carrying that net_id +
//                slot. The client adopts the assigned net_id as its predicted /
//                controlled entity. A SYN from an already-admitted peer is
//                idempotent (re-sends the same assignment) so a lost SYN|ACK is
//                recoverable without a new slot.
//
//   DISCONNECT - a leaving client sends a FIN frame (or the server times the
//                peer out). The server despawns the peer's controlled net_id
//                (via the caller's ReplicationServer::mark_despawned hook),
//                frees the slot + index, and clears authority. A FIN for an
//                unknown peer is a no-op.
//
// The slot table is fixed-capacity + pooled: there is no per-admission heap
// allocation. Slots, indices, and controlled net_ids are recycled on FIN.
// Everything here is deterministic (no RNG, no wall-clock) so the LoopbackBus
// test path reproduces exactly.
//
// Wire: handshake frames ride the existing reliable default channel with the
// SYN / FIN flags set; the small handshake payload is hand-serialized
// little-endian behind kHandshakeMagic so a stale / cross-build frame is
// rejected rather than mis-read.

#pragma once

#include "Frame.h"  // FrameFlag SYN / FIN, channels.
#include "core/Types.h"

#include <array>
#include <span>

namespace psynder::net {

// Per-peer connection state. Mirrors the classic TCP-ish handshake subset we
// need: a peer is admitted on SYN and torn down on FIN.
enum class ConnState : u8 {
    Disconnected = 0,  // no slot.
    Connected = 1,     // admitted; owns a slot + controlled net_id.
    Closing = 2,       // FIN seen; slot being released this tick.
};

// Maximum simultaneous admitted peers. Matches HostDesc::max_peers headroom for
// a 16-32 player session; the table is a flat array sized to this.
inline constexpr usize kMaxLobbySlots = 64;

// One admitted peer. `controlled_net_id` is the entity this peer has authority
// over (its avatar); the server applies that peer's inputs to it and the client
// predicts it. `player_index` is the dense 0..N-1 slot used by gameplay /
// scoreboard. `peer_key` is the stable transport key (PeerId.raw).
struct LobbySlot {
    bool occupied = false;
    u32 peer_key = 0;
    u32 player_index = 0;
    u32 controlled_net_id = 0;
    ConnState state = ConnState::Disconnected;
};

// The outcome of admitting a peer (SYN). `accepted` is false when the table is
// full. On success the assigned identity is echoed to the client.
struct AdmitResult {
    bool accepted = false;
    u32 player_index = 0;
    u32 controlled_net_id = 0;
    u32 slot = 0;  // index into the slot table.
    bool is_new = false;  // false == idempotent re-admit of an existing peer.
};

// --- Handshake wire payload --------------------------------------------------
// SYN (client->server) carries the client's protocol nonce so a future build
// mismatch is visible; SYN|ACK (server->client) carries the assigned identity.
// Hand-serialized LE behind a magic. 16 bytes:
//   magic(4) + player_index(4) + controlled_net_id(4) + flags(4)
inline constexpr u32 kHandshakeMagic = 0x50484B31u;  // 'PHK1'
inline constexpr usize kHandshakeBytes = 16;

struct HandshakeMsg {
    u32 player_index = 0;
    u32 controlled_net_id = 0;
    u32 flags = 0;  // bit0: accepted (server->client). reserved otherwise.
};
inline constexpr u32 kHandshakeFlagAccepted = 1u << 0;

bool encode_handshake(const HandshakeMsg& m, std::span<u8> out) noexcept;
bool decode_handshake(std::span<const u8> in, HandshakeMsg& m) noexcept;

// --- Server-side lobby -------------------------------------------------------
class Lobby {
   public:
    // `base_net_id` is the first controlled net_id handed out; subsequent
    // admissions take base+1, base+2, ... A freed net_id is recycled before a
    // fresh one is minted so the id space stays compact.
    explicit Lobby(u32 base_net_id = 1) noexcept : next_net_id_(base_net_id) {}

    // Admit (or idempotently re-admit) the peer keyed by `peer_key` (PeerId.raw)
    // in response to a SYN. Allocates a slot + player index + controlled net_id
    // on first contact; returns the same assignment for an already-admitted
    // peer. `accepted == false` only when the table is full.
    AdmitResult admit(u32 peer_key) noexcept;

    // Tear down the peer in response to a FIN (or a server-side timeout). The
    // peer's controlled_net_id is returned in `out_freed_net_id` so the caller
    // can mark_despawned() it; returns true if a peer was actually released.
    bool release(u32 peer_key, u32& out_freed_net_id) noexcept;

    // Connection state for a peer (Disconnected if unknown).
    ConnState state_of(u32 peer_key) const noexcept;

    // The controlled net_id a peer owns (0 if none / unknown).
    u32 controlled_net_id(u32 peer_key) const noexcept;

    // Find the slot index for a peer, or -1.
    isize slot_of(u32 peer_key) const noexcept;

    // Drive the connection state machine from a received frame's FrameHeader
    // FLAGS (the reserved kFlagSyn / kFlagFin). This is the bridge from the
    // transport to the lobby: a SYN frame admits, a FIN frame releases. The
    // outcome is reported via the out-params so the caller can reply SYN|ACK
    // (admit) or mark_despawned the freed net_id (release).
    //
    //   returns true  -> a state transition happened (admit or release).
    //   `out_admit`   -> populated (accepted/identity) when a SYN was handled.
    //   `out_freed_net_id` -> the controlled net_id to despawn when a FIN was
    //                          handled (0 otherwise).
    //
    // A frame with neither flag set is not a handshake frame: returns false.
    bool on_frame_flags(u32 peer_key, u8 frame_flags, AdmitResult& out_admit,
                        u32& out_freed_net_id) noexcept;

    // Introspection / tests.
    u32 admitted_count() const noexcept { return admitted_; }
    usize capacity() const noexcept { return kMaxLobbySlots; }
    const LobbySlot& slot(usize i) const noexcept { return slots_[i]; }

   private:
    isize find_free_() const noexcept;
    u32 alloc_net_id_() noexcept;
    void free_net_id_(u32 net_id) noexcept;

    std::array<LobbySlot, kMaxLobbySlots> slots_{};
    u32 admitted_ = 0;
    u32 next_player_index_ = 0;
    u32 next_net_id_ = 1;
    // Recycled controlled net_ids (freed on FIN), reused LIFO before minting a
    // fresh one. Fixed-capacity, pooled.
    std::array<u32, kMaxLobbySlots> freed_net_ids_{};
    u32 freed_count_ = 0;
    // Recycled player indices, same scheme.
    std::array<u32, kMaxLobbySlots> freed_indices_{};
    u32 freed_index_count_ = 0;
};

// --- Combined snapshot + input ack (piggyback) -------------------------------
// Wave-B echoed only the snapshot seq back to the server as a bare 4-byte
// payload. Wave-C piggybacks the per-peer highest-processed INPUT seq on the
// SAME ack so reconciliation no longer needs a side channel. Tagged behind a
// magic + versioned so a bare Wave-B ack is rejected (both ends share the
// build). 12 bytes: magic(4) + snapshot_seq(4) + input_seq(4).
inline constexpr u32 kAckMagic = 0x5041434Bu;  // 'PACK'
inline constexpr usize kAckBytes = 12;

struct SnapshotAck {
    u32 snapshot_seq = 0;
    u32 input_seq = 0;  // highest input seq the client has had acked / wants.
};

bool encode_ack(const SnapshotAck& a, std::span<u8> out) noexcept;
bool decode_ack(std::span<const u8> in, SnapshotAck& a) noexcept;

}  // namespace psynder::net
