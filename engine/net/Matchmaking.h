// SPDX-License-Identifier: MIT
// Psynder - dedicated server + lightweight matchmaking. Lane 14 (Wave 11).
//
// The netcode already has the pieces to stand a session up: HostImpl (the
// reliable-UDP transport over Loopback / real UDP), Lobby (the SYN/FIN slot
// table that admits a peer + assigns it a controlled net_id), ReplicationServer
// (AOI-gated snapshots), and the predict/reconcile input path. games/mp_demo
// (Wave 9) wires all of that into ONE process: a server half + two client halves
// sharing a world over Loopback.
//
// THIS module is purely ADDITIVE. It does NOT change Host / Lobby / AOI
// behaviour; it composes them into two reusable headless pieces:
//
//   Matchmaker     - a server-side "find or create a session + join" registry
//                    on top of Lobby. A client asks to join; the matchmaker
//                    finds an open session (or creates one) and admits the peer
//                    to a free slot, REJECTING with a clear deny reason when the
//                    session is at its configured capacity. The per-session
//                    capacity can be SMALLER than the Lobby's physical
//                    kMaxLobbySlots so a tiny "max=2" match rejects a 3rd joiner
//                    deterministically. A graceful leave frees the slot for a
//                    later joiner.
//
//   DedicatedServer- a standalone AUTHORITATIVE server loop with NO local
//                    client / avatar. It owns a HostImpl, a Matchmaker, a
//                    ReplicationServer + per-peer ServerInputProcessor, and a
//                    small shared world. Each step() it: accepts JOIN requests
//                    (matchmake -> admit -> spawn the peer's avatar), processes
//                    client inputs, advances the world, and replicates an
//                    AOI snapshot to every admitted peer. It shuts down cleanly
//                    once the last client leaves (or after a frame budget).
//
// Everything is deterministic (Loopback path: no RNG, no wall-clock) so the
// headless smoke + unit test reproduce exactly. Per-step scratch is pooled; the
// steady-state loop performs no heap allocation. ECS mutation happens on the
// calling (main) thread, matching the engine's threading contract.
//
// Wire: the matchmaking request / reply ride the existing reliable default
// channel as tagged little-endian payloads behind their own magics, exactly as
// Lobby's HandshakeMsg does, so a stale / cross-build frame is rejected rather
// than mis-read. The server distinguishes a JOIN request from a LEAVE request
// (and from an input command) purely by the payload magic + size, so no change
// to FrameHeader or the public Host::send surface is needed.

#pragma once

#include "Aoi.h"
#include "HostImpl.h"
#include "Lobby.h"
#include "Net.h"
#include "Prediction.h"
#include "Replication.h"
#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneGraph.h"

#include <array>
#include <span>
#include <vector>

