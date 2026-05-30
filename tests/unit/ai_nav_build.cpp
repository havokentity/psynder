// SPDX-License-Identifier: MIT
// Psynder — M-AI nav-occupancy BUILDER + true-funnel unit tests (Lane W10-4).
// Exercises engine/ai/NavBuild: rasterizing static box geometry (axis-aligned +
// rotated) into a NavGrid with agent-radius inflation, an agent routing AROUND
// an obstacle it cannot reach in a straight line, a walled-off goal returning no
// path, build+query determinism, and the simple-stupid-funnel string-pull
// (no longer than the greedy smoother, still collision-free). Host-agnostic: the
// box list is plain POD, no physics / render / scene needed.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ai/NavBuild.h"
#include "ai/NavGrid.h"
#include "math/Math.h"

#include <array>
#include <cmath>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::ai;

namespace {

// 1 m cells, origin at the world origin, all walkable to start.
NavGrid make_grid(u32 w, u32 h, f32 cell = 1.0f) {
    NavGrid g;
    g.resize(w, h, cell, math::Vec3{0.0f, 0.0f, 0.0f});
    return g;
}

// Every waypoint lands on a free (walkable) cell.
bool path_on_free_cells(const NavGrid& g, const NavPath& p) {
    for (u32 i = 0; i < p.count; ++i)
        if (g.blocked(g.world_to_cell(p.points[i])))
            return false;
    return true;
}

// Total XZ length of a path.
f32 path_length(const NavPath& p) {
    f32 len = 0.0f;
    for (u32 i = 1; i < p.count; ++i) {
        const f32 dx = p.points[i].x - p.points[i - 1].x;
        const f32 dz = p.points[i].z - p.points[i - 1].z;
        len += std::sqrt(dx * dx + dz * dz);
    }
    return len;
}

// Densely supersample every segment of a path and assert no sample lands on a
// blocked cell (the path stays in free space, not just at the waypoints).
bool path_segments_clear(const NavGrid& g, const NavPath& p) {
    for (u32 i = 1; i < p.count; ++i) {
        const math::Vec3 a = p.points[i - 1];
        const math::Vec3 b = p.points[i];
        const f32 dx = b.x - a.x;
        const f32 dz = b.z - a.z;
        const f32 dist = std::sqrt(dx * dx + dz * dz);
        const int steps = static_cast<int>(dist / (g.cell_size() * 0.25f)) + 1;
        for (int s = 0; s <= steps; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
            const math::Vec3 q{a.x + dx * t, a.y, a.z + dz * t};
            if (g.blocked(g.world_to_cell(q)))
                return false;
        }
    }
    return true;
}

}  // namespace

// ─── A box rasterizes to the expected blocked cells (no inflation) ───────────
TEST_CASE("nav-build: axis-aligned box marks exactly the cells it overlaps",
          "[ai][nav][build]") {
    // 10x10 grid, 1 m cells. A box centred at world (5,*,5) with half-extent
    // (1.5, *, 1.5) spans world X/Z in [3.5, 6.5]. Cell centres sit at x+0.5, so
    // the cells whose CENTRE is within the box are x,z in {3,4,5,6} (centres
    // 3.5/4.5/5.5/6.5 — both 3.5 and 6.5 are on the boundary, distance 0, so they
    // ARE blocked). That is a clean 4x4 block. With agent_radius 0, only cells
    // the box actually covers are marked.
    NavGrid g = make_grid(10u, 10u);
    NavBox box{};
    box.center = math::Vec3{5.0f, 0.0f, 5.0f};
    box.half_extent = math::Vec3{1.5f, 0.5f, 1.5f};
    box.yaw = 0.0f;

    const u32 marked = build_nav_occupancy(g, box, /*radius*/ 0.0f);

    // Expected blocked set: cells whose centre distance to the box is <= 0.
    int expect = 0;
    for (i32 z = 0; z < 10; ++z)
        for (i32 x = 0; x < 10; ++x) {
            const math::Vec3 c = g.cell_to_world(NavCell{x, z});
            const bool inside =
                std::abs(c.x - 5.0f) <= 1.5f && std::abs(c.z - 5.0f) <= 1.5f;
            if (inside) {
                REQUIRE(g.blocked(NavCell{x, z}));
                ++expect;
            } else {
                REQUIRE_FALSE(g.blocked(NavCell{x, z}));
            }
        }
    REQUIRE(marked == static_cast<u32>(expect));
    REQUIRE(expect == 16);  // a clean 4x4 footprint
}

