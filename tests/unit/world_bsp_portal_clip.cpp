// SPDX-License-Identifier: MIT
// Psynder — Lane 10 (world-bsp) Wave B unit tests:
//   * Portal-clip rejection: a leaf hidden behind a fully-occluding portal
//     (the portal is closed off by a degenerate-clip plane that lies entirely
//     outside the incoming frustum) must NOT be emitted by
//     walk_portal_visible_leaves.
//   * LightmapAtlas page identity: repeat queries for the same face id return
//     the same LightmapPage* — the atlas caches and LRU-touches, never
//     reallocates a fresh page for a hot id.

#include <catch2/catch_test_macros.hpp>

#include <vector>  // Bsp.h convention.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"
#include "world/bsp/LightmapAtlas.h"
#include "world/bsp/Portal.h"
#include "world/bsp/PortalClip.h"

#include "core/alloc/Allocator.h"

#include <set>
#include <vector>

using namespace psynder;
using namespace psynder::world::bsp;

namespace {

// Two-leaf BSP joined by a single portal. Leaf 0 (eye side) is in front of
// the splitter plane y=0; leaf 1 (the leaf we want to reject) is behind.
//
// We build the portal polygon (a unit square on y=0) far to one side of the
// eye so that when we wrap the eye in a tight initial frustum that DOESN'T
// include the portal polygon, the portal is rejected and leaf 1 stays hidden.
BspMap make_two_leaf_map() {
    BspMap map;

    BspNode n0{};
    n0.plane_normal = {0, 1, 0};
    n0.plane_d      = 0.0f;
    n0.front_child  = bsp_encode_leaf(0);   // leaf 0 (y > 0)
    n0.back_child   = bsp_encode_leaf(1);   // leaf 1 (y < 0)
    map.nodes = { n0 };

    BspLeaf l0{};
    l0.cluster = 0; l0.first_face = 0; l0.face_count = 0;
    l0.bounds.min = {-10, 0, -10}; l0.bounds.max = { 10, 10, 10};

    BspLeaf l1{};
    l1.cluster = 1; l1.first_face = 0; l1.face_count = 0;
    l1.bounds.min = {-10, -10, -10}; l1.bounds.max = { 10, 0, 10};

    map.leaves = { l0, l1 };
    map.pvs.clear();   // no PVS → conservative-visible default
    return map;
}

// Portal set for the two-leaf map: ONE portal connecting leaf 0 → leaf 1,
// polygon centred at (cx, 0, 0) so we can place it inside or outside the
// test frustum.
BspPortalSet make_portal_at(f32 cx) {
    BspPortalSet ps;
    ps.vertices = {
        math::Vec3{ cx - 0.5f, 0.0f, -0.5f },
        math::Vec3{ cx + 0.5f, 0.0f, -0.5f },
        math::Vec3{ cx + 0.5f, 0.0f,  0.5f },
        math::Vec3{ cx - 0.5f, 0.0f,  0.5f },
    };
    BspPortal p{};
    p.front_leaf    = 0;
    p.back_leaf     = 1;
    p.first_vertex  = 0;
    p.vertex_count  = 4;
    // CCW looking from front_leaf (y > 0) toward back_leaf (y < 0) → normal
    // points -y (front → back).
    p.plane_normal  = { 0.0f, -1.0f, 0.0f };
    p.plane_d       = 0.0f;
    ps.portals = { p };
    return ps;
}

struct Acc { std::set<i32> clusters; };
void acc_emit(const BspLeaf& l, const PortalFrustum&, void* u) {
    static_cast<Acc*>(u)->clusters.insert(l.cluster);
}

}  // namespace

