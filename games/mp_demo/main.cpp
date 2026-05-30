// SPDX-License-Identifier: MIT
// Psynder - psynder_mp_demo - DEMO GAME 5 (M-NET reference). One authoritative
// SERVER + TWO CLIENTS sharing a single replicated world, end-to-end, in one
// process. This is the runnable reference for the engine's netcode stack
// (engine/net): reliable-UDP HostImpl over the in-process LoopbackBus, the
// AOI-gated snapshot+delta replication layer (Replication.h), client-side
// prediction + server reconciliation (Prediction.h), and snapshot
// interpolation on the receiving clients.
//
// THE SHARED SESSION
//   The server simulates a small shared world:
//     * kNumBots bots walking deterministic linear ping-pong paths along X,
//       spread across the map so each client only ever sees a subset (AOI),
//     * one AVATAR entity per client, moved authoritatively by that client's
//       input (a ServerInputProcessor per client).
//   Every server entity is tagged NetId + SceneNode + Transform so the
//   replication gather query picks it up.
//
//   Each client owns a ReplicationClient (decodes snapshots, maps net_ids to
//   its OWN local entity rows, interpolates remote transforms) and a Predictor
//   bound to a locally-predicted copy of its avatar (zero-latency local input,
//   reconciled against the authoritative snapshot each tick). Its AOI interest
//   sphere is centred on its avatar, so as the two avatars roam the map the set
//   of bots each client sees changes - and a bot LEAVING a client's sphere is
//   despawned on that client.
//
// THE TRANSPORT IS REAL (for an in-process bus)
//   Inputs ride channel 0 (reliable), snapshots ride channel 2 (unreliable,
//   latest-wins) exactly as a shipping client/server would. Three HostImpls
//   (server + 2 clients) talk over the deterministic LoopbackBus, so the whole
//   smoke is reproducible with zero wall-clock / RNG / socket dependence.
//
// WHAT THE SMOKE PROVES (--smoke-frames=N, headless, exit 0 on PASS)
//   1. CLIENTS AGREE  - for a shared entity both clients can see, each client's
//      replicated transform agrees with the server's authoritative transform
//      within the interpolation tolerance.
//   2. AOI DESPAWN    - at least one entity left a client's interest sphere and
//      was torn down on that client (mapped_count dropped for a net_id the
//      client previously held).
//   3. BOUNDED PREDICTION - each client's predicted avatar position stayed
//      within a small bound of the authoritative position every tick (no
//      runaway / rubber-band).
//   A single greppable line "psynder_mp_demo: ... PASS|FAIL" is printed and the
//   process exits 0 (PASS) / 1 (FAIL).
//
// ALLOC-FREE STEADY STATE
//   All per-tick scratch (snapshot byte buffers, the host inbox vectors, the
//   input ring) is pooled and reused; the steady-state loop performs no heap
//   allocation. Entity spawns happen once at startup; AOI re-spawns reuse the
//   client's pooled net_id->Entity map.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "net/Aoi.h"
#include "net/Frame.h"
#include "net/HostImpl.h"
#include "net/Loopback.h"
#include "net/Matchmaking.h"
#include "net/Net.h"
#include "net/Prediction.h"
#include "net/Replication.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneEcs.h"
#include "scene/SceneGraph.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;
using psynder::scene::EcsRegistry;
using psynder::scene::LocalTransform;
using psynder::scene::SceneGraph;
using psynder::scene::TransformComponent;

