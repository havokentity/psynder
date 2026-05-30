// SPDX-License-Identifier: MIT
// Psynder - lane 14 / Wave-C handshake + lobby tests.
//
// The Lobby admits peers on SYN (assigning a player slot + a controlled
// net_id the peer has authority over) and tears them down on FIN (despawning
// the controlled net_id + freeing the slot). The handshake + combined ack
// wire forms round-trip byte-exactly behind their magics.
//
// Coverage:
//   * SYN admits a peer and assigns a slot + controlled net_id; a re-sent SYN
//     is idempotent (same assignment, no new slot);
//   * FIN despawns the peer's controlled net_id and frees the slot/index, which
//     is then recycled by the next admission;
//   * the table is finite: admission past capacity is rejected;
//   * handshake + ack wire round-trips are endian-stable and reject stale/short
//     payloads;
//   * an end-to-end SYN -> accept -> FIN flow over the deterministic loopback,
//     with the controlled net_id fed into ReplicationServer::mark_despawned.

#include <catch2/catch_test_macros.hpp>

#include "net/Frame.h"
#include "net/HostImpl.h"
#include "net/Lobby.h"
#include "net/Loopback.h"
#include "net/Net.h"
#include "net/Replication.h"

#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"

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
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

}  // namespace

TEST_CASE("net: SYN admits a peer with a slot + controlled net_id; re-SYN is idempotent",
          "[net][lobby][handshake]") {
    Lobby lobby(/*base_net_id=*/100u);

    const u32 peer_a = 0xAAu;
    AdmitResult ra = lobby.admit(peer_a);
    CHECK(ra.accepted);
    CHECK(ra.is_new);
    CHECK(ra.controlled_net_id == 100u);
    CHECK(lobby.state_of(peer_a) == ConnState::Connected);
    CHECK(lobby.admitted_count() == 1u);
    CHECK(lobby.controlled_net_id(peer_a) == 100u);

    // A second distinct peer takes the next slot + net_id.
    const u32 peer_b = 0xBBu;
    AdmitResult rb = lobby.admit(peer_b);
    CHECK(rb.accepted);
    CHECK(rb.is_new);
    CHECK(rb.controlled_net_id == 101u);
    CHECK(rb.player_index != ra.player_index);
    CHECK(lobby.admitted_count() == 2u);

    // Re-SYN from peer_a: idempotent. Same assignment, no new slot.
    AdmitResult ra2 = lobby.admit(peer_a);
    CHECK(ra2.accepted);
    CHECK_FALSE(ra2.is_new);
    CHECK(ra2.controlled_net_id == ra.controlled_net_id);
    CHECK(ra2.player_index == ra.player_index);
    CHECK(lobby.admitted_count() == 2u);
}

TEST_CASE("net: FIN despawns the controlled net_id and frees + recycles the slot",
          "[net][lobby][handshake][fin]") {
    Lobby lobby(/*base_net_id=*/1u);
    const u32 peer_a = 1u, peer_b = 2u;
    AdmitResult ra = lobby.admit(peer_a);
    AdmitResult rb = lobby.admit(peer_b);
    REQUIRE(ra.accepted);
    REQUIRE(rb.accepted);
    REQUIRE(lobby.admitted_count() == 2u);

    // FIN peer_a: its controlled net_id is surfaced for despawn + slot freed.
    u32 freed = 0;
    CHECK(lobby.release(peer_a, freed));
    CHECK(freed == ra.controlled_net_id);
    CHECK(lobby.state_of(peer_a) == ConnState::Disconnected);
    CHECK(lobby.controlled_net_id(peer_a) == 0u);
    CHECK(lobby.admitted_count() == 1u);

    // FIN on an unknown peer is a no-op.
    u32 freed2 = 123u;
    CHECK_FALSE(lobby.release(0xDEADu, freed2));
    CHECK(freed2 == 0u);

    // A new admission recycles the freed net_id + player index (compact reuse).
    AdmitResult rc = lobby.admit(0xCCu);
    CHECK(rc.accepted);
    CHECK(rc.controlled_net_id == ra.controlled_net_id);
    CHECK(rc.player_index == ra.player_index);
    CHECK(lobby.admitted_count() == 2u);
}

