// SPDX-License-Identifier: MIT
// Psynder — lane-06 Wave-B unit tests: spatial-index backends + query router.
//
// Coverage per the brief:
//   - BVH refit after movement still finds the moved entity.
//   - SAP detects a new pair when two AABBs start overlapping.
//   - Hashed grid radius query returns the N nearest entities.
//   - Query router picks BVH for raycast, SAP for broadphase,
//     hashed grid for nearest-neighbour, honoring per-region overrides.
//
// All four backends are reached via the public-ish `scene::detail` surface
// declared in Spatial.h.

#include <catch2/catch_test_macros.hpp>

#include "scene/Spatial.h"

#include "core/Types.h"
#include "math/Math.h"

#include <cmath>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::scene;
using psynder::scene::detail::ISpatialIndex;
using psynder::scene::detail::BvhRefitStats;
using psynder::scene::detail::SapPair;
using psynder::scene::detail::bvh_backend;
using psynder::scene::detail::sap_backend;
using psynder::scene::detail::grid_backend;
using psynder::scene::detail::resolve;
using psynder::scene::detail::bvh_refit;
using psynder::scene::detail::sap_step;
using psynder::scene::detail::sap_overlap_pairs;
using psynder::scene::detail::grid_radius_query;

namespace {

math::Aabb cube(math::Vec3 c, f32 r) {
    return math::Aabb{
        { c.x - r, c.y - r, c.z - r },
        { c.x + r, c.y + r, c.z + r },
    };
}

// Drain every backend between tests by removing every key the test owns.
// (The state is static-singleton; tests must clean up their own slots.)

}  // namespace

TEST_CASE("scene: spatial backends are non-null after Wave B", "[scene][spatial][routing]") {
    REQUIRE(bvh_backend()  != nullptr);
    REQUIRE(sap_backend()  != nullptr);
    REQUIRE(grid_backend() != nullptr);

    REQUIRE(resolve(SpatialBackend::Bvh)            != nullptr);
    REQUIRE(resolve(SpatialBackend::SweepAndPrune) != nullptr);
    REQUIRE(resolve(SpatialBackend::HashedGrid)    != nullptr);
    REQUIRE(resolve(SpatialBackend::None)          == nullptr);
}

TEST_CASE("scene: BVH refit after movement still finds the entity", "[scene][spatial][bvh]") {
    auto* bvh = bvh_backend();

    // Insert four entities; one of them is the "mover".
    SpatialKey k_static_a = bvh->insert(101, cube({-10, 0, 0}, 0.5f));
    SpatialKey k_mover    = bvh->insert(200, cube({ 0, 0, 0}, 0.5f));
    SpatialKey k_static_b = bvh->insert(102, cube({ 10, 0, 0}, 0.5f));
    SpatialKey k_static_c = bvh->insert(103, cube({ 0, 10, 0}, 0.5f));
    REQUIRE(k_mover.valid());

    // Trigger an initial build via a query.
    u32 buf[16] = {};
    bvh->query_aabb(cube({0, 0, 0}, 1.0f), std::span<u32>(buf, 16));
    bool found_at_origin = false;
    for (u32 v : buf) if (v == 200) { found_at_origin = true; break; }
    REQUIRE(found_at_origin);

    // Capture as-built SAH cost — first refit reports cost == as-built.
    BvhRefitStats s0 = bvh_refit();
    REQUIRE(s0.sah_cost_as_built > 0.0f);

    // Move the entity to a far-away cell and refit (no rebuild).
    bvh->update(k_mover, cube({100, 0, 0}, 0.5f));
    BvhRefitStats s1 = bvh_refit();
    // After a large displacement the refit cost should rise — and at this
    // magnitude the §9.4 threshold should trip.
    REQUIRE(s1.sah_cost >= s0.sah_cost_as_built);

    // Query at the new location: the moved entity must still be findable.
    for (auto& v : buf) v = 0;
    bvh->query_aabb(cube({100, 0, 0}, 1.0f), std::span<u32>(buf, 16));
    bool found_after_move = false;
    for (u32 v : buf) if (v == 200) { found_after_move = true; break; }
    REQUIRE(found_after_move);

    // Query at the old location: the moved entity must NOT show up.
    for (auto& v : buf) v = 0;
    bvh->query_aabb(cube({0, 0, 0}, 1.0f), std::span<u32>(buf, 16));
    bool found_at_old = false;
    for (u32 v : buf) if (v == 200) { found_at_old = true; break; }
    REQUIRE_FALSE(found_at_old);

    // Cleanup.
    bvh->remove(k_static_a);
    bvh->remove(k_static_b);
    bvh->remove(k_static_c);
    bvh->remove(k_mover);
}