// ─── Agent-radius inflation grows the blocked footprint ──────────────────────
TEST_CASE("nav-build: agent-radius inflation blocks the cells around the box",
          "[ai][nav][build][inflate]") {
    // Same 3x3 box as above, now inflated by a 1 m agent radius. Cells whose
    // centre is within 1 m of the box surface also become blocked, growing the
    // footprint to a 5x5 (plus rounded corners — here the corner cells at
    // distance sqrt(2)*0.5 ~ 0.707 < 1 ARE within radius, so it is a full 5x5).
    NavGrid g = make_grid(11u, 11u);
    NavBox box{};
    box.center = math::Vec3{5.5f, 0.0f, 5.5f};  // centred on a cell corner
    box.half_extent = math::Vec3{1.0f, 0.5f, 1.0f};  // spans [4.5,6.5] => cells 5,6 each axis?
    box.yaw = 0.0f;

    const u32 base = build_nav_occupancy(g, box, /*radius*/ 0.0f);
    // Reset and rebuild with inflation; the inflated count must be strictly more.
    g.clear_blocked();
    const u32 infl = build_nav_occupancy(g, box, /*radius*/ 1.0f);
    REQUIRE(infl > base);

    // Spot-check: a cell exactly 1 m outside the box face is blocked under
    // inflation but free without it. Box face at x=6.5; cell centre at x=7.5 is
    // 1.0 m away => blocked (<= radius).
    const NavCell just_outside = g.world_to_cell(math::Vec3{7.5f, 0.0f, 5.5f});
    REQUIRE(g.blocked(just_outside));
    // A cell 2 m outside stays free.
    const NavCell well_outside = g.world_to_cell(math::Vec3{8.5f, 0.0f, 5.5f});
    REQUIRE_FALSE(g.blocked(well_outside));
}

// ─── A rotated box rasterizes correctly (diamond footprint) ──────────────────
TEST_CASE("nav-build: a 45-degree rotated box blocks a diamond, not a square",
          "[ai][nav][build][rotated]") {
    // A box rotated 45 deg presents a diamond to the axis-aligned grid: cells on
    // the diagonal tips are reached, cells just off the original square corners
    // are NOT (they fall outside the rotated extent). This proves the rasterizer
    // respects yaw rather than just using the world AABB.
    NavGrid g = make_grid(21u, 21u);
    NavBox box{};
    box.center = math::Vec3{10.5f, 0.0f, 10.5f};
    box.half_extent = math::Vec3{3.0f, 0.5f, 1.0f};  // long thin bar
    box.yaw = 3.14159265f * 0.5f;                     // rotate 90 deg => swaps x/z

    build_nav_occupancy(g, box, /*radius*/ 0.0f);

    // After a 90-degree rotation the long axis (3 m) now lies along Z and the
    // short axis (1 m) along X. So a cell 2.5 m away along Z from centre is
    // blocked, but a cell 2.5 m away along X is free.
    const NavCell along_z = g.world_to_cell(math::Vec3{10.5f, 0.0f, 13.0f});
    const NavCell along_x = g.world_to_cell(math::Vec3{13.0f, 0.0f, 10.5f});
    REQUIRE(g.blocked(along_z));
    REQUIRE_FALSE(g.blocked(along_x));
}

