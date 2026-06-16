// SPDX-License-Identifier: MIT
// Psynder - runtime PVS builder via leaf-portal flood-fill. Lane 10 owns.
//
// WHY THIS EXISTS
// ---------------
// `Bsp::load` consumes a pre-baked PVS bit-vector table emitted by the offline
// `lm_qbsp` compiler (lane 24, .psybsp on disk). That covers shipped, compiled
// maps. But a level authored *in memory* at runtime - a procedural arena, an
// editor preview, or a game demo that assembles rooms+corridors from code (see
// games/duke_demo) - has a BspMap + BspPortalSet but no PVS rows. This builder
// fills that gap: it derives the per-cluster PVS bit-vectors from the leaf-
// portal adjacency graph so the same `walk_visible_leaves` runtime cull works
// on a hand-built map exactly as it does on a compiled one.
//
// TECHNIQUE - Quake-style leaf-portal flood (Teller/Carmack)
// ----------------------------------------------------------
// The full Quake VIS algorithm (Seth Teller, "Visibility Computations in
// Densely Occluded Polyhedral Environments", 1992; implemented in Quake's
// qvis by John Carmack) clips antipenumbra "separating planes" through every
// portal chain to compute an exact PVS. We implement the well-known *coarse*
// first stage of that pipeline - the "base PVS" / portal flood - which Quake
// itself computes before the expensive plane-clip refinement:
//
//   * Two leaves share visibility if there is a path of portals connecting
//     them through open (non-solid) space. We flood the portal graph from each
//     source cluster's leaves and mark every cluster reachable through portals
//     as potentially visible.
//
// This is conservative (it never marks a truly-visible cluster as hidden - the
// cardinal PVS safety rule: over-include is correct, under-include is a render
// bug) and it is exactly the granularity that produces real culling on an
// indoor portal level: a leaf in a room down a closed-off corridor branch that
// the camera's room cannot reach through any portal chain is culled. The
// frustum-clip refinement (PortalClip.cpp's `walk_portal_visible_leaves`)
// tightens this further at *runtime* per frame, so the coarse build + runtime
// portal clip together approximate full Quake VIS without the offline cost.
//
// DETERMINISM / ALLOC-FREE
// ------------------------
// The flood is a plain BFS over fixed integer indices: no floating-point, so
// the output bit-vectors are bit-identical across runs and platforms. The
// builder takes a caller-owned PvsBuildScratch so the work buffers can be
// reserved once and reused - `build_pvs` performs ZERO heap allocation when the
// scratch is already sized for the map (the demo reserves at load).

#pragma once

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "Bsp.h"
#include "Portal.h"

namespace psynder::world::bsp {

// Reusable scratch for `build_pvs`. Reserve once (reserve_for) and reuse across
// builds so the steady-state build is alloc-free. The vectors are only ever
// clear()ed + grown to a known bound, never freed, so a second build on a map
// no larger than the first allocates nothing.
struct PvsBuildScratch {
    std::vector<u32> leaf_adjacency_offset;  // CSR row offsets, size = leaf_count + 1
    std::vector<i32> leaf_adjacency;         // CSR neighbour leaf indices
    std::vector<i32> bfs_queue;              // BFS frontier ring (leaf indices)
    std::vector<u8> cluster_seen;            // per-cluster visited flag for one flood
    std::vector<u8> leaf_visited;            // per-leaf BFS frontier guard

    // Pre-size every buffer for a map of at most `leaf_count` leaves,
    // `cluster_count` clusters, and `portal_count` portals (each portal is a
    // directed edge, so adjacency holds up to portal_count entries). Safe to
    // over-reserve; the build only ever uses the exact counts of the live map.
    void reserve_for(usize leaf_count, usize cluster_count, usize portal_count);
};

// Build the per-cluster PVS bit-vector table for `map` from `portals` and write
// it into `out_pvs` (row-major, `out_row_bytes` bytes per cluster row, one row
// per cluster id 0..max_cluster). `out_row_bytes` is set to ceil(cluster_count
// / 8) - matching the on-disk `pvs_row_bytes` convention so the result can be
// dropped straight into `BspMap::pvs` and consumed by `walk_visible_leaves`.
//
// Returns the cluster count (number of rows). A cluster always sees itself
// (the diagonal bit is set) even if it has no portals. Solid leaves
// (cluster == kBspSolidCluster) contribute no rows and are never marked
// visible. Returns 0 (and clears out_pvs) if the map has no non-solid clusters.
//
// `scratch` may be empty on first call (it will grow); pass the same scratch on
// subsequent calls to avoid re-allocating. The function is deterministic and,
// once the scratch is sized, allocation-free apart from out_pvs growth.
u32 build_pvs(const BspMap& map,
              const BspPortalSet& portals,
              PvsBuildScratch& scratch,
              std::vector<u8>& out_pvs,
              u32& out_row_bytes);

}  // namespace psynder::world::bsp
