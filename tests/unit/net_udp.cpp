// SPDX-License-Identifier: MIT
// Psynder - lane 14 / real localhost-UDP transport smoke test.
//
// HostImpl can ride a real non-blocking UDP socket bound to 127.0.0.1 instead
// of the in-process LoopbackBus, carrying the SAME channel-framed packets. This
// smoke proves a host pair exchanges a packet over the actual wire.
//
// In a headless CI sandbox sockets may be unavailable (no loopback, sandbox
// policy). If start() cannot open the socket the host transparently falls back
// to the loopback bus and udp_active() reports false; this test then SKIPs
// rather than failing - the deterministic prediction/replication coverage runs
// over loopback regardless (see net_prediction.cpp / net_despawn.cpp).

#include <catch2/catch_test_macros.hpp>

#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Net.h"
#include "net/UdpSocket.h"

#include <span>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::net;

namespace {

// Probe whether localhost UDP works at all in this environment: bind an
// ephemeral port, send a byte to ourselves, read it back. Returns true if the
// loopback datagram path is functional.
bool udp_available() {
    if (!UdpSocket::platform_init())
        return false;
    UdpSocket a, b;
    if (!a.open(0) || !b.open(0))
        return false;
    const u8 ping = 0xABu;
    if (!a.send_to(b.local_port(), std::span<const u8>(&ping, 1)))
        return false;
    // Non-blocking: retry a bounded number of times to absorb scheduling.
    for (int i = 0; i < 1000; ++i) {
        u8 buf[8] = {};
        u16 src = 0;
        if (b.recv_from(std::span<u8>(buf, sizeof(buf)), src) == 1 && buf[0] == ping)
            return true;
    }
    return false;
}

}  // namespace

TEST_CASE("net: localhost UDP host pair exchanges a channel-framed packet",
          "[net][udp][smoke]") {
    if (!udp_available()) {
        WARN("localhost UDP unavailable in this environment - skipping real-socket "
             "smoke (prediction/replication coverage still runs over loopback).");
        SUCCEED("UDP unavailable: skipped");
        return;
    }

    // Two real-UDP hosts. set_use_udp(true) must be called before start();
    // make_test_host() calls start() internally, so build + start by hand.
    HostImpl host_a, host_b;
    host_a.set_use_udp(true);
    host_b.set_use_udp(true);

    HostDesc da{};
    da.port = 0;  // ephemeral: OS assigns; avoids fixed-port collisions in CI.
    da.max_peers = 4;
    HostDesc db{};
    db.port = 0;
    db.max_peers = 4;

    REQUIRE(host_a.start(da));
    REQUIRE(host_b.start(db));

    // If either host silently fell back to loopback, the real-socket path is
    // not exercised - skip rather than assert.
    if (!host_a.udp_active() || !host_b.udp_active()) {
        host_a.stop();
        host_b.stop();
        WARN("HostImpl fell back to loopback - skipping real-socket smoke.");
        SUCCEED("UDP fallback: skipped");
        return;
    }

    REQUIRE(host_b.local_port() != 0);
    PeerId a_to_b = host_a.connect(host_b.local_port());
    REQUIRE(a_to_b.valid());

    const std::string msg = "udp-ping";
    host_a.send(a_to_b,
                std::span<const u8>(reinterpret_cast<const u8*>(msg.data()), msg.size()),
                /*reliable=*/true, kChannelDefault);

    // Real UDP delivery is asynchronous; poll host_b in a bounded loop. Each
    // poll drains the socket. Drive both hosts' ticks so RTO can retransmit if
    // the single datagram is dropped (UDP is lossy even on loopback under
    // pressure).
    std::vector<InboundMessage> a_in, b_in;
    bool got = false;
    for (int i = 0; i < 2000 && !got; ++i) {
        host_a.poll(a_in);
        host_b.poll(b_in);
        for (const InboundMessage& m : b_in) {
            if (m.bytes.size() == msg.size()) {
                std::string s(reinterpret_cast<const char*>(m.bytes.data()), m.bytes.size());
                if (s == msg)
                    got = true;
            }
        }
        a_in.clear();
        b_in.clear();
        host_a.tick();
        host_b.tick();
    }
    CHECK(got);

    host_a.stop();
    host_b.stop();
}
