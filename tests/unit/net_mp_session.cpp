// SPDX-License-Identifier: MIT
// Psynder - lane 14 / M-NET shared-session integration test. This is the
// unit-suite mirror of games/mp_demo (psynder_mp_demo): one authoritative
// server + two clients sharing a single replicated world, driven end-to-end
// over the in-process LoopbackBus. It guards the exact three behaviours the
// demo's headless smoke asserts, so a regression in the net stack that would
// break the demo is caught here too:
//
//   1. CLIENTS AGREE - for a shared entity both clients can see, each client's
//      replicated transform agrees with the server's authoritative transform
//      within the interpolation tolerance (and the two clients agree with each
//      other).
//   2. AOI DESPAWN   - at least one entity left a client's interest sphere and
//      was torn down on that client.
//   3. BOUNDED PREDICTION - each client's predicted avatar stayed within a
//      small bound of the authoritative position (no runaway / rubber-band).
//
// Deterministic: loopback transport, fixed-step integrator, no RNG / wall-clock.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "net/Aoi.h"
#include "net/Frame.h"
#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Net.h"
#include "net/Prediction.h"
#include "net/Replication.h"

#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"

#include <array>
#include <cmath>
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
    RegistryReset() {
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        LoopbackBus::Get().reset();
    }
    ~RegistryReset() {
        psynder::scene::detail::EcsRegistryImpl::Get().shutdown();
        LoopbackBus::Get().reset();
    }
};

constexpr u32 kNumBots = 6u;
constexpr u32 kBotIdBase = 1u;
constexpr u32 kAvatarIdBase = kBotIdBase + kNumBots;
constexpr f32 kAoiRadius = 22.0f;
constexpr f32 kMapX = 90.0f;
constexpr f32 kBotSpan = 14.0f;
constexpr f32 kBotSpeed = 0.6f;
constexpr u32 kReportBytes = 16u;

void put_u32(u8* d, u32 v) noexcept {
    d[0] = u8(v); d[1] = u8(v >> 8); d[2] = u8(v >> 16); d[3] = u8(v >> 24);
}
u32 get_u32(const u8* d) noexcept {
    return u32(d[0]) | (u32(d[1]) << 8) | (u32(d[2]) << 16) | (u32(d[3]) << 24);
}
void put_f32(u8* d, f32 v) noexcept {
    u32 b; __builtin_memcpy(&b, &v, 4); put_u32(d, b);
}
f32 get_f32(const u8* d) noexcept {
    const u32 b = get_u32(d); f32 v; __builtin_memcpy(&v, &b, 4); return v;
}

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

math::Vec3 transform_pos(EcsRegistry& reg, Entity e) {
    const auto* tc = reg.get<TransformComponent>(e);
    return tc ? tc->local.translation : math::Vec3{1e9f, 1e9f, 1e9f};
}

math::Vec3 bot_path(u32 bot_index, u32 tick) {
    const f32 centre = (static_cast<f32>(bot_index) + 0.5f) * (kMapX / static_cast<f32>(kNumBots));
    const f32 phase =
        static_cast<f32>(tick) * kBotSpeed + static_cast<f32>(bot_index) * 5.0f;
    const f32 period = 4.0f * kBotSpan;
    f32 s = std::fmod(phase, period);
    if (s < 0.f) s += period;
    f32 tri = (s < 2.0f * kBotSpan) ? (s / kBotSpan - 1.0f) : (3.0f - s / kBotSpan);
    return math::Vec3{centre + tri * kBotSpan, 0.f, 0.f};
}

}  // namespace

