// SPDX-License-Identifier: MIT
// Psynder - lane 14 / client prediction + server reconciliation tests.
//
// The controlled entity is predicted locally (zero-latency response) and
// reconciled against authoritative server state by replaying unacked inputs.
// All deterministic over the in-process loopback.
//
// Coverage:
//   * prediction moves the local entity the SAME tick the input is applied
//     (no waiting for a server round-trip);
//   * with the server agreeing, reconciliation is a no-op: the converged
//     position equals the predicted one (no rubber-banding);
//   * after a DIVERGENT authoritative snapshot (server integrated the inputs
//     differently), reconciliation replays the unacked inputs from the
//     authoritative base and converges to server truth with no permanent
//     offset;
//   * the InputCmd wire round-trips byte-exactly (endian-stable).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Net.h"
#include "net/Prediction.h"

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

// Make a controlled entity: Transform + PredictedComponent at `pos`.
Entity make_controlled(EcsRegistry& reg, SceneGraph& graph, math::Vec3 pos) {
    LocalTransform local{};
    local.translation = pos;
    const Entity e =
        psynder::scene::create_scene_entity(reg, graph, psynder::scene::kInvalidSceneNode, local);
    PredictedComponent pc{};
    pc.pos = pos;
    reg.add<PredictedComponent>(e, pc);
    return e;
}

math::Vec3 transform_pos(EcsRegistry& reg, Entity e) {
    const auto* tc = reg.get<TransformComponent>(e);
    return tc ? tc->local.translation : math::Vec3{1e9f, 1e9f, 1e9f};
}

}  // namespace

TEST_CASE("net: InputCmd wire round-trips byte-exactly", "[net][prediction][wire]") {
    InputCmd c{};
    c.seq = 0x01020304u;
    c.tick = 0x0A0B0C0Du;
    c.move_x = 1.5f;
    c.move_y = -2.25f;
    c.move_z = 8.75f;
    u8 buf[kInputCmdBytes] = {};
    REQUIRE(encode_input(c, std::span<u8>(buf, sizeof(buf))));
    InputCmd back{};
    REQUIRE(decode_input(std::span<const u8>(buf, sizeof(buf)), back));
    CHECK(back.seq == c.seq);
    CHECK(back.tick == c.tick);
    CHECK(back.move_x == c.move_x);
    CHECK(back.move_y == c.move_y);
    CHECK(back.move_z == c.move_z);
    // Too-small buffers are rejected.
    CHECK_FALSE(encode_input(c, std::span<u8>(buf, 4)));
    CHECK_FALSE(decode_input(std::span<const u8>(buf, 4), back));
}

TEST_CASE("net: prediction moves the controlled entity the same tick",
          "[net][prediction][local]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    Entity e = make_controlled(reg, graph, {0.f, 0.f, 0.f});
    Predictor pred;
    pred.bind(e, {0.f, 0.f, 0.f});

    // Apply input +1 on X. The predicted position AND the ECS Transform move
    // immediately - no round-trip required.
    InputCmd cmd = pred.predict(reg, /*client_tick=*/1, {1.f, 0.f, 0.f});
    CHECK(cmd.seq == 1u);
    CHECK(pred.predicted_pos().x == Catch::Approx(1.f).margin(1e-5f));
    CHECK(transform_pos(reg, e).x == Catch::Approx(1.f).margin(1e-5f));
    CHECK(pred.unacked_count() == 1u);

    // A second input stacks on top, same tick semantics.
    pred.predict(reg, 2, {2.f, 0.f, 0.f});
    CHECK(pred.predicted_pos().x == Catch::Approx(3.f).margin(1e-5f));
    CHECK(transform_pos(reg, e).x == Catch::Approx(3.f).margin(1e-5f));
    CHECK(pred.unacked_count() == 2u);
}

