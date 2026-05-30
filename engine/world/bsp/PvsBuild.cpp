// SPDX-License-Identifier: MIT
// Psynder - runtime PVS builder via leaf-portal flood-fill. Lane 10 owns.
//
// See PvsBuild.h for the technique writeup (Quake-style coarse portal flood,
// Teller 1992 / Carmack qvis base-PVS stage). This TU owns the implementation:
//
//   1. Build a CSR (compressed-sparse-row) leaf adjacency list from the portal
//      set. Each portal (front_leaf -> back_leaf) is a directed open-space edge;
//      to make reachability symmetric (visibility through a portal is mutual) we
//      add BOTH directions. Solid leaves and out-of-range indices are skipped.
//
//   2. For each non-solid cluster, flood (BFS) the leaf graph from every leaf in
//      that cluster, marking every cluster reachable through portal chains as
//      visible. Set those bits in the cluster's PVS row. A cluster always sees
//      itself.
//
// Everything is integer-only -> bit-identical determinism across runs/arches.

// Bsp.h uses std::vector without including <vector>; pre-include to match the
// convention established by Bsp.cpp.
#include <vector>

#include "PvsBuild.h"

#include "Bsp.h"
#include "BspFormat.h"
#include "Portal.h"

#include <algorithm>

namespace psynder::world::bsp {

void PvsBuildScratch::reserve_for(usize leaf_count, usize cluster_count, usize portal_count) {
    leaf_adjacency_offset.reserve(leaf_count + 1);
    // Each portal becomes two directed edges (front->back and back->front).
    leaf_adjacency.reserve(portal_count * 2);
    bfs_queue.reserve(leaf_count);
    cluster_seen.reserve(cluster_count);
    leaf_visited.reserve(leaf_count);
}

namespace {

// Compute the highest cluster id over all non-solid leaves; returns -1 if every
// leaf is solid (no PVS to build).
i32 max_cluster_id(const BspMap& map) {
    i32 max_cluster = -1;
    for (const BspLeaf& l : map.leaves) {
        if (l.cluster > max_cluster)
            max_cluster = l.cluster;
    }
    return max_cluster;
}

// True if a leaf index is a real, non-solid leaf we can flood through.
bool open_leaf(const BspMap& map, i32 leaf_index) {
    if (leaf_index < 0 || static_cast<usize>(leaf_index) >= map.leaves.size())
        return false;
    return map.leaves[static_cast<usize>(leaf_index)].cluster >= 0;
}

}  // namespace

u32 build_pvs(const BspMap& map,
              const BspPortalSet& portals,
              PvsBuildScratch& scratch,
              std::vector<u8>& out_pvs,
              u32& out_row_bytes) {
    out_pvs.clear();
    out_row_bytes = 0u;

    const i32 max_cluster = max_cluster_id(map);
    if (max_cluster < 0) {
        return 0u;  // no non-solid clusters -> nothing to build
    }
    const u32 cluster_count = static_cast<u32>(max_cluster + 1);
    const u32 row_bytes = (cluster_count + 7u) / 8u;  // ceil(C/8), matches on-disk
    const usize leaf_count = map.leaves.size();

    // --- 1. CSR leaf adjacency from portals (both directions) --------------
    // First pass: count out-degree per leaf (front->back and back->front).
    auto& offset = scratch.leaf_adjacency_offset;
    auto& adjacency = scratch.leaf_adjacency;
    offset.assign(leaf_count + 1, 0u);

    auto edge_valid = [&](i32 a, i32 b) {
        return open_leaf(map, a) && open_leaf(map, b);
    };

    for (const BspPortal& p : portals.portals) {
        if (!edge_valid(p.front_leaf, p.back_leaf))
            continue;
        ++offset[static_cast<usize>(p.front_leaf) + 1];
        ++offset[static_cast<usize>(p.back_leaf) + 1];
    }
    // Prefix-sum the counts into row offsets.
    for (usize i = 1; i <= leaf_count; ++i) {
        offset[i] += offset[i - 1];
    }
    const u32 edge_total = offset.empty() ? 0u : offset.back();
    adjacency.assign(edge_total, -1);

    // Second pass: scatter neighbours into the CSR slots. We need a per-leaf
    // write cursor that starts at each row's offset; bfs_queue is unused at this
    // point (the flood below re-purposes it after the scatter completes), so we
    // borrow it as the fill cursor to avoid a separate allocation.
    auto& fill_cursor = scratch.bfs_queue;
    fill_cursor.assign(leaf_count, 0);
    for (usize i = 0; i < leaf_count; ++i) {
        fill_cursor[i] = static_cast<i32>(offset[i]);
    }
    auto push_edge = [&](i32 from, i32 to) {
        const i32 slot = fill_cursor[static_cast<usize>(from)]++;
        adjacency[static_cast<usize>(slot)] = to;
    };
    for (const BspPortal& p : portals.portals) {
        if (!edge_valid(p.front_leaf, p.back_leaf))
            continue;
        push_edge(p.front_leaf, p.back_leaf);
        push_edge(p.back_leaf, p.front_leaf);
    }

    // --- 2. Allocate the output table (zeroed) -----------------------------
    out_pvs.assign(static_cast<usize>(cluster_count) * row_bytes, 0u);
    out_row_bytes = row_bytes;

    auto set_bit = [&](u32 row, u32 cluster) {
        const usize byte = static_cast<usize>(row) * row_bytes + (cluster >> 3);
        out_pvs[byte] |= static_cast<u8>(1u << (cluster & 7u));
    };

    // --- 3. Flood the portal graph from each cluster -----------------------
    // For each cluster we BFS the leaf graph starting from EVERY leaf in that
    // cluster, marking every cluster we can reach as visible. A leaf-visited
    // flag isn't needed separately: once a leaf's cluster is "seen" we still
    // must keep flooding through it (two leaves can share a cluster yet open
    // onto different portals), so we track a per-leaf visited flag for the BFS
    // and a per-cluster seen flag for the output bits.
    auto& bfs = scratch.bfs_queue;  // re-purpose after CSR fill (fill_cursor done)
    auto& cluster_seen = scratch.cluster_seen;
    auto& leaf_visited = scratch.leaf_visited;  // BFS frontier guard (reused)
    leaf_visited.assign(leaf_count, 0u);

    for (u32 src = 0; src < cluster_count; ++src) {
        // A cluster always sees itself.
        set_bit(src, src);

        // Seed the BFS with every leaf belonging to this cluster.
        cluster_seen.assign(cluster_count, 0u);
        cluster_seen[src] = 1u;
        std::fill(leaf_visited.begin(), leaf_visited.end(), 0u);
        bfs.clear();
        for (usize li = 0; li < leaf_count; ++li) {
            if (map.leaves[li].cluster == static_cast<i32>(src)) {
                leaf_visited[li] = 1u;
                bfs.push_back(static_cast<i32>(li));
            }
        }

        // Standard BFS over the leaf adjacency. Visiting a leaf marks its
        // cluster bit; we enqueue every unvisited neighbour.
        usize head = 0;
        while (head < bfs.size()) {
            const i32 leaf = bfs[head++];
            const u32 lo = offset[static_cast<usize>(leaf)];
            const u32 hi = offset[static_cast<usize>(leaf) + 1];
            for (u32 e = lo; e < hi; ++e) {
                const i32 nb = adjacency[e];
                if (nb < 0 || static_cast<usize>(nb) >= leaf_count)
                    continue;
                if (leaf_visited[static_cast<usize>(nb)])
                    continue;
                leaf_visited[static_cast<usize>(nb)] = 1u;
                const i32 nb_cluster = map.leaves[static_cast<usize>(nb)].cluster;
                if (nb_cluster >= 0 && static_cast<u32>(nb_cluster) < cluster_count) {
                    if (!cluster_seen[static_cast<u32>(nb_cluster)]) {
                        cluster_seen[static_cast<u32>(nb_cluster)] = 1u;
                        set_bit(src, static_cast<u32>(nb_cluster));
                    }
                }
                bfs.push_back(nb);
            }
        }
    }

    return cluster_count;
}

}  // namespace psynder::world::bsp