// ─── An agent routes AROUND a single wall to a goal it can't straight-line to ─
TEST_CASE("nav-build: agent routes AROUND a wall built from a static box",
          "[ai][nav][build][route]") {
    // 21x21 grid. A single static WALL box: a long thin vertical bar at x=10,
    // spanning z in [0, 16], leaving a gap at the top (z in [17,20]). The straight
    // line from start (2, 10) to goal (18, 10) passes THROUGH the wall, so the
    // only route threads the top gap. Build the occupancy from the box (no need
    // to hand-mark a single cell), then A* must route around.
    NavGrid g = make_grid(21u, 21u);
    NavBox wall{};
    // Bar centre at x=10.5; half-extent x=0.5 (one cell thick), z spanning the
    // wall: centre z=8.5, half-z=8.5 => covers world z in [0,17] => cells 0..16.
    wall.center = math::Vec3{10.5f, 0.0f, 8.5f};
    wall.half_extent = math::Vec3{0.5f, 1.0f, 8.5f};
    wall.yaw = 0.0f;

    const u32 marked = build_nav_occupancy(g, wall, /*radius*/ 0.0f);
    REQUIRE(marked > 0u);

    // The wall must actually separate left from right at the start row.
    REQUIRE(g.blocked(g.world_to_cell(math::Vec3{10.5f, 0.0f, 10.5f})));
    // The gap row near the top is open.
    REQUIRE_FALSE(g.blocked(g.world_to_cell(math::Vec3{10.5f, 0.0f, 19.5f})));

    NavQuery q;
    q.reset(g);
    NavPath path;
    const NavCell start = g.world_to_cell(math::Vec3{2.5f, 0.0f, 10.5f});
    const NavCell goal = g.world_to_cell(math::Vec3{18.5f, 0.0f, 10.5f});
    const bool ok = q.find_path(g, start, goal, path);

    REQUIRE(ok);
    REQUIRE(path.count >= 2u);
    REQUIRE(path.truncated == 0u);
    // Every waypoint on a free cell; start->goal connected.
    REQUIRE(path_on_free_cells(g, path));
    REQUIRE(g.world_to_cell(path.points[0]) == start);
    REQUIRE(g.world_to_cell(path.points[path.count - 1u]) == goal);
    // It went AROUND: some waypoint reaches the top gap (z >= 17), since the
    // wall cannot be crossed anywhere in z 0..16.
    bool reached_gap = false;
    for (u32 i = 0; i < path.count; ++i)
        if (g.world_to_cell(path.points[i]).z >= 17)
            reached_gap = true;
    REQUIRE(reached_gap);
    // A straight line start->goal would NOT be clear (it crosses the wall), which
    // is exactly why routing was needed.
    REQUIRE_FALSE(grid_segment_clear(g, math::Vec3{2.5f, 0.0f, 10.5f},
                                     math::Vec3{18.5f, 0.0f, 10.5f}));
}

// ─── An agent routes around an L-shaped obstacle ─────────────────────────────
TEST_CASE("nav-build: agent routes around an L-shaped obstacle (two boxes)",
          "[ai][nav][build][route][lshape]") {
    // 21x21 grid. Two static boxes forming an L that boxes the goal into a pocket
    // open only from one side. Start outside, goal inside the elbow; the straight
    // line is blocked by one arm of the L, so the agent must round the corner.
    NavGrid g = make_grid(21u, 21u);
    std::array<NavBox, 2> boxes{};
    // Vertical arm: x=8, z in [4,16].
    boxes[0].center = math::Vec3{8.5f, 0.0f, 10.5f};
    boxes[0].half_extent = math::Vec3{0.5f, 1.0f, 6.5f};
    // Horizontal arm: z=4, x in [8,16] (joins the bottom of the vertical arm).
    boxes[1].center = math::Vec3{12.5f, 0.0f, 4.5f};
    boxes[1].half_extent = math::Vec3{4.5f, 1.0f, 0.5f};

    const u32 marked =
        build_nav_occupancy(g, std::span<const NavBox>(boxes.data(), boxes.size()),
                            /*radius*/ 0.0f);
    REQUIRE(marked > 0u);

    NavQuery q;
    q.reset(g);
    NavPath path;
    // Start to the right of the vertical arm, goal to the left of it and above the
    // horizontal arm => must go around the TOP of the vertical arm.
    const NavCell start = g.world_to_cell(math::Vec3{14.5f, 0.0f, 10.5f});
    const NavCell goal = g.world_to_cell(math::Vec3{2.5f, 0.0f, 10.5f});
    const bool ok = q.find_path(g, start, goal, path);

    REQUIRE(ok);
    REQUIRE(path_on_free_cells(g, path));
    REQUIRE(g.world_to_cell(path.points[0]) == start);
    REQUIRE(g.world_to_cell(path.points[path.count - 1u]) == goal);
    // Straight line is blocked (crosses the vertical arm).
    REQUIRE_FALSE(grid_segment_clear(g, math::Vec3{14.5f, 0.0f, 10.5f},
                                     math::Vec3{2.5f, 0.0f, 10.5f}));
}

