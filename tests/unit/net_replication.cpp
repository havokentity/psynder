// SPDX-License-Identifier: MIT
// Psynder - lane 14 / M-NET ECS entity replication tests.
//
// These tests exercise the authoritative-server -> client snapshot/delta
// replication layer (engine/net/Replication.{h,cpp}) over the in-process
// LoopbackBus transport. The single global EcsRegistry hosts BOTH worlds in
// one process by partitioning archetypes:
//
//   * SERVER entities carry NetIdComponent + SceneNodeComponent +
//     TransformComponent. The gather query requires SceneNodeComponent
//     (it recovers the Entity handle from SceneNodeComponent.entity), so it
//     only ever sees server entities.
//   * CLIENT entities are created by ReplicationClient::apply_snapshot with
//     NetIdComponent (+ the replicated components it writes). They have NO
//     SceneNodeComponent, so the server gather skips them entirely.
//
// This keeps the server and client worlds disjoint inside the one global
// registry while still driving the real DOTS gather query.
//
// Coverage:
//   * full snapshot then delta: a steady-state delta is smaller than the
//     full snapshot when little changed;
//   * client converges to server positions over N ticks across the loopback;
//   * net_id -> Entity mapping is stable across snapshots;
//   * interpolation yields a between-ticks position;
//   * malformed / truncated snapshots are rejected cleanly.

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
using psynder::scene::SceneNodeComponent;
using psynder::scene::TransformComponent;

namespace {

// EcsRegistry::Get() is a process-global singleton and Catch2 randomizes case
// order; reset it around every case so rows never leak between tests (mirrors
// gameplay_combat.cpp's RegistryReset).
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

// Build the replicated set used by every test: just TransformComponent (the
// motion that interpolation smooths). Adding more POD components would just
// add mask bits; the format is component-agnostic.
ReplicatedComponentSet make_set() {
    ReplicatedComponentSet set;
    set.add<TransformComponent>();
    return set;
}

// Spawn a SERVER entity: NetId + SceneNode + Transform at `pos`.
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

void set_server_pos(EcsRegistry& reg, Entity e, math::Vec3 pos) {
    if (auto* tc = reg.get<TransformComponent>(e))
        tc->local.translation = pos;
}

math::Vec3 client_pos(ReplicationClient& client, EcsRegistry& reg, u32 net_id) {
    const Entity e = client.entity_for(net_id);
    if (!e.valid())
        return math::Vec3{1e9f, 1e9f, 1e9f};  // sentinel "not mapped".
    const auto* tc = reg.get<TransformComponent>(e);
    return tc ? tc->local.translation : math::Vec3{1e9f, 1e9f, 1e9f};
}

}  // namespace

TEST_CASE("net: full snapshot then delta - delta is smaller when unchanged",
          "[net][replication][delta]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);

    // Three server entities.
    spawn_server_entity(reg, graph, 1, {0.f, 0.f, 0.f});
    spawn_server_entity(reg, graph, 2, {1.f, 0.f, 0.f});
    Entity e3 = spawn_server_entity(reg, graph, 3, {2.f, 0.f, 0.f});

    const u32 client_key = 0xC0FFEEu;
    std::vector<u8> full_bytes;
    const usize full_n = server.serialize_snapshot(reg, client_key, /*tick=*/1, full_bytes);
    REQUIRE(full_n > kReplHeaderBytes);
    const u32 full_seq = server.last_seq();

    // Client acks the full snapshot, establishing a baseline.
    server.ack_from_client(client_key, full_seq);

    // Nothing changed -> the next snapshot is a pure delta with zero entity
    // records (all carried forward from the baseline).
    std::vector<u8> delta_bytes;
    const usize delta_n = server.serialize_snapshot(reg, client_key, /*tick=*/2, delta_bytes);
    INFO("full=" << full_n << " delta=" << delta_n);
    CHECK(delta_n < full_n);
    CHECK(delta_n == kReplHeaderBytes);  // header only: no changed entities.

    // Move ONE entity -> the delta carries exactly one entity record.
    server.ack_from_client(client_key, server.last_seq());
    set_server_pos(reg, e3, {2.f, 5.f, 0.f});
    std::vector<u8> delta2;
    const usize delta2_n = server.serialize_snapshot(reg, client_key, /*tick=*/3, delta2);
    CHECK(delta2_n > kReplHeaderBytes);   // one record present.
    CHECK(delta2_n < full_n);             // still smaller than a full of three.
}