namespace {

// --- Session shape ----------------------------------------------------------
constexpr u32 kNumBots = 6u;       // deterministic ping-pong bots along X.
constexpr u32 kNumClients = 2u;    // the two players sharing the session.
constexpr f32 kAoiRadius = 22.0f;  // each client's interest sphere radius.

// Map extents: bots spread along X from 0..kMapX so a client centred on its
// avatar only sees the nearby ones. Wide enough that bots cross in/out of the
// per-client sphere as the avatars move, exercising AOI spawn + despawn.
constexpr f32 kMapX = 90.0f;
constexpr f32 kBotSpan = 14.0f;  // half-amplitude of each bot's ping-pong.
constexpr f32 kBotSpeed = 0.6f;  // bot displacement per tick along its path.

// Net ids. Bots take 1..kNumBots; avatars take the ids right after.
constexpr u32 kBotIdBase = 1u;
constexpr u32 kAvatarIdBase = kBotIdBase + kNumBots;  // 7, 8 for clients 0,1.

// Loopback ports for the three hosts.
constexpr u16 kServerPort = 41000u;
constexpr u16 kClientPort0 = 41001u;

// Per-tick snapshot byte budget per client (0 == uncapped). Generous: we want
// AOI sphere gating to drive despawns here, not the byte budget.
constexpr u32 kSnapshotBudget = 0u;

// Tolerances for the PASS assertions.
constexpr f32 kAgreeTol = 1.25f;       // client-vs-server transform agreement.
constexpr f32 kPredictionBound = 2.5f; // |predicted - authoritative| per tick.

// --- Wire helpers: server->client authoritative-avatar report (channel 0) ---
// Carries the client's authoritative avatar position + the highest input seq
// the server processed, so the client can reconcile. Little-endian, fixed 16
// bytes (mirrors the prediction unit test's hand-serialized report).
constexpr usize kReportBytes = 16;

void put_u32(u8* d, u32 v) noexcept {
    d[0] = u8(v);
    d[1] = u8(v >> 8);
    d[2] = u8(v >> 16);
    d[3] = u8(v >> 24);
}
u32 get_u32(const u8* d) noexcept {
    return u32(d[0]) | (u32(d[1]) << 8) | (u32(d[2]) << 16) | (u32(d[3]) << 24);
}
void put_f32(u8* d, f32 v) noexcept {
    u32 bits;
    __builtin_memcpy(&bits, &v, 4);
    put_u32(d, bits);
}
f32 get_f32(const u8* d) noexcept {
    const u32 bits = get_u32(d);
    f32 v;
    __builtin_memcpy(&v, &bits, 4);
    return v;
}

math::Vec3 transform_pos(EcsRegistry& reg, Entity e) noexcept {
    const auto* tc = reg.get<TransformComponent>(e);
    return tc ? tc->local.translation : math::Vec3{1e9f, 1e9f, 1e9f};
}

// The fixed set of replicated components. Server + client register the SAME
// components in the SAME order so mask bits agree. Only TransformComponent is
// replicated here (the interpolated remote state); the avatar's local
// prediction is reconciled separately via the authoritative report.
net::ReplicatedComponentSet make_set() {
    net::ReplicatedComponentSet set;
    set.add<TransformComponent>();
    return set;
}

// One bot's deterministic ping-pong path. Position is a pure function of tick:
// triangle wave along X about a fixed centre. No RNG, no integration drift.
math::Vec3 bot_path(u32 bot_index, u32 tick) noexcept {
    const f32 centre = (static_cast<f32>(bot_index) + 0.5f) *
                       (kMapX / static_cast<f32>(kNumBots));
    const f32 phase = static_cast<f32>(tick) * kBotSpeed +
                      static_cast<f32>(bot_index) * 5.0f;
    // Triangle wave in [-1, 1] from a sawtooth, period 4*kBotSpan.
    const f32 period = 4.0f * kBotSpan;
    f32 s = std::fmod(phase, period);
    if (s < 0.f)
        s += period;
    f32 tri;
    if (s < 2.0f * kBotSpan)
        tri = s / kBotSpan - 1.0f;  // -1 -> 1
    else
        tri = 3.0f - s / kBotSpan;  // 1 -> -1
    return math::Vec3{centre + tri * kBotSpan, 0.f, 0.f};
}

// Each client's intended avatar move for this tick: a steady sweep across the
// map so the two avatars roam through different bot neighbourhoods (driving the
// per-client AOI sets apart). Deterministic per (client, tick).
math::Vec3 avatar_move(u32 client_index, u32 tick) noexcept {
    (void)tick;
    // Client 0 sweeps +X, client 1 sweeps +X starting from the far end via a
    // different sign window so the two visit disjoint bot sets over the run.
    const f32 dir = (client_index == 0u) ? +1.0f : -1.0f;
    return math::Vec3{dir * 0.55f, 0.f, 0.f};
}

// --- Per-client runtime state ----------------------------------------------
struct ClientRuntime {
    net::HostImpl* host = nullptr;     // this client's loopback host.
    net::PeerId to_server{};           // peer handle: client -> server.
    net::ReplicationClient repl;       // decodes server snapshots.
    net::Predictor predictor;          // local prediction for the avatar.
    Entity predicted_avatar = kInvalidEntity;  // client-local predicted row.
    math::Vec3 avatar_start{0.f, 0.f, 0.f};
    u32 client_key = 0;                // server-side key == server's PeerId.raw.
    // True once we have observed a net_id that was mapped on a prior tick
    // disappear from this client (an AOI-leave despawn).
    bool observed_despawn = false;
    // Pooled per-tick scratch (alloc-free steady state).
    std::vector<net::InboundMessage> inbox;
    f32 max_prediction_error = 0.f;