namespace psynder::net {

// --- Matchmaking wire ---------------------------------------------------------
// A JOIN request (client->server). `session_id` is the session the client wants
// to join; 0 means "find or create any open session" (the common matchmaking
// path). `requested_max` is the client's desired session capacity, used ONLY
// when the matchmaker has to CREATE a session (ignored when joining an existing
// one). 12 bytes: magic(4) + session_id(4) + requested_max(4).
inline constexpr u32 kMatchJoinMagic = 0x504D4A31u;  // 'PMJ1'
inline constexpr usize kMatchJoinBytes = 12;

struct MatchJoinMsg {
    u32 session_id = 0;     // 0 == find-or-create-any.
    u32 requested_max = 0;  // desired capacity when creating; 0 == server default.
};

bool encode_match_join(const MatchJoinMsg& m, std::span<u8> out) noexcept;
bool decode_match_join(std::span<const u8> in, MatchJoinMsg& m) noexcept;

// Why a join was denied. `Accepted` is the success case.
enum class MatchDeny : u32 {
    Accepted = 0,    // not a deny: the join succeeded.
    SessionFull = 1, // the matched session is at capacity.
    NoSession = 2,   // a specific session_id was requested but does not exist.
    ServerFull = 3,  // no session has room and none could be created.
};

// A JOIN reply (server->client). On accept (`deny == Accepted`) it carries the
// session the client joined, the assigned player slot index, and the controlled
// net_id (the avatar the client predicts + the server simulates). On deny only
// `deny` is meaningful. 20 bytes:
//   magic(4) + deny(4) + session_id(4) + player_index(4) + controlled_net_id(4).
inline constexpr u32 kMatchReplyMagic = 0x504D5231u;  // 'PMR1'
inline constexpr usize kMatchReplyBytes = 20;

struct MatchReplyMsg {
    MatchDeny deny = MatchDeny::ServerFull;
    u32 session_id = 0;
    u32 player_index = 0;
    u32 controlled_net_id = 0;
    bool accepted() const noexcept { return deny == MatchDeny::Accepted; }
};

bool encode_match_reply(const MatchReplyMsg& m, std::span<u8> out) noexcept;
bool decode_match_reply(std::span<const u8> in, MatchReplyMsg& m) noexcept;

// A LEAVE request (client->server). `session_id` is informational; the server
// keys the leave off the transport peer it arrived on. 8 bytes:
//   magic(4) + session_id(4).
inline constexpr u32 kMatchLeaveMagic = 0x504D4C31u;  // 'PML1'
inline constexpr usize kMatchLeaveBytes = 8;

struct MatchLeaveMsg {
    u32 session_id = 0;
};

bool encode_match_leave(const MatchLeaveMsg& m, std::span<u8> out) noexcept;
bool decode_match_leave(std::span<const u8> in, MatchLeaveMsg& m) noexcept;

// --- Matchmaker (server-side) ------------------------------------------------
// A registry of sessions. Each session wraps a Lobby (the slot table that
// assigns controlled net_ids) plus a CONFIGURED max-players cap that may be
// smaller than the Lobby's physical kMaxLobbySlots. find_or_create() routes a
// join to an open session (creating one when needed); try_join() admits a peer
// and reports an explicit accept / deny. A leave frees the slot.
//
// Pooled + deterministic: sessions live in a fixed-capacity array; admission /
// release recycle Lobby slots exactly as the Lobby does. No RNG / wall-clock.
inline constexpr usize kMaxSessions = 16;

// The result of try_join(): the deny reason, plus (on accept) the assigned
// identity the server echoes to the client.
struct MatchJoinResult {
    MatchDeny deny = MatchDeny::ServerFull;
    u32 session_id = 0;
    u32 player_index = 0;
    u32 controlled_net_id = 0;
    bool is_new_session = false;  // a session was created to satisfy this join.
    bool is_new_peer = false;     // false == idempotent re-join of an existing peer.
    bool accepted() const noexcept { return deny == MatchDeny::Accepted; }
};

class Matchmaker {
   public:
    // `default_max_players` caps every CREATED session unless the join carries a
    // smaller positive `requested_max`. `base_net_id` is the first controlled
    // net_id minted across the whole server; each session's Lobby continues from
    // a disjoint id block so net_ids stay globally unique across sessions.
    // `max_sessions` caps how many sessions can exist at once (clamped to
    // kMaxSessions); set it to 1 for a single-match server so a full session
    // rejects the next joiner with ServerFull instead of spinning up a second.
    explicit Matchmaker(u32 default_max_players = 8, u32 base_net_id = 1,
                        u32 max_sessions = kMaxSessions) noexcept;

    // Find an open session (or create one) and admit the peer keyed by
    // `peer_key` (PeerId.raw). When `session_id != 0` the join targets THAT
    // session only (deny NoSession if it does not exist, SessionFull if it is
    // at capacity). When `session_id == 0` the matchmaker joins the first
    // session with room, creating a new one (capacity `requested_max` or the
    // server default) only if none has room and a session slot is free.
    //
    // Idempotent: a peer already admitted to a session re-joins it (same
    // assignment, no new slot). A peer admitted to one session that asks to join
    // another is first released from the old one.
    MatchJoinResult try_join(u32 peer_key, u32 session_id, u32 requested_max) noexcept;

    // Convenience for the common "find or create any open session" path.
    MatchJoinResult try_join_any(u32 peer_key) noexcept { return try_join(peer_key, 0, 0); }

    // Release the peer from whatever session it is in (a graceful leave or a
    // server-side drop). `out_session_id` / `out_freed_net_id` report the
    // session and the controlled net_id to despawn. Returns true if a peer was
    // actually released. An empty session is reclaimed so its slot can host a
    // fresh session later.
    bool leave(u32 peer_key, u32& out_session_id, u32& out_freed_net_id) noexcept;

    // The session a peer is in (0 if none), and its controlled net_id (0 if none).
    u32 session_of(u32 peer_key) const noexcept;
    u32 controlled_net_id(u32 peer_key) const noexcept;

    // Introspection / tests.
    u32 active_sessions() const noexcept;
    u32 admitted_in(u32 session_id) const noexcept;
    u32 capacity_of(u32 session_id) const noexcept;
    u32 total_admitted() const noexcept;

   private:
    struct Session {
        bool active = false;
        u32 id = 0;
        u32 max_players = 0;
        Lobby lobby{1};
        Session() noexcept : lobby(1) {}
    };

    isize find_session_(u32 session_id) const noexcept;
    isize find_session_with_room_() const noexcept;
    isize find_session_of_peer_(u32 peer_key) const noexcept;
    isize alloc_session_(u32 max_players) noexcept;

