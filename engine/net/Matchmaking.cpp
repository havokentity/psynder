// SPDX-License-Identifier: MIT
// Psynder - dedicated server + matchmaking impl. Lane 14 (Wave 11).

#include "Matchmaking.h"

#include "Frame.h"  // kChannelDefault / kChannelSnapshot.
#include "scene/SceneEcs.h"

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

// --- Matchmaking wire --------------------------------------------------------
bool encode_match_join(const MatchJoinMsg& m, std::span<u8> out) noexcept {
    if (out.size() < kMatchJoinBytes)
        return false;
    u8* p = out.data();
    write_u32_le(p + 0, kMatchJoinMagic);
    write_u32_le(p + 4, m.session_id);
    write_u32_le(p + 8, m.requested_max);
    return true;
}

bool decode_match_join(std::span<const u8> in, MatchJoinMsg& m) noexcept {
    if (in.size() < kMatchJoinBytes)
        return false;
    const u8* p = in.data();
    if (read_u32_le(p + 0) != kMatchJoinMagic)
        return false;
    m.session_id = read_u32_le(p + 4);
    m.requested_max = read_u32_le(p + 8);
    return true;
}

bool encode_match_reply(const MatchReplyMsg& m, std::span<u8> out) noexcept {
    if (out.size() < kMatchReplyBytes)
        return false;
    u8* p = out.data();
    write_u32_le(p + 0, kMatchReplyMagic);
    write_u32_le(p + 4, static_cast<u32>(m.deny));
    write_u32_le(p + 8, m.session_id);
    write_u32_le(p + 12, m.player_index);
    write_u32_le(p + 16, m.controlled_net_id);
    return true;
}

bool decode_match_reply(std::span<const u8> in, MatchReplyMsg& m) noexcept {
    if (in.size() < kMatchReplyBytes)
        return false;
    const u8* p = in.data();
    if (read_u32_le(p + 0) != kMatchReplyMagic)
        return false;
    m.deny = static_cast<MatchDeny>(read_u32_le(p + 4));
    m.session_id = read_u32_le(p + 8);
    m.player_index = read_u32_le(p + 12);
    m.controlled_net_id = read_u32_le(p + 16);
    return true;
}

bool encode_match_leave(const MatchLeaveMsg& m, std::span<u8> out) noexcept {
    if (out.size() < kMatchLeaveBytes)
        return false;
    u8* p = out.data();
    write_u32_le(p + 0, kMatchLeaveMagic);
    write_u32_le(p + 4, m.session_id);
    return true;
}

bool decode_match_leave(std::span<const u8> in, MatchLeaveMsg& m) noexcept {
    if (in.size() < kMatchLeaveBytes)
        return false;
    const u8* p = in.data();
    if (read_u32_le(p + 0) != kMatchLeaveMagic)
        return false;
    m.session_id = read_u32_le(p + 4);
    return true;
}

// --- Matchmaker --------------------------------------------------------------
Matchmaker::Matchmaker(u32 default_max_players, u32 base_net_id,
                       u32 max_sessions) noexcept
    : default_max_players_(default_max_players == 0 ? 8u : default_max_players),
      max_sessions_(max_sessions == 0 ? static_cast<u32>(kMaxSessions)
                    : (max_sessions > static_cast<u32>(kMaxSessions)
                           ? static_cast<u32>(kMaxSessions)
                           : max_sessions)),
      next_session_id_(1),
      next_net_id_block_(base_net_id == 0 ? 1u : base_net_id) {}

isize Matchmaker::find_session_(u32 session_id) const noexcept {
    if (session_id == 0)
        return -1;
    for (usize i = 0; i < kMaxSessions; ++i) {
        if (sessions_[i].active && sessions_[i].id == session_id)
            return static_cast<isize>(i);
    }
    return -1;
}

isize Matchmaker::find_session_with_room_() const noexcept {
    // Deterministic: lowest session id with a free slot under its cap.
    isize best = -1;
    u32 best_id = 0;
    for (usize i = 0; i < kMaxSessions; ++i) {
        const Session& s = sessions_[i];
        if (!s.active)
            continue;
        if (s.lobby.admitted_count() >= s.max_players)
            continue;
        if (best < 0 || s.id < best_id) {
            best = static_cast<isize>(i);
            best_id = s.id;
        }
    }
    return best;
}

isize Matchmaker::find_session_of_peer_(u32 peer_key) const noexcept {
    for (usize i = 0; i < kMaxSessions; ++i) {
        const Session& s = sessions_[i];
        if (s.active && s.lobby.state_of(peer_key) == ConnState::Connected)
            return static_cast<isize>(i);
    }
    return -1;
}

