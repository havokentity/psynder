// SPDX-License-Identifier: MIT
// Psynder - Lane 10 (world-bsp) unit test for the runtime PVS *builder*.
//
// world_bsp_pvs.cpp tests that a PRE-BAKED PVS table is consumed correctly.
// This test covers the opposite direction: given a leaf-portal graph but NO
// pre-baked PVS, `build_pvs` must FLOOD the portal graph and produce a PVS
// table that culls leaves unreachable through portals.
//
// Topology - a four-leaf "linear corridor with a dead-end branch":
//
//        portal A         portal B
//   leaf0  <----->  leaf1  <----->  leaf2        leaf3 (isolated, no portal)
//   clu0            clu1            clu2          clu3
//
//   * leaf0 (cluster 0) opens onto leaf1 via portal A.
//   * leaf1 (cluster 1) opens onto leaf2 via portal B.
//   * leaf3 (cluster 3) has NO portal - it is an island the camera can never
//     reach through open space, so it must be culled from every other PVS row.
//
// Expected flood result (each cluster sees itself + everything portal-reachable):
//   cluster 0 -> {0, 1, 2}   (0 reaches 1 directly, 2 transitively via 1)
//   cluster 1 -> {0, 1, 2}
//   cluster 2 -> {0, 1, 2}
//   cluster 3 -> {3}         (island - sees only itself)
//
// We then feed the built table back into `walk_visible_leaves` and assert the
// runtime cull agrees, proving the builder output is drop-in compatible with
// the on-disk PVS path.

#include <catch2/catch_test_macros.hpp>

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/Portal.h"
#include "world/bsp/PvsBuild.h"

#include <set>

using namespace psynder;
using namespace psynder::world::bsp;

namespace {

// Build a 4-leaf map. Tree: a chain of x-split planes carving 4 slabs along x,
// so locate() lands cleanly in each leaf for representative points.
//
//   x: [-2,-1)=leaf0  [-1,0)=leaf1  [0,1)=leaf2  [1,2)=leaf3
BspMap make_corridor_map() {
    BspMap map;

    // node0: x = -1; front (x>=-1) -> node1, back (x<-1) -> leaf0
    BspNode n0{};
    n0.plane_normal = {1, 0, 0};
    n0.plane_d = -1.0f;
    n0.front_child = 1;
    n0.back_child = bsp_encode_leaf(0);

    // node1: x = 0; front (x>=0) -> node2, back (x<0) -> leaf1
    BspNode n1{};
    n1.plane_normal = {1, 0, 0};
    n1.plane_d = 0.0f;
    n1.front_child = 2;
    n1.back_child = bsp_encode_leaf(1);

    // node2: x = 1; front (x>=1) -> leaf3, back (x<1) -> leaf2
    BspNode n2{};
    n2.plane_normal = {1, 0, 0};
    n2.plane_d = 1.0f;
    n2.front_child = bsp_encode_leaf(3);
    n2.back_child = bsp_encode_leaf(2);

    map.nodes = {n0, n1, n2};

    auto leaf = [](i32 cluster, f32 x0, f32 x1) {
        BspLeaf l{};
        l.cluster = cluster;
        l.first_face = 0;
        l.face_count = 0;
        l.bounds.min = {x0, -1, -1};
        l.bounds.max = {x1, 1, 1};
        return l;
    };
    map.leaves = {leaf(0, -2, -1), leaf(1, -1, 0), leaf(2, 0, 1), leaf(3, 1, 2)};
    map.faces = {};
    map.pvs = {};  // intentionally empty - the builder must synthesise it
    return map;
}

// Two portals: A connects leaf0<->leaf1, B connects leaf1<->leaf2. leaf3 has no
// portal. The polygon windings/planes are irrelevant to the coarse flood (it
// only uses front_leaf/back_leaf adjacency), so we leave them at defaults.
BspPortalSet make_corridor_portals() {
    BspPortalSet set;
    BspPortal a{};
    a.front_leaf = 0;
    a.back_leaf = 1;
    set.portals.push_back(a);

    BspPortal b{};
    b.front_leaf = 1;
    b.back_leaf = 2;
    set.portals.push_back(b);
    return set;
}

struct Visited {
    std::set<i32> clusters;
};
void collect_emit(const BspLeaf& leaf, void* user) {
    static_cast<Visited*>(user)->clusters.insert(leaf.cluster);
}

}  // namespace

