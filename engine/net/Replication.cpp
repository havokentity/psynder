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

void ReplicationServer::queue_despawn_for_client_(ClientState& cs, u32 net_id) noexcept {
    if (net_id == 0)
        return;
    // Dedup: don't double-queue the same net_id for one client.
    for (const PendingDespawn& pd : cs.pending_despawns) {
        if (pd.net_id == net_id)
            return;
    }
    if (cs.pending_despawns.size() >= kMaxPendingDespawns) {
        // Pool cap reached: drop the oldest. Worst case the client carries the
        // stale entity until the next despawn frees a slot; correctness is
        // preserved because the entity simply stops updating.
        cs.pending_despawns.erase(cs.pending_despawns.begin());
    }
    cs.pending_despawns.push_back(PendingDespawn{net_id, 0u});
    // A despawned entity must also drop out of the baseline so the delta diff
    // never tries to carry it forward as "unchanged".
    if (cs.has_baseline) {
        auto& ents = cs.baseline.entities;
        for (usize i = 0; i < ents.size(); ++i) {
            if (ents[i].net_id == net_id) {
                ents.erase(ents.begin() + static_cast<isize>(i));
                break;
            }
        }
    }
}

void ReplicationServer::mark_despawned(u32 net_id) noexcept {
    if (net_id == 0)
        return;
    // Queue the despawn for every known client. A client that connects later
    // never knew the entity, so it does not need the despawn - this only
    // touches clients with live state.
    for (auto& [key, cs] : clients_)
        queue_despawn_for_client_(cs, net_id);
}

u32 ReplicationServer::gather_and_record_(scene::EcsRegistry& registry,
                                          u32 client_key,
                                          u32 server_tick) noexcept {
    // --- Gather (DOTS query, parallel per-chunk, mutex'd accumulation) ------
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
    // type-erased raw_get trampoline. We also capture the row's world position
    // (TransformComponent translation) for the AOI gate; it is server-only
    // scratch and never serialized. The query fires once per chunk across
    // worker threads; the shared `staging_.entities` append is serialized under
    // gather_mu_. Per-row work is memcpy into a stack-local record, so the lock
    // is held only for the push_back.
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
                if (const auto* tc = registry.get<scene::TransformComponent>(e))
                    st.world_pos = tc->local.translation;
                std::lock_guard<std::mutex> lk(gather_mu_);
                staging_.entities.push_back(st);
            }
        });

    // Deterministic, base-matchable order: sort gathered entities by net_id.
    std::sort(staging_.entities.begin(), staging_.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.net_id < b.net_id;
              });

    // Assign a seq. History is recorded later (in encode_body_) once we know
    // exactly which entities this client receives - the recorded snapshot must
    // mirror the client's reconstructed world (baseline + emitted - despawns),
    // NOT the full gather, or an AOI-gated client would delta against entities
    // it never received.
    const u32 seq = next_seq_++;
    staging_.seq = seq;
    (void)client_(client_key);  // ensure the ClientState exists.
    return seq;
}