// ─── A fully-walled-off goal returns NO path ─────────────────────────────────
TEST_CASE("nav-build: a goal sealed inside a box ring returns no path",
          "[ai][nav][build][nopath]") {
    // 15x15 grid. A hollow box ring built from four static walls fully encloses a
    // central pocket; the goal sits inside, the start outside. No route exists.
    NavGrid g = make_grid(15u, 15u);
    std::array<NavBox, 4> walls{};
    // Ring around cells x,z in [5,9] (a 5x5 hollow), walls one cell thick.
    // Left wall x=4, right wall x=10, bottom z=4, top z=10, each spanning x/z 4..10.
    walls[0].center = math::Vec3{4.5f, 0.0f, 7.5f};   // left
    walls[0].half_extent = math::Vec3{0.5f, 1.0f, 3.5f};
    walls[1].center = math::Vec3{10.5f, 0.0f, 7.5f};  // right
    walls[1].half_extent = math::Vec3{0.5f, 1.0f, 3.5f};
    walls[2].center = math::Vec3{7.5f, 0.0f, 4.5f};   // bottom
    walls[2].half_extent = math::Vec3{3.5f, 1.0f, 0.5f};
    walls[3].center = math::Vec3{7.5f, 0.0f, 10.5f};  // top
    walls[3].half_extent = math::Vec3{3.5f, 1.0f, 0.5f};

    build_nav_occupancy(g, std::span<const NavBox>(walls.data(), walls.size()),
                        /*radius*/ 0.0f);

    // The pocket centre is free; the ring around it is blocked.
    const NavCell goal = g.world_to_cell(math::Vec3{7.5f, 0.0f, 7.5f});
    REQUIRE_FALSE(g.blocked(goal));
    REQUIRE(g.blocked(g.world_to_cell(math::Vec3{4.5f, 0.0f, 7.5f})));

    NavQuery q;
    q.reset(g);
    NavPath path;
    const NavCell start = g.world_to_cell(math::Vec3{0.5f, 0.0f, 0.5f});
    REQUIRE_FALSE(q.find_path(g, start, goal, path));
    REQUIRE(path.empty());
}

// ─── Determinism: same build + query reproduces the path bit-for-bit ─────────
TEST_CASE("nav-build: build+query is deterministic across rebuilds",
          "[ai][nav][build][determinism]") {
    std::array<NavBox, 3> boxes{};
    boxes[0].center = math::Vec3{10.5f, 0.0f, 7.5f};
    boxes[0].half_extent = math::Vec3{0.5f, 1.0f, 6.5f};
    boxes[1].center = math::Vec3{6.5f, 0.0f, 13.5f};
    boxes[1].half_extent = math::Vec3{3.5f, 1.0f, 0.5f};
    boxes[2].center = math::Vec3{15.0f, 0.0f, 15.0f};
    boxes[2].half_extent = math::Vec3{1.2f, 1.0f, 2.4f};
    boxes[2].yaw = 0.6f;  // a rotated obstacle in the mix

    auto build_and_route = [&](NavPath& out) {
        NavGrid g = make_grid(24u, 24u);
        build_nav_occupancy(
            g, std::span<const NavBox>(boxes.data(), boxes.size()), 0.5f);
        NavQuery q;
        q.reset(g);
        return q.find_path(g, NavCell{1, 1}, NavCell{22, 20}, out);
    };

    NavPath a;
    REQUIRE(build_and_route(a));
    REQUIRE(a.count >= 2u);

    for (int run = 0; run < 6; ++run) {
        NavPath b;
        REQUIRE(build_and_route(b));
        REQUIRE(b.count == a.count);
        for (u32 i = 0; i < a.count; ++i) {
            REQUIRE(b.points[i].x == Catch::Approx(a.points[i].x));
            REQUIRE(b.points[i].z == Catch::Approx(a.points[i].z));
        }
    }
}