TEST_CASE("scene: BVH refit recommends async rebuild after big displacement", "[scene][spatial][bvh][rebuild]") {
    auto* bvh = bvh_backend();
    // Build a small tree, then displace one leaf far enough to triple
    // the SAH cost. Threshold per DESIGN.md §9.4 is 1.3×.
    std::vector<SpatialKey> keys;
    for (int i = 0; i < 8; ++i) {
        keys.push_back(bvh->insert(static_cast<u32>(300 + i),
                                    cube({static_cast<f32>(i), 0, 0}, 0.5f)));
    }
    // Force build.
    u32 buf[8] = {};
    bvh->query_aabb(cube({4, 0, 0}, 100.0f), std::span<u32>(buf, 8));
    const BvhRefitStats baseline = bvh_refit();
    REQUIRE(baseline.sah_cost_as_built > 0.0f);

    // Punt one entity far away.
    bvh->update(keys[0], cube({1000, 1000, 1000}, 0.5f));
    const BvhRefitStats after = bvh_refit();
    REQUIRE(after.sah_cost > baseline.sah_cost_as_built);
    REQUIRE(after.should_async_rebuild);

    for (auto k : keys) bvh->remove(k);
}

TEST_CASE("scene: SAP detects new overlap pair when two AABBs collide", "[scene][spatial][sap]") {
    auto* sap = sap_backend();

    // Two boxes that start far apart.
    SpatialKey ka = sap->insert(400, cube({-5, 0, 0}, 0.5f));
    SpatialKey kb = sap->insert(401, cube({ 5, 0, 0}, 0.5f));
    SpatialKey kc = sap->insert(402, cube({ 0, 20, 0}, 0.5f));   // distractor

    sap_step();
    {
        auto pairs = sap_overlap_pairs();
        bool found_400_401 = false;
        for (const auto& p : pairs) {
            if ((p.a == 400 && p.b == 401) || (p.a == 401 && p.b == 400)) {
                found_400_401 = true; break;
            }
        }
        REQUIRE_FALSE(found_400_401);
    }

    // Slide A into B's AABB.
    sap->update(ka, cube({4.7f, 0, 0}, 0.5f));
    sap_step();
    {
        auto pairs = sap_overlap_pairs();
        bool found_400_401 = false;
        for (const auto& p : pairs) {
            if ((p.a == 400 && p.b == 401) || (p.a == 401 && p.b == 400)) {
                found_400_401 = true; break;
            }
        }
        REQUIRE(found_400_401);
    }

    // Slide A back out — pair must drop.
    sap->update(ka, cube({-5, 0, 0}, 0.5f));
    sap_step();
    {
        auto pairs = sap_overlap_pairs();
        bool found_400_401 = false;
        for (const auto& p : pairs) {
            if ((p.a == 400 && p.b == 401) || (p.a == 401 && p.b == 400)) {
                found_400_401 = true; break;
            }
        }
        REQUIRE_FALSE(found_400_401);
    }

    sap->remove(ka);
    sap->remove(kb);
    sap->remove(kc);
}

TEST_CASE("scene: hashed-grid radius query returns nearest entities", "[scene][spatial][grid]") {
    auto* grid = grid_backend();

    // Lay down a 7×7 grid of point-like entities; assert the radius
    // query returns the entities whose AABB intersects the query sphere.
    // (Semantics match grid_radius_query: closest-point on AABB to sphere
    // center must be within `radius`.)
    constexpr f32 kRadius = 3.5f;
    constexpr f32 kExtent = 0.4f;
    std::vector<SpatialKey> keys;
    std::vector<u32>        in_radius;
    for (int i = -3; i <= 3; ++i) {
        for (int j = -3; j <= 3; ++j) {
            const u32 ent = static_cast<u32>(500 + (i + 3) * 7 + (j + 3));
            keys.push_back(grid->insert(ent, cube({ static_cast<f32>(i),
                                                    static_cast<f32>(j),
                                                    0.0f }, kExtent)));
            // Closest point on the entity's AABB to origin.
            const f32 cx = std::max(std::abs(static_cast<f32>(i)) - kExtent, 0.0f);
            const f32 cy = std::max(std::abs(static_cast<f32>(j)) - kExtent, 0.0f);
            if (std::sqrt(cx*cx + cy*cy) <= kRadius) in_radius.push_back(ent);
        }
    }

    u32 out[128] = {};
    const u32 n = grid_radius_query(math::Vec3{0, 0, 0}, kRadius,
                                    std::span<u32>(out, 128));
    REQUIRE(n > 0);

    // Every returned entity must really be inside the radius — closest
    // point on the entity AABB to the sphere center is ≤ kRadius.
    for (u32 i = 0; i < n; ++i) {
        bool matched = false;
        for (u32 e : in_radius) if (out[i] == e) { matched = true; break; }
        REQUIRE(matched);
    }

    // We expect at least the four cardinal neighbours + origin = five.
    REQUIRE(n >= 5);

    // Every entity that SHOULD be returned must actually be in the output
    // (no false negatives).
    for (u32 e : in_radius) {
        bool found = false;
        for (u32 i = 0; i < n; ++i) if (out[i] == e) { found = true; break; }
        REQUIRE(found);
    }

    for (auto k : keys) grid->remove(k);
}

