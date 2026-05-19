// SPDX-License-Identifier: MIT
// Psynder — portal-graph traversal + frustum clipping. Lane 10 owns.
//
// See PortalClip.h for the strategy. This TU owns:
//   * the public Portal.h entry point `walk_portal_visible_leaves`
//     (Wave A's PVS-fallback stub in Bsp.cpp is replaced here);
//   * the internal helpers declared in PortalClip.h;
//   * `build_portal_set` for now — Wave B's lane 24 lm_qbsp will eventually
//     ship portals in a chunk of .psybsp v2; until that lands we still hand
//     back an empty set so callers fall through to PVS-only behaviour.

// Bsp.h uses std::vector without including <vector>; pre-include here to
// match the convention established by Bsp.cpp.
#include <vector>

#include "PortalClip.h"
#include "Portal.h"
#include "Bsp.h"
#include "BspFormat.h"

#include "math/Math.h"

#include <algorithm>
#include <cmath>

namespace psynder::world::bsp {

namespace {

// Plane representation: dot(normal, p) >= d → inside.
// Side-plane construction: given an eye `e` and a portal edge (v0, v1) in CCW
// winding (looking through the portal toward back_leaf), the inward-pointing
// side plane has normal = cross(v0 - e, v1 - e) (a vector that points into the
// frustum interior because of the CCW order). The plane passes through `e` so
// d = dot(normal, e).
math::Vec3 side_plane_normal(math::Vec3 eye, math::Vec3 v0, math::Vec3 v1) {
    return math::cross(math::sub(v0, eye), math::sub(v1, eye));
}

// Reject a portal polygon against a single plane. Returns true if at least
// one vertex is on the inside (we keep the portal); false if every vertex is
// strictly outside (we can drop this portal entirely).
bool portal_intersects_plane(const BspPortal&                  portal,
                             const std::vector<math::Vec3>&    verts,
                             math::Vec3                        plane_normal,
                             f32                               plane_d) {
    for (u32 i = 0; i < portal.vertex_count; ++i) {
        const u32 vi = portal.first_vertex + i;
        if (vi >= verts.size()) continue;
        if (math::dot(plane_normal, verts[vi]) >= plane_d) {
            return true;
        }
    }
    return false;
}

// Reject a portal polygon against *every* plane in a frustum. Returns true
// iff at least one vertex is on the inside of all planes simultaneously OR
// at least one vertex is inside each plane individually (a strict-inside
// test would require full Sutherland–Hodgman; we use the looser per-plane
// test, which over-approximates visibility — safe).
bool portal_intersects_frustum(const PortalFrustum&            f,
                               const BspPortal&                portal,
                               const std::vector<math::Vec3>&  verts) {
    for (u32 i = 0; i < f.plane_count; ++i) {
        if (!portal_intersects_plane(portal, verts, f.normals[i], f.d[i])) {
            return false;  // every vertex strictly outside plane i → reject
        }
    }
    return true;
}

}  // namespace

// ─── PortalClip.h helpers ────────────────────────────────────────────────

bool point_in_frustum(const PortalFrustum& f, math::Vec3 p) {
    for (u32 i = 0; i < f.plane_count; ++i) {
        if (math::dot(f.normals[i], p) < f.d[i]) {
            return false;
        }
    }
    return true;
}

bool portal_visible_from_eye(const BspPortal& portal, math::Vec3 eye) {
    // Portal winding convention (see Portal.h): CCW when looking from
    // front_leaf toward back_leaf → polygon normal points front → back.
    // An eye in front_leaf therefore lies on the *negative* side of the
    // plane: dot(n, eye) <= d. Strict equality is OK too (eye exactly on
    // the portal plane — boundary case; let the portal through).
    return math::dot(portal.plane_normal, eye) <= portal.plane_d;
}

bool clip_frustum_by_portal(const PortalFrustum&             in,
                            const BspPortal&                 portal,
                            const std::vector<math::Vec3>&   verts,
                            math::Vec3                       eye,
                            PortalFrustum&                   out) {
    // 1. Quick reject: portal polygon entirely outside the incoming frustum.
    if (!portal_intersects_frustum(in, portal, verts)) {
        return false;
    }

    // 2. Copy the existing planes (we only *tighten* on the way in).
    out.plane_count = 0;
    const u32 carry = std::min<u32>(in.plane_count, kMaxFrustumPlanes);
    for (u32 i = 0; i < carry; ++i) {
        out.normals[i] = in.normals[i];
        out.d[i]       = in.d[i];
    }
    out.plane_count = carry;

    // 3. Add one side plane per portal edge until we hit the cap. CCW edges
    //    (v0 → v1) produce inward-pointing normals via the cross-product
    //    above; the plane passes through `eye`.
    if (portal.vertex_count < 3) {
        // Degenerate portal — keep the incoming frustum verbatim.
        return true;
    }
    for (u32 i = 0; i < portal.vertex_count; ++i) {
        if (out.plane_count >= kMaxFrustumPlanes) break;
        const u32 i0 = portal.first_vertex + i;
        const u32 i1 = portal.first_vertex + ((i + 1) % portal.vertex_count);
        if (i0 >= verts.size() || i1 >= verts.size()) continue;
        const math::Vec3 v0 = verts[i0];
        const math::Vec3 v1 = verts[i1];
        const math::Vec3 n  = side_plane_normal(eye, v0, v1);
        // Filter out zero-area edges (collinear with eye — no side plane).
        if (math::dot(n, n) <= 1e-12f) continue;
        const math::Vec3 nn = math::normalize(n);
        out.normals[out.plane_count] = nn;
        out.d[out.plane_count]       = math::dot(nn, eye);
        out.plane_count++;
    }
    return true;
}

// ─── Portal.h public API ─────────────────────────────────────────────────

// Walk leaves visible from `eye` via portal clipping. When `portals.portals`
// is empty (lm_qbsp hasn't emitted a portals chunk yet, or the level is
// portal-free), we degrade to the PVS-only walk — matching Wave A behaviour.
//
// When portals are present we do a BFS:
//   * the eye's leaf is emitted with the initial frustum;
//   * for each portal whose `front_leaf` is the current leaf, clip the
//     incoming frustum by the portal polygon's side planes; if the clipped
//     frustum is non-empty, enqueue (back_leaf, clipped) for visit.
// Each leaf is visited at most once per walk.
void walk_portal_visible_leaves(const BspMap&         map,
                                const BspPortalSet&   portals,
                                math::Vec3            eye,
                                const PortalFrustum&  initial,
                                void (*emit)(const BspLeaf&,
                                             const PortalFrustum&,
                                             void* user),
                                void*                 user) {
    if (emit == nullptr || map.leaves.empty()) return;

    // No portal data → fall back to PVS-only with the unclipped frustum.
    if (portals.portals.empty()) {
        struct Ctx {
            void (*cb)(const BspLeaf&, const PortalFrustum&, void*);
            void* user;
            const PortalFrustum* frustum;
        };
        Ctx ctx{ emit, user, &initial };
        auto bridge = +[](const BspLeaf& leaf, void* u) {
            Ctx& c = *static_cast<Ctx*>(u);
            c.cb(leaf, *c.frustum, c.user);
        };
        walk_visible_leaves(map, eye, bridge, &ctx);
        return;
    }

    // We need the leaf *index* (BspPortal::front_leaf is an index, not a
    // cluster id). Re-run the locate descent ourselves so we recover the
    // index directly instead of a value copy.
    auto locate_leaf_index = [&](math::Vec3 p) -> i32 {
        const usize leaves = map.leaves.size();
        if (leaves == 0) return -1;
        if (map.nodes.empty()) return 0;
        i32 node_index   = 0;
        const i32 max_depth = static_cast<i32>(leaves) * 2 + 64;
        for (i32 step = 0; step < max_depth; ++step) {
            const BspNode& n = map.nodes[static_cast<usize>(node_index)];
            const f32 d = math::dot(n.plane_normal, p) - n.plane_d;
            const i32 child = (d >= 0.0f) ? n.front_child : n.back_child;
            if (bsp_is_leaf(child)) {
                return bsp_leaf_index(child);
            }
            node_index = child;
        }
        return 0;
    };

    const i32 root_idx = locate_leaf_index(eye);
    if (root_idx < 0 || static_cast<usize>(root_idx) >= map.leaves.size()) return;
    if (map.leaves[static_cast<usize>(root_idx)].cluster < 0) {
        // Eye inside solid geometry — nothing to draw.
        return;
    }

    const usize leaf_count = map.leaves.size();
    std::vector<u8> visited(leaf_count, 0u);

    // BFS frontier: small static-ish ring buffer is overkill — use a vector.
    struct Frame { i32 leaf; PortalFrustum frustum; };
    std::vector<Frame> queue;
    queue.reserve(leaf_count);
    queue.push_back(Frame{ root_idx, initial });
    visited[static_cast<usize>(root_idx)] = 1u;

    // Emit the eye leaf first (renderer assumes the eye leaf is always drawn).
    emit(map.leaves[static_cast<usize>(root_idx)], initial, user);

    usize head = 0;
    while (head < queue.size()) {
        const Frame frame = queue[head++];
        // Iterate every portal whose front_leaf == frame.leaf.
        for (const BspPortal& portal : portals.portals) {
            if (portal.front_leaf != frame.leaf) continue;
            if (!portal_visible_from_eye(portal, eye))         continue;
            if (portal.back_leaf < 0 ||
                static_cast<usize>(portal.back_leaf) >= leaf_count) continue;
            if (visited[static_cast<usize>(portal.back_leaf)])  continue;

            PortalFrustum clipped{};
            if (!clip_frustum_by_portal(frame.frustum, portal,
                                        portals.vertices, eye, clipped)) {
                continue;  // portal fully occluded
            }

            visited[static_cast<usize>(portal.back_leaf)] = 1u;
            const BspLeaf& neighbour =
                map.leaves[static_cast<usize>(portal.back_leaf)];
            emit(neighbour, clipped, user);
            queue.push_back(Frame{ portal.back_leaf, clipped });
        }
    }
}

BspPortalSet build_portal_set(const BspMap& /*map*/) {
    // Wave B: lm_qbsp still doesn't emit a portals chunk on disk (.psybsp v1
    // has no portals; v2 will). When v2 lands and we have actual portal data,
    // we'll parse it here. Until then we return empty so callers transparently
    // fall back to the PVS-only walk above.
    return BspPortalSet{};
}

}  // namespace psynder::world::bsp