TEST_CASE("net: client converges to server positions over the loopback",
          "[net][replication][convergence]") {
    RegistryReset reset;
    LoopbackBus::Get().reset();
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client(set);

    // Two in-process hosts on channel 2 (snapshot) carry the bytes; this
    // proves the layer rides the existing transport/framing path.
    HostDesc ds{};
    ds.port = 51001;
    ds.max_peers = 4;
    HostDesc dc{};
    dc.port = 51002;
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

    // Server entities; they move along +X each tick.
    Entity e1 = spawn_server_entity(reg, graph, 10, {0.f, 0.f, 0.f});
    Entity e2 = spawn_server_entity(reg, graph, 20, {0.f, 1.f, 0.f});

    std::vector<u8> snap;
    std::vector<InboundMessage> s_in, c_in;

    constexpr u32 kTicks = 16;
    for (u32 t = 1; t <= kTicks; ++t) {
        // Advance the authoritative world.
        set_server_pos(reg, e1, {f32(t), 0.f, 0.f});
        set_server_pos(reg, e2, {0.f, 1.f, f32(t) * 0.5f});

        // Server: gather + delta-encode for this client, ship on channel 2.
        server.serialize_snapshot(reg, client_key, t, snap);
        host_s->send(s_to_c, std::span<const u8>(snap.data(), snap.size()),
                     /*reliable=*/false, kChannelSnapshot);

        // Pump the bus; deliver to the client.
        host_s->poll(s_in);
        host_c->poll(c_in);
        for (const InboundMessage& m : c_in) {
            if (m.channel == kChannelSnapshot)
                client.apply_snapshot(reg, std::span<const u8>(m.bytes.data(), m.bytes.size()));
        }
        c_in.clear();
        s_in.clear();

        // Client echoes its ack back to the server on channel 2.
        const u32 ack = client.ack_seq();
        u8 ackbuf[4] = {u8(ack & 0xFF), u8((ack >> 8) & 0xFF), u8((ack >> 16) & 0xFF),
                        u8((ack >> 24) & 0xFF)};
        host_c->send(c_to_s, std::span<const u8>(ackbuf, 4), /*reliable=*/false, kChannelSnapshot);
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

    // After N ticks the client world matches the server world (snapshots
    // applied verbatim at tick boundaries; interp t=1 == newest frame).
    client.interpolate_transforms(reg, /*t=*/1.0f);
    const math::Vec3 c1 = client_pos(client, reg, 10);
    const math::Vec3 c2 = client_pos(client, reg, 20);
    CHECK(c1.x == Catch::Approx(f32(kTicks)).margin(1e-4f));
    CHECK(c1.y == Catch::Approx(0.f).margin(1e-4f));
    CHECK(c2.z == Catch::Approx(f32(kTicks) * 0.5f).margin(1e-4f));

    // net_id -> Entity mapping is stable: the same two net_ids, same handles.
    CHECK(client.mapped_count() == 2u);
    const Entity m1 = client.entity_for(10);
    const Entity m2 = client.entity_for(20);
    CHECK(m1.valid());
    CHECK(m2.valid());
    // Re-apply does not remap.
    server.serialize_snapshot(reg, client_key, kTicks + 1, snap);
    client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size()));
    CHECK(client.entity_for(10) == m1);
    CHECK(client.entity_for(20) == m2);

    destroy_test_host(host_s);
    destroy_test_host(host_c);
    LoopbackBus::Get().reset();
    (void)e1;
    (void)e2;
}

TEST_CASE("net: interpolation yields a between-ticks position",
          "[net][replication][interp]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client(set);

    Entity e = spawn_server_entity(reg, graph, 7, {0.f, 0.f, 0.f});
    const u32 client_key = 1u;
    std::vector<u8> snap;

    // Tick 1: position (0,0,0). Full snapshot.
    set_server_pos(reg, e, {0.f, 0.f, 0.f});
    server.serialize_snapshot(reg, client_key, 1, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    server.ack_from_client(client_key, server.last_seq());

    // Tick 2: position (10,0,0). Delta.
    set_server_pos(reg, e, {10.f, 0.f, 0.f});
    server.serialize_snapshot(reg, client_key, 2, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));

    // Halfway between the two applied snapshots -> x == 5.
    client.interpolate_transforms(reg, 0.5f);
    CHECK(client_pos(client, reg, 7).x == Catch::Approx(5.f).margin(1e-4f));

    // t=0 -> older frame (0); t=1 -> newer frame (10).
    client.interpolate_transforms(reg, 0.0f);
    CHECK(client_pos(client, reg, 7).x == Catch::Approx(0.f).margin(1e-4f));
    client.interpolate_transforms(reg, 1.0f);
    CHECK(client_pos(client, reg, 7).x == Catch::Approx(10.f).margin(1e-4f));
}

TEST_CASE("net: malformed / truncated snapshots are rejected cleanly",
          "[net][replication][malformed]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client(set);

    Entity e = spawn_server_entity(reg, graph, 99, {1.f, 2.f, 3.f});
    (void)e;
    std::vector<u8> good;
    server.serialize_snapshot(reg, /*client_key=*/5u, 1, good);
    REQUIRE(good.size() > kReplHeaderBytes);

    // Empty buffer -> rejected.
    CHECK_FALSE(client.apply_snapshot(reg, std::span<const u8>{}));

    // Bad magic -> rejected.
    std::vector<u8> bad_magic = good;
    bad_magic[0] ^= 0xFFu;
    CHECK_FALSE(client.apply_snapshot(reg, std::span<const u8>(bad_magic.data(), bad_magic.size())));

    // Truncated mid-record -> rejected (and nothing applied: no mapping made).
    std::vector<u8> trunc(good.begin(), good.begin() + kReplHeaderBytes + 4);
    CHECK_FALSE(client.apply_snapshot(reg, std::span<const u8>(trunc.data(), trunc.size())));

    // Header claims an entity but the body is missing it entirely -> rejected.
    std::vector<u8> hdr_only(good.begin(), good.begin() + kReplHeaderBytes);
    // entity_count field (offset 16) is already >0 from the good snapshot.
    CHECK_FALSE(client.apply_snapshot(reg, std::span<const u8>(hdr_only.data(), hdr_only.size())));

    // The good snapshot still applies after the rejected ones (clean state).
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(good.data(), good.size())));
    CHECK(client.entity_for(99).valid());
    CHECK(client_pos(client, reg, 99).x == Catch::Approx(1.f).margin(1e-4f));
}