TEST_CASE("scene: query router picks the right backend per QueryKind", "[scene][spatial][router]") {
    clear_region_overrides();
    REQUIRE(route_query(QueryKind::Raycast)          == SpatialBackend::Bvh);
    REQUIRE(route_query(QueryKind::FrustumCull)      == SpatialBackend::Bvh);
    REQUIRE(route_query(QueryKind::AabbOverlap)      == SpatialBackend::Bvh);
    REQUIRE(route_query(QueryKind::Broadphase)       == SpatialBackend::SweepAndPrune);
    REQUIRE(route_query(QueryKind::NearestNeighbour) == SpatialBackend::HashedGrid);
}

TEST_CASE("scene: query router honors per-region overrides", "[scene][spatial][router]") {
    clear_region_overrides();
    const RegionId combat_region = 42;
    const RegionId hallway_region = 99;

    // Default: physics broadphase = SAP everywhere.
    REQUIRE(route_query(QueryKind::Broadphase, combat_region) == SpatialBackend::SweepAndPrune);

    // Combat region overrides broadphase → HashedGrid (high body count).
    set_region_override(QueryKind::Broadphase, combat_region, SpatialBackend::HashedGrid);
    REQUIRE(route_query(QueryKind::Broadphase, combat_region) == SpatialBackend::HashedGrid);
    // Hallway still gets the default.
    REQUIRE(route_query(QueryKind::Broadphase, hallway_region) == SpatialBackend::SweepAndPrune);

    // A global override for broadphase applies to regions without their
    // own per-region override.
    set_region_override(QueryKind::Broadphase, kGlobalRegion, SpatialBackend::HashedGrid);
    REQUIRE(route_query(QueryKind::Broadphase, hallway_region) == SpatialBackend::HashedGrid);
    // The combat-region override still wins over the global.
    REQUIRE(route_query(QueryKind::Broadphase, combat_region) == SpatialBackend::HashedGrid);

    // Clearing an override falls back to the table.
    set_region_override(QueryKind::Broadphase, combat_region, SpatialBackend::None);
    set_region_override(QueryKind::Broadphase, kGlobalRegion, SpatialBackend::None);
    REQUIRE(route_query(QueryKind::Broadphase, combat_region) == SpatialBackend::SweepAndPrune);
    REQUIRE(route_query(QueryKind::Broadphase, hallway_region) == SpatialBackend::SweepAndPrune);

    clear_region_overrides();
}

TEST_CASE("scene: BVH backend handles bursty insert/remove without dangling state", "[scene][spatial][bvh]") {
    auto* bvh = bvh_backend();
    std::vector<SpatialKey> keys;
    std::vector<SpatialKey> new_keys;

    // Insert 32, remove 16 (every other), insert 16 more — the slot
    // reuse path must not corrupt queries.
    for (int i = 0; i < 32; ++i) {
        keys.push_back(bvh->insert(static_cast<u32>(700 + i),
                                    cube({ static_cast<f32>(i) * 2.0f, 0, 0 }, 0.4f)));
    }
    for (int i = 0; i < 32; i += 2) {
        bvh->remove(keys[static_cast<usize>(i)]);
    }
    for (int i = 0; i < 16; ++i) {
        const u32 ent = static_cast<u32>(800 + i);
        new_keys.push_back(bvh->insert(ent,
                                       cube({ static_cast<f32>(i) * 2.0f, 5, 0 }, 0.4f)));
    }

    // Query the cluster — we must see the surviving "700-series" odd
    // entities + the new "800-series" entities, never the removed evens.
    u32 buf[64] = {};
    bvh->query_aabb(cube({ 0, 2.5f, 0 }, 100.0f), std::span<u32>(buf, 64));

    bool saw_removed = false;
    bool saw_survivor = false;
    bool saw_new = false;
    for (u32 v : buf) {
        if (v == 0) continue;
        if (v >= 700 && v < 732 && (v - 700) % 2 == 0) saw_removed = true;
        if (v >= 700 && v < 732 && (v - 700) % 2 == 1) saw_survivor = true;
        if (v >= 800 && v < 816) saw_new = true;
    }
    REQUIRE_FALSE(saw_removed);
    REQUIRE(saw_survivor);
    REQUIRE(saw_new);

    // Drain everything so we don't pollute downstream tests sharing the
    // process-global BVH state.
    for (int i = 1; i < 32; i += 2) bvh->remove(keys[static_cast<usize>(i)]);
    for (auto k : new_keys) bvh->remove(k);
}
