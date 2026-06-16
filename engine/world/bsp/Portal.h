// SPDX-License-Identifier: MIT
// Psynder — portal-culling internal API. Lane 10 (world-bsp) owns.
//
// PVS gives us coarse per-leaf visibility. Portals tighten that by clipping
// the view frustum at each leaf boundary so distant leaves whose portals fall
// outside the *clipped* frustum get rejected even when PVS-visible.
//
// Wave A declares the data structures and the API surface. The traversal
// implementation lands in Wave B; in Wave A `walk_portal_visible_leaves`
// degrades to the same answer as PVS-only `walk_visible_leaves` (every PVS
// hit is reported with a no-op clipped frustum). That keeps integration with
// the renderer stable as Wave B fills in the clipping math.

#pragma once

#include <vector>  // Bsp.h uses std::vector without including <vector>; see Bsp.cpp.
#include "Bsp.h"
#include "math/Math.h"

#include <span>

namespace psynder::world::bsp {

// A portal is a convex polygon on a leaf boundary, sharing an edge with the
// two leaves it connects. Stored as an indexed run into a flat vertex pool;
// `winding` is in CCW order when looking from `front_leaf` toward
// `back_leaf` (so the polygon normal points front → back).
struct BspPortal {
    i32 front_leaf;
    i32 back_leaf;
    u32 first_vertex;
    u32 vertex_count;
    math::Vec3 plane_normal;
    f32 plane_d;
};

// Per-BSP portal table. Lives next to the BspMap (or beside it on disk in a
// future .psybsp format bump); kept separate from BspMap so the public ABI
// in Bsp.h is preserved while Wave B fills this in.
struct BspPortalSet {
    std::vector<BspPortal> portals;
    std::vector<math::Vec3> vertices;
};

// Max planes a clipped portal frustum can hold: the carried-over incoming
// planes plus one inward side plane per portal edge. `clip_to_portal`
// (PortalClip.cpp) accumulates up to this many, so the arrays below MUST be
// sized to it — they used to be [6] while the clip cap was 16, which let the
// clip loop write past the end (stack-buffer-overflow caught by ASan/UBSan).
inline constexpr u32 kMaxFrustumPlanes = 16;

// Convex frustum used for portal clipping. Normals point inward; a point p is
// inside iff dot(p, normals[i]) >= d[i] for all `plane_count` planes. Seeded
// with the camera frustum (≤6 planes); portal clipping adds side planes up to
// kMaxFrustumPlanes.
struct PortalFrustum {
    math::Vec3 normals[kMaxFrustumPlanes];
    f32 d[kMaxFrustumPlanes];
    u32 plane_count;
};

// Walk leaves visible from `eye` via portal clipping. Each emit() invocation
// passes the leaf and the clipped frustum that reached it (useful for the
// renderer to in-leaf-cull DrawItems). Wave A impl: degrades to PVS-only.
void walk_portal_visible_leaves(const BspMap& map,
                                const BspPortalSet& portals,
                                math::Vec3 eye,
                                const PortalFrustum& initial,
                                void (*emit)(const BspLeaf&, const PortalFrustum&, void* user),
                                void* user);

// Build a portal set from a compiled BspMap. Wave B will derive portals from
// the leaf-pairing edges produced by lm_qbsp; the Wave A stub returns an
// empty set so callers see "no portals known → fall back to PVS".
BspPortalSet build_portal_set(const BspMap& map);

}  // namespace psynder::world::bsp