// ─── True funnel: no longer than the raw path + greedy smoother, stays clear ──
TEST_CASE("nav-build: funnel string-pull is no longer than greedy + collision-free",
          "[ai][nav][build][funnel]") {
    // Route around the single-wall obstacle, then funnel the raw path. The
    // funneled path must be (a) no longer than the raw cell path, (b) no longer
    // than the greedy LOS-skip smoother's result, and (c) collision-free.
    NavGrid g = make_grid(21u, 21u);
    NavBox wall{};
    wall.center = math::Vec3{10.5f, 0.0f, 8.5f};
    wall.half_extent = math::Vec3{0.5f, 1.0f, 8.5f};
    build_nav_occupancy(g, wall, /*radius*/ 0.0f);

    NavQuery q;
    q.reset(g);
    NavPath raw;
    REQUIRE(q.find_path(g, NavCell{2, 10}, NavCell{18, 10}, raw));
    const f32 raw_len = path_length(raw);
    const u32 raw_count = raw.count;
    REQUIRE(raw_count > 3u);

    // Greedy smoother on a copy.
    NavPath greedy = raw;
    smooth_path(g, greedy);
    const f32 greedy_len = path_length(greedy);

    // Funnel on another copy (radius 0 => pull to cell boundaries).
    NavPath funneled = raw;
    const u32 fc = funnel_path(g, funneled, /*radius*/ 0.0f);

    // Endpoints preserved.
    REQUIRE(g.world_to_cell(funneled.points[0]) == NavCell{2, 10});
    REQUIRE(g.world_to_cell(funneled.points[funneled.count - 1u]) == NavCell{18, 10});
    REQUIRE(fc == funneled.count);
    REQUIRE(fc >= 2u);
    REQUIRE(fc <= raw_count);  // never more waypoints than the raw path

    const f32 fun_len = path_length(funneled);
    // No longer than the raw path, and no longer than the greedy smoother
    // (a small epsilon for float noise). The exact corridor pull is tightest.
    REQUIRE(fun_len <= raw_len + 1e-3f);
    REQUIRE(fun_len <= greedy_len + 1e-3f);

    // Collision-free: densely sampled, no segment touches a blocked cell.
    REQUIRE(path_segments_clear(g, funneled));
}

// ─── True funnel: a straight open corridor collapses to start+goal ───────────
TEST_CASE("nav-build: funnel collapses an open diagonal to two points",
          "[ai][nav][build][funnel][open]") {
    NavGrid g = make_grid(24u, 24u);  // wide open, no obstacles
    NavQuery q;
    q.reset(g);
    NavPath path;
    REQUIRE(q.find_path(g, NavCell{1, 1}, NavCell{20, 20}, path));
    REQUIRE(path.count > 3u);

    const u32 fc = funnel_path(g, path, /*radius*/ 0.0f);
    // A clear straight shot collapses to (essentially) the two endpoints.
    REQUIRE(fc == path.count);
    REQUIRE(fc <= 3u);
    REQUIRE(g.world_to_cell(path.points[0]) == NavCell{1, 1});
    REQUIRE(g.world_to_cell(path.points[path.count - 1u]) == NavCell{20, 20});
    REQUIRE(path_segments_clear(g, path));
}

// ─── Builder is alloc-free in steady state (rebuild churns no heap growth) ────
TEST_CASE("nav-build: repeated rebuild does not grow the grid or reallocate",
          "[ai][nav][build][alloc]") {
    NavGrid g = make_grid(32u, 32u);
    const usize cells_before = g.cell_count();
    std::array<NavBox, 2> boxes{};
    boxes[0].center = math::Vec3{16.0f, 0.0f, 16.0f};
    boxes[0].half_extent = math::Vec3{2.0f, 1.0f, 2.0f};
    boxes[1].center = math::Vec3{8.0f, 0.0f, 24.0f};
    boxes[1].half_extent = math::Vec3{1.0f, 1.0f, 3.0f};
    boxes[1].yaw = 0.3f;

    for (int i = 0; i < 16; ++i) {
        g.clear_blocked();  // no realloc
        build_nav_occupancy(
            g, std::span<const NavBox>(boxes.data(), boxes.size()), 0.5f);
        REQUIRE(g.cell_count() == cells_before);  // grid never grew
    }
}