TEST_CASE("net: the lobby table is finite - admission past capacity is rejected",
          "[net][lobby][capacity]") {
    Lobby lobby;
    u32 admitted = 0;
    for (u32 i = 0; i < kMaxLobbySlots; ++i) {
        if (lobby.admit(1000u + i).accepted)
            ++admitted;
    }
    CHECK(admitted == kMaxLobbySlots);
    CHECK(lobby.admitted_count() == kMaxLobbySlots);
    // One past capacity: rejected, no slot.
    AdmitResult over = lobby.admit(0xFFFFu);
    CHECK_FALSE(over.accepted);
    CHECK(lobby.admitted_count() == kMaxLobbySlots);
}

TEST_CASE("net: SYN/FIN FrameHeader flags drive the lobby state machine",
          "[net][lobby][handshake][flags]") {
    Lobby lobby(/*base_net_id=*/10u);
    const u32 peer = 0x77u;

    // A SYN-flagged frame admits the peer.
    AdmitResult adm{};
    u32 freed = 0;
    CHECK(lobby.on_frame_flags(peer, kFlagSyn, adm, freed));
    CHECK(adm.accepted);
    CHECK(adm.is_new);
    CHECK(adm.controlled_net_id == 10u);
    CHECK(freed == 0u);
    CHECK(lobby.state_of(peer) == ConnState::Connected);

    // A reliable (no SYN/FIN) frame is not a handshake frame: no transition.
    AdmitResult none{};
    CHECK_FALSE(lobby.on_frame_flags(peer, kFlagReliable, none, freed));
    CHECK_FALSE(none.accepted);
    CHECK(lobby.admitted_count() == 1u);

    // A FIN-flagged frame releases the peer + surfaces the controlled net_id.
    AdmitResult unused{};
    CHECK(lobby.on_frame_flags(peer, kFlagFin, unused, freed));
    CHECK(freed == 10u);
    CHECK(lobby.state_of(peer) == ConnState::Disconnected);
    CHECK(lobby.admitted_count() == 0u);
}

TEST_CASE("net: handshake + combined-ack wire round-trips byte-exactly",
          "[net][lobby][wire]") {
    HandshakeMsg m{};
    m.player_index = 0x01020304u;
    m.controlled_net_id = 0x0A0B0C0Du;
    m.flags = kHandshakeFlagAccepted;
    u8 buf[kHandshakeBytes] = {};
    REQUIRE(encode_handshake(m, std::span<u8>(buf, sizeof(buf))));
    HandshakeMsg back{};
    REQUIRE(decode_handshake(std::span<const u8>(buf, sizeof(buf)), back));
    CHECK(back.player_index == m.player_index);
    CHECK(back.controlled_net_id == m.controlled_net_id);
    CHECK(back.flags == m.flags);
    // Short buffer + bad magic are rejected.
    CHECK_FALSE(encode_handshake(m, std::span<u8>(buf, 4)));
    buf[0] ^= 0xFFu;
    CHECK_FALSE(decode_handshake(std::span<const u8>(buf, sizeof(buf)), back));

    SnapshotAck a{};
    a.snapshot_seq = 0xDEADBEEFu;
    a.input_seq = 0x12345678u;
    u8 abuf[kAckBytes] = {};
    REQUIRE(encode_ack(a, std::span<u8>(abuf, sizeof(abuf))));
    SnapshotAck aback{};
    REQUIRE(decode_ack(std::span<const u8>(abuf, sizeof(abuf)), aback));
    CHECK(aback.snapshot_seq == a.snapshot_seq);
    CHECK(aback.input_seq == a.input_seq);
    // A bare Wave-B 4-byte ack (no magic) is rejected by the tagged decoder.
    CHECK_FALSE(decode_ack(std::span<const u8>(abuf, 4), aback));
}

