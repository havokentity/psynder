// SPDX-License-Identifier: MIT
// Psynder - authoritative ECS entity replication (M-NET). Lane 14 internal.
//
// Server: gather replicated entities via a DOTS query, sort by net_id, then
// delta-encode the snapshot vs the client's acked baseline. Client: decode,
// rebuild against the matching base, map net_ids to local entities, write
// component bytes, and interpolate Transforms between the last two snapshots.
//
// All multi-byte fields are little-endian on the wire (mirrors Frame.cpp /
// Snapshot.cpp), so the format is endian-stable. Per-tick scratch is pooled;
// the only allocations are one-time growth of the staging / history vectors.

#include "Replication.h"

#include "scene/SceneGraph.h"

#include <algorithm>
#include <cstring>

namespace psynder::net {

namespace {

// --- little-endian primitive writers / readers (match Frame.cpp) -----------
PSY_FORCEINLINE void write_u32_le(u8* p, u32 v) noexcept {
    p[0] = u8(v & 0xFFu);
    p[1] = u8((v >> 8) & 0xFFu);
    p[2] = u8((v >> 16) & 0xFFu);
    p[3] = u8((v >> 24) & 0xFFu);
}
PSY_FORCEINLINE u32 read_u32_le(const u8* p) noexcept {
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

// Compare two component slots' bytes for equality (delta change detection).
PSY_FORCEINLINE bool slot_bytes_equal(const ReplEntityState& a,
                                      const ReplEntityState& b,
                                      u32 offset,
                                      u32 size) noexcept {
    return std::memcmp(a.bytes.data() + offset, b.bytes.data() + offset, size) == 0;
}

}  // namespace

// --- ReplWorldSnapshot -------------------------------------------------------
const ReplEntityState* ReplWorldSnapshot::find(u32 net_id) const noexcept {
    // entities are kept sorted by net_id, so binary search.
    usize lo = 0, hi = entities.size();
    while (lo < hi) {
        usize mid = lo + (hi - lo) / 2;
        if (entities[mid].net_id < net_id)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < entities.size() && entities[lo].net_id == net_id)
        return &entities[lo];
    return nullptr;
}

// --- ReplicationServer -------------------------------------------------------
ReplicationServer::ClientState& ReplicationServer::client_(u32 client_key) noexcept {
    return clients_[client_key];
}

usize ReplicationServer::serialize_snapshot(scene::EcsRegistry& registry,
                                            u32 client_key,
                                            u32 server_tick,
                                            std::vector<u8>& out_bytes) noexcept {
    // --- 1. Gather (DOTS query, parallel per-chunk, mutex'd accumulation) ---
    // We read NetIdComponent (identity) + each replicated component column.
    // The query fires once per chunk across worker threads, so the shared
    // staging vector is appended under gather_mu_. Per-chunk work is pure
    // memcpy into pre-stamped ReplEntityState records; the only heap touch is
    // staging_.entities growth, amortized away after the first few ticks.
    const u32 ncomp = set_.count();
    {
        std::lock_guard<std::mutex> lk(gather_mu_);
        staging_.clear();
        staging_.tick = server_tick;
    }

    // We drive the gather off the (NetIdComponent, SceneNodeComponent) column
    // pair: NetIdComponent supplies network identity, SceneNodeComponent
    // supplies the row's Entity handle (the only column-resident way to recover
    // it). For each replicated component we read its bytes through the desc's
    // type-erased raw_get trampoline. The query fires once per chunk across
    // worker threads; the shared `staging_.entities` append is serialized under
    // gather_mu_. Per-row work is memcpy into a stack-local record, so the
    // lock is held only for the push_back.
    registry.query<scene::reads<NetIdComponent, scene::SceneNodeComponent>, scene::writes<>>(
        [&](std::span<const NetIdComponent> ids,
            std::span<const scene::SceneNodeComponent> nodes) {
            const usize n = ids.size();
            for (usize i = 0; i < n; ++i) {
                ReplEntityState st{};
                st.net_id = ids[i].net_id;
                if (st.net_id == 0)
                    continue;  // unassigned tag => not yet replicated.
                const Entity e = nodes[i].entity;
                u32 mask = 0;
                for (u32 c = 0; c < ncomp; ++c) {
                    const ReplicatedComponentDesc& d = set_.desc(c);
                    const void* src = d.raw_get(registry, e);
                    if (!src)
                        continue;
                    std::memcpy(st.bytes.data() + d.flat_offset, src, d.byte_size);
                    mask |= (1u << c);
                }
                st.present_mask = mask;
                std::lock_guard<std::mutex> lk(gather_mu_);
                staging_.entities.push_back(st);
            }
        });

    // Deterministic, base-matchable order: sort gathered entities by net_id.
    std::sort(staging_.entities.begin(), staging_.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.net_id < b.net_id;
              });