isize Matchmaker::alloc_session_(u32 max_players) noexcept {
    if (active_sessions() >= max_sessions_)
        return -1;  // concurrent-session cap reached.
    for (usize i = 0; i < kMaxSessions; ++i) {
        if (sessions_[i].active)
            continue;
        Session& s = sessions_[i];
        s.active = true;
        s.id = next_session_id_++;
        u32 cap = (max_players == 0) ? default_max_players_ : max_players;
        if (cap > static_cast<u32>(kMaxLobbySlots))
            cap = static_cast<u32>(kMaxLobbySlots);
        s.max_players = cap;
        // Fresh Lobby with a disjoint controlled-net-id block.
        s.lobby = Lobby(next_net_id_block_);
        next_net_id_block_ += kNetIdBlockStride;
        return static_cast<isize>(i);
    }
    return -1;
}

MatchJoinResult Matchmaker::try_join(u32 peer_key, u32 session_id,
                                     u32 requested_max) noexcept {
    MatchJoinResult r{};

    // Already admitted somewhere? Idempotent re-join of the SAME session.
    const isize cur = find_session_of_peer_(peer_key);
    if (cur >= 0) {
        Session& s = sessions_[static_cast<usize>(cur)];
        if (session_id == 0 || session_id == s.id) {
            AdmitResult a = s.lobby.admit(peer_key);  // idempotent.
            r.deny = a.accepted ? MatchDeny::Accepted : MatchDeny::SessionFull;
            r.session_id = s.id;
            r.player_index = a.player_index;
            r.controlled_net_id = a.controlled_net_id;
            r.is_new_session = false;
            r.is_new_peer = a.is_new;
            return r;
        }
        // Asked to move to a different session: leave the current one first.
        u32 freed_sid = 0, freed_net = 0;
        leave(peer_key, freed_sid, freed_net);
    }

    isize si = -1;
    bool created = false;
    if (session_id != 0) {
        // Targeted join: that session must exist + have room.
        si = find_session_(session_id);
        if (si < 0) {
            r.deny = MatchDeny::NoSession;
            return r;
        }
        Session& s = sessions_[static_cast<usize>(si)];
        if (s.lobby.admitted_count() >= s.max_players) {
            r.deny = MatchDeny::SessionFull;
            r.session_id = s.id;
            return r;
        }
    } else {
        // Find-or-create-any.
        si = find_session_with_room_();
        if (si < 0) {
            si = alloc_session_(requested_max);
            if (si < 0) {
                r.deny = MatchDeny::ServerFull;
                return r;
            }
            created = true;
        }
    }

    Session& s = sessions_[static_cast<usize>(si)];
    AdmitResult a = s.lobby.admit(peer_key);
    if (!a.accepted) {
        // Should not happen (we checked room), but report a deny rather than a
        // false accept. Reclaim a just-created empty session.
        if (created && s.lobby.admitted_count() == 0u)
            s = Session{};
        r.deny = MatchDeny::SessionFull;
        r.session_id = s.active ? s.id : 0u;
        return r;
    }
    r.deny = MatchDeny::Accepted;
    r.session_id = s.id;
    r.player_index = a.player_index;
    r.controlled_net_id = a.controlled_net_id;
    r.is_new_session = created;
    r.is_new_peer = a.is_new;
    return r;
}

bool Matchmaker::leave(u32 peer_key, u32& out_session_id,
                       u32& out_freed_net_id) noexcept {
    out_session_id = 0;
    out_freed_net_id = 0;
    const isize si = find_session_of_peer_(peer_key);
    if (si < 0)
        return false;
    Session& s = sessions_[static_cast<usize>(si)];
    u32 freed = 0;
    if (!s.lobby.release(peer_key, freed))
        return false;
    out_session_id = s.id;
    out_freed_net_id = freed;
    // Reclaim an emptied session so its slot hosts a fresh session later.
    if (s.lobby.admitted_count() == 0u)
        s = Session{};
    return true;
}

u32 Matchmaker::session_of(u32 peer_key) const noexcept {
    const isize si = find_session_of_peer_(peer_key);
    return si < 0 ? 0u : sessions_[static_cast<usize>(si)].id;
}

u32 Matchmaker::controlled_net_id(u32 peer_key) const noexcept {
    const isize si = find_session_of_peer_(peer_key);
    return si < 0 ? 0u : sessions_[static_cast<usize>(si)].lobby.controlled_net_id(peer_key);
}

u32 Matchmaker::active_sessions() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < kMaxSessions; ++i)
        if (sessions_[i].active)
            ++n;
    return n;
}

u32 Matchmaker::admitted_in(u32 session_id) const noexcept {
    const isize si = find_session_(session_id);
    return si < 0 ? 0u : sessions_[static_cast<usize>(si)].lobby.admitted_count();
}

u32 Matchmaker::capacity_of(u32 session_id) const noexcept {
    const isize si = find_session_(session_id);
    return si < 0 ? 0u : sessions_[static_cast<usize>(si)].max_players;
}