TEST_CASE("net: SYN -> accept -> FIN flow over the loopback despawns the peer entity",
          "[net][lobby][handshake][loopback]") {
    RegistryReset reset;
    LoopbackBus::Get().reset();
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set;
    set.add<TransformComponent>();
    ReplicationServer server(set);
    ReplicationClient client(set);
    Lobby lobby(/*base_net_id=*/500u);

    HostDesc ds{};
    ds.port = 54001;
    ds.max_peers = 4;
    HostDesc dc{};
    dc.port = 54002;
    dc.max_peers = 4;
    HostImpl* host_s = make_test_host(ds);
    HostImpl* host_c = make_test_host(dc);
    REQUIRE(host_s);
    REQUIRE(host_c);
    PeerId s_to_c = host_s->connect(dc.port);
    PeerId c_to_s = host_c->connect(ds.port);
    REQUIRE(s_to_c.valid());
    REQUIRE(c_to_s.valid());
    const u32 client_key = c_to_s.raw;

    std::vector<InboundMessage> s_in, c_in;

    // 1. Client sends SYN on the default channel. (The public Host::send()
    //    surface is frozen and does not expose FrameHeader flags, so over the
    //    loopback we carry the handshake as a tagged default-channel payload;
    //    the flag-driven dispatch path is exercised directly by the
    //    "SYN/FIN FrameHeader flags drive the lobby" case below.)
    {
        u8 syn[kHandshakeBytes] = {};
        HandshakeMsg req{};
        encode_handshake(req, std::span<u8>(syn, sizeof(syn)));
        host_c->send(c_to_s, std::span<const u8>(syn, sizeof(syn)), /*reliable=*/true,
                     kChannelDefault);
    }
    host_c->poll(c_in);
    host_s->poll(s_in);

    u32 assigned_net_id = 0;
    for (const InboundMessage& m : s_in) {
        if (m.channel == kChannelDefault && m.bytes.size() == kHandshakeBytes) {
            HandshakeMsg req{};
            if (decode_handshake(std::span<const u8>(m.bytes.data(), m.bytes.size()), req)) {
                AdmitResult r = lobby.admit(m.from.raw);
                CHECK(r.accepted);
                assigned_net_id = r.controlled_net_id;
                // Reply SYN|ACK with the assigned identity.
                HandshakeMsg ack{};
                ack.player_index = r.player_index;
                ack.controlled_net_id = r.controlled_net_id;
                ack.flags = kHandshakeFlagAccepted;
                u8 rep[kHandshakeBytes] = {};
                encode_handshake(ack, std::span<u8>(rep, sizeof(rep)));
                host_s->send(m.from, std::span<const u8>(rep, sizeof(rep)), /*reliable=*/true,
                             kChannelDefault);
            }
        }
    }
    s_in.clear();
    c_in.clear();
    REQUIRE(assigned_net_id == 500u);

    // 2. Client receives SYN|ACK + adopts the assigned net_id.
    host_s->poll(s_in);
    host_c->poll(c_in);
    u32 client_controlled = 0;
    for (const InboundMessage& m : c_in) {
        if (m.channel == kChannelDefault && m.bytes.size() == kHandshakeBytes) {
            HandshakeMsg ack{};
            if (decode_handshake(std::span<const u8>(m.bytes.data(), m.bytes.size()), ack) &&
                (ack.flags & kHandshakeFlagAccepted)) {
                client_controlled = ack.controlled_net_id;
            }
        }
    }
    s_in.clear();
    c_in.clear();
    CHECK(client_controlled == assigned_net_id);

    // 3. The server spawns the peer's controlled entity + replicates it.
    LocalTransform local{};
    local.translation = {7.f, 0.f, 0.f};
    Entity ent =
        psynder::scene::create_scene_entity(reg, graph, psynder::scene::kInvalidSceneNode, local);
    NetIdComponent tag{};
    tag.net_id = assigned_net_id;
    reg.add<NetIdComponent>(ent, tag);

    std::vector<u8> snap;
    server.serialize_snapshot(reg, client_key, 1, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    CHECK(client.entity_for(assigned_net_id).valid());
    server.ack_from_client(client_key, server.last_seq());

    // 4. Client sends FIN; server releases the slot + despawns the controlled
    //    net_id; the next snapshot tears it down on the client.
    {
        u8 fin[kHandshakeBytes] = {};
        HandshakeMsg bye{};
        encode_handshake(bye, std::span<u8>(fin, sizeof(fin)));
        host_c->send(c_to_s, std::span<const u8>(fin, sizeof(fin)), /*reliable=*/true,
                     kChannelDefault);
    }
    host_c->poll(c_in);
    host_s->poll(s_in);
    for (const InboundMessage& m : s_in) {
        if (m.channel == kChannelDefault && m.bytes.size() == kHandshakeBytes) {
            u32 freed = 0;
            if (lobby.release(m.from.raw, freed) && freed != 0)
                server.mark_despawned(freed);
        }
    }
    s_in.clear();
    c_in.clear();
    CHECK(lobby.state_of(client_key) == ConnState::Disconnected);
    CHECK(lobby.admitted_count() == 0u);

    reg.destroy(ent);
    server.serialize_snapshot(reg, client_key, 2, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    CHECK_FALSE(client.entity_for(assigned_net_id).valid());

    destroy_test_host(host_s);
    destroy_test_host(host_c);
    LoopbackBus::Get().reset();
}
