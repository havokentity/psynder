// SPDX-License-Identifier: MIT
// Psynder — lane 14 / net sliding-window + selective-ACK tests.

#include <catch2/catch_test_macros.hpp>

#include "net/Reliability.h"

#include <array>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::net;

TEST_CASE("net: reliability enqueue assigns monotonically increasing seqs",
          "[net][reliability]") {
    Reliability r;
    std::array<u8, 4> payload{1, 2, 3, 4};
    u32 a = r.enqueue(payload, 0);
    u32 b = r.enqueue(payload, 0);
    u32 c = r.enqueue(payload, 0);
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
    CHECK(r.inflight_count() == 3);
}

TEST_CASE("net: reliability honours sliding-window cap", "[net][reliability]") {
    Reliability r;
    std::array<u8, 1> payload{0xAB};
    // Fill the window exactly.
    for (u32 i = 0; i < kSendWindow; ++i) {
        REQUIRE(r.enqueue(payload, 0) != 0);
    }
    // Next enqueue must fail until something gets acked.
    CHECK(r.enqueue(payload, 0) == 0);

    // Ack everything via a synthetic bitmap covering seq=64 with the low
    // 32 bits of history.
    AckTracker fake;
    for (u32 i = 1; i <= kSendWindow; ++i) fake.observe(i);
    u32 base = 0;
    u32 bits = 0;
    fake.snapshot(base, bits);
    CHECK(base == kSendWindow);
    u32 retired = r.apply_ack(base, bits);
    // ack_base + 32 selective bits ≈ retire 33 frames in one pass.
    CHECK(retired >= 33);
}

TEST_CASE("net: selective ACK retires arbitrary subsets", "[net][reliability]") {
    Reliability r;
    std::array<u8, 1> payload{0x77};
    for (u32 i = 0; i < 10; ++i) (void)r.enqueue(payload, 0);

    // Construct an ack saying "I got seq=10 and seq=8 but not 9".
    // ack_base=10; bits: 1 (i=0) -> seq 9 missing; bit i=1 -> seq 8 received.
    u32 ack_base = 10;
    u32 ack_bits = (1u << 1);  // mark seq=8 as received
    u32 retired = r.apply_ack(ack_base, ack_bits);
    CHECK(retired == 2);             // 10 and 8.
    CHECK(r.inflight_count() == 8);  // 9, 7, 6, 5, 4, 3, 2, 1 still in flight.
}

TEST_CASE("net: AckTracker observes new seqs and dedups", "[net][reliability]") {
    AckTracker a;
    CHECK(a.observe(5));      // first sight
    CHECK_FALSE(a.observe(5));// duplicate
    CHECK(a.observe(6));      // forward
    CHECK(a.observe(4));      // back-fill within window
    CHECK_FALSE(a.observe(4));// duplicate inside window
    u32 base = 0, bits = 0;
    a.snapshot(base, bits);
    CHECK(base == 6);
    // seq=5 was observed -> diff=1 -> bit 0 set.
    // seq=4 was observed -> diff=2 -> bit 1 set.
    CHECK((bits & 0x3u) == 0x3u);
}

TEST_CASE("net: ACK pipelining acks multiple in-flight frames in one reply",
          "[net][reliability][pipelining]") {
    Reliability sender;
    AckTracker  receiver;
    std::array<u8, 1> payload{0xC0};

    // Pipeline: sender pumps out 8 frames before any reply.
    std::vector<u32> seqs;
    for (u32 i = 0; i < 8; ++i) seqs.push_back(sender.enqueue(payload, 0));
    CHECK(sender.inflight_count() == 8);

    // Receiver sees them all and builds a single ack reply.
    for (u32 s : seqs) receiver.observe(s);
    u32 base = 0, bits = 0;
    receiver.snapshot(base, bits);

    // Sender applies the single reply — every in-flight frame must retire.
    u32 retired = sender.apply_ack(base, bits);
    CHECK(retired == 8);
    CHECK(sender.inflight_count() == 0);
}
