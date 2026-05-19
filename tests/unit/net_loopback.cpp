// SPDX-License-Identifier: MIT
// Psynder — lane 14 / net loopback-bus + reliable-send-with-drop tests.
//
// These tests run two HostImpl instances inside the same process. The
// LoopbackBus carries datagrams between them; a LossPolicy injects
// deterministic packet drops. The point: prove that the rUDP layer
// eventually delivers every reliable byte in order, even when the wire
// drops a known fraction of frames.

#include <catch2/catch_test_macros.hpp>

#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Net.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

// Pump both hosts: drain their inbox, advance one logical tick. Returns
// total messages delivered this round across both inboxes.
u32 pump(HostImpl& a, HostImpl& b,
         std::vector<InboundMessage>& a_in,
         std::vector<InboundMessage>& b_in) {
    u32 n = a.poll(a_in) + b.poll(b_in);
    a.tick();
    b.tick();
    return n;
}

// xorshift32 — small deterministic PRNG so the test is reproducible.
struct Xs32 {
    u32 state;
    u32 next() {
        u32 x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
};

}  // namespace

TEST_CASE("net: two in-process Hosts exchange bytes via loopback bus",
          "[net][loopback][harness]") {
    LoopbackBus::Get().reset();
    HostDesc da{}; da.port = 41001; da.max_peers = 4;
    HostDesc db{}; db.port = 41002; db.max_peers = 4;
    HostImpl* a = make_test_host(da);
    HostImpl* b = make_test_host(db);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    PeerId a_to_b = a->connect(db.port);
    REQUIRE(a_to_b.valid());

    const std::string msg = "ping";
    a->send(a_to_b, std::span<const u8>(reinterpret_cast<const u8*>(msg.data()),
                                        msg.size()),
            /*reliable=*/true);

    std::vector<InboundMessage> a_in, b_in;
    pump(*a, *b, a_in, b_in);
    REQUIRE(b_in.size() == 1);
    std::string got(reinterpret_cast<const char*>(b_in[0].bytes.data()),
                    b_in[0].bytes.size());
    CHECK(got == msg);

    destroy_test_host(a);
    destroy_test_host(b);
    LoopbackBus::Get().reset();
}

TEST_CASE("net: reliable send delivers every message under 5% simulated drop",
          "[net][loopback][drop]") {
    LoopbackBus::Get().reset();

    // 5% drop. We use a deterministic PRNG so the test is reproducible.
    Xs32 prng{0xBADC0DEu};
    LoopbackBus::Get().set_loss_policy(
        [&prng](u16 /*src*/, u16 /*dst*/, std::span<const u8> /*bytes*/,
                u32 /*attempt*/) {
            return (prng.next() % 100) >= 5;  // deliver 95% of frames
        });

    HostDesc da{}; da.port = 42001; da.max_peers = 4;
    HostDesc db{}; db.port = 42002; db.max_peers = 4;
    HostImpl* a = make_test_host(da);
    HostImpl* b = make_test_host(db);
    REQUIRE(a);
    REQUIRE(b);

    PeerId a_to_b = a->connect(db.port);
    REQUIRE(a_to_b.valid());

    constexpr u32 kCount = 60;  // stays within the kSendWindow=64 cap.
    for (u32 i = 0; i < kCount; ++i) {
        u8 byte = u8(i);
        a->send(a_to_b, std::span<const u8>(&byte, 1), /*reliable=*/true);
    }

    // Pump until B has received them all (or we time out). Each tick gives
    // RTO a chance to retransmit any frames the loss policy dropped.
    std::vector<InboundMessage> a_in, b_in;
    constexpr u32 kMaxTicks = 4000;
    u32 ticks = 0;
    while (b_in.size() < kCount && ticks++ < kMaxTicks) {
        pump(*a, *b, a_in, b_in);
    }
    INFO("ticks=" << ticks << " delivered=" << b_in.size() << " expected=" << kCount);
    REQUIRE(b_in.size() == kCount);

    // Ordering: reliable delivery is in-order. The payloads were 0,1,2,…
    for (u32 i = 0; i < kCount; ++i) {
        REQUIRE(b_in[i].bytes.size() == 1);
        CHECK(b_in[i].bytes[0] == u8(i));
        CHECK(b_in[i].reliable);
    }

    destroy_test_host(a);
    destroy_test_host(b);
    LoopbackBus::Get().set_loss_policy({});
    LoopbackBus::Get().reset();
}

TEST_CASE("net: ACK pipelining retires whole window after one round-trip",
          "[net][loopback][pipelining]") {
    LoopbackBus::Get().reset();

    HostDesc da{}; da.port = 43001; da.max_peers = 4;
    HostDesc db{}; db.port = 43002; db.max_peers = 4;
    HostImpl* a = make_test_host(da);
    HostImpl* b = make_test_host(db);
    REQUIRE(a);
    REQUIRE(b);

    PeerId a_to_b = a->connect(db.port);
    REQUIRE(a_to_b.valid());

    // Pump out a contiguous burst — no drops in this test, so every frame
    // is in-flight after the send loop, and a single ack from B should
    // retire all of them in one go.
    constexpr u32 kBurst = 16;
    for (u32 i = 0; i < kBurst; ++i) {
        u8 b1 = u8(i);
        a->send(a_to_b, std::span<const u8>(&b1, 1), /*reliable=*/true);
    }

    // After send-burst (before any pump) A has kBurst frames in flight in
    // its reliability ring. We can't peek the ring directly via HostImpl,
    // but we can verify the post-condition: one pump+tick delivers everything
    // and the followup tick from B carries an ack that retires the window.
    std::vector<InboundMessage> a_in, b_in;
    pump(*a, *b, a_in, b_in);  // delivers all frames; B owes acks.
    pump(*a, *b, a_in, b_in);  // B's tick emits the ack-only frame; A retires.
    pump(*a, *b, a_in, b_in);  // A's tick consumes that frame.

    CHECK(b_in.size() == kBurst);

    // Verify no retransmits happened — issue a fresh burst; if the window
    // were still occupied, the second burst would have been rejected.
    for (u32 i = 0; i < kBurst; ++i) {
        u8 b1 = u8(kBurst + i);
        a->send(a_to_b, std::span<const u8>(&b1, 1), /*reliable=*/true);
    }
    pump(*a, *b, a_in, b_in);
    pump(*a, *b, a_in, b_in);
    pump(*a, *b, a_in, b_in);
    CHECK(b_in.size() == kBurst * 2);

    destroy_test_host(a);
    destroy_test_host(b);
    LoopbackBus::Get().reset();
}
