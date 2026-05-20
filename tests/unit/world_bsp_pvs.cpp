// SPDX-License-Identifier: MIT
// Psynder — Lane 10 (world-bsp) unit test.
//
// Synthetic 4-leaf BSP: a cross-shaped 2D layout extruded to 3D so PVS bits
// are easy to reason about. Topology:
//
//                       z
//                       │
//        Leaf 2 (cluster 2)
//                       │
//   ───── x ──── + ──── x ───── leaf 1 vs 3 across X
//                       │
//        Leaf 0 (cluster 0)
//                       │
//
//   Leaf 0: cluster 0,   AABB x in [-1,1], z in [-1,0]
//   Leaf 1: cluster 1,   AABB x in [-1,0], z in [0, 1]   (NW quadrant)
//   Leaf 2: cluster 2,   AABB x in [0, 1], z in [0, 1]   (NE quadrant)
//   Leaf 3: cluster 3,   solid (no PVS row used)
//
// PVS rules baked into the test data:
//   cluster 0 sees {0, 1}
//   cluster 1 sees {1, 2}
//   cluster 2 sees {2}
//   cluster 3 is the solid sentinel (never the eye's leaf in a valid scene).
//
// We then assert:
//   * locate() finds the correct leaf for representative points
//   * walk_visible_leaves emits exactly the PVS-visible set
//   * BSP face → DrawItem conversion produces the right material ids

#include <catch2/catch_test_macros.hpp>

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspDraw.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/Portal.h"

#include <algorithm>
#include <set>

using namespace psynder;
using namespace psynder::world::bsp;

namespace {

// Build the synthetic map described above.
BspMap make_four_leaf_map() {
    BspMap map;

    // Tree shape:
    //
    //         node 0 (plane z = 0, +z = front)
    //         /                \
    //   node 1 (z>0, x split)   leaf 0  (back, z<0)
    //   /              \
    // leaf 1 (back)   node 2 (x>0, sub-split for solid pocket)
    //                  /              \
    //              leaf 2 (front)  leaf 3 (back, solid)
    //
    // We construct the "solid pocket" so leaf 3 only fires when a point lands
    // in a narrow band. That's fine for the test — we only call locate()
    // with points that *don't* land in the solid leaf.

    BspNode n0{};  // splits on plane z = 0; front = +z half.
    n0.plane_normal = {0, 0, 1};
    n0.plane_d = 0.0f;
    n0.front_child = 1;                  // node 1 (upper half)
    n0.back_child = bsp_encode_leaf(0);  // leaf 0 (lower half)

    BspNode n1{};  // upper-half split on plane x = 0.
    n1.plane_normal = {1, 0, 0};
    n1.plane_d = 0.0f;
    n1.front_child = 2;                  // node 2 (right side, x > 0)
    n1.back_child = bsp_encode_leaf(1);  // leaf 1 (left side, x < 0)

    BspNode n2{};  // right-side split, plane x = 0.9 (narrow solid wall).
    n2.plane_normal = {1, 0, 0};
    n2.plane_d = 0.9f;
    n2.front_child = bsp_encode_leaf(3);  // leaf 3 (solid pocket x > 0.9)
    n2.back_child = bsp_encode_leaf(2);   // leaf 2 (open right room 0 < x < 0.9)

    map.nodes = {n0, n1, n2};

    // Leaves with sensible bboxes for sanity.
    BspLeaf l0{};
    l0.cluster = 0;
    l0.first_face = 0;
    l0.face_count = 1;
    l0.bounds.min = {-1, -1, -1};
    l0.bounds.max = {1, 1, 0};

    BspLeaf l1{};
    l1.cluster = 1;
    l1.first_face = 1;
    l1.face_count = 1;
    l1.bounds.min = {-1, -1, 0};
    l1.bounds.max = {0, 1, 1};

    BspLeaf l2{};
    l2.cluster = 2;
    l2.first_face = 2;
    l2.face_count = 1;
    l2.bounds.min = {0, -1, 0};
    l2.bounds.max = {0.9f, 1, 1};

    BspLeaf l3{};
    l3.cluster = kBspSolidCluster;
    l3.first_face = 0;
    l3.face_count = 0;
    l3.bounds.min = {0.9f, -1, 0};
    l3.bounds.max = {1, 1, 1};

    map.leaves = {l0, l1, l2, l3};

    // Faces — one per non-solid leaf. Materials are 100/101/102 so the test
    // can check the BSP→DrawItem converter wires `material` through.
    map.faces = {
        BspFace{/*first_vertex*/ 0, /*vertex_count*/ 3, /*material*/ 100, /*lightmap*/ 0},
        BspFace{/*first_vertex*/ 3, /*vertex_count*/ 3, /*material*/ 101, /*lightmap*/ 0},
        BspFace{/*first_vertex*/ 6, /*vertex_count*/ 3, /*material*/ 102, /*lightmap*/ 0},
    };

    // PVS — 3 clusters, 1 byte per row.
    //   row 0: bits 0,1 set → 0b00000011 = 0x03
    //   row 1: bits 1,2 set → 0b00000110 = 0x06
    //   row 2: bit  2  set  → 0b00000100 = 0x04
    map.pvs = {0x03, 0x06, 0x04};

    return map;
}

}  // namespace