TEST_CASE("world_bsp/build_pvs floods the portal graph", "[world_bsp][pvs_build]") {
    const BspMap map = make_corridor_map();
    const BspPortalSet portals = make_corridor_portals();

    PvsBuildScratch scratch;
    std::vector<u8> pvs;
    u32 row_bytes = 0u;
    const u32 clusters = build_pvs(map, portals, scratch, pvs, row_bytes);

    REQUIRE(clusters == 4u);
    REQUIRE(row_bytes == 1u);  // ceil(4/8) == 1
    REQUIRE(pvs.size() == 4u);

    auto sees = [&](u32 src, u32 dst) -> bool {
        return (pvs[static_cast<usize>(src) * row_bytes + (dst >> 3)] & (1u << (dst & 7u))) != 0u;
    };

    // The connected corridor {0,1,2} forms a fully mutually-visible component.
    for (u32 a = 0; a < 3; ++a) {
        for (u32 b = 0; b < 3; ++b) {
            REQUIRE(sees(a, b));
        }
        REQUIRE_FALSE(sees(a, 3));  // none of the corridor sees the island
    }
    // The island only sees itself.
    REQUIRE(sees(3, 3));
    REQUIRE_FALSE(sees(3, 0));
    REQUIRE_FALSE(sees(3, 1));
    REQUIRE_FALSE(sees(3, 2));
}

TEST_CASE("world_bsp/build_pvs output drives walk_visible_leaves", "[world_bsp][pvs_build]") {
    BspMap map = make_corridor_map();
    const BspPortalSet portals = make_corridor_portals();

    PvsBuildScratch scratch;
    u32 row_bytes = 0u;
    const u32 clusters = build_pvs(map, portals, scratch, map.pvs, row_bytes);
    REQUIRE(clusters == 4u);
    REQUIRE_FALSE(map.pvs.empty());

    // Eye in leaf0 (cluster 0) at x=-1.5: PVS-visible set is the corridor {0,1,2}
    // and the island (cluster 3) is culled.
    Visited v;
    walk_visible_leaves(map, {-1.5f, 0.0f, 0.0f}, &collect_emit, &v);
    REQUIRE(v.clusters == std::set<i32>{0, 1, 2});

    // Eye in the island (leaf3, x=1.5) sees only itself.
    Visited isle;
    walk_visible_leaves(map, {1.5f, 0.0f, 0.0f}, &collect_emit, &isle);
    REQUIRE(isle.clusters == std::set<i32>{3});
}

TEST_CASE("world_bsp/build_pvs is deterministic and alloc-stable on reuse",
          "[world_bsp][pvs_build]") {
    const BspMap map = make_corridor_map();
    const BspPortalSet portals = make_corridor_portals();

    PvsBuildScratch scratch;
    scratch.reserve_for(map.leaves.size(), 4u, portals.portals.size());

    std::vector<u8> a;
    std::vector<u8> b;
    u32 rb_a = 0u;
    u32 rb_b = 0u;
    (void)build_pvs(map, portals, scratch, a, rb_a);
    // Re-running with the same (now-warm) scratch must yield a bit-identical
    // table - the flood is integer-only, so there is no FP nondeterminism.
    (void)build_pvs(map, portals, scratch, b, rb_b);
    REQUIRE(rb_a == rb_b);
    REQUIRE(a == b);
}

TEST_CASE("world_bsp/build_pvs handles a portal-free map", "[world_bsp][pvs_build]") {
    const BspMap map = make_corridor_map();
    const BspPortalSet empty_portals;  // no portals at all

    PvsBuildScratch scratch;
    std::vector<u8> pvs;
    u32 row_bytes = 0u;
    const u32 clusters = build_pvs(map, empty_portals, scratch, pvs, row_bytes);

    REQUIRE(clusters == 4u);
    // With no portals every cluster is an island: each row has only its own bit.
    for (u32 c = 0; c < 4; ++c) {
        REQUIRE(pvs[c] == static_cast<u8>(1u << c));
    }
}