    // --- 2. Assign a seq + record the snapshot for ack-promotion ------------
    const u32 seq = next_seq_++;
    staging_.seq = seq;
    ClientState& cs = client_(client_key);
    const usize slot = seq % kHistory;
    cs.history[slot] = staging_;  // copy into the history ring.
    cs.history_valid[slot] = true;

    // --- 3. Delta-encode vs the client's acked baseline ---------------------
    const bool full = !cs.has_baseline;
    const u32 baseline_seq = full ? 0u : cs.baseline.seq;

    const u32 max_record = u32(kReplEntityPrefixBytes) + set_.max_entity_bytes();
    const usize need = kReplHeaderBytes +
                       staging_.entities.size() * usize(max_record);
    if (out_bytes.size() < need)
        out_bytes.resize(need);
    u8* p = out_bytes.data();
    write_u32_le(p + 0, kReplMagic);
    write_u32_le(p + 4, server_tick);
    write_u32_le(p + 8, seq);
    write_u32_le(p + 12, baseline_seq);
    // entity_count patched after the loop (we may skip unchanged entities).
    u8* count_field = p + 16;
    usize off = kReplHeaderBytes;
    u32 emitted = 0;

    for (const ReplEntityState& st : staging_.entities) {
        const ReplEntityState* base = full ? nullptr : cs.baseline.find(st.net_id);
        // Determine which components changed vs the baseline.
        u32 send_mask = 0;
        for (u32 c = 0; c < ncomp; ++c) {
            if ((st.present_mask & (1u << c)) == 0)
                continue;  // entity does not carry this component.
            const ReplicatedComponentDesc& d = set_.desc(c);
            if (base && (base->present_mask & (1u << c)) &&
                slot_bytes_equal(st, *base, d.flat_offset, d.byte_size)) {
                continue;  // unchanged since baseline => omit (delta).
            }
            send_mask |= (1u << c);
        }
        // In a delta, an entity whose components are all unchanged is skipped
        // entirely (it is implicitly carried forward from the baseline). In a
        // full snapshot we always emit (send_mask == present_mask).
        if (!full && send_mask == 0 && base != nullptr)
            continue;

        write_u32_le(out_bytes.data() + off + 0, st.net_id);
        write_u32_le(out_bytes.data() + off + 4, send_mask);
        off += kReplEntityPrefixBytes;
        for (u32 c = 0; c < ncomp; ++c) {
            if ((send_mask & (1u << c)) == 0)
                continue;
            const ReplicatedComponentDesc& d = set_.desc(c);
            std::memcpy(out_bytes.data() + off, st.bytes.data() + d.flat_offset, d.byte_size);
            off += d.byte_size;
        }
        ++emitted;
    }
    write_u32_le(count_field, emitted);
    return off;
}

void ReplicationServer::ack_from_client(u32 client_key, u32 acked_seq) noexcept {
    if (acked_seq == 0)
        return;
    auto it = clients_.find(client_key);
    if (it == clients_.end())
        return;
    ClientState& cs = it->second;
    const usize slot = acked_seq % kHistory;
    if (!cs.history_valid[slot] || cs.history[slot].seq != acked_seq)
        return;  // stale / evicted ack: keep the existing baseline.
    // Only advance forward (ignore out-of-order older acks).
    if (cs.has_baseline && acked_seq <= cs.baseline.seq)
        return;
    cs.baseline = cs.history[slot];
    cs.has_baseline = true;
}

// --- ReplicationClient -------------------------------------------------------
Entity ReplicationClient::entity_for(u32 net_id) const noexcept {
    auto it = net_to_entity_.find(net_id);
    return it == net_to_entity_.end() ? kInvalidEntity : it->second;
}

const ReplWorldSnapshot* ReplicationClient::base_for_(u32 baseline_seq) const noexcept {
    if (baseline_seq == 0)
        return nullptr;
    const usize slot = baseline_seq % kHistory;
    if (history_valid_[slot] && history_[slot].seq == baseline_seq)
        return &history_[slot];
    return nullptr;
}

Entity ReplicationClient::ensure_entity_(scene::EcsRegistry& registry, u32 net_id) noexcept {
    auto it = net_to_entity_.find(net_id);
    if (it != net_to_entity_.end())
        return it->second;
    const Entity e = registry.create();
    NetIdComponent tag{};
    tag.net_id = net_id;
    registry.add<NetIdComponent>(e, tag);
    net_to_entity_.emplace(net_id, e);
    return e;
}

bool ReplicationClient::apply_snapshot(scene::EcsRegistry& registry,
                                       std::span<const u8> bytes) noexcept {
    // --- 1. Validate the header ---------------------------------------------
    if (bytes.size() < kReplHeaderBytes)
        return false;
    const u8* p = bytes.data();
    if (read_u32_le(p + 0) != kReplMagic)
        return false;
    const u32 tick = read_u32_le(p + 4);
    const u32 seq = read_u32_le(p + 8);
    const u32 baseline_seq = read_u32_le(p + 12);
    const u32 count = read_u32_le(p + 16);

    // Drop stale / duplicate snapshots (latest-wins on channel 2).
    if (applied_seq_ != 0 && seq <= applied_seq_)
        return false;

    // --- 2. Resolve the delta base ------------------------------------------
    const ReplWorldSnapshot* base = base_for_(baseline_seq);
    if (baseline_seq != 0 && base == nullptr)
        return false;  // we lost the base: cannot rebuild this delta cleanly.

    const u32 ncomp = set_.count();

    // --- 3. Decode into scratch, seeded from the base -----------------------
    // Start from the base (carry-forward unchanged entities/components), then
    // overlay the delta records. Full snapshots start from empty.
    decode_scratch_.clear();
    decode_scratch_.tick = tick;
    decode_scratch_.seq = seq;
    if (base)
        decode_scratch_.entities = base->entities;  // copy-forward.

    usize off = kReplHeaderBytes;
    for (u32 i = 0; i < count; ++i) {
        if (off + kReplEntityPrefixBytes > bytes.size())
            return false;  // truncated record prefix.
        const u32 net_id = read_u32_le(bytes.data() + off + 0);
        const u32 mask = read_u32_le(bytes.data() + off + 4);
        off += kReplEntityPrefixBytes;
        // Reject masks that reference component slots outside the set. Guard
        // the shift: `1u << 32` is UB, and ncomp can be up to 32.
        const u32 valid_bits = (ncomp >= 32u) ? 0xFFFFFFFFu : ((1u << ncomp) - 1u);
        if (mask & ~valid_bits)
            return false;  // mask references an unknown component slot.

        // Find-or-insert this net_id in the (sorted) scratch entities.
        usize idx = decode_scratch_.entities.size();
        bool found = false;
        for (usize j = 0; j < decode_scratch_.entities.size(); ++j) {
            if (decode_scratch_.entities[j].net_id == net_id) {
                idx = j;
                found = true;
                break;
            }
        }
        if (!found) {
            ReplEntityState st{};
            st.net_id = net_id;
            decode_scratch_.entities.push_back(st);
            idx = decode_scratch_.entities.size() - 1;
        }
        ReplEntityState& dst = decode_scratch_.entities[idx];

        for (u32 c = 0; c < ncomp; ++c) {
            if ((mask & (1u << c)) == 0)
                continue;
            const ReplicatedComponentDesc& d = set_.desc(c);
            if (off + d.byte_size > bytes.size())
                return false;  // truncated component payload.
            if (d.flat_offset + d.byte_size > kReplEntityByteCap)
                return false;  // defensive: oversized set.
            std::memcpy(dst.bytes.data() + d.flat_offset, bytes.data() + off, d.byte_size);
            dst.present_mask |= (1u << c);
            off += d.byte_size;
        }
    }

    // Keep scratch sorted by net_id so find()/interp binary-searches work.
    std::sort(decode_scratch_.entities.begin(), decode_scratch_.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.net_id < b.net_id;
              });