TEST_CASE("net: reconcile is a no-op when the server agrees (no rubber-band)",
          "[net][prediction][reconcile]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    Entity e = make_controlled(reg, graph, {0.f, 0.f, 0.f});
    Predictor pred;
    pred.bind(e, {0.f, 0.f, 0.f});
    ServerInputProcessor server;
    server.bind(kInvalidEntity, {0.f, 0.f, 0.f});  // server-side state only.

    // Client predicts three inputs and ships them; the server processes them
    // identically (same step_state), so authoritative == predicted.
    for (u32 t = 1; t <= 3; ++t) {
        InputCmd cmd = pred.predict(reg, t, {1.f, 0.f, 0.f});
        server.process(reg, cmd);
    }
    const f32 predicted_before = pred.predicted_pos().x;
    CHECK(predicted_before == Catch::Approx(3.f).margin(1e-5f));
    CHECK(server.authoritative_pos().x == Catch::Approx(3.f).margin(1e-5f));
    CHECK(server.acked_input() == 3u);

    // Reconcile against the agreeing authoritative state. Nothing should snap;
    // all inputs are acked so the ring drains and predicted == authoritative.
    pred.reconcile(reg, server.authoritative_pos(), server.acked_input());
    CHECK(pred.predicted_pos().x == Catch::Approx(predicted_before).margin(1e-5f));
    CHECK(pred.unacked_count() == 0u);
    CHECK(transform_pos(reg, e).x == Catch::Approx(3.f).margin(1e-5f));
}

TEST_CASE("net: reconcile replays unacked inputs and converges after divergence",
          "[net][prediction][reconcile][divergence]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    Entity e = make_controlled(reg, graph, {0.f, 0.f, 0.f});
    Predictor pred;
    pred.bind(e, {0.f, 0.f, 0.f});

    // The client predicts inputs 1..5 (each +1 on X) -> predicted x == 5,
    // five inputs unacked.
    std::vector<InputCmd> sent;
    for (u32 t = 1; t <= 5; ++t)
        sent.push_back(pred.predict(reg, t, {1.f, 0.f, 0.f}));
    REQUIRE(pred.predicted_pos().x == Catch::Approx(5.f).margin(1e-5f));
    REQUIRE(pred.unacked_count() == 5u);

    // The SERVER diverged: it processed only inputs 1..2, and (e.g. a wall it
    // saw that the client didn't) clamped the result so authoritative x == 1.5
    // after input 2 (not the 2.0 the client predicted). server_acked_input==2.
    const math::Vec3 auth_pos{1.5f, 0.f, 0.f};
    const u32 server_acked = 2u;

    pred.reconcile(reg, auth_pos, server_acked);

    // Inputs 1..2 are dropped (acked); inputs 3..5 (each +1) replay from the
    // authoritative base 1.5 -> 1.5 + 3 == 4.5. The predicted state converged
    // to server truth WITHOUT a permanent offset: it equals
    // auth + (sum of still-unacked inputs), with NO trace of the diverged 1..2.
    CHECK(pred.predicted_pos().x == Catch::Approx(4.5f).margin(1e-5f));
    CHECK(pred.unacked_count() == 3u);
    CHECK(transform_pos(reg, e).x == Catch::Approx(4.5f).margin(1e-5f));

    // Now the server catches up: it processes 3..5 from 1.5 -> 4.5 and acks 5.
    pred.reconcile(reg, math::Vec3{4.5f, 0.f, 0.f}, 5u);
    CHECK(pred.predicted_pos().x == Catch::Approx(4.5f).margin(1e-5f));
    CHECK(pred.unacked_count() == 0u);  // fully reconciled: no permanent offset.
}

