// SPDX-License-Identifier: MIT
// Psynder - lane 14 / Wave-C AOI-gated per-peer replication tests.
//
// serialize_snapshot_aoi() gates each peer's snapshot to the entities inside
// that peer's interest sphere, fits them into a per-snapshot byte budget
// (closest-first priority, drop the overflow this frame), and despawns on the
// client any entity that LEAVES the peer's interest.
//
// Coverage:
//   * a far entity outside a peer's AOI is NOT in that peer's snapshot, and
//     once it leaves a previously-in-view sphere it despawns on that client;
//   * two peers with different viewpoints get DIFFERENT entity sets;
//   * the byte budget caps a snapshot and the dropped (lower-priority, farther)
//     entities arrive a later frame once the budget allows.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/Aoi.h"
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

void set_server_pos(EcsRegistry& reg, Entity e, math::Vec3 pos) {
    if (auto* tc = reg.get<TransformComponent>(e))
        tc->local.translation = pos;
}

// Apply one AOI snapshot to a client and return the net_ids it now maps.
std::vector<u32> mapped_ids(ReplicationClient& client) {
    std::vector<u32> ids;
    // mapped_count + entity_for probe over a small id space used in these tests.
    for (u32 id = 1; id <= 64; ++id) {
        if (client.entity_for(id).valid())
            ids.push_back(id);
    }
    return ids;
}

}  // namespace

TEST_CASE("net: a far entity outside the AOI is excluded and despawns on its client",
          "[net][replication][aoi]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client(set);

    // One near entity (id 1, inside) + one far entity (id 2, outside the sphere).
    Entity near = spawn_server_entity(reg, graph, 1, {0.f, 0.f, 0.f});
    Entity far = spawn_server_entity(reg, graph, 2, {100.f, 0.f, 0.f});
    (void)near;

    AoiFilter aoi;
    const PeerId peer{0xA1u};
    aoi.set_peer(peer, {0.f, 0.f, 0.f}, /*radius=*/10.f);
    const u32 client_key = peer.raw;

    std::vector<u8> snap;
    // Tick 1: full snapshot, AOI-gated, generous budget. Only id 1 is in range.
    const usize n1 =
        server.serialize_snapshot_aoi(reg, client_key, 1, aoi, peer, /*budget=*/0, snap);
    (void)n1;
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    server.ack_from_client(client_key, server.last_seq());

    // The far entity (id 2) never entered this peer's snapshot.
    CHECK(client.entity_for(1).valid());
    CHECK_FALSE(client.entity_for(2).valid());
    CHECK(client.mapped_count() == 1u);

    // Now move the far entity INTO range -> it spawns on the client.
    set_server_pos(reg, far, {3.f, 0.f, 0.f});
    server.serialize_snapshot_aoi(reg, client_key, 2, aoi, peer, 0, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    server.ack_from_client(client_key, server.last_seq());
    CHECK(client.entity_for(2).valid());
    CHECK(client.mapped_count() == 2u);

    // Move it back OUT of range -> it must despawn on the client (left interest).
    set_server_pos(reg, far, {100.f, 0.f, 0.f});
    server.serialize_snapshot_aoi(reg, client_key, 3, aoi, peer, 0, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    server.ack_from_client(client_key, server.last_seq());
    CHECK_FALSE(client.entity_for(2).valid());
    CHECK(client.entity_for(1).valid());
    CHECK(client.mapped_count() == 1u);
}

TEST_CASE("net: two peers with different viewpoints get different entity sets",
          "[net][replication][aoi][perpeer]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client_a(set);
    ReplicationClient client_b(set);

    // Three entities spread along X: 1 @0, 2 @50, 3 @100.
    spawn_server_entity(reg, graph, 1, {0.f, 0.f, 0.f});
    spawn_server_entity(reg, graph, 2, {50.f, 0.f, 0.f});
    spawn_server_entity(reg, graph, 3, {100.f, 0.f, 0.f});

    AoiFilter aoi;
    const PeerId pa{0xAu};
    const PeerId pb{0xBu};
    aoi.set_peer(pa, {0.f, 0.f, 0.f}, /*radius=*/20.f);    // sees 1 only.
    aoi.set_peer(pb, {100.f, 0.f, 0.f}, /*radius=*/20.f);  // sees 3 only.

    std::vector<u8> snap;
    server.serialize_snapshot_aoi(reg, pa.raw, 1, aoi, pa, 0, snap);
    REQUIRE(client_a.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    server.serialize_snapshot_aoi(reg, pb.raw, 1, aoi, pb, 0, snap);
    REQUIRE(client_b.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));

    const std::vector<u32> a_ids = mapped_ids(client_a);
    const std::vector<u32> b_ids = mapped_ids(client_b);
    CHECK(a_ids == std::vector<u32>{1u});
    CHECK(b_ids == std::vector<u32>{3u});
    // Disjoint sets prove the gating is per-peer, not global.
    CHECK(a_ids != b_ids);
}

TEST_CASE("net: byte budget caps a snapshot; dropped entities arrive a later frame",
          "[net][replication][aoi][budget]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);
    ReplicationClient client(set);

    // Four entities all inside the sphere, at increasing distance so priority
    // (closest-first) is deterministic: 1 @1, 2 @2, 3 @3, 4 @4 along X.
    spawn_server_entity(reg, graph, 1, {1.f, 0.f, 0.f});
    spawn_server_entity(reg, graph, 2, {2.f, 0.f, 0.f});
    spawn_server_entity(reg, graph, 3, {3.f, 0.f, 0.f});
    spawn_server_entity(reg, graph, 4, {4.f, 0.f, 0.f});

    AoiFilter aoi;
    const PeerId peer{0xC0u};
    aoi.set_peer(peer, {0.f, 0.f, 0.f}, /*radius=*/100.f);  // all four visible.
    const u32 client_key = peer.raw;

    // One entity record = prefix(8) + TransformComponent bytes. Budget that
    // fits the header + exactly TWO records, forcing the two farthest to drop.
    const u32 record = u32(kReplEntityPrefixBytes) + set.max_entity_bytes();
    const u32 budget = u32(kReplHeaderBytes) + 2u * record;

    std::vector<u8> snap;
    const usize n1 =
        server.serialize_snapshot_aoi(reg, client_key, 1, aoi, peer, budget, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    // The snapshot is capped: at most the header + two records of bytes.
    CHECK(n1 <= usize(budget));
    // The two CLOSEST (1, 2) made the cut; the two farthest (3, 4) were dropped.
    CHECK(client.entity_for(1).valid());
    CHECK(client.entity_for(2).valid());
    CHECK_FALSE(client.entity_for(3).valid());
    CHECK_FALSE(client.entity_for(4).valid());
    server.ack_from_client(client_key, server.last_seq());

    // The dropped entities were NOT despawned (they stayed in interest). A
    // later frame with a bigger budget delivers them - they were not lost.
    server.serialize_snapshot_aoi(reg, client_key, 2, aoi, peer, /*budget=*/0, snap);
    REQUIRE(client.apply_snapshot(reg, std::span<const u8>(snap.data(), snap.size())));
    CHECK(client.entity_for(3).valid());
    CHECK(client.entity_for(4).valid());
    CHECK(client.mapped_count() == 4u);
}
