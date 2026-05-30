// SPDX-License-Identifier: MIT
// Psynder - lane 14 / Wave-11 dedicated-server + matchmaking tests.
//
// The Matchmaker is a "find or create a session + join" registry on top of the
// Lobby slot table; the DedicatedServer is a standalone authoritative loop (no
// local avatar) that matchmakes joins, spawns per-peer avatars, replicates AOI
// snapshots, and shuts down cleanly when the last client leaves. Both are
// purely additive over the existing Host / Lobby / AOI stack.
//
// Coverage:
//   * the matchmaking wire (join / reply / leave) round-trips byte-exactly
//     behind its magics and rejects stale / short payloads;
//   * Matchmaker: find-or-create admits to distinct slots, a join past the
//     configured per-session cap is REJECTED with SessionFull, a graceful leave
//     frees the slot for a later joiner, and an emptied session is reclaimed;
//   * a targeted join to a non-existent session denies with NoSession;
//   * end-to-end over the deterministic loopback: a DedicatedServer starts, two
//     clients matchmake + join (distinct slots + controlled net_ids), a THIRD
//     join is REJECTED when the session cap is 2, a client LEAVES (freeing its
//     slot) and a fresh client takes the freed slot, every admitted client sees
//     the shared world, and the server SHUTS DOWN cleanly once the last client
//     leaves.

#include <catch2/catch_test_macros.hpp>

#include "net/Aoi.h"
#include "net/Frame.h"
#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Matchmaking.h"
#include "net/Net.h"
#include "net/Prediction.h"
#include "net/Replication.h"

#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::net;
using psynder::scene::EcsRegistry;
using psynder::scene::LocalTransform;
using psynder::scene::SceneGraph;
using psynder::scene::TransformComponent;

namespace {

struct RegistryReset {
    RegistryReset() {
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        LoopbackBus::Get().reset();
    }
    ~RegistryReset() {
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        LoopbackBus::Get().reset();
    }
};

void put_u32(u8* d, u32 v) noexcept {
    d[0] = u8(v); d[1] = u8(v >> 8); d[2] = u8(v >> 16); d[3] = u8(v >> 24);
}

ReplicatedComponentSet make_set() {
    ReplicatedComponentSet set;
    set.add<TransformComponent>();
    return set;
}

// One deterministic ping-pong bot along X (shared world entity), advanced via
// the server's world-step hook so every admitted client sees it.
constexpr u32 kBotNetId = 9001u;
Entity g_bot = kInvalidEntity;

void world_step(EcsRegistry& reg, u32 tick, void* /*user*/) noexcept {
    if (!g_bot.valid())
        return;
    if (auto* tc = reg.get<TransformComponent>(g_bot)) {
        const f32 x = 20.0f + 6.0f * static_cast<f32>(tick % 8);
        tc->local.translation = math::Vec3{x, 0.f, 0.f};
    }
}

}  // namespace

