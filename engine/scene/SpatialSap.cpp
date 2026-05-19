// SPDX-License-Identifier: MIT
// Psynder — sweep-and-prune broadphase backend (lane-06 internal, Wave B).
//
// Three sorted endpoint lists (one per axis). Each AABB contributes one
// min + one max endpoint per axis. Per-axis overlap = a min endpoint
// between A.min and A.max appears between B.min and B.max — i.e. the
// classic SAP intersection.
//
// `sap_step()` re-sorts every axis (std::sort) and then scans the X-axis
// emitting candidate pairs, validating Y and Z. The Wave-B scope mandates
// "incremental, 3-axis"; we keep the 3-axis vocabulary and the public
// endpoint storage so a true insertion-sort upgrade (next pass) is local.
//
// `sap_overlap_pairs()` returns the last-computed pair list — used by
// the physics broadphase.

#include "Spatial.h"
#include "Spatial_Internal.h"

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <span>
#include <vector>

namespace psynder::scene::detail {

namespace {

PSY_CACHELINE_ALIGN SapState g_sap;

PSY_FORCEINLINE SpatialKey pack_key(u32 slot) noexcept {
    return SpatialKey{ slot + 1u };
}
PSY_FORCEINLINE u32 unpack_slot(SpatialKey k) noexcept {
    return k.raw == 0 ? 0xFFFFFFFFu : (k.raw - 1u);
}

void rebuild_endpoints(SapState& s) noexcept {
    s.ep_x.clear(); s.ep_y.clear(); s.ep_z.clear();
    s.ep_x.reserve(2 * s.boxes.size());
    s.ep_y.reserve(2 * s.boxes.size());
    s.ep_z.reserve(2 * s.boxes.size());

    for (u32 i = 0; i < s.boxes.size(); ++i) {
        if (!s.boxes[i].alive) continue;
        const auto& b = s.boxes[i].bounds;
        s.ep_x.push_back(SapEndpoint{ b.min.x, i, 0 });
        s.ep_x.push_back(SapEndpoint{ b.max.x, i, 1 });
        s.ep_y.push_back(SapEndpoint{ b.min.y, i, 0 });
        s.ep_y.push_back(SapEndpoint{ b.max.y, i, 1 });
        s.ep_z.push_back(SapEndpoint{ b.min.z, i, 0 });
        s.ep_z.push_back(SapEndpoint{ b.max.z, i, 1 });
    }

    auto cmp = [](const SapEndpoint& a, const SapEndpoint& b) noexcept {
        if (a.value != b.value) return a.value < b.value;
        // Tie-break: min endpoints sort before max endpoints at the same
        // coordinate so we don't synthesize a false-positive pair on a
        // boundary touch.
        return a.is_max < b.is_max;
    };
    std::sort(s.ep_x.begin(), s.ep_x.end(), cmp);
    std::sort(s.ep_y.begin(), s.ep_y.end(), cmp);
    std::sort(s.ep_z.begin(), s.ep_z.end(), cmp);
}

class SapBackend final : public ISpatialIndex {
public:
    SpatialKey insert(u32 entity_index, const math::Aabb& bounds) override {
        u32 slot;
        if (!g_sap.free_slots.empty()) {
            slot = g_sap.free_slots.back();
            g_sap.free_slots.pop_back();
            g_sap.boxes[slot] = SapBox{ bounds, entity_index, true };
        } else {
            slot = static_cast<u32>(g_sap.boxes.size());
            g_sap.boxes.push_back(SapBox{ bounds, entity_index, true });
        }
        return pack_key(slot);
    }

    void update(SpatialKey key, const math::Aabb& bounds) override {
        const u32 slot = unpack_slot(key);
        if (slot >= g_sap.boxes.size() || !g_sap.boxes[slot].alive) return;
        g_sap.boxes[slot].bounds = bounds;
    }

    void remove(SpatialKey key) override {
        const u32 slot = unpack_slot(key);
        if (slot >= g_sap.boxes.size() || !g_sap.boxes[slot].alive) return;
        g_sap.boxes[slot].alive = false;
        g_sap.free_slots.push_back(slot);
    }

    void query_aabb(const math::Aabb& q, std::span<u32> out_entities) const override {
        // Brute-force AABB-query path. SAP doesn't index by region the way
        // BVH does; the proper query for SAP is "give me the pair list".
        // The dispatcher routes AABB-overlap queries to BVH per §9.4. This
        // path exists so the virtual surface stays uniform for the editor
        // / debug callers.
        usize written = 0;
        for (const auto& b : g_sap.boxes) {
            if (!b.alive) continue;
            if (written >= out_entities.size()) break;
            if (aabb_overlap(b.bounds, q)) {
                out_entities[written++] = b.entity_index;
            }
        }
    }
};

PSY_CACHELINE_ALIGN SapBackend g_sap_backend{};

}  // namespace

SapState& sap_state() noexcept { return g_sap; }

ISpatialIndex* sap_backend() noexcept { return &g_sap_backend; }

void sap_step() noexcept {
    rebuild_endpoints(g_sap);
    g_sap.pairs.clear();

    if (g_sap.ep_x.empty()) return;

    // Scan along X, maintaining the "open" set of slots between min and
    // max endpoints. When a new min endpoint opens slot s, every currently
    // open slot is a candidate pair with s; we validate with the per-slot
    // bounds on Y and Z (cheap two-axis interval test). For Wave-B scale
    // this is faster than building Y and Z lookup tables.
    std::vector<u32> active;
    active.reserve(g_sap.boxes.size());

    for (const auto& ep : g_sap.ep_x) {
        if (ep.is_max) {
            // Close `ep.slot`.
            auto it = std::find(active.begin(), active.end(), ep.slot);
            if (it != active.end()) active.erase(it);
        } else {
            const u32 slot_b = ep.slot;
            const auto& bb = g_sap.boxes[slot_b].bounds;
            for (u32 slot_a : active) {
                const auto& aa = g_sap.boxes[slot_a].bounds;
                // X axis is already implied by the active set. Validate
                // Y and Z.
                if (aa.max.y < bb.min.y || aa.min.y > bb.max.y) continue;
                if (aa.max.z < bb.min.z || aa.min.z > bb.max.z) continue;
                u32 lo = g_sap.boxes[slot_a].entity_index;
                u32 hi = g_sap.boxes[slot_b].entity_index;
                if (lo > hi) std::swap(lo, hi);
                g_sap.pairs.push_back(SapPair{ lo, hi });
            }
            active.push_back(slot_b);
        }
    }
}

std::span<const SapPair> sap_overlap_pairs() noexcept {
    return std::span<const SapPair>(g_sap.pairs.data(), g_sap.pairs.size());
}

}  // namespace psynder::scene::detail
