// SPDX-License-Identifier: MIT
// Psynder — lane 14 / net lockstep coordinator tests.
//
// Lockstep is the racing-mode coordinator (DESIGN.md §10.4). For tick T to
// advance, EVERY peer must have submitted inputs for T. We verify:
//   - bundle is only "ready" after all expected peers post.
//   - inputs are deterministically ordered by peer_index (the engine relies
//     on this to feed lane-13 deterministic physics).
//   - a peer re-submitting the same tick replaces, not duplicates.

#include <catch2/catch_test_macros.hpp>

#include "net/Lockstep.h"

#include <array>

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net: lockstep gates tick until every peer posts", "[net][lockstep]") {
    LockstepCoordinator lc(/*expected_peers=*/3);

    LockstepInput a{};
    a.peer_index = 0;
    a.tick = 10;
    a.payload = {0x01};
    REQUIRE_FALSE(lc.submit(a));
    CHECK_FALSE(lc.is_ready(10));

    LockstepInput b{};
    b.peer_index = 1;
    b.tick = 10;
    b.payload = {0x02};
    REQUIRE_FALSE(lc.submit(b));
    CHECK_FALSE(lc.is_ready(10));

    LockstepInput c{};
    c.peer_index = 2;
    c.tick = 10;
    c.payload = {0x03};
    REQUIRE(lc.submit(c));  // returning true == bundle complete.
    CHECK(lc.is_ready(10));

    LockstepBundle bundle = lc.take_bundle(10);
    CHECK(bundle.tick == 10);
    REQUIRE(bundle.inputs.size() == 3);
    // Sorted by peer_index (0,1,2) for determinism.
    CHECK(bundle.inputs[0].peer_index == 0);
    CHECK(bundle.inputs[0].payload[0] == 0x01);
    CHECK(bundle.inputs[1].peer_index == 1);
    CHECK(bundle.inputs[1].payload[0] == 0x02);
    CHECK(bundle.inputs[2].peer_index == 2);
    CHECK(bundle.inputs[2].payload[0] == 0x03);
}

TEST_CASE("net: lockstep replaces re-submission for same peer+tick", "[net][lockstep]") {
    LockstepCoordinator lc(2);
    LockstepInput a{};
    a.peer_index = 0;
    a.tick = 5;
    a.payload = {0xAA};
    lc.submit(a);
    a.payload = {0xBB};
    REQUIRE_FALSE(lc.submit(a));  // still missing peer 1.
    LockstepInput second{};
    second.peer_index = 1;
    second.tick = 5;
    second.payload = {0xCC};
    REQUIRE(lc.submit(second));  // bundle now complete.

    LockstepBundle bundle = lc.take_bundle(5);
    REQUIRE(bundle.inputs.size() == 2);
    // The replaced peer-0 input must reflect the LATER value, not the first.
    CHECK(bundle.inputs[0].payload[0] == 0xBB);
}

TEST_CASE("net: lockstep buckets independent ticks separately", "[net][lockstep]") {
    LockstepCoordinator lc(2);
    LockstepInput a{};
    a.peer_index = 0;
    a.tick = 7;
    a.payload = {0x11};
    LockstepInput b{};
    b.peer_index = 0;
    b.tick = 8;
    b.payload = {0x22};
    lc.submit(a);
    lc.submit(b);
    CHECK_FALSE(lc.is_ready(7));
    CHECK_FALSE(lc.is_ready(8));
    CHECK(lc.pending_buckets() == 2);

    LockstepInput a2{};
    a2.peer_index = 1;
    a2.tick = 7;
    a2.payload = {0x33};
    REQUIRE(lc.submit(a2));
    CHECK(lc.is_ready(7));
    CHECK_FALSE(lc.is_ready(8));  // tick 8 still missing peer 1.
}

TEST_CASE("net: lockstep racing default is 8 peers", "[net][lockstep]") {
    LockstepCoordinator lc(/*expected_peers=*/8);
    CHECK(lc.expected_peers() == 8);
    for (u32 i = 0; i < 7; ++i) {
        LockstepInput in{};
        in.peer_index = i;
        in.tick = 1;
        in.payload = {u8(i)};
        REQUIRE_FALSE(lc.submit(in));
    }
    LockstepInput last{};
    last.peer_index = 7;
    last.tick = 1;
    last.payload = {7};
    REQUIRE(lc.submit(last));
    CHECK(lc.is_ready(1));
}