TEST_CASE("net: matchmaking wire round-trips byte-exactly + rejects stale",
          "[net][matchmaking][wire]") {
    MatchJoinMsg j{};
    j.session_id = 0x01020304u;
    j.requested_max = 0x0A0B0C0Du;
    u8 jb[kMatchJoinBytes] = {};
    REQUIRE(encode_match_join(j, std::span<u8>(jb, sizeof(jb))));
    MatchJoinMsg jback{};
    REQUIRE(decode_match_join(std::span<const u8>(jb, sizeof(jb)), jback));
    CHECK(jback.session_id == j.session_id);
    CHECK(jback.requested_max == j.requested_max);
    CHECK_FALSE(encode_match_join(j, std::span<u8>(jb, 4)));  // short.
    jb[0] ^= 0xFFu;
    CHECK_FALSE(decode_match_join(std::span<const u8>(jb, sizeof(jb)), jback));  // bad magic.

    MatchReplyMsg r{};
    r.deny = MatchDeny::Accepted;
    r.session_id = 7u;
    r.player_index = 3u;
    r.controlled_net_id = 1234u;
    u8 rb[kMatchReplyBytes] = {};
    REQUIRE(encode_match_reply(r, std::span<u8>(rb, sizeof(rb))));
    MatchReplyMsg rback{};
    REQUIRE(decode_match_reply(std::span<const u8>(rb, sizeof(rb)), rback));
    CHECK(rback.accepted());
    CHECK(rback.session_id == r.session_id);
    CHECK(rback.player_index == r.player_index);
    CHECK(rback.controlled_net_id == r.controlled_net_id);

    MatchReplyMsg deny{};
    deny.deny = MatchDeny::SessionFull;
    REQUIRE(encode_match_reply(deny, std::span<u8>(rb, sizeof(rb))));
    REQUIRE(decode_match_reply(std::span<const u8>(rb, sizeof(rb)), rback));
    CHECK_FALSE(rback.accepted());
    CHECK(rback.deny == MatchDeny::SessionFull);

    MatchLeaveMsg l{};
    l.session_id = 42u;
    u8 lb[kMatchLeaveBytes] = {};
    REQUIRE(encode_match_leave(l, std::span<u8>(lb, sizeof(lb))));
    MatchLeaveMsg lback{};
    REQUIRE(decode_match_leave(std::span<const u8>(lb, sizeof(lb)), lback));
    CHECK(lback.session_id == l.session_id);
    lb[0] ^= 0xFFu;
    CHECK_FALSE(decode_match_leave(std::span<const u8>(lb, sizeof(lb)), lback));  // bad magic.
}

TEST_CASE("net: Matchmaker find-or-create admits distinct slots, caps, frees, reclaims",
          "[net][matchmaking][cap]") {
    // Per-session capacity of 2 so a third joiner is rejected.
    Matchmaker mm(/*default_max_players=*/2u, /*base_net_id=*/100u);

    const u32 pa = 0xA1u, pb = 0xB2u, pc = 0xC3u;

    MatchJoinResult ra = mm.try_join_any(pa);
    REQUIRE(ra.accepted());
    CHECK(ra.is_new_session);
    CHECK(ra.is_new_peer);
    const u32 sid = ra.session_id;
    CHECK(mm.capacity_of(sid) == 2u);

    MatchJoinResult rb = mm.try_join_any(pb);
    REQUIRE(rb.accepted());
    // Same session (find-or-create routed it to the existing open one).
    CHECK(rb.session_id == sid);
    CHECK_FALSE(rb.is_new_session);
    // Distinct slot identity: distinct player index + controlled net_id.
    CHECK(rb.player_index != ra.player_index);
    CHECK(rb.controlled_net_id != ra.controlled_net_id);
    CHECK(mm.admitted_in(sid) == 2u);

    // A re-join from pa is idempotent: same assignment, no new slot.
    MatchJoinResult ra2 = mm.try_join_any(pa);
    CHECK(ra2.accepted());
    CHECK_FALSE(ra2.is_new_peer);
    CHECK(ra2.controlled_net_id == ra.controlled_net_id);
    CHECK(mm.admitted_in(sid) == 2u);

    // A THIRD distinct peer requesting THIS session is rejected (cap == 2).
    MatchJoinResult rc = mm.try_join(pc, sid, 0u);
    CHECK_FALSE(rc.accepted());
    CHECK(rc.deny == MatchDeny::SessionFull);
    CHECK(mm.admitted_in(sid) == 2u);

    // A find-or-create-any from pc creates a SECOND session (no room in the
    // first), since a session slot is free.
    MatchJoinResult rc_any = mm.try_join_any(pc);
    REQUIRE(rc_any.accepted());
    CHECK(rc_any.is_new_session);
    CHECK(rc_any.session_id != sid);
    CHECK(mm.active_sessions() == 2u);

    // A targeted join to a session that does not exist denies with NoSession.
    MatchJoinResult miss = mm.try_join(0xD4u, /*session_id=*/999u, 0u);
    CHECK_FALSE(miss.accepted());
    CHECK(miss.deny == MatchDeny::NoSession);

    // pa LEAVES the first session: its slot frees + controlled net_id surfaces.
    u32 left_sid = 0, freed = 0;
    CHECK(mm.leave(pa, left_sid, freed));
    CHECK(left_sid == sid);
    CHECK(freed == ra.controlled_net_id);
    CHECK(mm.admitted_in(sid) == 1u);

    // A fresh joiner targeting the first session now FITS in the freed slot.
    MatchJoinResult rd = mm.try_join(0xE5u, sid, 0u);
    REQUIRE(rd.accepted());
    CHECK(rd.session_id == sid);
    CHECK(mm.admitted_in(sid) == 2u);

    // Drain the first session entirely; it is reclaimed (no longer active).
    u32 s2 = 0, f2 = 0;
    CHECK(mm.leave(pb, s2, f2));
    CHECK(mm.leave(0xE5u, s2, f2));
    CHECK(mm.admitted_in(sid) == 0u);
}