TEST_CASE("world_bsp/portal-clip rejects a leaf behind an occluded portal",
          "[world_bsp][portal]") {
    const BspMap map = make_two_leaf_map();
    // Eye at (0, 0.5, 0) — in leaf 0 (y > 0).
    const math::Vec3 eye{0.0f, 0.5f, 0.0f};

    // Very tight frustum centred on +x, missing the portal at x = -5.
    // We use a 4-plane frustum whose inward normals each pass through the
    // eye, leaving a thin wedge of +x space visible. The portal polygon
    // lives entirely at negative x → fully outside the frustum.
    PortalFrustum frustum{};
    frustum.plane_count = 4;
    // +x half-space (everything left of x=eye.x rejected). normal=(+1,0,0)
    // through eye: dot(n, p) >= dot(n, eye) → x >= 0.
    frustum.normals[0] = { 1.0f, 0.0f, 0.0f };
    frustum.d[0]       = math::dot(frustum.normals[0], eye);
    // Upper bound: x <= 100 (normal = -1,0,0 through (100,...))
    frustum.normals[1] = { -1.0f, 0.0f, 0.0f };
    frustum.d[1]       = math::dot(frustum.normals[1], math::Vec3{100.0f, 0.0f, 0.0f});
    // z slab: |z| <= 100
    frustum.normals[2] = { 0.0f, 0.0f,  1.0f };
    frustum.d[2]       = math::dot(frustum.normals[2], math::Vec3{0.0f, 0.0f, -100.0f});
    frustum.normals[3] = { 0.0f, 0.0f, -1.0f };
    frustum.d[3]       = math::dot(frustum.normals[3], math::Vec3{0.0f, 0.0f,  100.0f});

    SECTION("portal placed inside frustum → back leaf is visible") {
        BspPortalSet portals = make_portal_at(5.0f);  // +x → inside the wedge
        Acc acc;
        walk_portal_visible_leaves(map, portals, eye, frustum, acc_emit, &acc);
        // Eye leaf (cluster 0) always emitted; portal accepts → leaf 1 too.
        REQUIRE(acc.clusters.count(0) == 1);
        REQUIRE(acc.clusters.count(1) == 1);
    }

    SECTION("portal placed outside frustum → back leaf is rejected") {
        BspPortalSet portals = make_portal_at(-5.0f);  // -x → outside the wedge
        Acc acc;
        walk_portal_visible_leaves(map, portals, eye, frustum, acc_emit, &acc);
        // Eye leaf still emitted unconditionally; the occluded portal must
        // not let leaf 1 through.
        REQUIRE(acc.clusters.count(0) == 1);
        REQUIRE(acc.clusters.count(1) == 0);
    }

    SECTION("clip_frustum_by_portal agrees with the BFS rejection") {
        BspPortalSet portals = make_portal_at(-5.0f);
        PortalFrustum clipped{};
        const bool ok = clip_frustum_by_portal(frustum, portals.portals[0],
                                               portals.vertices, eye, clipped);
        REQUIRE_FALSE(ok);  // every portal vertex is strictly outside plane 0
    }
}

TEST_CASE("world_bsp/lightmap atlas returns the same page for repeat queries",
          "[world_bsp][lightmap]") {
    // Carve a 4 MiB slab — enough for ~40 pages × 96 KiB. The test doesn't
    // exercise the full 256-page resident cap, just the cache-hit behaviour.
    std::vector<u8> backing(4u * 1024u * 1024u);
    mem::LinearArena arena{ backing.data(), backing.size() };

    LightmapAtlas atlas;
    atlas.init(&arena);

    LightmapPage* a = atlas.atlas_page_for_surface(42u);
    REQUIRE(a != nullptr);
    REQUIRE(a->page_id == 42u);
    REQUIRE(a->width   == kLightmapPageWidth);
    REQUIRE(a->height  == kLightmapPageHeight);
    REQUIRE(a->pixels  != nullptr);            // arena was non-null
    REQUIRE(atlas.resident_page_count() == 1u);

    // Repeat query for the same id MUST return the same page.
    LightmapPage* b = atlas.atlas_page_for_surface(42u);
    REQUIRE(b == a);
    REQUIRE(atlas.resident_page_count() == 1u);

    // Different id allocates a new page; the original is still cached.
    LightmapPage* c = atlas.atlas_page_for_surface(99u);
    REQUIRE(c != nullptr);
    REQUIRE(c != a);
    REQUIRE(c->page_id == 99u);
    REQUIRE(atlas.resident_page_count() == 2u);

    // Original id is still resident — third query still matches the first.
    LightmapPage* d = atlas.atlas_page_for_surface(42u);
    REQUIRE(d == a);
    REQUIRE(atlas.resident_page_count() == 2u);
}