    // --- 4. Commit: map net_ids -> entities, write component bytes ----------
    for (const ReplEntityState& st : decode_scratch_.entities) {
        const Entity e = ensure_entity_(registry, st.net_id);
        for (u32 c = 0; c < ncomp; ++c) {
            if ((st.present_mask & (1u << c)) == 0)
                continue;
            const ReplicatedComponentDesc& d = set_.desc(c);
            d.apply(registry, e, st.bytes.data() + d.flat_offset);
        }
    }

    // --- 5. Advance interp buffers + history --------------------------------
    prev_ = curr_;
    have_prev_ = have_curr_;
    curr_ = decode_scratch_;
    have_curr_ = true;
    applied_seq_ = seq;
    const usize slot = seq % kHistory;
    history_[slot] = decode_scratch_;
    history_valid_[slot] = true;
    return true;
}

void ReplicationClient::interpolate_transforms(scene::EcsRegistry& registry, f32 t) noexcept {
    if (!have_prev_ || !have_curr_)
        return;
    if (t < 0.f)
        t = 0.f;
    if (t > 1.f)
        t = 1.f;
    // The replicated set must include TransformComponent for interpolation to
    // do anything; find its slot once.
    const u32 ncomp = set_.count();
    u32 tslot = ncomp;
    for (u32 c = 0; c < ncomp; ++c) {
        if (set_.desc(c).component == scene::component_id<scene::TransformComponent>()) {
            tslot = c;
            break;
        }
    }
    if (tslot == ncomp)
        return;
    const ReplicatedComponentDesc& d = set_.desc(tslot);

    for (const ReplEntityState& cur : curr_.entities) {
        if ((cur.present_mask & (1u << tslot)) == 0)
            continue;
        const ReplEntityState* old = prev_.find(cur.net_id);
        const Entity e = entity_for(cur.net_id);
        if (!e.valid())
            continue;
        scene::TransformComponent* tc = registry.get<scene::TransformComponent>(e);
        if (!tc)
            continue;
        scene::TransformComponent newer{};
        std::memcpy(&newer, cur.bytes.data() + d.flat_offset, sizeof(newer));
        if (!old || (old->present_mask & (1u << tslot)) == 0) {
            *tc = newer;  // no prior frame for this entity: snap.
            continue;
        }
        scene::TransformComponent older{};
        std::memcpy(&older, old->bytes.data() + d.flat_offset, sizeof(older));
        // Lerp translation; rotation/scale snap to newer (Wave-A: translation
        // interpolation is what smooths motion between ticks; slerp lands with
        // prediction/reconciliation in the next wave).
        math::Vec3 a = older.local.translation;
        math::Vec3 b = newer.local.translation;
        tc->local = newer.local;
        tc->local.translation = math::Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                           a.z + (b.z - a.z) * t};
    }
}

}  // namespace psynder::net
