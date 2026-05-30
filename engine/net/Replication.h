// SPDX-License-Identifier: MIT
// Psynder - authoritative ECS entity replication (M-NET). Lane 14 internal.
//
// This layer turns the engine's archetype-chunked ECS into a client-server
// replicated world. The server is authoritative: each tick it gathers the
// replicated subset of entities (a DOTS query over NetIdComponent + a fixed
// ReplicatedComponentSet), serializes a snapshot, and DELTA-encodes it
// against the snapshot the client last acknowledged. The client decodes the
// stream, maps net_ids to its own local entities, writes the component
// bytes, and INTERPOLATES Transforms between the last two snapshots so motion
// stays smooth between server ticks.
//
// The on-wire snapshot rides the existing channel-2 (snapshot) path of the
// reliable-UDP HostImpl: snapshots are sent unreliable (latest-wins), and the
// client echoes the highest snapshot seq it has applied back on the same
// channel. The server keeps a per-client baseline keyed off that ack - this
// is the classic Quake3 / Source delta-snapshot scheme. We REUSE the existing
// transport (HostImpl + Loopback), framing (Frame.h channels), and the
// AckTracker sequencing primitive from Reliability.h; we do NOT reinvent any
// of them.
//
// Wire format is hand-serialized little-endian (mirrors Frame.cpp /
// Snapshot.cpp) so it is endian-stable and independent of host struct
// padding. Components must be trivially copyable POD (their raw bytes ARE the
// payload). Per-tick work is alloc-free: all scratch lives in pooled buffers
// owned by the Server/Client objects.

#pragma once

#include "core/Types.h"
#include "scene/EcsRegistry.h"
#include "scene/SceneEcs.h"

#include <array>
#include <cstring>
#include <mutex>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace psynder::net {

// --- Network identity ------------------------------------------------------
// A NetIdComponent tags an ECS entity as replicated. `net_id` is assigned by
// the server and is stable for the life of the session. The client maintains
// its own entities tagged with the SAME net_id so both ends agree on identity
// independent of local Entity handle values (which differ per world).
PSYNDER_COMPONENT(NetIdComponent) {
    u32 net_id = 0;
    u32 _pad = 0;  // keep 8-byte size stable / trivially copyable.
};

// --- Replicated component policy ---------------------------------------------
// Describes ONE replicated POD component: a stable network id (its index in
// the set, 0..kMaxReplicatedComponents-1) and its byte size. The component's
// trivially-copyable bytes are the wire payload for that slot.
inline constexpr usize kMaxReplicatedComponents = 32;  // one bit per mask bit.

// Per-entity flat byte cap. Sized to hold every replicated component's bytes
// for one entity (TransformComponent is 40 bytes; a generous fixed cap keeps
// ReplEntityState trivially poolable with no per-tick allocation).
inline constexpr usize kReplEntityByteCap = 256;

// The frozen EcsRegistry contract only exposes TYPED get<T>()/add<T>(); there
// is no get-by-ComponentId-at-runtime surface. To stay generic over the
// replicated set without touching that contract, each desc captures two type-
// erased trampolines at registration time (when the concrete `T` is known):
//   - raw_get : read the component's bytes for an entity (nullptr if absent).
//   - apply   : write the component's bytes to an entity (add-or-overwrite).
// Both are stateless free functions instantiated per `T`, so they hold no
// per-call state and are safe to call from the parallel gather.
using ReplRawGetFn = const void* (*)(scene::EcsRegistry&, Entity) noexcept;
using ReplApplyFn = void (*)(scene::EcsRegistry&, Entity, const void*) noexcept;

struct ReplicatedComponentDesc {
    scene::ComponentId component = 0;  // ECS component id (runtime-assigned).
    u32 net_component_id = 0;          // stable index in the set / mask bit.
    u32 byte_size = 0;                 // sizeof(component); == wire bytes.
    u32 flat_offset = 0;               // offset into ReplEntityState::bytes.
    ReplRawGetFn raw_get = nullptr;    // typed reader trampoline.
    ReplApplyFn apply = nullptr;       // typed writer trampoline.
};

// The fixed set of components that replicate. Built once on the server and
// mirrored on the client (same order => same mask bit meaning). The set is
// endian-neutral: it only carries sizes + ids, never raw component bytes.
class ReplicatedComponentSet {
   public:
    // Register `T` as a replicated component. Returns its net_component_id
    // (the mask bit). Order of registration defines the bit layout, so the
    // server and client MUST register the same components in the same order.
    template <class T>
    u32 add() noexcept {
        static_assert(std::is_trivially_copyable_v<T>,
                      "replicated components must be trivially copyable POD");
        static_assert(sizeof(T) <= kReplEntityByteCap,
                      "replicated component too large for the flat record cap");
        const u32 bit = count_;
        ReplicatedComponentDesc d{};
        d.component = scene::component_id<T>();
        d.net_component_id = bit;
        d.byte_size = static_cast<u32>(sizeof(T));
        d.flat_offset = flat_bytes_;
        d.raw_get = +[](scene::EcsRegistry& reg, Entity e) noexcept -> const void* {
            return reg.get<T>(e);
        };
        d.apply = +[](scene::EcsRegistry& reg, Entity e, const void* src) noexcept {
            T value{};
            std::memcpy(&value, src, sizeof(T));
            if (T* existing = reg.get<T>(e))
                *existing = value;
            else
                reg.add<T>(e, value);
        };
        descs_[bit] = d;
        flat_bytes_ += static_cast<u32>(sizeof(T));
        ++count_;
        return bit;
    }

