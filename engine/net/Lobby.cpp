// SPDX-License-Identifier: MIT
// Psynder - handshake / lobby impl. Lane 14 (Wave C).

#include "Lobby.h"

#include <cstring>

namespace psynder::net {

namespace {

PSY_FORCEINLINE void write_u32_le(u8* p, u32 v) noexcept {
    p[0] = u8(v & 0xFFu);
    p[1] = u8((v >> 8) & 0xFFu);
    p[2] = u8((v >> 16) & 0xFFu);
    p[3] = u8((v >> 24) & 0xFFu);
}
PSY_FORCEINLINE u32 read_u32_le(const u8* p) noexcept {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

}  // namespace

// --- Handshake wire ----------------------------------------------------------
bool encode_handshake(const HandshakeMsg& m, std::span<u8> out) noexcept {
    if (out.size() < kHandshakeBytes)
        return false;
    u8* p = out.data();
    write_u32_le(p + 0, kHandshakeMagic);
    write_u32_le(p + 4, m.player_index);
    write_u32_le(p + 8, m.controlled_net_id);
    write_u32_le(p + 12, m.flags);
    return true;
}

bool decode_handshake(std::span<const u8> in, HandshakeMsg& m) noexcept {
    if (in.size() < kHandshakeBytes)
        return false;
    const u8* p = in.data();
    if (read_u32_le(p + 0) != kHandshakeMagic)
        return false;
    m.player_index = read_u32_le(p + 4);
    m.controlled_net_id = read_u32_le(p + 8);
    m.flags = read_u32_le(p + 12);
    return true;
}

// --- Combined ack wire -------------------------------------------------------
bool encode_ack(const SnapshotAck& a, std::span<u8> out) noexcept {
    if (out.size() < kAckBytes)
        return false;
    u8* p = out.data();
    write_u32_le(p + 0, kAckMagic);
    write_u32_le(p + 4, a.snapshot_seq);
    write_u32_le(p + 8, a.input_seq);
    return true;
}

bool decode_ack(std::span<const u8> in, SnapshotAck& a) noexcept {
    if (in.size() < kAckBytes)
        return false;
    const u8* p = in.data();
    if (read_u32_le(p + 0) != kAckMagic)
        return false;
    a.snapshot_seq = read_u32_le(p + 4);
    a.input_seq = read_u32_le(p + 8);
    return true;
}

// --- Lobby -------------------------------------------------------------------
isize Lobby::find_free_() const noexcept {
    for (usize i = 0; i < kMaxLobbySlots; ++i) {
        if (!slots_[i].occupied)
            return static_cast<isize>(i);
    }
    return -1;
}

u32 Lobby::alloc_net_id_() noexcept {
    if (freed_count_ > 0)
        return freed_net_ids_[--freed_count_];  // recycle LIFO.
    return next_net_id_++;
}

void Lobby::free_net_id_(u32 net_id) noexcept {
    if (net_id == 0)
        return;
    if (freed_count_ < kMaxLobbySlots)
        freed_net_ids_[freed_count_++] = net_id;
}

AdmitResult Lobby::admit(u32 peer_key) noexcept {
    AdmitResult r{};
    // Idempotent: an already-admitted peer keeps its assignment (a re-sent SYN
    // after a lost SYN|ACK must not burn a second slot).
    const isize existing = slot_of(peer_key);
    if (existing >= 0) {
        const LobbySlot& s = slots_[static_cast<usize>(existing)];
        r.accepted = true;
        r.player_index = s.player_index;
        r.controlled_net_id = s.controlled_net_id;
        r.slot = static_cast<u32>(existing);
        r.is_new = false;
        return r;
    }

    const isize free = find_free_();
    if (free < 0)
        return r;  // table full: accepted stays false.

    const usize idx = static_cast<usize>(free);
    LobbySlot& s = slots_[idx];
    s.occupied = true;
    s.peer_key = peer_key;
    // Recycle a freed player index before minting a fresh one (keeps the dense
    // index space compact across churn).
    s.player_index =
        (freed_index_count_ > 0) ? freed_indices_[--freed_index_count_] : next_player_index_++;
    s.controlled_net_id = alloc_net_id_();
    s.state = ConnState::Connected;
    ++admitted_;

    r.accepted = true;
    r.player_index = s.player_index;
    r.controlled_net_id = s.controlled_net_id;
    r.slot = static_cast<u32>(idx);
    r.is_new = true;
    return r;
}

bool Lobby::release(u32 peer_key, u32& out_freed_net_id) noexcept {
    out_freed_net_id = 0;
    const isize si = slot_of(peer_key);
    if (si < 0)
        return false;
    LobbySlot& s = slots_[static_cast<usize>(si)];
    s.state = ConnState::Closing;
    out_freed_net_id = s.controlled_net_id;
    free_net_id_(s.controlled_net_id);
    if (freed_index_count_ < kMaxLobbySlots)
        freed_indices_[freed_index_count_++] = s.player_index;
    // Reset the slot to the disconnected/free state.
    s = LobbySlot{};
    --admitted_;
    return true;
}

ConnState Lobby::state_of(u32 peer_key) const noexcept {
    const isize si = slot_of(peer_key);
    if (si < 0)
        return ConnState::Disconnected;
    return slots_[static_cast<usize>(si)].state;
}

u32 Lobby::controlled_net_id(u32 peer_key) const noexcept {
    const isize si = slot_of(peer_key);
    if (si < 0)
        return 0;
    return slots_[static_cast<usize>(si)].controlled_net_id;
}

isize Lobby::slot_of(u32 peer_key) const noexcept {
    for (usize i = 0; i < kMaxLobbySlots; ++i) {
        if (slots_[i].occupied && slots_[i].peer_key == peer_key)
            return static_cast<isize>(i);
    }
    return -1;
}

bool Lobby::on_frame_flags(u32 peer_key, u8 frame_flags, AdmitResult& out_admit,
                           u32& out_freed_net_id) noexcept {
    out_admit = AdmitResult{};
    out_freed_net_id = 0;
    // FIN takes precedence over SYN if (pathologically) both are set: a closing
    // peer should not be re-admitted by the same frame.
    if (frame_flags & kFlagFin) {
        return release(peer_key, out_freed_net_id);
    }
    if (frame_flags & kFlagSyn) {
        out_admit = admit(peer_key);
        return true;
    }
    return false;  // not a handshake frame.
}

}  // namespace psynder::net