u32 Matchmaker::total_admitted() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < kMaxSessions; ++i)
        if (sessions_[i].active)
            n += sessions_[i].lobby.admitted_count();
    return n;
}

// --- DedicatedServer ---------------------------------------------------------
DedicatedServer::DedicatedServer(scene::EcsRegistry& reg, scene::SceneGraph& graph,
                                 const ReplicatedComponentSet& set) noexcept
    : reg_(reg), graph_(graph), mm_(8, 100), repl_(set) {
    inbox_.reserve(64);
    snap_scratch_.reserve(2048);
}

DedicatedServer::~DedicatedServer() { stop(); }

bool DedicatedServer::start(const Desc& desc) noexcept {
    if (host_)
        return false;  // already started.
    desc_ = desc;
    mm_ = Matchmaker(desc.default_max_players, desc.base_net_id, desc.max_sessions);

    HostDesc hd{};
    hd.port = desc.port;
    hd.max_peers = desc.max_peers;
    host_ = make_test_host(hd);
    if (!host_)
        return false;
    tick_ = 0;
    running_ = true;
    any_joined_ = false;
    total_joins_ = 0;
    total_rejections_ = 0;
    for (auto& c : clients_)
        c = ClientState{};
    return true;
}

void DedicatedServer::stop() noexcept {
    if (host_) {
        destroy_test_host(host_);
        host_ = nullptr;
    }
    running_ = false;
}

u32 DedicatedServer::connect_to_client(u16 client_port) noexcept {
    if (!host_)
        return 0;
    const PeerId s_to_c = host_->connect(client_port);
    return s_to_c.raw;
}

isize DedicatedServer::find_client_(u32 peer_key) const noexcept {
    for (usize i = 0; i < clients_.size(); ++i)
        if (clients_[i].active && clients_[i].peer_key == peer_key)
            return static_cast<isize>(i);
    return -1;
}

isize DedicatedServer::alloc_client_() noexcept {
    for (usize i = 0; i < clients_.size(); ++i)
        if (!clients_[i].active)
            return static_cast<isize>(i);
    return -1;
}

math::Vec3 DedicatedServer::spawn_for_(u32 player_index) const noexcept {
    // Deterministic spawn ring: spread avatars along X so their AOI spheres are
    // distinct at join. No RNG.
    const f32 x = 8.0f + static_cast<f32>(player_index) * 12.0f;
    return math::Vec3{x, 0.f, 0.f};
}

void DedicatedServer::admit_avatar_(u32 peer_key, const MatchJoinResult& r) noexcept {
    isize ci = find_client_(peer_key);
    if (ci < 0)
        ci = alloc_client_();
    if (ci < 0)
        return;  // client table full (shouldn't happen: sized to kMaxLobbySlots).
    ClientState& cs = clients_[static_cast<usize>(ci)];
    if (cs.active && cs.controlled_net_id == r.controlled_net_id)
        return;  // already spawned (idempotent re-join).

    const math::Vec3 spawn = spawn_for_(r.player_index);
    scene::LocalTransform local{};
    local.translation = spawn;
    const Entity avatar =
        scene::create_scene_entity(reg_, graph_, scene::kInvalidSceneNode, local);
    NetIdComponent tag{};
    tag.net_id = r.controlled_net_id;
    reg_.add<NetIdComponent>(avatar, tag);

    cs.active = true;
    cs.peer_key = peer_key;
    cs.controlled_net_id = r.controlled_net_id;
    cs.avatar = avatar;
    cs.spawn = spawn;
    cs.input = ServerInputProcessor{};
    cs.input.bind(avatar, spawn);
    aoi_.set_peer(PeerId{peer_key}, spawn, desc_.aoi_radius);
}

void DedicatedServer::drop_avatar_(isize ci, u32 freed_net_id) noexcept {
    if (ci < 0)
        return;
    ClientState& cs = clients_[static_cast<usize>(ci)];
    if (cs.avatar.valid())
        reg_.destroy(cs.avatar);
    if (freed_net_id != 0)
        repl_.mark_despawned(freed_net_id);
    cs = ClientState{};
}