    u32 count() const noexcept { return count_; }
    const ReplicatedComponentDesc& desc(u32 net_component_id) const noexcept {
        return descs_[net_component_id];
    }

    // Total bytes if every component in the set is present for one entity.
    u32 max_entity_bytes() const noexcept { return flat_bytes_; }

   private:
    std::array<ReplicatedComponentDesc, kMaxReplicatedComponents> descs_{};
    u32 count_ = 0;
    u32 flat_bytes_ = 0;
};

// --- Wire constants ----------------------------------------------------------
// 'PRP2' - bumped from 'PRP1' when the despawn set joined the header. The
// magic change makes a v1 decoder reject a v2 stream (and vice-versa) rather
// than silently mis-reading the despawn_count word as entity bytes.
inline constexpr u32 kReplMagic = 0x50525032u;  // 'PRP2'
// Header: magic(4) + tick(4) + seq(4) + baseline_seq(4) + entity_count(4) +
//         despawn_count(4). The despawn records (net_id each) follow the
//         entity records in the body.
inline constexpr usize kReplHeaderBytes = 24;
// Per-entity record prefix: net_id(4) + mask(4). Component bytes follow.
inline constexpr usize kReplEntityPrefixBytes = 8;
// Per-despawn record: a single net_id(4). The client destroys the mapped
// entity and drops the net_id when it sees one.
inline constexpr usize kReplDespawnRecordBytes = 4;

// --- Per-entity decoded snapshot state (server baselines + client buffer) ---
// One replicated entity's full component bytes, addressed by net_component_id.
// Stored flat so it is trivially poolable. `present_mask` tracks which
// components this entity currently carries (so a missing component isn't
// mistaken for all-zero bytes).
struct ReplEntityState {
    u32 net_id = 0;
    u32 present_mask = 0;  // which net_component_ids are populated.
    // Flat backing for every component slot, packed at the set's per-slot
    // offsets. Sized to the set's max entity bytes; never grows per tick.
    std::array<u8, kReplEntityByteCap> bytes{};
};

// A full decoded world snapshot: every replicated entity at one server tick.
// Used as the server's per-client baseline and as the client's interp buffer.
struct ReplWorldSnapshot {
    u32 tick = 0;
    u32 seq = 0;
    std::vector<ReplEntityState> entities;  // sorted by net_id for fast diff.

    void clear() noexcept {
        tick = 0;
        seq = 0;
        entities.clear();
    }
    const ReplEntityState* find(u32 net_id) const noexcept;
};

// --- Server ------------------------------------------------------------------
// Authoritative side. Owns: the replicated set, the next snapshot seq, a
// per-client baseline (the snapshot each client last acked), and pooled
// scratch. serialize_snapshot() gathers + delta-encodes; ack_from_client()
// advances a client's baseline.
class ReplicationServer {
   public:
    explicit ReplicationServer(const ReplicatedComponentSet& set) noexcept : set_(set) {}

    // Gather the replicated entities from `registry` into the staging
    // snapshot, then delta-encode vs the baseline for `client_key` into
    // `out_bytes` (resized to fit). Returns bytes written. When the client
    // has no acked baseline, emits a FULL snapshot (every present component).
    //
    // `client_key` is any stable per-client key (e.g. PeerId.raw). The gather
    // is a DOTS query (parallel per-chunk); shared accumulation is mutex'd.
    // No per-tick heap alloc beyond the one-time growth of pooled buffers.
    usize serialize_snapshot(scene::EcsRegistry& registry,
                             u32 client_key,
                             u32 server_tick,
                             std::vector<u8>& out_bytes) noexcept;

    // Mark a net_id as despawned: the entity has been removed from the
    // authoritative world. The despawn is broadcast to every client until each
    // acknowledges a snapshot at-or-after the one that first carried it, then
    // pruned. Idempotent; despawning an unknown / already-despawned id is a
    // no-op. The despawn signal is what stops M-NET from carrying a removed
    // entity forward in every subsequent delta.
    //
    // Call this BEFORE the entity's ECS row is destroyed (the gather no longer
    // sees it once destroyed; the explicit mark is the authoritative signal).
    void mark_despawned(u32 net_id) noexcept;

    // The client acked snapshot `seq`. Promote our cached copy of that
    // snapshot to the client's baseline and prune despawns the client has now
    // seen. Stale / unknown acks are ignored.
    void ack_from_client(u32 client_key, u32 acked_seq) noexcept;

    u32 last_seq() const noexcept { return next_seq_ - 1; }

   private:
    static constexpr usize kHistory = 8;  // sent-snapshot ring depth per client.
    // Max distinct despawns we keep pending per client before the oldest is
    // dropped (it will simply stop being carried-forward instead). Sized
    // generously for a 16-32 player FPS churn rate; pooled, never per-tick.
    static constexpr usize kMaxPendingDespawns = 256;