    explicit ClientRuntime(const net::ReplicatedComponentSet& set) : repl(set) {}
};

// Build a server-side replicated entity at `pos` with the given net_id.
Entity spawn_server_entity(EcsRegistry& reg, SceneGraph& graph, u32 net_id,
                           math::Vec3 pos) noexcept {
    LocalTransform local{};
    local.translation = pos;
    const Entity e =
        scene::create_scene_entity(reg, graph, scene::kInvalidSceneNode, local);
    net::NetIdComponent tag{};
    tag.net_id = net_id;
    reg.add<net::NetIdComponent>(e, tag);
    return e;
}

void set_server_pos(EcsRegistry& reg, Entity e, math::Vec3 pos) noexcept {
    if (auto* tc = reg.get<TransformComponent>(e))
        tc->local.translation = pos;
}

// The original Wave-9 in-process server + 2-clients shared-session smoke. Kept
// as the default mode; the dedicated-server + matchmaking smoke below is a
// separate mode selected by --dedicated / --server.
int run_session_demo(u32 frames) {
    // --- Bring up the shared registry + the three hosts ---------------------
    scene::detail::EcsRegistryImpl::Get().shutdown();  // clean slate.
    net::LoopbackBus::Get().reset();
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    const net::ReplicatedComponentSet set = make_set();
    net::ReplicationServer server(set);

    net::HostDesc server_desc{};
    server_desc.port = kServerPort;
    server_desc.max_peers = 8;
    net::HostImpl* server_host = net::make_test_host(server_desc);
    if (!server_host) {
        std::printf("psynder_mp_demo: server host failed to start FAIL\n");
        return EXIT_FAILURE;
    }

    // --- Server world: bots + one avatar per client -------------------------
    std::array<Entity, kNumBots> bots{};
    for (u32 b = 0; b < kNumBots; ++b)
        bots[b] = spawn_server_entity(reg, graph, kBotIdBase + b, bot_path(b, 0));

    // Per-client server-side state: the authoritative avatar entity, the input
    // processor, and the AOI filter peer entry.
    std::array<Entity, kNumClients> server_avatars{};
    std::array<net::ServerInputProcessor, kNumClients> server_inputs{};
    net::AoiFilter aoi;

    std::vector<ClientRuntime> clients;
    clients.reserve(kNumClients);

    for (u32 c = 0; c < kNumClients; ++c) {
        // Avatars start at opposite ends so the two clients begin with disjoint
        // bot neighbourhoods and sweep toward / through each other.
        const f32 start_x = (c == 0u) ? 6.0f : (kMapX - 6.0f);
        const math::Vec3 start{start_x, 0.f, 0.f};
        const u32 avatar_id = kAvatarIdBase + c;
        server_avatars[c] = spawn_server_entity(reg, graph, avatar_id, start);
        server_inputs[c].bind(server_avatars[c], start);

        clients.emplace_back(set);
        ClientRuntime& cr = clients.back();
        cr.avatar_start = start;
        cr.inbox.reserve(32);

        net::HostDesc cd{};
        cd.port = static_cast<u16>(kClientPort0 + c);
        cd.max_peers = 4;
        cr.host = net::make_test_host(cd);
        if (!cr.host) {
            std::printf("psynder_mp_demo: client %u host failed FAIL\n", c);
            return EXIT_FAILURE;
        }
        cr.to_server = cr.host->connect(kServerPort);
        // The server learns this peer's key when it first receives a datagram
        // (auto-register-on-first-datagram). We key the server's per-client
        // state + AOI off the server-side PeerId.raw, established below.
    }

    // Establish the server's peer handles by connecting back to each client and
    // seeding the AOI peer entry. Keying both the replication client_key and the
    // AOI peer on the SAME server-side PeerId.raw keeps the gate consistent.
    for (u32 c = 0; c < kNumClients; ++c) {
        const net::PeerId s_to_c =
            server_host->connect(static_cast<u16>(kClientPort0 + c));
        clients[c].client_key = s_to_c.raw;
        aoi.set_peer(s_to_c, clients[c].avatar_start, kAoiRadius);
    }

    // --- Each client's locally-predicted avatar -----------------------------
    // A client-owned entity row (Transform + PredictedComponent) the Predictor
    // drives at zero latency, reconciled against the server report each tick.
    for (u32 c = 0; c < kNumClients; ++c) {
        ClientRuntime& cr = clients[c];
        LocalTransform local{};
        local.translation = cr.avatar_start;
        cr.predicted_avatar =
            scene::create_scene_entity(reg, graph, scene::kInvalidSceneNode, local);
        net::PredictedComponent pc{};
        pc.pos = cr.avatar_start;
        reg.add<net::PredictedComponent>(cr.predicted_avatar, pc);
        cr.predictor.bind(cr.predicted_avatar, cr.avatar_start);
    }

    // --- Pooled per-tick scratch (no heap alloc in steady state) ------------
    std::vector<net::InboundMessage> server_inbox;
    server_inbox.reserve(64);
    std::vector<u8> snap_scratch;  // reused snapshot byte buffer.
    snap_scratch.reserve(2048);

    // Smoke accumulators.
    bool agree_failed = false;
    u32 agree_samples = 0;  // ticks where both clients shared a visible entity.

    PSY_LOG_INFO("psynder_mp_demo: server + {} clients up, {} bots, {} ticks",
                 kNumClients, kNumBots, frames);

    // ===================== MAIN SHARED-SESSION LOOP =========================
    for (u32 tick = 1; tick <= frames; ++tick) {
        // --- 1. Clients predict local input + ship it to the server ---------
        for (u32 c = 0; c < kNumClients; ++c) {
            ClientRuntime& cr = clients[c];
            const math::Vec3 move = avatar_move(c, tick);
            const net::InputCmd cmd = cr.predictor.predict(reg, tick, move);
            std::array<u8, net::kInputCmdBytes> ibuf{};
            net::encode_input(cmd, std::span<u8>(ibuf.data(), ibuf.size()));
            cr.host->send(cr.to_server,
                          std::span<const u8>(ibuf.data(), ibuf.size()),
                          /*reliable=*/true, net::kChannelDefault);
        }

        // --- 2. Pump client->server transport; server applies inputs --------
        // Flush each client's outbound queue, then drain the server inbox and
        // apply every input to the ServerInputProcessor of the client it came
        // from. The server connected back to each client on a distinct port, so
        // the server-side peer handle for a client's datagram (InboundMessage
        // .from) equals the handle we stored as client_key. That identity is
        // how we route an input to the right avatar.
        for (u32 c = 0; c < kNumClients; ++c) {
            clients[c].inbox.clear();
            clients[c].host->poll(clients[c].inbox);  // flush sends.
        }
        server_inbox.clear();
        server_host->poll(server_inbox);
        for (const net::InboundMessage& m : server_inbox) {
            if (m.channel != net::kChannelDefault || m.bytes.size() != net::kInputCmdBytes)
                continue;
            net::InputCmd in{};
            net::decode_input(std::span<const u8>(m.bytes.data(), m.bytes.size()), in);
            for (u32 c = 0; c < kNumClients; ++c) {
                if (clients[c].client_key == m.from.raw) {
                    server_inputs[c].process(reg, in);
                    set_server_pos(reg, server_avatars[c],
                                   server_inputs[c].authoritative_pos());
                    break;
                }
            }
        }

        // --- 3. Advance bots authoritatively (deterministic paths) ----------
        for (u32 b = 0; b < kNumBots; ++b)
            set_server_pos(reg, bots[b], bot_path(b, tick));

        // --- 4. Per-client: update AOI centre, serialize AOI snapshot -------
        for (u32 c = 0; c < kNumClients; ++c) {
            ClientRuntime& cr = clients[c];
            const math::Vec3 avatar_pos = server_inputs[c].authoritative_pos();
            // Re-key the AOI peer with the avatar's current position so the
            // interest sphere follows the player (drives bot spawn/despawn).
            aoi.set_peer(net::PeerId{cr.client_key}, avatar_pos, kAoiRadius);

            snap_scratch.clear();
            server.serialize_snapshot_aoi(reg, cr.client_key, tick, aoi,
                                          net::PeerId{cr.client_key},
                                          kSnapshotBudget, snap_scratch);
            // Ship the snapshot on the snapshot channel (unreliable, latest-wins).
            server_host->send(net::PeerId{cr.client_key},
                              std::span<const u8>(snap_scratch.data(), snap_scratch.size()),
                              /*reliable=*/false, net::kChannelSnapshot);

            // Server reports the client's authoritative avatar state for
            // reconciliation on the default channel.
            std::array<u8, kReportBytes> rep{};
            put_f32(rep.data() + 0, avatar_pos.x);
            put_f32(rep.data() + 4, avatar_pos.y);
            put_f32(rep.data() + 8, avatar_pos.z);
            put_u32(rep.data() + 12, server_inputs[c].acked_input());
            server_host->send(net::PeerId{cr.client_key},
                              std::span<const u8>(rep.data(), rep.size()),
                              /*reliable=*/true, net::kChannelDefault);
        }

        // --- 5. Pump server->client transport; clients apply + reconcile ----
        server_host->poll(server_inbox);  // flush server sends.
        for (u32 c = 0; c < kNumClients; ++c) {
            ClientRuntime& cr = clients[c];
            cr.inbox.clear();
            cr.host->poll(cr.inbox);

            // Record the client's pre-apply mapped set so we can detect a
            // despawn (a previously-mapped net_id that disappears this tick).
            for (const net::InboundMessage& m : cr.inbox) {
                if (m.channel == net::kChannelSnapshot) {
                    // Snapshot the "before" mapping for despawn detection.
                    std::array<bool, 64> before{};
                    for (u32 id = 1; id < 64; ++id)
                        before[id] = cr.repl.entity_for(id).valid();

                    cr.repl.apply_snapshot(
                        reg, std::span<const u8>(m.bytes.data(), m.bytes.size()));

                    // Echo the ack back on the snapshot channel.
                    const u32 ack = cr.repl.ack_seq();
                    std::array<u8, 4> abuf{};
                    put_u32(abuf.data(), ack);
                    cr.host->send(cr.to_server,
                                  std::span<const u8>(abuf.data(), abuf.size()),
                                  /*reliable=*/false, net::kChannelSnapshot);

                    // Detect AOI despawn: a net_id mapped before, gone after.
                    for (u32 id = 1; id < 64; ++id) {
                        if (before[id] && !cr.repl.entity_for(id).valid())
                            cr.observed_despawn = true;
                    }
                } else if (m.channel == net::kChannelDefault &&
                           m.bytes.size() == kReportBytes) {
                    // Authoritative avatar report -> reconcile prediction.
                    math::Vec3 auth{};
                    auth.x = get_f32(m.bytes.data() + 0);
                    auth.y = get_f32(m.bytes.data() + 4);
                    auth.z = get_f32(m.bytes.data() + 8);
                    const u32 acked = get_u32(m.bytes.data() + 12);
                    cr.predictor.reconcile(reg, auth, acked);
                    const f32 err = std::fabs(cr.predictor.predicted_pos().x - auth.x);
                    if (err > cr.max_prediction_error)
                        cr.max_prediction_error = err;
                }
            }
            // Interpolate remote transforms toward the newest snapshot. At the
            // end of the tick we sample at t=1 (snap to newest) for the
            // agreement check; a renderer would pass a sub-tick fraction.
            cr.repl.interpolate_transforms(reg, 1.0f);
        }

        // --- 6. Server drains client acks; advance logical ticks ------------
        server_inbox.clear();
        server_host->poll(server_inbox);
        for (const net::InboundMessage& m : server_inbox) {
            if (m.channel == net::kChannelSnapshot && m.bytes.size() == 4) {
                const u32 ack = get_u32(m.bytes.data());
                for (u32 c = 0; c < kNumClients; ++c)
                    if (clients[c].client_key == m.from.raw)
                        server.ack_from_client(clients[c].client_key, ack);
            }
        }
        server_host->tick();
        for (u32 c = 0; c < kNumClients; ++c)
            clients[c].host->tick();

        // --- 7. Convergence sampling: a shared entity both clients can see --
        // The two avatars sweep toward each other; near the midpoint both
        // clients' AOI spheres overlap a common bot. Whenever BOTH clients map
        // the same bot net_id, assert each client's replicated transform agrees
        // with the server's authoritative transform within tolerance.
        for (u32 b = 0; b < kNumBots; ++b) {
            const u32 id = kBotIdBase + b;
            const Entity e0 = clients[0].repl.entity_for(id);
            const Entity e1 = clients[1].repl.entity_for(id);
            if (!e0.valid() || !e1.valid())
                continue;
            const math::Vec3 srv = transform_pos(reg, bots[b]);
            const math::Vec3 c0 = transform_pos(reg, e0);
            const math::Vec3 c1 = transform_pos(reg, e1);
            const f32 d0 = std::fabs(c0.x - srv.x);
            const f32 d1 = std::fabs(c1.x - srv.x);
            const f32 dcc = std::fabs(c0.x - c1.x);
            ++agree_samples;
            if (d0 > kAgreeTol || d1 > kAgreeTol || dcc > kAgreeTol) {
                agree_failed = true;
                PSY_LOG_WARN(
                    "psynder_mp_demo: tick {} bot {} disagreement d0={} d1={} dcc={}",
                    tick, id, d0, d1, dcc);
            }
        }
    }
    // ====================== END SESSION LOOP ================================

    // --- Tally the three PASS conditions ------------------------------------
    const bool any_despawn =
        clients[0].observed_despawn || clients[1].observed_despawn;
    f32 worst_prediction = 0.f;
    for (u32 c = 0; c < kNumClients; ++c)
        if (clients[c].max_prediction_error > worst_prediction)
            worst_prediction = clients[c].max_prediction_error;
    const bool prediction_ok = worst_prediction <= kPredictionBound;
    const bool agree_ok = agree_samples > 0u && !agree_failed;

    const bool pass = agree_ok && any_despawn && prediction_ok;

    PSY_LOG_INFO(
        "psynder_mp_demo: agree_samples={} agree_ok={} despawn[c0={} c1={}] "
        "worst_pred_err={}",
        agree_samples, agree_ok, clients[0].observed_despawn,
        clients[1].observed_despawn, worst_prediction);

    std::printf(
        "psynder_mp_demo: frames=%u clients-agree=%d aoi-despawn=%d "
        "bounded-prediction=%d (worst_err=%.3f) %s\n",
        frames, agree_ok ? 1 : 0, any_despawn ? 1 : 0, prediction_ok ? 1 : 0,
        static_cast<double>(worst_prediction), pass ? "PASS" : "FAIL");
    std::fflush(stdout);

    // --- Teardown -----------------------------------------------------------
    for (u32 c = 0; c < kNumClients; ++c)
        net::destroy_test_host(clients[c].host);
    net::destroy_test_host(server_host);
    net::LoopbackBus::Get().reset();
    scene::detail::EcsRegistryImpl::Get().shutdown();

    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ============================================================================
// DEDICATED-SERVER + MATCHMAKING demo (Wave 11, --dedicated / --server).
//
// A standalone authoritative net::DedicatedServer (no local avatar) runs the
// world sim + accepts client JOINs via the matchmaker, assigns lobby slots,
// spawns per-client avatars, replicates AOI snapshots, and processes client
// inputs. Several lightweight clients matchmake (find-or-create + join) into it
// over the deterministic LoopbackBus. The smoke proves, headless:
//   * two clients matchmake + JOIN and get DISTINCT slots / controlled net_ids,
//   * a THIRD join is REJECTED when the single session is at its small cap,
//   * a client LEAVES, freeing its slot, and the rejected client RE-JOINS into
//     the freed slot,
//   * every admitted client sees the shared world (a server-owned bot),
//   * the server SHUTS DOWN cleanly once the last client leaves.
// Emits a single greppable line and exits 0 (PASS) / 1 (FAIL).
// ----------------------------------------------------------------------------

// A server-owned shared-world bot, advanced each server step so every admitted
// client replicates it. Deterministic ping-pong along X.
constexpr u32 kDedBotNetId = 7000u;
Entity g_ded_bot = kInvalidEntity;

void ded_world_step(EcsRegistry& reg, u32 tick, void* /*user*/) noexcept {
    if (!g_ded_bot.valid())
        return;
    if (auto* tc = reg.get<TransformComponent>(g_ded_bot)) {
        const f32 x = 24.0f + 5.0f * static_cast<f32>(tick % 8);
        tc->local.translation = math::Vec3{x, 0.f, 0.f};
    }
}

// One matchmaking client: its own loopback host + a ReplicationClient. Drives a
// find-or-create JOIN, decodes the accept/deny reply, and replicates snapshots.
struct MatchClient {
    net::HostImpl* host = nullptr;
    net::PeerId to_server{};
    u32 server_key = 0;
    net::ReplicationClient repl;
    bool joined = false;
    bool rejected = false;
    net::MatchDeny last_deny = net::MatchDeny::ServerFull;
    u32 controlled_net_id = 0;
    bool saw_bot = false;
    std::vector<net::InboundMessage> inbox;
    explicit MatchClient(const net::ReplicatedComponentSet& s) : repl(s) {}
};

void mc_send_join(MatchClient& c) {
    net::MatchJoinMsg req{};  // session_id 0 == find-or-create-any.
    std::array<u8, net::kMatchJoinBytes> jb{};
    net::encode_match_join(req, std::span<u8>(jb.data(), jb.size()));
    c.host->send(c.to_server, std::span<const u8>(jb.data(), jb.size()), true,
                 net::kChannelDefault);
}

void mc_send_leave(MatchClient& c) {
    net::MatchLeaveMsg bye{};
    std::array<u8, net::kMatchLeaveBytes> lb{};
    net::encode_match_leave(bye, std::span<u8>(lb.data(), lb.size()));
    c.host->send(c.to_server, std::span<const u8>(lb.data(), lb.size()), true,
                 net::kChannelDefault);
}

void mc_pump(MatchClient& c, EcsRegistry& reg) {
    c.inbox.clear();
    c.host->poll(c.inbox);
    for (const net::InboundMessage& m : c.inbox) {
        if (m.channel == net::kChannelSnapshot && m.bytes.size() >= net::kReplHeaderBytes) {
            c.repl.apply_snapshot(reg, std::span<const u8>(m.bytes.data(), m.bytes.size()));
            std::array<u8, 4> abuf{};
            put_u32(abuf.data(), c.repl.ack_seq());
            c.host->send(c.to_server, std::span<const u8>(abuf.data(), 4), false,
                         net::kChannelSnapshot);
            if (c.repl.entity_for(kDedBotNetId).valid())
                c.saw_bot = true;
        } else if (m.channel == net::kChannelDefault && m.bytes.size() == net::kMatchReplyBytes) {
            net::MatchReplyMsg rep{};
            if (net::decode_match_reply(std::span<const u8>(m.bytes.data(), m.bytes.size()), rep)) {
                c.last_deny = rep.deny;
                if (rep.accepted()) {
                    c.joined = true;
                    c.controlled_net_id = rep.controlled_net_id;
                } else {
                    c.rejected = true;
                }
            }
        }
    }
}

int run_dedicated_demo(u32 frames) {
    (void)frames;  // the matchmaking flow is event-driven; frames bounds the run.
    scene::detail::EcsRegistryImpl::Get().shutdown();
    net::LoopbackBus::Get().reset();
    EcsRegistry& reg = EcsRegistry::Get();
    SceneGraph graph;

    const net::ReplicatedComponentSet set = make_set();

    // Server-owned shared-world bot.
    {
        LocalTransform local{};
        local.translation = math::Vec3{24.f, 0.f, 0.f};
        g_ded_bot = scene::create_scene_entity(reg, graph, scene::kInvalidSceneNode, local);
        net::NetIdComponent tag{};
        tag.net_id = kDedBotNetId;
        reg.add<net::NetIdComponent>(g_ded_bot, tag);
    }

    net::DedicatedServer srv(reg, graph, set);
    net::DedicatedServer::Desc d{};
    d.port = 43500;
    d.max_peers = 8;
    d.default_max_players = 2u;  // tiny cap: a 3rd join is rejected.
    d.max_sessions = 1u;         // single match: full session rejects the 3rd.
    d.base_net_id = 300u;
    d.aoi_radius = 1000.0f;      // generous: every client sees the shared bot.
    d.frame_budget = 0;          // run until the last client leaves.
    d.shutdown_when_empty = true;
    if (!srv.start(d)) {
        std::printf("psynder_mp_demo: dedicated server failed to start FAIL\n");
        std::fflush(stdout);
        return EXIT_FAILURE;
    }
    srv.set_world_step(&ded_world_step, nullptr);

    auto make_client = [&](u16 port) -> MatchClient* {
        MatchClient* c = new MatchClient(set);
        net::HostDesc cd{};
        cd.port = port;
        cd.max_peers = 4;
        c->host = net::make_test_host(cd);
        c->to_server = c->host->connect(d.port);
        c->server_key = srv.connect_to_client(port);
        c->inbox.reserve(32);
        return c;
    };

    MatchClient* a = make_client(43501);
    MatchClient* b = make_client(43502);

    // 1. Two clients matchmake + join.
    mc_send_join(*a);
    mc_send_join(*b);
    for (u32 i = 0; i < 6; ++i) {
        srv.step();
        mc_pump(*a, reg);
        mc_pump(*b, reg);
    }
    const bool two_joined = a->joined && b->joined;
    const bool distinct_slots = (a->controlled_net_id != 0u) &&
                                (b->controlled_net_id != 0u) &&
                                (a->controlled_net_id != b->controlled_net_id);

    // 2. A third join is REJECTED at the cap.
    MatchClient* c3 = make_client(43503);
    mc_send_join(*c3);
    for (u32 i = 0; i < 4; ++i) {
        srv.step();
        mc_pump(*a, reg);
        mc_pump(*b, reg);
        mc_pump(*c3, reg);
    }
    const bool third_rejected = c3->rejected && !c3->joined;

    // 3. Client A leaves; the slot frees. 4. The third client re-joins it.
    mc_send_leave(*a);
    for (u32 i = 0; i < 4; ++i) {
        srv.step();
        mc_pump(*b, reg);
        mc_pump(*c3, reg);
    }
    const bool freed_after_leave = (srv.admitted_peers() == 1u);
    c3->rejected = false;
    c3->last_deny = net::MatchDeny::ServerFull;
    mc_send_join(*c3);
    for (u32 i = 0; i < 6; ++i) {
        srv.step();
        mc_pump(*b, reg);
        mc_pump(*c3, reg);
    }
    const bool rejoined = c3->joined && (srv.admitted_peers() == 2u);

    // 5. Shared world: every admitted client saw the bot.
    const bool shared_world = b->saw_bot && c3->saw_bot;

    // 6/7. Everyone leaves: clean shutdown.
    const bool running_while_clients = srv.running();
    mc_send_leave(*b);
    mc_send_leave(*c3);
    for (u32 i = 0; i < 8 && srv.running(); ++i) {
        srv.step();
        mc_pump(*b, reg);
        mc_pump(*c3, reg);
    }
    const bool clean_shutdown = (srv.admitted_peers() == 0u) && !srv.running();

    const bool pass = two_joined && distinct_slots && third_rejected &&
                      freed_after_leave && rejoined && shared_world &&
                      running_while_clients && clean_shutdown;

    std::printf(
        "psynder_mp_demo: dedicated joins=%u rejections=%u two-join=%d distinct=%d "
        "reject-3rd=%d leave-frees=%d rejoin=%d shared-world=%d clean-shutdown=%d %s\n",
        srv.total_joins(), srv.total_rejections(), two_joined ? 1 : 0,
        distinct_slots ? 1 : 0, third_rejected ? 1 : 0, freed_after_leave ? 1 : 0,
        rejoined ? 1 : 0, shared_world ? 1 : 0, clean_shutdown ? 1 : 0,
        pass ? "PASS" : "FAIL");
    std::fflush(stdout);

    net::destroy_test_host(a->host);
    net::destroy_test_host(b->host);
    net::destroy_test_host(c3->host);
    delete a;
    delete b;
    delete c3;
    srv.stop();
    g_ded_bot = kInvalidEntity;
    net::LoopbackBus::Get().reset();
    scene::detail::EcsRegistryImpl::Get().shutdown();
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
    const app::AppArgParseResult parsed = app::parse_common_args(argc, argv);
    const u32 smoke_frames = parsed.args.smoke_frames;
    // Default to a healthy headless run when launched without --smoke-frames so
    // a bare invocation still demonstrates the session and reports PASS/FAIL.
    const u32 frames = smoke_frames > 0 ? smoke_frames : 300u;

    // Mode select: --dedicated / --server runs the dedicated-server + matchmaking
    // smoke; default runs the Wave-9 in-process shared-session smoke. (The
    // client half of a real split-process run is the matchmaking JOIN flow the
    // dedicated demo drives over the same deterministic loopback; --client is
    // accepted as an alias so a launcher can name the two halves.)
    bool dedicated = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i] ? argv[i] : "";
        if (a == "--dedicated" || a == "--server" || a == "--matchmaking")
            dedicated = true;
    }

    if (dedicated)
        return run_dedicated_demo(frames);
    return run_session_demo(frames);
}
