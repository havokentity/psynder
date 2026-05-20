// SPDX-License-Identifier: MIT
// Psynder — lane 14 / net Area-Of-Interest filter tests.

#include <catch2/catch_test_macros.hpp>

#include "net/Aoi.h"
#include "net/Net.h"
#include "net/Snapshot.h"

#include <array>

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net: AOI filter includes entities inside the sphere", "[net][aoi]") {
    AoiFilter f;
    PeerId p{1};
    f.set_peer(p, math::Vec3{0, 0, 0}, /*radius=*/100.f);

    CHECK(f.visible(p, math::Vec3{50.f, 0.f, 0.f}));
    CHECK(f.visible(p, math::Vec3{0.f, 99.f, 0.f}));
    CHECK(f.visible(p, math::Vec3{0.f, 0.f, 0.f}));
}

TEST_CASE("net: AOI filter excludes entities outside the sphere", "[net][aoi]") {
    AoiFilter f;
    PeerId p{1};
    f.set_peer(p, math::Vec3{0, 0, 0}, /*radius=*/100.f);

    CHECK_FALSE(f.visible(p, math::Vec3{101.f, 0.f, 0.f}));
    CHECK_FALSE(f.visible(p, math::Vec3{0.f, 0.f, 1000.f}));
}

TEST_CASE("net: AOI boundary is inclusive — entity exactly on radius", "[net][aoi][boundary]") {
    AoiFilter f;
    PeerId p{1};
    f.set_peer(p, math::Vec3{0, 0, 0}, /*radius=*/10.f);

    // Exactly on the sphere — 6/8/10 right-angled triangle.
    CHECK(f.visible(p, math::Vec3{6.f, 8.f, 0.f}));
    // One ULP past the boundary (radius_sq = 100, dx^2+dy^2 = 100.0001).
    CHECK_FALSE(f.visible(p, math::Vec3{6.f, 8.0001f, 0.f}));
}

TEST_CASE("net: AOI returns false for unknown peer", "[net][aoi]") {
    AoiFilter f;
    PeerId unknown{42};
    CHECK_FALSE(f.visible(unknown, math::Vec3{0, 0, 0}));
}

TEST_CASE("net: compose_for_peer drops entities outside AOI", "[net][aoi][snapshot]") {
    AoiFilter f;
    PeerId p{7};
    f.set_peer(p, math::Vec3{0, 0, 0}, /*radius=*/50.f);

    SnapshotFrame world;
    world.tick = 100;
    world.entities = {
        {1, math::Vec3{0.f, 0.f, 0.f}, 0xAAu},      // inside
        {2, math::Vec3{10.f, 0.f, 0.f}, 0xBBu},     // inside
        {3, math::Vec3{100.f, 0.f, 0.f}, 0xCCu},    // outside
        {4, math::Vec3{0.f, 50.f, 0.f}, 0xDDu},     // on the boundary (inside)
        {5, math::Vec3{100.f, 100.f, 0.f}, 0xEEu},  // far outside
    };

    SnapshotFrame visible;
    compose_for_peer(world, f, p, visible);
    CHECK(visible.tick == 100);
    REQUIRE(visible.entities.size() == 3);
    CHECK(visible.entities[0].entity_id == 1);
    CHECK(visible.entities[1].entity_id == 2);
    CHECK(visible.entities[2].entity_id == 4);
}

TEST_CASE("net: snapshot encode/decode round-trips entity table", "[net][snapshot]") {
    SnapshotFrame f;
    f.tick = 0xCAFEBABEu;
    f.entities = {
        {1, math::Vec3{1.f, 2.f, 3.f}, 0xA5A5A5A5u},
        {2, math::Vec3{-1.f, 0.5f, 99.f}, 0xDEADBEEFu},
    };
    std::array<u8, 1024> buf{};
    usize n = encode_snapshot(f, std::span<u8>(buf.data(), buf.size()));
    REQUIRE(n > 0);

    SnapshotFrame g;
    REQUIRE(decode_snapshot(std::span<const u8>(buf.data(), n), g));
    CHECK(g.tick == f.tick);
    REQUIRE(g.entities.size() == f.entities.size());
    for (usize i = 0; i < f.entities.size(); ++i) {
        CHECK(g.entities[i].entity_id == f.entities[i].entity_id);
        CHECK(g.entities[i].position.x == f.entities[i].position.x);
        CHECK(g.entities[i].position.y == f.entities[i].position.y);
        CHECK(g.entities[i].position.z == f.entities[i].position.z);
        CHECK(g.entities[i].state_bits == f.entities[i].state_bits);
    }
}