    std::array<Session, kMaxSessions> sessions_{};
    u32 default_max_players_ = 8;
    u32 max_sessions_ = kMaxSessions;
    u32 next_session_id_ = 1;
    // Each created session's Lobby gets a disjoint controlled-net-id block so
    // ids never collide across sessions sharing one replicated world.
    u32 next_net_id_block_ = 1;
    static constexpr u32 kNetIdBlockStride = 1000u;
};

// --- DedicatedServer ---------------------------------------------------------
// A standalone authoritative server with NO local client / avatar. Owns the
// transport, the matchmaker, the replication server, and a small shared world.
// The caller supplies the EcsRegistry + SceneGraph (so the server shares the
// engine's world) and pumps it with step() each tick. The server:
//   * accepts JOIN requests   (matchmake -> spawn the peer's avatar),
//   * processes client inputs (per-peer ServerInputProcessor),
//   * advances any server-owned world entities (caller hook),
//   * replicates an AOI snapshot to every admitted peer,
//   * releases a peer on LEAVE (despawn its avatar, free its slot),
//   * reports running() == false once the last client has left after at least
//     one client has ever joined (clean shutdown), or after a frame budget.
//
// The shared world's non-avatar entities (bots, scenery) are owned by the
// CALLER and advanced via an optional per-step hook; the server only owns the
// per-peer avatars it spawns on join. This keeps the server reusable for any
// world shape without it reaching into gameplay.
class DedicatedServer {
   public:
    struct Desc {
        u16 port = 47000;             // transport port.
        u16 max_peers = 16;           // transport peer table.
        u32 default_max_players = 8;  // per-session capacity default.
        u32 max_sessions = 1;         // concurrent sessions (1 == single match).
        u32 base_net_id = 100;        // first controlled net_id minted.
        f32 aoi_radius = 28.0f;       // per-peer interest sphere radius.
        u32 frame_budget = 0;         // 0 == run until the last client leaves.
        bool shutdown_when_empty = true;  // stop once the last client departs.
    };

    DedicatedServer(scene::EcsRegistry& reg, scene::SceneGraph& graph,
                    const ReplicatedComponentSet& set) noexcept;
    ~DedicatedServer();

    DedicatedServer(const DedicatedServer&) = delete;
    DedicatedServer& operator=(const DedicatedServer&) = delete;

    // Bring the transport up. Returns false if the host could not be created.
    bool start(const Desc& desc) noexcept;
    void stop() noexcept;

    // True until the server should shut down (frame budget exhausted, or the
    // last client left after at least one had joined and shutdown_when_empty).
    bool running() const noexcept { return running_; }

    // The transport host (so a co-process test can connect a client back to it).
    HostImpl* host() noexcept { return host_; }
    Matchmaker& matchmaker() noexcept { return mm_; }
    ReplicationServer& replication() noexcept { return repl_; }

    // Register the server's view of a client so an AOI snapshot can be addressed
    // to it: in the in-process test the server connects BACK to each client's
    // port and keys per-client state off that server-side PeerId.raw, exactly as
    // mp_demo does. Returns the server-side peer key. Call after the client's
    // host exists.
    u32 connect_to_client(u16 client_port) noexcept;

    // Per-step world hook: advance caller-owned world entities (bots, scenery)
    // for `server_tick`. Optional; default is a no-op.
    using WorldStepFn = void (*)(scene::EcsRegistry&, u32 server_tick, void* user) noexcept;
    void set_world_step(WorldStepFn fn, void* user) noexcept {
        world_step_ = fn;
        world_step_user_ = user;
    }

    // Advance one server tick. Drains JOIN / LEAVE / input messages, advances
    // the world, and replicates an AOI snapshot to every admitted peer. Returns
    // the number of admitted peers after the step (0 once everyone has left).
    // Updates running() per the shutdown policy.
    u32 step() noexcept;

    // Introspection.
    u32 admitted_peers() const noexcept;
    u32 logical_tick() const noexcept { return tick_; }
    u32 total_joins() const noexcept { return total_joins_; }
    u32 total_rejections() const noexcept { return total_rejections_; }

   private:
    // One admitted client's server-side state.
    struct ClientState {
        bool active = false;
        u32 peer_key = 0;  // server-side PeerId.raw addressing this client.
        u32 controlled_net_id = 0;
        Entity avatar = kInvalidEntity;
        ServerInputProcessor input{};
        math::Vec3 spawn{0.f, 0.f, 0.f};
    };

    isize find_client_(u32 peer_key) const noexcept;
    isize alloc_client_() noexcept;
    void admit_avatar_(u32 peer_key, const MatchJoinResult& r) noexcept;
    void drop_avatar_(isize ci, u32 freed_net_id) noexcept;
    math::Vec3 spawn_for_(u32 player_index) const noexcept;

    scene::EcsRegistry& reg_;
    scene::SceneGraph& graph_;
    Matchmaker mm_;
    ReplicationServer repl_;
    AoiFilter aoi_;
    HostImpl* host_ = nullptr;
    Desc desc_{};
    u32 tick_ = 0;
    bool running_ = false;
    bool any_joined_ = false;
    u32 total_joins_ = 0;
    u32 total_rejections_ = 0;
    WorldStepFn world_step_ = nullptr;
    void* world_step_user_ = nullptr;

    std::array<ClientState, kMaxLobbySlots> clients_{};
    // Pooled per-step scratch (alloc-free steady state).
    std::vector<InboundMessage> inbox_;
    std::vector<u8> snap_scratch_;
    std::array<u8, kMatchReplyBytes> reply_buf_{};
};

}  // namespace psynder::net