TEST_CASE("net: dedicated server - matchmake join/reject/leave/rejoin over loopback, shared world",
          "[net][matchmaking][dedicated][loopback][integration]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    const ReplicatedComponentSet set = make_set();

    // Shared-world bot entity advanced by the server's world-step hook.
    {
        LocalTransform local{};
        local.translation = math::Vec3{20.f, 0.f, 0.f};
        g_bot = psynder::scene::create_scene_entity(reg, graph,
                                                    psynder::scene::kInvalidSceneNode, local);
        NetIdComponent tag{};
        tag.net_id = kBotNetId;
        reg.add<NetIdComponent>(g_bot, tag);
    }

    DedicatedServer srv(reg, graph, set);
    DedicatedServer::Desc d{};
    d.port = 47100;
    d.max_peers = 8;
    d.default_max_players = 2u;  // tiny cap: a 3rd join is rejected.
    d.max_sessions = 1u;          // single match: full session rejects (no 2nd).
    d.base_net_id = 200u;
    d.aoi_radius = 1000.0f;       // generous: every client sees the shared bot.
    d.frame_budget = 0;           // run until the last client leaves.
    d.shutdown_when_empty = true;
    REQUIRE(srv.start(d));
    srv.set_world_step(&world_step, nullptr);

    // A lightweight client harness: its own loopback host + a ReplicationClient.
    struct Client {
        HostImpl* host = nullptr;
        PeerId to_server{};
        u32 server_key = 0;  // server-side PeerId.raw addressing this client.
        ReplicationClient repl;
        bool joined = false;
        bool rejected = false;
        MatchDeny last_deny = MatchDeny::ServerFull;
        u32 controlled_net_id = 0;
        bool saw_bot = false;
        std::vector<InboundMessage> inbox;
        explicit Client(const ReplicatedComponentSet& s) : repl(s) {}
    };

    auto make_client = [&](u16 port) -> Client* {
        Client* c = new Client(set);
        HostDesc cd{};
        cd.port = port;
        cd.max_peers = 4;
        c->host = make_test_host(cd);
        REQUIRE(c->host);
        c->to_server = c->host->connect(d.port);
        c->server_key = srv.connect_to_client(port);
        c->inbox.reserve(32);
        return c;
    };

    auto send_join = [&](Client* c) {
        MatchJoinMsg req{};  // session_id 0 == find-or-create-any.
        std::array<u8, kMatchJoinBytes> jb{};
        encode_match_join(req, std::span<u8>(jb.data(), jb.size()));
        c->host->send(c->to_server, std::span<const u8>(jb.data(), jb.size()), true,
                      kChannelDefault);
    };

    auto send_leave = [&](Client* c) {
        MatchLeaveMsg bye{};
        std::array<u8, kMatchLeaveBytes> lb{};
        encode_match_leave(bye, std::span<u8>(lb.data(), lb.size()));
        c->host->send(c->to_server, std::span<const u8>(lb.data(), lb.size()), true,
                      kChannelDefault);
    };

    // Drain a client's inbox: apply snapshots + parse the match reply.
    auto pump_client = [&](Client* c) {
        c->inbox.clear();
        c->host->poll(c->inbox);
        for (const InboundMessage& m : c->inbox) {
            if (m.channel == kChannelSnapshot && m.bytes.size() >= kReplHeaderBytes) {
                c->repl.apply_snapshot(reg, std::span<const u8>(m.bytes.data(), m.bytes.size()));
                std::array<u8, 4> abuf{};
                put_u32(abuf.data(), c->repl.ack_seq());
                c->host->send(c->to_server, std::span<const u8>(abuf.data(), 4), false,
                              kChannelSnapshot);
                if (c->repl.entity_for(kBotNetId).valid())
                    c->saw_bot = true;
            } else if (m.channel == kChannelDefault && m.bytes.size() == kMatchReplyBytes) {
                MatchReplyMsg rep{};
                if (decode_match_reply(std::span<const u8>(m.bytes.data(), m.bytes.size()), rep)) {
                    c->last_deny = rep.deny;
                    if (rep.accepted()) {
                        c->joined = true;
                        c->controlled_net_id = rep.controlled_net_id;
                    } else {
                        c->rejected = true;
                    }
                }
            }
        }
    };

    Client* a = make_client(47101);
    Client* b = make_client(47102);

    // --- 1. Two clients matchmake + join: each gets a distinct slot ---------
    send_join(a);
    send_join(b);
    // A handful of server steps lets the JOIN flow + first snapshots complete.
    for (u32 i = 0; i < 6; ++i) {
        srv.step();
        pump_client(a);
        pump_client(b);
    }
    REQUIRE(a->joined);
    REQUIRE(b->joined);
    CHECK(a->controlled_net_id != 0u);
    CHECK(b->controlled_net_id != 0u);
    CHECK(a->controlled_net_id != b->controlled_net_id);  // distinct slots.
    CHECK(srv.admitted_peers() == 2u);
    CHECK(srv.total_joins() == 2u);

    // --- 2. A THIRD client is REJECTED (session cap == 2) -------------------
    Client* cthird = make_client(47103);
    send_join(cthird);
    for (u32 i = 0; i < 4; ++i) {
        srv.step();
        pump_client(a);
        pump_client(b);
        pump_client(cthird);
    }
    REQUIRE(cthird->rejected);
    CHECK_FALSE(cthird->joined);
    // Single-session server (Desc::max_sessions == 1): a full session can't spin
    // up a second, so the deny is ServerFull (capacity exhausted server-wide).
    CHECK(cthird->last_deny == MatchDeny::ServerFull);
    CHECK(srv.admitted_peers() == 2u);
    CHECK(srv.total_rejections() == 1u);

    // --- 3. Client A LEAVES; its slot frees ---------------------------------
    send_leave(a);
    for (u32 i = 0; i < 4; ++i) {
        srv.step();
        pump_client(a);
        pump_client(b);
        pump_client(cthird);
    }
    CHECK(srv.admitted_peers() == 1u);

    // --- 4. The third client RE-JOINS and now takes the freed slot ----------
    cthird->rejected = false;
    cthird->last_deny = MatchDeny::ServerFull;
    send_join(cthird);
    for (u32 i = 0; i < 6; ++i) {
        srv.step();
        pump_client(b);
        pump_client(cthird);
    }
    REQUIRE(cthird->joined);
    CHECK(cthird->controlled_net_id != 0u);
    CHECK(srv.admitted_peers() == 2u);
    CHECK(srv.total_joins() == 3u);

    // --- 5. Every admitted client saw the shared world bot ------------------
    CHECK(b->saw_bot);
    CHECK(cthird->saw_bot);

    // --- 6. The server is still running while clients remain ----------------
    CHECK(srv.running());

    // --- 7. Everyone leaves: the server shuts down cleanly ------------------
    send_leave(b);
    send_leave(cthird);
    for (u32 i = 0; i < 6 && srv.running(); ++i) {
        srv.step();
        pump_client(b);
        pump_client(cthird);
    }
    CHECK(srv.admitted_peers() == 0u);
    CHECK_FALSE(srv.running());  // clean shutdown once the last client left.

    destroy_test_host(a->host);
    destroy_test_host(b->host);
    destroy_test_host(cthird->host);
    delete a;
    delete b;
    delete cthird;
    srv.stop();
    g_bot = kInvalidEntity;
}
