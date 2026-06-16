// SPDX-License-Identifier: MIT
// Psynder — M-AI nav-occupancy builder + true-funnel string-pull implementation.
// Deterministic, alloc-free: rasterizes a static box list into a host-owned
// NavGrid with agent-radius inflation, and pulls a raw grid path tight with the
// simple stupid funnel algorithm. See NavBuild.h for the contract.

#include "ai/NavBuild.h"

#include <cmath>

namespace psynder::ai {

namespace {

// 2D point in the XZ plane (the nav plane). Local helpers keep the math explicit
// and dependency-free (no include juggling); all deterministic float ops.
struct P2 {
    f32 x;
    f32 z;
};

[[nodiscard]] constexpr f32 absf(f32 v) noexcept { return v < 0.0f ? -v : v; }
[[nodiscard]] constexpr f32 maxf(f32 a, f32 b) noexcept { return a > b ? a : b; }
[[nodiscard]] constexpr f32 minf(f32 a, f32 b) noexcept { return a < b ? a : b; }

// Signed area of triangle (a,b,c) x2 in the XZ plane (the funnel "sidedness"
// test, a.k.a. triarea2). > 0 => c is left of a->b; < 0 => right; 0 => collinear.
[[nodiscard]] constexpr f32 cross2(P2 a, P2 b, P2 c) noexcept {
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

// Squared XZ distance between two points.
[[nodiscard]] constexpr f32 dist2(P2 a, P2 b) noexcept {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return dx * dx + dz * dz;
}

// Closest distance (XZ) from a world point to a box given by world center,
// XZ half-extents and yaw. Transform the point into the box's LOCAL frame
// (un-rotate by -yaw), clamp to the box, and measure the residual: this is the
// exact point-to-OBB distance. 0 inside the box. Deterministic.
[[nodiscard]] f32 point_box_distance(P2 p,
                                     P2 center,
                                     f32 half_x,
                                     f32 half_z,
                                     f32 cos_y,
                                     f32 sin_y) noexcept {
    // World->local: translate to center, then rotate by -yaw. A +yaw rotation of
    // a local vector v into world is [cos -sin; sin cos] (CCW about +Y, with the
    // XZ plane handed so this matches world_to_cell's axis order); the inverse
    // (world->local) is the transpose [cos sin; -sin cos].
    const f32 rx = p.x - center.x;
    const f32 rz = p.z - center.z;
    const f32 lx = cos_y * rx + sin_y * rz;
    const f32 lz = -sin_y * rx + cos_y * rz;
    // Clamp into the box extents; the excess on each axis is the distance vector.
    const f32 dx = absf(lx) - half_x;
    const f32 dz = absf(lz) - half_z;
    const f32 ex = maxf(dx, 0.0f);
    const f32 ez = maxf(dz, 0.0f);
    return std::sqrt(ex * ex + ez * ez);
}

}  // namespace

// ─── build_nav_occupancy (box list) ──────────────────────────────────────────
u32 build_nav_occupancy(NavGrid& grid,
                        std::span<const NavBox> boxes,
                        f32 agent_radius) noexcept {
    const f32 radius = agent_radius > 0.0f ? agent_radius : 0.0f;
    const f32 cell = grid.cell_size();
    if (!(cell > 0.0f) || grid.cell_count() == 0u)
        return 0u;

    u32 newly_blocked = 0u;

    for (const NavBox& box : boxes) {
        const f32 half_x = absf(box.half_extent.x);
        const f32 half_z = absf(box.half_extent.z);
        const f32 cos_y = std::cos(box.yaw);
        const f32 sin_y = std::sin(box.yaw);

        // World-AABB of the rotated box, grown by the agent radius, bounds the
        // cell scan. A rotated box's world half-extents are
        // (|cos|*hx + |sin|*hz, |sin|*hx + |cos|*hz); add the radius so any cell
        // whose center could be within radius is in range.
        const f32 world_hx = absf(cos_y) * half_x + absf(sin_y) * half_z + radius;
        const f32 world_hz = absf(sin_y) * half_x + absf(cos_y) * half_z + radius;
        const math::Vec3 mn{box.center.x - world_hx, 0.0f, box.center.z - world_hz};
        const math::Vec3 mx{box.center.x + world_hx, 0.0f, box.center.z + world_hz};

        // Cell range covering that world AABB, clamped to the grid.
        const NavCell c0 = grid.world_to_cell(mn);
        const NavCell c1 = grid.world_to_cell(mx);
        const i32 lo_x = c0.x < 0 ? 0 : c0.x;
        const i32 lo_z = c0.z < 0 ? 0 : c0.z;
        const i32 w = static_cast<i32>(grid.width());
        const i32 h = static_cast<i32>(grid.height());
        const i32 hi_x = c1.x >= w ? w - 1 : c1.x;
        const i32 hi_z = c1.z >= h ? h - 1 : c1.z;

        const P2 center{box.center.x, box.center.z};
        for (i32 z = lo_z; z <= hi_z; ++z) {
            for (i32 x = lo_x; x <= hi_x; ++x) {
                const NavCell c{x, z};
                if (grid.blocked(c))
                    continue;  // already blocked (preserve + don't double-count)
                const math::Vec3 wc = grid.cell_to_world(c);
                const P2 p{wc.x, wc.z};
                const f32 d =
                    point_box_distance(p, center, half_x, half_z, cos_y, sin_y);
                // A cell is blocked when its CENTER is within the agent radius of
                // the box surface (inside the box => d==0 => always blocked).
                // '<=' so a cell whose center sits exactly radius away is blocked
                // (conservative: prefer over- to under-marking clearance).
                if (d <= radius) {
                    grid.set_blocked(c, true);
                    ++newly_blocked;
                }
            }
        }
    }
    return newly_blocked;
}

// ─── build_nav_occupancy (single box) ────────────────────────────────────────
u32 build_nav_occupancy(NavGrid& grid, const NavBox& box, f32 agent_radius) noexcept {
    return build_nav_occupancy(grid, std::span<const NavBox>(&box, 1u), agent_radius);
}

// ─── True funnel (simple stupid funnel algorithm) ─────────────────────────────
//
// The robust grid funnel needs NON-degenerate portals. A diagonal grid step
// shares only a single corner (a zero-width portal), which the funnel cannot
// pull through. So we first EXPAND the raw cell path into a sequence in which
// every step is ORTHOGONAL: a diagonal A->B is split into A -> M -> B through the
// shared-corner-adjacent intermediate cell M (the one orthogonally adjacent to
// both A and B on the WALKABLE side — the A* corner rule already guarantees at
// least one such free cell, since it forbids cutting a blocked diagonal corner).
// Every step in the expanded sequence then shares a full cell EDGE, so each
// portal is a proper segment (its two shared grid corners), shrunk inward by the
// agent radius. We then run Mikko Mononen's canonical "stringPull" funnel over
// those left/right portal endpoints. Deterministic + alloc-free (fixed scratch).
namespace {

// World-space corner of cell `c` at integer corner offset (cx,cz) in {0,1}
// (0 = the cell's min corner along that axis, 1 = the max corner).
[[nodiscard]] P2 cell_corner(const NavGrid& grid, NavCell c, i32 cx, i32 cz) noexcept {
    const math::Vec3 o = grid.origin();
    const f32 s = grid.cell_size();
    return P2{o.x + static_cast<f32>(c.x + cx) * s,
              o.z + static_cast<f32>(c.z + cz) * s};
}

// The shared EDGE between two orthogonally-adjacent cells a,b as (left,right)
// world points, oriented so `left` is on the +left side of the travel direction
// a->b. The edge is shrunk toward its midpoint by `radius` so the pulled string
// keeps clearance from the wall the edge borders.
struct Portal {
    P2 left;
    P2 right;
};

[[nodiscard]] Portal edge_portal(const NavGrid& grid,
                                 NavCell a,
                                 NavCell b,
                                 f32 radius) noexcept {
    const i32 dx = b.x - a.x;
    const i32 dz = b.z - a.z;
    // The shared edge runs along the boundary between a and b. Identify its two
    // grid corners on cell b's side facing a.
    P2 e0, e1;
    if (dx == 1) {            // b is to the +X of a => shared edge is a's max-X
        e0 = cell_corner(grid, b, 0, 0);
        e1 = cell_corner(grid, b, 0, 1);
    } else if (dx == -1) {    // b is to the -X of a => shared edge is a's min-X
        e0 = cell_corner(grid, b, 1, 0);
        e1 = cell_corner(grid, b, 1, 1);
    } else if (dz == 1) {     // b is to the +Z of a
        e0 = cell_corner(grid, b, 0, 0);
        e1 = cell_corner(grid, b, 1, 0);
    } else {                  // dz == -1 : b is to the -Z of a
        e0 = cell_corner(grid, b, 0, 1);
        e1 = cell_corner(grid, b, 1, 1);
    }
    // Shrink toward the midpoint by `radius` (clamped so it never inverts).
    const f32 s = grid.cell_size();
    const f32 shrink = minf(maxf(radius, 0.0f), 0.5f * s);
    const P2 mid{0.5f * (e0.x + e1.x), 0.5f * (e0.z + e1.z)};
    auto pull = [&](P2 e) -> P2 {
        const f32 vx = mid.x - e.x;
        const f32 vz = mid.z - e.z;
        const f32 len = std::sqrt(vx * vx + vz * vz);
        if (len <= 1e-6f)
            return e;
        const f32 t = shrink / len;
        return P2{e.x + vx * t, e.z + vz * t};
    };
    e0 = pull(e0);
    e1 = pull(e1);
    // Orient: left is on the +left side of travel a->b. Travel dir t=(dx,dz);
    // sidedness of e0 about the ray through the midpoint.
    const P2 mp{0.5f * (e0.x + e1.x), 0.5f * (e0.z + e1.z)};
    const P2 ray_b{mp.x + static_cast<f32>(dx), mp.z + static_cast<f32>(dz)};
    // Mononen's funnel expects, for travel direction d, that `left` is the portal
    // endpoint on the LEFT of d and `right` on the right, with the sidedness sign
    // matching triarea2/cross2 in THIS plane. In the XZ grid plane (X right, Z the
    // row axis that world_to_cell floors along), cross2(mp, mp+d, e) > 0 selects
    // the endpoint that must be `right` for the funnel comparisons below to hold
    // (the plane's handedness flips the naive "left = positive" intuition). We
    // assign accordingly so triarea2(apex,left,right) stays >= 0 for an open
    // funnel — verified by the open-diagonal collapse + around-wall tests.
    Portal p;
    if (cross2(mp, ray_b, e0) >= 0.0f) {
        p.right = e0;
        p.left = e1;
    } else {
        p.right = e1;
        p.left = e0;
    }
    return p;
}

}  // namespace

u32 funnel_path(const NavGrid& grid, NavPath& path, f32 radius_in) noexcept {
    if (path.count <= 2u)
        return path.count;
    const f32 radius = radius_in > 0.0f ? radius_in : 0.0f;
    const u32 n = path.count;

    // Cache the raw cell sequence.
    NavCell cells[NavPath::kMaxWaypoints];
    for (u32 i = 0; i < n; ++i)
        cells[i] = grid.world_to_cell(path.points[i]);
    const f32 y = path.points[0].y;

    // ── Expand diagonal steps into orthogonal steps ─────────────────────────
    // Each diagonal A->B inserts an intermediate orthogonally-adjacent WALKABLE
    // cell M. The expanded run can be at most 2x the raw length; cap at the
    // buffer. (The A* never cuts a blocked diagonal corner, so a free M exists.)
    static constexpr u32 kCap = 2u * NavPath::kMaxWaypoints;
    NavCell run[kCap];
    u32 run_n = 0u;
    run[run_n++] = cells[0];
    for (u32 i = 1u; i < n && run_n + 2u <= kCap; ++i) {
        const NavCell prev = run[run_n - 1u];
        const NavCell cur = cells[i];
        const i32 ddx = cur.x - prev.x;
        const i32 ddz = cur.z - prev.z;
        if (ddx != 0 && ddz != 0) {
            // Diagonal: insert a walkable orthogonal pivot. Prefer the cell that
            // shares prev's row then prev's column (deterministic), choosing the
            // one that is walkable (at least one is, by the corner rule).
            const NavCell pivot_a{prev.x + ddx, prev.z};
            const NavCell pivot_b{prev.x, prev.z + ddz};
            const NavCell pivot = grid.walkable(pivot_a) ? pivot_a : pivot_b;
            run[run_n++] = pivot;
        }
        run[run_n++] = cur;
    }

    // ── Assemble the full portal sequence (Mononen layout) ──────────────────
    // pf[0]              = (start, start)        — the degenerate start portal
    // pf[1 .. P]         = edge portals          — one per orthogonal step
    // pf[P+1]            = (goal, goal)          — the degenerate goal portal
    const P2 start{path.points[0].x, path.points[0].z};
    const P2 goal{path.points[n - 1u].x, path.points[n - 1u].z};
    Portal pf[kCap];
    u32 pf_n = 0u;
    pf[pf_n++] = Portal{start, start};
    const u32 num_edges = run_n - 1u;
    for (u32 i = 0; i < num_edges && pf_n + 1u < kCap; ++i)
        pf[pf_n++] = edge_portal(grid, run[i], run[i + 1u], radius);
    pf[pf_n++] = Portal{goal, goal};

    // ── Mononen stringPull funnel ───────────────────────────────────────────
    P2 out[kCap];
    u32 out_n = 0u;
    out[out_n++] = start;

    P2 apex = pf[0].left;   // == start
    P2 left = pf[0].left;
    P2 right = pf[0].right;
    u32 apex_i = 0u, left_i = 0u, right_i = 0u;

    // Emit a corner, suppressing a zero-length repeat of the previous point (a
    // restart can re-emit the apex; collinear segments coalesce naturally).
    const auto emit = [&](P2 pnt) noexcept {
        if (out_n > 0u && dist2(out[out_n - 1u], pnt) < 1e-12f)
            return;
        if (out_n < kCap)
            out[out_n++] = pnt;
    };

    for (u32 i = 1u; i < pf_n; ++i) {
        const P2 pl = pf[i].left;
        const P2 pr = pf[i].right;

        // Update the RIGHT vertex.
        if (cross2(apex, right, pr) <= 0.0f) {
            if (dist2(apex, right) < 1e-12f || cross2(apex, left, pr) > 0.0f) {
                right = pr;
                right_i = i;
            } else {
                // Right over left => insert left as a corner, restart at left.
                emit(left);
                apex = left;
                apex_i = left_i;
                left = apex;
                right = apex;
                left_i = apex_i;
                right_i = apex_i;
                i = apex_i;  // loop ++ advances to apex_i+1
                continue;
            }
        }
        // Update the LEFT vertex.
        if (cross2(apex, left, pl) >= 0.0f) {
            if (dist2(apex, left) < 1e-12f || cross2(apex, right, pl) < 0.0f) {
                left = pl;
                left_i = i;
            } else {
                emit(right);
                apex = right;
                apex_i = right_i;
                left = apex;
                right = apex;
                left_i = apex_i;
                right_i = apex_i;
                i = apex_i;
                continue;
            }
        }
    }

    // Append the goal (emit() suppresses a duplicate if the last apex was it).
    emit(goal);

    // Write back, clamped to the NavPath capacity.
    if (out_n > NavPath::kMaxWaypoints) {
        out_n = NavPath::kMaxWaypoints;
        path.truncated = 1u;
    }
    for (u32 i = 0; i < out_n; ++i)
        path.points[i] = math::Vec3{out[i].x, y, out[i].z};
    path.count = out_n;
    return out_n;
}

}  // namespace psynder::ai