    // A despawn awaiting client ack. `first_seq` is the snapshot seq that first
    // carried this net_id; once the client acks a seq >= first_seq the despawn
    // is delivered and we prune it.
    struct PendingDespawn {
        u32 net_id = 0;
        u32 first_seq = 0;  // 0 == not yet emitted in any snapshot.
    };

    struct ClientState {
        ReplWorldSnapshot baseline;  // last snapshot this client acked.
        bool has_baseline = false;
        // History of recently-sent snapshots, keyed by seq % kHistory, so an
        // ack can promote the exact snapshot the client saw. Snapshots are
        // latest-wins; a handful of slots is enough.
        std::array<ReplWorldSnapshot, kHistory> history{};
        std::array<bool, kHistory> history_valid{};
        // Despawns this client has not yet acknowledged. Carried in every
        // snapshot (latest-wins) until acked, then pruned. Pooled vector; only
        // grows on first use up to kMaxPendingDespawns.
        std::vector<PendingDespawn> pending_despawns;
    };

    ClientState& client_(u32 client_key) noexcept;

    const ReplicatedComponentSet& set_;
    u32 next_seq_ = 1;  // 0 reserved => "no baseline".
    std::unordered_map<u32, ClientState> clients_;
    // Pooled gather staging - reused every tick. Guarded by gather_mu_ during
    // the parallel query.
    ReplWorldSnapshot staging_;
    std::mutex gather_mu_;
};

// --- Client ----------------------------------------------------------------
// Receiving side. Owns: the net_id->Entity map, the last two decoded
// snapshots (for interpolation), and the highest seq applied (echoed back as
// the ack). apply_snapshot() decodes + writes component bytes; interpolate()
// blends Transforms by render time.
class ReplicationClient {
   public:
    explicit ReplicationClient(const ReplicatedComponentSet& set) noexcept : set_(set) {}

    // Decode `bytes`, reconcile against the prior decoded snapshot (delta
    // base), create/lookup entities by net_id in `registry`, and write the
    // component bytes. Buffers this as the newest of the two interp frames.
    // Returns false if the snapshot is malformed / truncated (nothing is
    // applied in that case). Alloc-free per call (pooled buffers).
    bool apply_snapshot(scene::EcsRegistry& registry, std::span<const u8> bytes) noexcept;

    // Interpolate TransformComponent translations between the last two
    // applied snapshots at fraction `t` in [0,1] and write the blended
    // transform back to each mapped entity. Other component bytes snap to the
    // newest frame (already written by apply_snapshot). Call at render time.
    void interpolate_transforms(scene::EcsRegistry& registry, f32 t) noexcept;

    // The seq the client should echo back to the server as its ack (highest
    // applied). 0 until the first snapshot is applied.
    u32 ack_seq() const noexcept { return applied_seq_; }

    // Lookup the local Entity a net_id maps to (kInvalidEntity if none).
    Entity entity_for(u32 net_id) const noexcept;

    // Test introspection: how many net_ids are mapped.
    usize mapped_count() const noexcept { return net_to_entity_.size(); }

   private:
    static constexpr usize kHistory = 8;  // applied-snapshot ring depth.

    Entity ensure_entity_(scene::EcsRegistry& registry, u32 net_id) noexcept;
    // Resolve the full snapshot a delta with `baseline_seq` is encoded
    // against. Returns nullptr if we no longer hold it (forces resend logic
    // upstream; in practice the server only deltas vs an acked seq we kept).
    const ReplWorldSnapshot* base_for_(u32 baseline_seq) const noexcept;
    // Apply a despawn: destroy the entity mapped to `net_id`, drop the mapping,
    // and erase the net_id from every cached snapshot (decode scratch + interp
    // frames + history) so it is never carried forward again.
    void despawn_(scene::EcsRegistry& registry, u32 net_id) noexcept;

    const ReplicatedComponentSet& set_;
    // net_id -> local Entity. Persistent session state (not per-frame), so a
    // map here is allowed by the DOTS rules (it is NOT per-tick scratch).
    std::unordered_map<u32, Entity> net_to_entity_;
    // Double-buffered decoded snapshots for interpolation: prev_ is older.
    ReplWorldSnapshot prev_;
    ReplWorldSnapshot curr_;
    bool have_prev_ = false;
    bool have_curr_ = false;
    u32 applied_seq_ = 0;
    // Ring of recently-applied full snapshots, keyed by seq % kHistory, so a
    // delta can be rebuilt against the exact base the server used. Pooled -
    // sized once, reused across the session.
    std::array<ReplWorldSnapshot, kHistory> history_{};
    std::array<bool, kHistory> history_valid_{};
    // Scratch decode target, reused each call (alloc-free per tick).
    ReplWorldSnapshot decode_scratch_;
    // Scratch list of net_ids despawned by the current snapshot. Pooled -
    // cleared (not freed) each apply_snapshot, so no per-tick heap alloc.
    std::vector<u32> despawn_scratch_;
};

}  // namespace psynder::net