TEST_CASE("net: prediction + reconciliation converge over the loopback",
          "[net][prediction][loopback]") {
    RegistryReset reset;
    LoopbackBus::Get().reset();
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    Entity client_ent = make_controlled(reg, graph, {0.f, 0.f, 0.f});
    Predictor pred;
    pred.bind(client_ent, {0.f, 0.f, 0.f});
    ServerInputProcessor server;
    server.bind(kInvalidEntity, {0.f, 0.f, 0.f});

    HostDesc ds{};
    ds.port = 53001;
    ds.max_peers = 4;
    HostDesc dc{};
    dc.port = 53002;
    dc.max_peers = 4;
    HostImpl* host_s = make_test_host(ds);
    HostImpl* host_c = make_test_host(dc);
    REQUIRE(host_s);
    REQUIRE(host_c);
    PeerId s_to_c = host_s->connect(dc.port);
    PeerId c_to_s = host_c->connect(ds.port);
    REQUIRE(s_to_c.valid());
    REQUIRE(c_to_s.valid());

    std::vector<InboundMessage> s_in, c_in;
    constexpr u32 kTicks = 20;
    for (u32 t = 1; t <= kTicks; ++t) {
        // Client predicts and ships the input on the default channel.
        InputCmd cmd = pred.predict(reg, t, {1.f, 0.f, 0.f});
        u8 ibuf[kInputCmdBytes];
        encode_input(cmd, std::span<u8>(ibuf, sizeof(ibuf)));
        host_c->send(c_to_s, std::span<const u8>(ibuf, sizeof(ibuf)), /*reliable=*/true,
                     kChannelDefault);

        // Server receives inputs, processes them authoritatively.
        host_c->poll(c_in);
        host_s->poll(s_in);
        for (const InboundMessage& m : s_in) {
            if (m.channel == kChannelDefault && m.bytes.size() == kInputCmdBytes) {
                InputCmd in{};
                decode_input(std::span<const u8>(m.bytes.data(), m.bytes.size()), in);
                server.process(reg, in);
            }
        }
        s_in.clear();
        c_in.clear();

        // Server reports (authoritative_pos, acked_input) back to the client.
        u8 sbuf[16];
        const math::Vec3 ap = server.authoritative_pos();
        u32 ax, ay, az;
        __builtin_memcpy(&ax, &ap.x, 4);
        __builtin_memcpy(&ay, &ap.y, 4);
        __builtin_memcpy(&az, &ap.z, 4);
        const u32 acked = server.acked_input();
        auto put = [&](u8* d, u32 v) {
            d[0] = u8(v); d[1] = u8(v >> 8); d[2] = u8(v >> 16); d[3] = u8(v >> 24);
        };
        put(sbuf + 0, ax);
        put(sbuf + 4, ay);
        put(sbuf + 8, az);
        put(sbuf + 12, acked);
        host_s->send(s_to_c, std::span<const u8>(sbuf, sizeof(sbuf)), /*reliable=*/true,
                     kChannelDefault);

        host_s->poll(s_in);
        host_c->poll(c_in);
        for (const InboundMessage& m : c_in) {
            if (m.channel == kChannelDefault && m.bytes.size() == 16) {
                auto get = [&](const u8* d) {
                    return u32(d[0]) | (u32(d[1]) << 8) | (u32(d[2]) << 16) | (u32(d[3]) << 24);
                };
                u32 bx = get(m.bytes.data() + 0), by = get(m.bytes.data() + 4),
                    bz = get(m.bytes.data() + 8), bk = get(m.bytes.data() + 12);
                math::Vec3 ap2;
                __builtin_memcpy(&ap2.x, &bx, 4);
                __builtin_memcpy(&ap2.y, &by, 4);
                __builtin_memcpy(&ap2.z, &bz, 4);
                pred.reconcile(reg, ap2, bk);
            }
        }
        s_in.clear();
        c_in.clear();
        host_s->tick();
        host_c->tick();
    }

    // After the round, the client has fully reconciled: no permanent offset,
    // predicted == authoritative == kTicks units travelled, ring drained.
    CHECK(server.authoritative_pos().x == Catch::Approx(f32(kTicks)).margin(1e-4f));
    CHECK(pred.predicted_pos().x == Catch::Approx(f32(kTicks)).margin(1e-4f));
    CHECK(pred.unacked_count() == 0u);
    CHECK(transform_pos(reg, client_ent).x == Catch::Approx(f32(kTicks)).margin(1e-4f));

    destroy_test_host(host_s);
    destroy_test_host(host_c);
    LoopbackBus::Get().reset();
}