usize ReplicationServer::encode_body_(ClientState& cs,
                                      u32 seq,
                                      u32 server_tick,
                                      const std::vector<u32>& emit_ids,
                                      std::vector<u8>& out_bytes) noexcept {
    const u32 ncomp = set_.count();
    const bool full = !cs.has_baseline;
    const u32 baseline_seq = full ? 0u : cs.baseline.seq;

    // Stamp this seq onto pending despawns that have not yet been emitted, so
    // an ack at-or-after `seq` can prune them. (Already-stamped ones keep their
    // original first_seq.)
    for (PendingDespawn& pd : cs.pending_despawns) {
        if (pd.first_seq == 0)
            pd.first_seq = seq;
    }

    const u32 max_record = u32(kReplEntityPrefixBytes) + set_.max_entity_bytes();
    const usize need = kReplHeaderBytes +
                       emit_ids.size() * usize(max_record) +
                       cs.pending_despawns.size() * kReplDespawnRecordBytes;
    if (out_bytes.size() < need)
        out_bytes.resize(need);
    u8* p = out_bytes.data();
    write_u32_le(p + 0, kReplMagic);
    write_u32_le(p + 4, server_tick);
    write_u32_le(p + 8, seq);
    write_u32_le(p + 12, baseline_seq);
    // entity_count (offset 16) + despawn_count (offset 20) patched after the
    // bodies are written.
    u8* count_field = p + 16;
    u8* despawn_count_field = p + 20;
    usize off = kReplHeaderBytes;
    u32 emitted = 0;

    for (u32 net_id : emit_ids) {
        const ReplEntityState* stp = staging_.find(net_id);
        if (!stp)
            continue;  // defensive: id no longer in staging.
        const ReplEntityState& st = *stp;
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

    // Append the despawn set. Every pending despawn rides this snapshot
    // (latest-wins) until the client acks it. The client destroys the mapped
    // entity + drops the net_id.
    u32 despawned = 0;
    for (const PendingDespawn& pd : cs.pending_despawns) {
        write_u32_le(out_bytes.data() + off, pd.net_id);
        off += kReplDespawnRecordBytes;
        ++despawned;
    }
    write_u32_le(despawn_count_field, despawned);

    // --- Record the per-client history snapshot ----------------------------
    // The recorded snapshot MUST equal the world the client reconstructs from
    // this datagram, so a later ack promotes a baseline the client actually
    // holds. That reconstructed world is: the client's prior baseline, carried
    // forward, MINUS every despawned net_id, with each emitted entity overlaid
    // to its full current state. (For a full snapshot the baseline is empty, so
    // it reduces to "the emitted entities" = the whole gathered set.)
    ReplWorldSnapshot& rec = record_scratch_;
    rec.clear();
    rec.tick = server_tick;
    rec.seq = seq;
    if (cs.has_baseline)
        rec.entities = cs.baseline.entities;  // carry forward what the client had.
    // Drop despawned ids from the carried-forward set.
    for (const PendingDespawn& pd : cs.pending_despawns) {
        for (usize i = 0; i < rec.entities.size(); ++i) {
            if (rec.entities[i].net_id == pd.net_id) {
                rec.entities.erase(rec.entities.begin() + static_cast<isize>(i));
                break;
            }
        }
    }
    // Overlay every emitted entity's FULL current state (the client ends up
    // holding the full state after decoding the delta onto its base).
    for (u32 net_id : emit_ids) {
        const ReplEntityState* stp = staging_.find(net_id);
        if (!stp)
            continue;
        // Only entities that actually produced a record (passed the delta /
        // full emit rules above) belong in the client's world. Re-derive that
        // by checking against the same skip rule: an unchanged delta entity was
        // NOT emitted but IS still carried forward from the baseline (already
        // present in rec from the carry-forward copy), so overlaying its full
        // state here is harmless and keeps rec consistent.
        bool found = false;
        for (ReplEntityState& e : rec.entities) {
            if (e.net_id == net_id) {
                e = *stp;
                found = true;
                break;
            }
        }
        if (!found)
            rec.entities.push_back(*stp);
    }
    std::sort(rec.entities.begin(), rec.entities.end(),
              [](const ReplEntityState& a, const ReplEntityState& b) {
                  return a.net_id < b.net_id;
              });
    const usize hslot = seq % kHistory;
    cs.history[hslot] = rec;  // copy into the client's history ring.
    cs.history_valid[hslot] = true;
    return off;
}

usize ReplicationServer::serialize_snapshot(scene::EcsRegistry& registry,
                                            u32 client_key,
                                            u32 server_tick,
                                            std::vector<u8>& out_bytes) noexcept {
    const u32 seq = gather_and_record_(registry, client_key, server_tick);
    ClientState& cs = client_(client_key);
    // Whole-world: emit every gathered net_id, in the staging (net_id-sorted)
    // order so the byte layout matches the legacy path exactly.
    emit_.clear();
    emit_.reserve(staging_.entities.size());
    for (const ReplEntityState& st : staging_.entities)
        emit_.push_back(st.net_id);
    return encode_body_(cs, seq, server_tick, emit_, out_bytes);
}

namespace {

// Estimate the delta-encoded record size (prefix + changed-component bytes) of
// `st` against `base`, used by the AOI budget fit. We over-estimate by ignoring
// the "skip if all-unchanged" rule (a changed-nothing delta record costs 0 on
// the wire but we count it as the prefix here): the budget is a soft cap and
// over-estimating only makes it more conservative, never unsafe.
u32 aoi_record_bytes(const ReplicatedComponentSet& set,
                     const ReplEntityState& st,
                     const ReplEntityState* base) noexcept {
    const u32 ncomp = set.count();
    u32 bytes = u32(kReplEntityPrefixBytes);
    for (u32 c = 0; c < ncomp; ++c) {
        if ((st.present_mask & (1u << c)) == 0)
            continue;
        const ReplicatedComponentDesc& d = set.desc(c);
        if (base && (base->present_mask & (1u << c)) &&
            std::memcmp(st.bytes.data() + d.flat_offset,
                        base->bytes.data() + d.flat_offset, d.byte_size) == 0) {
            continue;  // unchanged vs baseline: not on the wire.
        }
        bytes += d.byte_size;
    }
    return bytes;
}

}  // namespace

usize ReplicationServer::serialize_snapshot_aoi(scene::EcsRegistry& registry,
                                                u32 client_key,
                                                u32 server_tick,
                                                const AoiFilter& aoi,
                                                PeerId viewpoint,
                                                u32 byte_budget,
                                                std::vector<u8>& out_bytes) noexcept {
    const u32 seq = gather_and_record_(registry, client_key, server_tick);
    ClientState& cs = client_(client_key);
    const bool full = !cs.has_baseline;

    // --- 1. Visibility pass: collect the entities inside the peer's interest
    //        sphere, with a closest-first priority key + delta-encoded size. ---
    cand_.clear();
    cand_.reserve(staging_.entities.size());
    const math::Vec3 centre = aoi.peer_centre(viewpoint);
    for (const ReplEntityState& st : staging_.entities) {
        if (!aoi.visible(viewpoint, st.world_pos))
            continue;
        const f32 dx = st.world_pos.x - centre.x;
        const f32 dy = st.world_pos.y - centre.y;
        const f32 dz = st.world_pos.z - centre.z;
        const ReplEntityState* base = full ? nullptr : cs.baseline.find(st.net_id);
        AoiCandidate cd{};
        cd.net_id = st.net_id;
        cd.dist_sq = dx * dx + dy * dy + dz * dz;
        cd.record_bytes = aoi_record_bytes(set_, st, base);
        cand_.push_back(cd);
    }
    // Deterministic priority: closest first, ties broken by net_id. No RNG, no
    // pointer-order dependence.
    std::sort(cand_.begin(), cand_.end(), [](const AoiCandidate& a, const AoiCandidate& b) {
        if (a.dist_sq != b.dist_sq)
            return a.dist_sq < b.dist_sq;
        return a.net_id < b.net_id;
    });

    // --- 2. Budget fit: take the priority prefix that fits. A budget of 0
    //        means "no cap". The despawn records that ride this snapshot also
    //        consume budget, so reserve their bytes up front. The header always
    //        fits (budget is floored to it). ---
    const usize despawn_bytes = cs.pending_despawns.size() * kReplDespawnRecordBytes;
    usize budget = byte_budget;
    if (byte_budget != 0 && budget < kReplHeaderBytes)
        budget = kReplHeaderBytes;  // always produce a valid (empty) snapshot.
    usize used = kReplHeaderBytes + despawn_bytes;

    emit_.clear();
    emit_.reserve(cand_.size());
    for (const AoiCandidate& cd : cand_) {
        if (byte_budget != 0 && used + cd.record_bytes > budget) {
            // Over budget this frame. A carried-forward entity (already in the
            // baseline) simply stays; a not-yet-sent one arrives a later frame
            // once nearer entities settle. Either way it stays in interest so
            // it is NOT despawned below.
            continue;
        }
        emit_.push_back(cd.net_id);
        used += cd.record_bytes;
    }

    // --- 3. Per-peer AOI despawns: any net_id that WAS in this peer's interest
    //        last frame but is no longer visible must be torn down on the
    //        client. We reuse the same pending-despawn set + baseline scrub the
    //        explicit mark_despawned() path uses, so the client tears it down
    //        identically. (`cs.in_view` is kept sorted; the new visible set is
    //        the sorted `cand_` net_ids.) ---
    // Build the new sorted visible set into emit_-adjacent scratch reusing
    // cand_'s ordering is by distance, so collect+sort net_ids separately.
    // We walk cand_ to gather visible ids, sort them, then diff vs cs.in_view.
    std::vector<u32>& visible_now = aoi_visible_scratch_;
    visible_now.clear();
    visible_now.reserve(cand_.size());
    for (const AoiCandidate& cd : cand_)
        visible_now.push_back(cd.net_id);
    std::sort(visible_now.begin(), visible_now.end());

    // Linear merge diff: ids in cs.in_view (sorted) absent from visible_now ->
    // they left interest -> queue a per-peer despawn.
    {
        usize i = 0, j = 0;
        const std::vector<u32>& prev = cs.in_view;
        while (i < prev.size()) {
            const u32 id = prev[i];
            while (j < visible_now.size() && visible_now[j] < id)
                ++j;
            const bool still_visible = (j < visible_now.size() && visible_now[j] == id);
            if (!still_visible)
                queue_despawn_for_client_(cs, id);
            ++i;
        }
    }

    const usize n = encode_body_(cs, seq, server_tick, emit_, out_bytes);

    // The peer's interest set for the NEXT diff is everything in the sphere
    // this frame (budget-dropped entities included: they remain in interest).
    cs.in_view.swap(visible_now);
    return n;
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

    // Prune despawns the client has now observed: any pending despawn that was
    // first emitted in a snapshot at-or-before `acked_seq` has been delivered.
    // (first_seq == 0 means it has not been emitted yet -> keep.) Erasing here
    // is what removes the net_id from the latest-wins despawn set so we stop
    // re-sending it every tick.
    auto& pds = cs.pending_despawns;
    for (usize i = 0; i < pds.size();) {
        if (pds[i].first_seq != 0 && pds[i].first_seq <= acked_seq)
            pds.erase(pds.begin() + static_cast<isize>(i));
        else
            ++i;
    }
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
    const u32 despawn_count = read_u32_le(p + 20);

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

    // --- Despawn set: read the net_ids, drop them from the carried-forward
    //     scratch so they are never re-created, and stage them for destroy.
    despawn_scratch_.clear();
    for (u32 i = 0; i < despawn_count; ++i) {
        if (off + kReplDespawnRecordBytes > bytes.size())
            return false;  // truncated despawn record.
        const u32 dead_id = read_u32_le(bytes.data() + off);
        off += kReplDespawnRecordBytes;
        despawn_scratch_.push_back(dead_id);
        // Remove from the carried-forward scratch (it may have been seeded from
        // the base) so the commit loop below does not recreate the entity.
        auto& ents = decode_scratch_.entities;
        for (usize j = 0; j < ents.size(); ++j) {
            if (ents[j].net_id == dead_id) {
                ents.erase(ents.begin() + static_cast<isize>(j));
                break;
            }
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

    // --- 6. Apply despawns: destroy entities + scrub every cached snapshot ---
    // Done last so the freshly-rotated prev_/curr_/history_ are also scrubbed
    // (they were copied from older frames that may still hold the dead id).
    for (u32 dead_id : despawn_scratch_)
        despawn_(registry, dead_id);
    return true;
}

void ReplicationClient::despawn_(scene::EcsRegistry& registry, u32 net_id) noexcept {
    if (net_id == 0)
        return;
    auto it = net_to_entity_.find(net_id);
    if (it != net_to_entity_.end()) {
        const Entity e = it->second;
        if (e.valid())
            registry.destroy(e);
        net_to_entity_.erase(it);
    }
    // Scrub the net_id from every cached snapshot so it is never carried
    // forward into a future decode (which would resurrect it).
    auto scrub = [net_id](ReplWorldSnapshot& s) noexcept {
        auto& ents = s.entities;
        for (usize i = 0; i < ents.size(); ++i) {
            if (ents[i].net_id == net_id) {
                ents.erase(ents.begin() + static_cast<isize>(i));
                return;
            }
        }
    };
    scrub(prev_);
    scrub(curr_);
    scrub(decode_scratch_);
    for (usize h = 0; h < kHistory; ++h) {
        if (history_valid_[h])
            scrub(history_[h]);
    }
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
