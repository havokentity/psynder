// SPDX-License-Identifier: MIT
// Psynder - lane 14 / M-NET entity-despawn replication tests.
//
// M-NET deltas carry a removed entity forward forever (an entity absent from
// a delta is treated as "unchanged since baseline"). Wave B adds an explicit
// despawn signal: the server marks a net_id removed, the snapshot carries a
// despawn set, and the client destroys the mapped entity + drops the net_id.
//
// Coverage:
//   * despawn propagates over the loopback: after the server marks a net_id
//     removed and ships a snapshot, the client destroys the mapped entity and
//     drops the net_id (mapped_count shrinks);
//   * the despawn is not resurrected by a later carry-forward delta;
//   * surviving entities keep replicating normally through the despawn.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/HostImpl.h"
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

ReplicatedComponentSet make_set() {
    ReplicatedComponentSet set;
    set.add<TransformComponent>();
    return set;
}

Entity spawn_server_entity(EcsRegistry& reg, SceneGraph& graph, u32 net_id, math::Vec3 pos) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e =
        psynder::scene::create_scene_entity(reg, graph, psynder::scene::kInvalidSceneNode, local);
    NetIdComponent tag{};
    tag.net_id = net_id;
    reg.add<NetIdComponent>(e, tag);
    return e;
}

// One full send->ack round-trip of a snapshot over two loopback hosts.
void round_trip(ReplicationServer& server, ReplicationClient& client, EcsRegistry& reg,
                HostImpl* host_s, HostImpl* host_c, PeerId s_to_c, PeerId c_to_s,
                u32 client_key, u32 tick, std::vector<u8>& snap,
                std::vector<InboundMessage>& s_in, std::vector<InboundMessage>& c_in) {
    server.serialize_snapshot(reg, client_key, tick, snap);
    host_s->send(s_to_c, std::span<const u8>(snap.data(), snap.size()), false, kChannelSnapshot);
    host_s->poll(s_in);
    host_c->poll(c_in);
    for (const InboundMessage& m : c_in) {
        if (m.channel == kChannelSnapshot)
            client.apply_snapshot(reg, std::span<const u8>(m.bytes.data(), m.bytes.size()));
    }
    s_in.clear();
    c_in.clear();
    const u32 ack = client.ack_seq();
    u8 ackbuf[4] = {u8(ack & 0xFF), u8((ack >> 8) & 0xFF), u8((ack >> 16) & 0xFF),
                    u8((ack >> 24) & 0xFF)};
    host_c->send(c_to_s, std::span<const u8>(ackbuf, 4), false, kChannelSnapshot);
    host_s->poll(s_in);
    host_c->poll(c_in);
    for (const InboundMessage& m : s_in) {
        if (m.channel == kChannelSnapshot && m.bytes.size() == 4) {
            u32 a = u32(m.bytes[0]) | (u32(m.bytes[1]) << 8) | (u32(m.bytes[2]) << 16) |
                    (u32(m.bytes[3]) << 24);
            server.ack_from_client(client_key, a);
        }
    }
    s_in.clear();
    c_in.clear();
    host_s->tick();
    host_c->tick();
}

}  // namespace

TEST_CASE("net: despawn destroys the client entity and drops the net_id",
          "[net][replication][despawn]") {
    RegistryReset reset;
    LoopbackBus::Get().reset();
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client(set);

    HostDesc ds{};
    ds.port = 52001;
    ds.max_peers = 4;
    HostDesc dc{};
    dc.port = 52002;
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

    Entity e1 = spawn_server_entity(reg, graph, 100, {0.f, 0.f, 0.f});
    Entity e2 = spawn_server_entity(reg, graph, 200, {5.f, 0.f, 0.f});
    (void)e1;

    std::vector<u8> snap;
    std::vector<InboundMessage> s_in, c_in;

    // Two round-trips: full snapshot + a steady delta. Client maps both ids.
    round_trip(server, client, reg, host_s, host_c, s_to_c, c_to_s, client_key, 1, snap, s_in, c_in);
    round_trip(server, client, reg, host_s, host_c, s_to_c, c_to_s, client_key, 2, snap, s_in, c_in);
    REQUIRE(client.mapped_count() == 2u);
    const Entity mapped200 = client.entity_for(200);
    REQUIRE(mapped200.valid());
    REQUIRE(reg.alive(mapped200));

    // Despawn net_id 200 on the server, remove its ECS row, and ship.
    server.mark_despawned(200);
    reg.destroy(e2);
    round_trip(server, client, reg, host_s, host_c, s_to_c, c_to_s, client_key, 3, snap, s_in, c_in);

    // Client destroyed the mapped entity + dropped the net_id.
    CHECK(client.mapped_count() == 1u);
    CHECK_FALSE(client.entity_for(200).valid());
    CHECK_FALSE(reg.alive(mapped200));
    // The survivor (100) is still mapped and alive.
    CHECK(client.entity_for(100).valid());

    // A later carry-forward delta must NOT resurrect the despawned id.
    round_trip(server, client, reg, host_s, host_c, s_to_c, c_to_s, client_key, 4, snap, s_in, c_in);
    CHECK(client.mapped_count() == 1u);
    CHECK_FALSE(client.entity_for(200).valid());

    destroy_test_host(host_s);
    destroy_test_host(host_c);
    LoopbackBus::Get().reset();
}