TEST_CASE("net: shared session - 2 clients converge, AOI despawns, prediction bounded",
          "[net][mp][session][integration]") {
    RegistryReset reset;
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    const ReplicatedComponentSet set = make_set();
    ReplicationServer server(set);

    HostDesc sd{};
    sd.port = 42000;
    sd.max_peers = 8;
    HostImpl* server_host = make_test_host(sd);
    REQUIRE(server_host);

    // Server world: bots + one authoritative avatar per client.
    std::array<Entity, kNumBots> bots{};
    for (u32 b = 0; b < kNumBots; ++b)
        bots[b] = spawn_server_entity(reg, graph, kBotIdBase + b, bot_path(b, 0));

    constexpr u32 kClients = 2u;
    std::array<Entity, kClients> server_avatars{};
    std::array<ServerInputProcessor, kClients> server_inputs{};
    std::array<HostImpl*, kClients> client_hosts{};
    std::array<PeerId, kClients> to_server{};
    std::array<u32, kClients> client_key{};
    std::array<math::Vec3, kClients> avatar_start{};
    std::array<Entity, kClients> predicted_avatar{};
    std::vector<ReplicationClient> repl;
    std::vector<Predictor> predictor(kClients);
    AoiFilter aoi;

    repl.reserve(kClients);
    for (u32 c = 0; c < kClients; ++c) {
        const f32 sx = (c == 0u) ? 6.0f : (kMapX - 6.0f);
        avatar_start[c] = math::Vec3{sx, 0.f, 0.f};
        server_avatars[c] = spawn_server_entity(reg, graph, kAvatarIdBase + c, avatar_start[c]);
        server_inputs[c].bind(server_avatars[c], avatar_start[c]);

        HostDesc cd{};
        cd.port = static_cast<u16>(42001 + c);
        cd.max_peers = 4;
        client_hosts[c] = make_test_host(cd);
        REQUIRE(client_hosts[c]);
        to_server[c] = client_hosts[c]->connect(sd.port);
        repl.emplace_back(set);
    }
    for (u32 c = 0; c < kClients; ++c) {
        const PeerId s_to_c = server_host->connect(static_cast<u16>(42001 + c));
        client_key[c] = s_to_c.raw;
        aoi.set_peer(s_to_c, avatar_start[c], kAoiRadius);

        LocalTransform local{};
        local.translation = avatar_start[c];
        predicted_avatar[c] =
            psynder::scene::create_scene_entity(reg, graph, psynder::scene::kInvalidSceneNode, local);
        PredictedComponent pc{};
        pc.pos = avatar_start[c];
        reg.add<PredictedComponent>(predicted_avatar[c], pc);
        predictor[c].bind(predicted_avatar[c], avatar_start[c]);
    }

    std::vector<InboundMessage> server_inbox;
    std::array<std::vector<InboundMessage>, kClients> client_inbox;
    std::vector<u8> snap;

    std::array<bool, kClients> observed_despawn{false, false};
    std::array<f32, kClients> worst_pred{0.f, 0.f};
    bool agree_failed = false;
    u32 agree_samples = 0;

    constexpr f32 kAgreeTol = 1.25f;
    constexpr f32 kPredBound = 2.5f;
    constexpr u32 kTicks = 240u;

    for (u32 tick = 1; tick <= kTicks; ++tick) {
        // 1. Clients predict + ship input.
        for (u32 c = 0; c < kClients; ++c) {
            const f32 dir = (c == 0u) ? +0.55f : -0.55f;
            const InputCmd cmd = predictor[c].predict(reg, tick, math::Vec3{dir, 0.f, 0.f});
            std::array<u8, kInputCmdBytes> ibuf{};
            encode_input(cmd, std::span<u8>(ibuf.data(), ibuf.size()));
            client_hosts[c]->send(to_server[c], std::span<const u8>(ibuf.data(), ibuf.size()),
                                  true, kChannelDefault);
        }
        // 2. Server applies inputs per source peer.
        for (u32 c = 0; c < kClients; ++c) {
            client_inbox[c].clear();
            client_hosts[c]->poll(client_inbox[c]);
        }
        server_inbox.clear();
        server_host->poll(server_inbox);
        for (const InboundMessage& m : server_inbox) {
            if (m.channel != kChannelDefault || m.bytes.size() != kInputCmdBytes)
                continue;
            InputCmd in{};
            decode_input(std::span<const u8>(m.bytes.data(), m.bytes.size()), in);
            for (u32 c = 0; c < kClients; ++c)
                if (client_key[c] == m.from.raw) {
                    server_inputs[c].process(reg, in);
                    set_server_pos(reg, server_avatars[c], server_inputs[c].authoritative_pos());
                    break;
                }
        }
        // 3. Bots advance.
        for (u32 b = 0; b < kNumBots; ++b)
            set_server_pos(reg, bots[b], bot_path(b, tick));
        // 4. Per-client AOI snapshot + authoritative report.
        for (u32 c = 0; c < kClients; ++c) {
            const math::Vec3 apos = server_inputs[c].authoritative_pos();
            aoi.set_peer(PeerId{client_key[c]}, apos, kAoiRadius);
            snap.clear();
            server.serialize_snapshot_aoi(reg, client_key[c], tick, aoi, PeerId{client_key[c]}, 0,
                                          snap);
            server_host->send(PeerId{client_key[c]}, std::span<const u8>(snap.data(), snap.size()),
                              false, kChannelSnapshot);
            std::array<u8, kReportBytes> rep{};
            put_f32(rep.data() + 0, apos.x);
            put_f32(rep.data() + 4, apos.y);
            put_f32(rep.data() + 8, apos.z);
            put_u32(rep.data() + 12, server_inputs[c].acked_input());
            server_host->send(PeerId{client_key[c]}, std::span<const u8>(rep.data(), rep.size()),
                              true, kChannelDefault);
        }
        // 5. Clients apply + reconcile.
        server_host->poll(server_inbox);  // flush.
        for (u32 c = 0; c < kClients; ++c) {
            client_inbox[c].clear();
            client_hosts[c]->poll(client_inbox[c]);
            for (const InboundMessage& m : client_inbox[c]) {
                if (m.channel == kChannelSnapshot && m.bytes.size() >= kReplHeaderBytes) {
                    std::array<bool, 32> before{};
                    for (u32 id = 1; id < 32; ++id)
                        before[id] = repl[c].entity_for(id).valid();
                    repl[c].apply_snapshot(reg, std::span<const u8>(m.bytes.data(), m.bytes.size()));
                    std::array<u8, 4> abuf{};
                    put_u32(abuf.data(), repl[c].ack_seq());
                    client_hosts[c]->send(to_server[c], std::span<const u8>(abuf.data(), 4), false,
                                          kChannelSnapshot);
                    for (u32 id = 1; id < 32; ++id)
                        if (before[id] && !repl[c].entity_for(id).valid())
                            observed_despawn[c] = true;
                } else if (m.channel == kChannelDefault && m.bytes.size() == kReportBytes) {
                    math::Vec3 auth{get_f32(m.bytes.data() + 0), get_f32(m.bytes.data() + 4),
                                    get_f32(m.bytes.data() + 8)};
                    predictor[c].reconcile(reg, auth, get_u32(m.bytes.data() + 12));
                    const f32 err = std::fabs(predictor[c].predicted_pos().x - auth.x);
                    if (err > worst_pred[c]) worst_pred[c] = err;
                }
            }
            repl[c].interpolate_transforms(reg, 1.0f);
        }
        // 6. Server drains acks + ticks.
        server_inbox.clear();
        server_host->poll(server_inbox);
        for (const InboundMessage& m : server_inbox)
            if (m.channel == kChannelSnapshot && m.bytes.size() == 4)
                for (u32 c = 0; c < kClients; ++c)
                    if (client_key[c] == m.from.raw)
                        server.ack_from_client(client_key[c], get_u32(m.bytes.data()));
        server_host->tick();
        for (u32 c = 0; c < kClients; ++c) client_hosts[c]->tick();

        // 7. Convergence sampling: a bot both clients can see.
        for (u32 b = 0; b < kNumBots; ++b) {
            const u32 id = kBotIdBase + b;
            const Entity e0 = repl[0].entity_for(id);
            const Entity e1 = repl[1].entity_for(id);
            if (!e0.valid() || !e1.valid()) continue;
            const f32 srv = transform_pos(reg, bots[b]).x;
            const f32 d0 = std::fabs(transform_pos(reg, e0).x - srv);
            const f32 d1 = std::fabs(transform_pos(reg, e1).x - srv);
            const f32 dcc = std::fabs(transform_pos(reg, e0).x - transform_pos(reg, e1).x);
            ++agree_samples;
            if (d0 > kAgreeTol || d1 > kAgreeTol || dcc > kAgreeTol) agree_failed = true;
        }
    }

    // The two clients sweep until their AOI spheres overlap shared bots.
    CHECK(agree_samples > 0u);
    CHECK_FALSE(agree_failed);
    // At least one client saw a bot leave its interest sphere and despawn.
    CHECK((observed_despawn[0] || observed_despawn[1]));
    // Prediction never diverged from authority (perfect on the same-tick
    // loopback; the bound guards against any runaway).
    CHECK(worst_pred[0] <= kPredBound);
    CHECK(worst_pred[1] <= kPredBound);

    for (u32 c = 0; c < kClients; ++c) destroy_test_host(client_hosts[c]);
    destroy_test_host(server_host);
}
