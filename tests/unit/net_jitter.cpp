// SPDX-License-Identifier: MIT
// Psynder - lane 14 / Wave-C server-side input jitter buffer tests.
//
// InputJitterBuffer reorders bursty / out-of-order arrivals and releases them
// in strict seq order at a steady fixed-step cadence, feeding
// ServerInputProcessor. This makes the authoritative integration (and the
// client's reconciliation against it) tolerate real RTT jitter.
//
// Coverage:
//   * out-of-order arrivals are released in seq order (not arrival order);
//   * a duplicate / stale arrival is dropped, not double-applied;
//   * a lost (never-arriving) input is skipped once the holdback lead is
//     exceeded so the stream keeps moving;
//   * end-to-end: jittered inputs drained through the buffer into the server
//     produce the SAME authoritative position as the in-order stream, and a
//     predictor reconciling against it converges with no permanent offset.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/Jitter.h"
#include "net/Prediction.h"

#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"

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

InputCmd make_cmd(u32 seq, f32 dx) {
    InputCmd c{};
    c.seq = seq;
    c.tick = seq;
    c.move_x = dx;
    return c;
}

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

}  // namespace

TEST_CASE("net: jitter buffer releases out-of-order arrivals in seq order",
          "[net][jitter][reorder]") {
    InputJitterBuffer jb(/*holdback=*/2);

    // Arrive out of order: 1, 3, 2, 4. The buffer must release 1,2,3,4.
    REQUIRE(jb.push(make_cmd(1, 1.f)));
    REQUIRE(jb.push(make_cmd(3, 3.f)));
    REQUIRE(jb.push(make_cmd(2, 2.f)));
    REQUIRE(jb.push(make_cmd(4, 4.f)));

    std::vector<u32> released;
    InputCmd out{};
    while (jb.release(out))
        released.push_back(out.seq);

    CHECK(released == std::vector<u32>{1u, 2u, 3u, 4u});
    CHECK(jb.buffered() == 0u);
}

TEST_CASE("net: jitter buffer drops duplicate and stale arrivals",
          "[net][jitter][dedup]") {
    InputJitterBuffer jb(/*holdback=*/1);

    REQUIRE(jb.push(make_cmd(1, 1.f)));
    CHECK_FALSE(jb.push(make_cmd(1, 9.f)));  // duplicate seq -> dropped.

    InputCmd out{};
    REQUIRE(jb.release(out));
    CHECK(out.seq == 1u);
    CHECK(out.move_x == Catch::Approx(1.f));  // original, not the duplicate.

    // A stale arrival (seq below the now-advanced head) is dropped.
    CHECK_FALSE(jb.push(make_cmd(1, 1.f)));
    CHECK(jb.buffered() == 0u);
}

TEST_CASE("net: jitter buffer skips a lost input once the holdback lead is exceeded",
          "[net][jitter][loss]") {
    InputJitterBuffer jb(/*holdback=*/2);

    // 1 arrives (seeds the expected seq), then seq 2 is LOST while 3,4 arrive.
    // With a holdback of 2, once the lead is buffered the buffer gives up on the
    // missing head (2) and skips forward to 3 so the stream keeps moving.
    REQUIRE(jb.push(make_cmd(1, 1.f)));
    REQUIRE(jb.push(make_cmd(3, 3.f)));
    REQUIRE(jb.push(make_cmd(4, 4.f)));

    std::vector<u32> released;
    InputCmd out{};
    while (jb.release(out))
        released.push_back(out.seq);

    // 1 drains; the missing 2 is skipped; 3,4 drain in order.
    CHECK(released == std::vector<u32>{1u, 3u, 4u});
    CHECK(jb.buffered() == 0u);
}

TEST_CASE("net: jittered inputs converge to the in-order authoritative result",
          "[net][jitter][converge]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    // Ground truth: the in-order integration of inputs 1..6 (each +1 on X).
    constexpr u32 kN = 6;
    math::Vec3 truth{0.f, 0.f, 0.f};
    for (u32 s = 1; s <= kN; ++s)
        truth = step_state(truth, make_cmd(s, 1.f));

    // Server drains a jitter buffer fed in a scrambled, bursty order.
    Entity server_ent = make_controlled(reg, graph, {0.f, 0.f, 0.f});
    ServerInputProcessor server;
    server.bind(server_ent, {0.f, 0.f, 0.f});
    InputJitterBuffer jb(/*holdback=*/2);

    // Bursty/reordered arrival schedule: {2,1}, {4,3,6}, {5}.
    const u32 arrival[] = {2, 1, 4, 3, 6, 5};
    usize ai = 0;
    // Pump several server fixed steps: each step ingests up to two arrivals then
    // releases at most one buffered input in seq order into the processor.
    for (u32 stepi = 0; stepi < 32; ++stepi) {
        for (int k = 0; k < 2 && ai < std::size(arrival); ++k, ++ai)
            jb.push(make_cmd(arrival[ai], 1.f));
        InputCmd out{};
        if (jb.release(out))
            server.process(reg, out);
    }

    // The processor applied every input exactly once, in seq order, so the
    // authoritative position equals the in-order ground truth.
    CHECK(server.authoritative_pos().x == Catch::Approx(truth.x).margin(1e-4f));
    CHECK(server.acked_input() == kN);

    // A predictor that predicted all six inputs reconciles against the jittered
    // authoritative result and converges with no permanent offset.
    Entity client_ent = make_controlled(reg, graph, {0.f, 0.f, 0.f});
    Predictor pred;
    pred.bind(client_ent, {0.f, 0.f, 0.f});
    for (u32 s = 1; s <= kN; ++s)
        pred.predict(reg, s, {1.f, 0.f, 0.f});
    pred.reconcile(reg, server.authoritative_pos(), server.acked_input());
    CHECK(pred.predicted_pos().x == Catch::Approx(truth.x).margin(1e-4f));
    CHECK(pred.unacked_count() == 0u);
}