u32 DedicatedServer::step() noexcept {
    if (!host_ || !running_)
        return admitted_peers();
    ++tick_;

    // --- 1. Drain client->server messages: JOIN / LEAVE / input ------------
    inbox_.clear();
    host_->poll(inbox_);
    for (const InboundMessage& m : inbox_) {
        if (m.channel != kChannelDefault)
            continue;
        std::span<const u8> b(m.bytes.data(), m.bytes.size());

        // JOIN request.
        if (m.bytes.size() == kMatchJoinBytes) {
            MatchJoinMsg req{};
            if (decode_match_join(b, req)) {
                const MatchJoinResult res =
                    mm_.try_join(m.from.raw, req.session_id, req.requested_max);
                if (res.accepted()) {
                    ++total_joins_;
                    any_joined_ = true;
                    admit_avatar_(m.from.raw, res);
                } else {
                    ++total_rejections_;
                }
                // Reply to the client (accept identity OR deny reason).
                MatchReplyMsg rep{};
                rep.deny = res.deny;
                rep.session_id = res.session_id;
                rep.player_index = res.player_index;
                rep.controlled_net_id = res.controlled_net_id;
                encode_match_reply(rep, std::span<u8>(reply_buf_.data(), reply_buf_.size()));
                host_->send(m.from,
                            std::span<const u8>(reply_buf_.data(), reply_buf_.size()),
                            /*reliable=*/true, kChannelDefault);
            }
            continue;
        }

        // LEAVE request.
        if (m.bytes.size() == kMatchLeaveBytes) {
            MatchLeaveMsg bye{};
            if (decode_match_leave(b, bye)) {
                u32 sid = 0, freed = 0;
                if (mm_.leave(m.from.raw, sid, freed)) {
                    const isize ci = find_client_(m.from.raw);
                    drop_avatar_(ci, freed);
                }
            }
            continue;
        }

        // Input command for an admitted peer's avatar.
        if (m.bytes.size() == kInputCmdBytes) {
            const isize ci = find_client_(m.from.raw);
            if (ci >= 0) {
                InputCmd in{};
                decode_input(b, in);
                ClientState& cs = clients_[static_cast<usize>(ci)];
                cs.input.process(reg_, in);
                if (auto* tc = reg_.get<scene::TransformComponent>(cs.avatar))
                    tc->local.translation = cs.input.authoritative_pos();
            }
            continue;
        }

        // Snapshot-ack on the default channel is not expected; acks ride the
        // snapshot channel (handled below).
    }

    // --- 2. Advance caller-owned world entities ----------------------------
    if (world_step_)
        world_step_(reg_, tick_, world_step_user_);

    // --- 3. Replicate an AOI snapshot to every admitted peer ---------------
    for (auto& cs : clients_) {
        if (!cs.active)
            continue;
        const math::Vec3 apos = cs.input.authoritative_pos();
        aoi_.set_peer(PeerId{cs.peer_key}, apos, desc_.aoi_radius);
        snap_scratch_.clear();
        repl_.serialize_snapshot_aoi(reg_, cs.peer_key, tick_, aoi_,
                                     PeerId{cs.peer_key}, /*byte_budget=*/0,
                                     snap_scratch_);
        host_->send(PeerId{cs.peer_key},
                    std::span<const u8>(snap_scratch_.data(), snap_scratch_.size()),
                    /*reliable=*/false, kChannelSnapshot);

        // Authoritative avatar report for the client's reconciliation, on the
        // default channel: pos(12) + acked_input(4) == 16 bytes (mirrors mp_demo).
        std::array<u8, 16> rep{};
        const math::Vec3 a = cs.input.authoritative_pos();
        u32 bx, by, bz;
        __builtin_memcpy(&bx, &a.x, 4);
        __builtin_memcpy(&by, &a.y, 4);
        __builtin_memcpy(&bz, &a.z, 4);
        write_u32_le(rep.data() + 0, bx);
        write_u32_le(rep.data() + 4, by);
        write_u32_le(rep.data() + 8, bz);
        write_u32_le(rep.data() + 12, cs.input.acked_input());
        host_->send(PeerId{cs.peer_key},
                    std::span<const u8>(rep.data(), rep.size()),
                    /*reliable=*/true, kChannelDefault);
    }

    // --- 4. Flush server sends + drain client snapshot acks ----------------
    inbox_.clear();
    host_->poll(inbox_);
    for (const InboundMessage& m : inbox_) {
        if (m.channel == kChannelSnapshot && m.bytes.size() == 4) {
            const u32 ack = read_u32_le(m.bytes.data());
            repl_.ack_from_client(m.from.raw, ack);
        } else if (m.channel == kChannelDefault) {
            // A late JOIN/LEAVE/input that arrived after the first poll this
            // tick is handled next tick; nothing to do here.
        }
    }

    host_->tick();

    // --- 5. Shutdown policy ------------------------------------------------
    const u32 n = admitted_peers();
    if (desc_.frame_budget != 0 && tick_ >= desc_.frame_budget)
        running_ = false;
    if (desc_.shutdown_when_empty && any_joined_ && n == 0u)
        running_ = false;

    return n;
}

u32 DedicatedServer::admitted_peers() const noexcept {
    u32 n = 0;
    for (const auto& c : clients_)
        if (c.active)
            ++n;
    return n;
}

}  // namespace psynder::net