TEST_CASE("world_bsp/locate finds the right leaf for representative points", "[world_bsp]") {
    const BspMap map = make_four_leaf_map();

    // (0, 0, -0.5) — lower half, leaf 0.
    REQUIRE(locate(map, {0.0f, 0.0f, -0.5f}).cluster == 0);
    // (-0.5, 0, 0.5) — upper half, x < 0, leaf 1.
    REQUIRE(locate(map, {-0.5f, 0.0f, 0.5f}).cluster == 1);
    // (0.5, 0, 0.5) — upper half, 0 < x < 0.9, leaf 2.
    REQUIRE(locate(map, {0.5f, 0.0f, 0.5f}).cluster == 2);
    // (0.95, 0, 0.5) — solid pocket, cluster < 0.
    REQUIRE(locate(map, {0.95f, 0.0f, 0.5f}).cluster == kBspSolidCluster);
}

namespace {

struct Visited {
    std::set<i32> clusters;
};

static void collect_emit(const BspLeaf& leaf, void* user) {
    static_cast<Visited*>(user)->clusters.insert(leaf.cluster);
}

}  // namespace

TEST_CASE("world_bsp/walk_visible_leaves honours the PVS bit vector", "[world_bsp]") {
    const BspMap map = make_four_leaf_map();

    SECTION("eye in cluster 0 sees clusters {0,1}") {
        Visited v;
        walk_visible_leaves(map, {0.0f, 0.0f, -0.5f}, &collect_emit, &v);
        REQUIRE(v.clusters == std::set<i32>{0, 1});
    }

    SECTION("eye in cluster 1 sees clusters {1,2}") {
        Visited v;
        walk_visible_leaves(map, {-0.5f, 0.0f, 0.5f}, &collect_emit, &v);
        REQUIRE(v.clusters == std::set<i32>{1, 2});
    }

    SECTION("eye in cluster 2 sees only {2}") {
        Visited v;
        walk_visible_leaves(map, {0.5f, 0.0f, 0.5f}, &collect_emit, &v);
        REQUIRE(v.clusters == std::set<i32>{2});
    }

    SECTION("eye in solid leaf sees nothing") {
        Visited v;
        walk_visible_leaves(map, {0.95f, 0.0f, 0.5f}, &collect_emit, &v);
        REQUIRE(v.clusters.empty());
    }
}

TEST_CASE("world_bsp/load rejects malformed blobs", "[world_bsp]") {
    BspMap out;
    // Non-existent vfs path — Vfs::read returns empty blob; load must fail
    // cleanly rather than UB.
    REQUIRE_FALSE(load("nonexistent/whatever.psybsp", out));
    REQUIRE(out.nodes.empty());
    REQUIRE(out.leaves.empty());
}

TEST_CASE("world_bsp/build_leaf_draws produces one DrawItem per face", "[world_bsp]") {
    const BspMap map = make_four_leaf_map();
    BspGeometry geom;
    // Lay down 9 vertices (3 per leaf face). The DrawItem's `vertices`
    // pointer aliases into this vector, so the geometry must outlive the
    // resulting DrawItems.
    geom.vertices.resize(9);
    // Likewise for indices — lm_qbsp emits a parallel index slab; fake the
    // simplest content here.
    geom.indices.resize(9);

    BspMaterialResolve resolve{};  // no override → pass material id through

    std::vector<render::raster::DrawItem> draws;
    build_leaf_draws(map, geom, map.leaves[2], resolve, draws);
    REQUIRE(draws.size() == 1);
    REQUIRE(draws[0].material.raw == 102u);
    REQUIRE(draws[0].vertex_count == 3u);
    REQUIRE(draws[0].vertices != nullptr);
}

TEST_CASE("world_bsp/portal walk degrades to PVS in Wave A", "[world_bsp]") {
    const BspMap map = make_four_leaf_map();
    const BspPortalSet portals = build_portal_set(map);
    REQUIRE(portals.portals.empty());  // Wave A stub returns empty

    PortalFrustum frustum{};
    frustum.plane_count = 0;

    struct Acc {
        std::set<i32> clusters;
    };
    Acc acc;
    auto cb = +[](const BspLeaf& l, const PortalFrustum&, void* u) {
        static_cast<Acc*>(u)->clusters.insert(l.cluster);
    };
    walk_portal_visible_leaves(map, portals, {0.0f, 0.0f, -0.5f}, frustum, cb, &acc);
    // PVS-only fallback must match the cluster-0 result above.
    REQUIRE(acc.clusters == std::set<i32>{0, 1});
}
