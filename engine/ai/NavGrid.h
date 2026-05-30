// SPDX-License-Identifier: MIT
// Psynder — M-AI navigation grid + deterministic pooled A* pathfinder.
//
// This is the navigation upgrade from steer-v1 (straight-line chase that walks
// into walls) to real grid-based routing: AI soldiers find a path AROUND static
// geometry instead of burrowing through it. A polygon navmesh is the eventual
// target (see "NEXT STEP" at the bottom); a uniform NavGrid is the pragmatic
// first cut — deterministic, alloc-free to query, and trivial to feed from a
// heightmap / box-occupancy set / terrain-slope test the HOST supplies.
//
// HOST-AGNOSTIC, like the rest of engine/ai: this module pulls in NOTHING from
// physics / render / world / host. The host samples its own world and marks
// blocked cells through the builder API (NavGrid::set_blocked /
// NavGrid::block_aabb), exactly the same decoupling as the LosFn hook in
// AiSystems.h. The pathfinder then queries the grid with ZERO heap allocation:
// all open/closed/came-from scratch is preallocated once (sized to the grid)
// inside a reusable NavQuery and reused across every query.
//
// DOTS / determinism contract:
//   * NavGrid is a flat row-major byte grid (POD-ish: a small header + one
//     pooled std::vector<u8> sized ONCE at build time; never grows per query).
//   * A* is fully deterministic: integer cell coordinates, an octile-distance
//     heuristic, a binary min-heap keyed on (f-score, then cell index) so ties
//     break on the lowest linear cell index — no RNG, no clock, no pointer /
//     iteration-order dependence. The same grid + endpoints always yields the
//     identical waypoint list across runs and across threads.
//   * NO per-query heap allocation: NavQuery owns fixed scratch arrays
//     (g-score, came-from, an open-heap, a "visited" generation stamp) sized to
//     the grid's cell count once at reset(); find_path() only writes into a
//     caller-supplied fixed waypoint buffer (NavPath).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <cstddef>
#include <span>
#include <vector>

namespace psynder::ai {

using ::psynder::f32;
using ::psynder::i32;
using ::psynder::u8;
using ::psynder::u32;
using ::psynder::usize;

// ─── Cell coordinate ────────────────────────────────────────────────────────
// Integer grid coordinate. Trivially copyable; compared / hashed by value.
struct NavCell {
    i32 x = 0;
    i32 z = 0;  // grid lies in the world XZ plane (Y is up; nav is 2.5D)
    [[nodiscard]] constexpr bool operator==(const NavCell&) const noexcept = default;
};

// ─── NavPath ──────────────────────────────────────────────────────────────
// A caller-owned, fixed-capacity waypoint buffer. find_path() and the smoother
// write into it; nothing here allocates. World-space waypoints (cell centres,
// after optional string-pull smoothing). Capacity is a compile-time bound so an
// agent can embed one with zero heap (kMaxWaypoints chosen to comfortably hold
// a routed corridor across a mid-size grid; longer raw paths are smoothed down
// well under this before they are handed back, and a raw path that would exceed
// it is reported as truncated rather than overflowing).
struct NavPath {
    static constexpr u32 kMaxWaypoints = 64u;
    math::Vec3 points[kMaxWaypoints] = {};
    u32 count = 0u;
    // True when a raw cell path was longer than kMaxWaypoints and got clamped.
    // (Smoothing normally keeps real paths far under the cap; this is a safety
    // signal, never an allocation.)
    u32 truncated = 0u;

    void clear() noexcept {
        count = 0u;
        truncated = 0u;
    }
    [[nodiscard]] bool empty() const noexcept { return count == 0u; }
};

// ─── NavGrid ──────────────────────────────────────────────────────────────
// A uniform walkable/blocked grid sampled from the world by the host. Cells are
// row-major (index = z * width + x). The grid maps to world space by an origin
// (the world position of cell (0,0)'s corner) and a uniform cell_size in metres.
//
// BUILDER API (host-filled):
//   resize(w, h, cell, origin)  — allocate ONCE; all cells start walkable.
//   set_blocked(cell, blocked)  — mark a single cell.
//   block_aabb(min, max)        — mark every cell an AABB overlaps (static
//                                 colliders / terrain too steep -> blocked).
//   clear_blocked()             — reset all cells to walkable (no realloc).
// QUERY API (alloc-free):
//   in_bounds / blocked / walkable / world_to_cell / cell_to_world.
//
// The single std::vector is sized once at resize() and reused; per-query and
// per-tick paths NEVER touch it for growth.
class NavGrid {
public:
    NavGrid() = default;

    // Allocate a width x height grid (cells). cell_size is the side length in
    // metres; origin is the world position of the (0,0) cell's MIN corner, so
    // cell (cx,cz) spans [origin + (cx,cz)*cell, origin + (cx+1,cz+1)*cell] in
    // the XZ plane. All cells start walkable. This is the ONLY growth point.
    void resize(u32 width, u32 height, f32 cell_size, math::Vec3 origin) {
        width_ = width;
        height_ = height;
        cell_size_ = (cell_size > 0.0f) ? cell_size : 1.0f;
        origin_ = origin;
        cells_.assign(static_cast<usize>(width_) * static_cast<usize>(height_), u8{0});
    }

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] f32 cell_size() const noexcept { return cell_size_; }
    [[nodiscard]] math::Vec3 origin() const noexcept { return origin_; }
    [[nodiscard]] usize cell_count() const noexcept { return cells_.size(); }

    [[nodiscard]] bool in_bounds(NavCell c) const noexcept {
        return c.x >= 0 && c.z >= 0 && static_cast<u32>(c.x) < width_ &&
               static_cast<u32>(c.z) < height_;
    }

    [[nodiscard]] usize index_of(NavCell c) const noexcept {
        return static_cast<usize>(c.z) * static_cast<usize>(width_) +
               static_cast<usize>(c.x);
    }

    [[nodiscard]] bool blocked(NavCell c) const noexcept {
        if (!in_bounds(c))
            return true;  // out-of-bounds is impassable
        return cells_[index_of(c)] != 0u;
    }
    [[nodiscard]] bool walkable(NavCell c) const noexcept {
        return in_bounds(c) && cells_[index_of(c)] == 0u;
    }

    // Mark / clear a single cell. Out-of-bounds is ignored.
    void set_blocked(NavCell c, bool b) noexcept {
        if (!in_bounds(c))
            return;
        cells_[index_of(c)] = b ? u8{1} : u8{0};
    }

    // Reset every cell to walkable without reallocating (rebuild per level
    // without churning the heap).
    void clear_blocked() noexcept {
        for (u8& cell : cells_)
            cell = 0u;
    }

    // World <-> cell. world_to_cell floors into the grid (clamping is the
    // caller's job via in_bounds); cell_to_world returns the cell CENTRE so
    // waypoints sit mid-cell, not on a corner.
    [[nodiscard]] NavCell world_to_cell(math::Vec3 p) const noexcept {
        const f32 fx = (p.x - origin_.x) / cell_size_;
        const f32 fz = (p.z - origin_.z) / cell_size_;
        return NavCell{floor_i32(fx), floor_i32(fz)};
    }
    [[nodiscard]] math::Vec3 cell_to_world(NavCell c) const noexcept {
        return math::Vec3{
            origin_.x + (static_cast<f32>(c.x) + 0.5f) * cell_size_,
            origin_.y,
            origin_.z + (static_cast<f32>(c.z) + 0.5f) * cell_size_,
        };
    }

    // Mark every cell whose footprint overlaps the world-space XZ AABB as
    // blocked. The host calls this once per static collider / steep terrain
    // patch while building the grid. Y is ignored (the grid is 2.5D); the host
    // decides which colliders are nav-relevant before calling. Alloc-free.
    void block_aabb(math::Vec3 min, math::Vec3 max) noexcept {
        // Normalise so min<=max even if the host passed them swapped.
        const f32 x0 = min.x < max.x ? min.x : max.x;
        const f32 x1 = min.x < max.x ? max.x : min.x;
        const f32 z0 = min.z < max.z ? min.z : max.z;
        const f32 z1 = min.z < max.z ? max.z : min.z;
        const NavCell c0 = world_to_cell(math::Vec3{x0, 0.0f, z0});
        const NavCell c1 = world_to_cell(math::Vec3{x1, 0.0f, z1});
        const i32 lo_x = c0.x < 0 ? 0 : c0.x;
        const i32 lo_z = c0.z < 0 ? 0 : c0.z;
        const i32 hi_x =
            c1.x >= static_cast<i32>(width_) ? static_cast<i32>(width_) - 1 : c1.x;
        const i32 hi_z =
            c1.z >= static_cast<i32>(height_) ? static_cast<i32>(height_) - 1 : c1.z;
        for (i32 z = lo_z; z <= hi_z; ++z)
            for (i32 x = lo_x; x <= hi_x; ++x)
                cells_[index_of(NavCell{x, z})] = 1u;
    }

private:
    [[nodiscard]] static i32 floor_i32(f32 v) noexcept {
        const i32 i = static_cast<i32>(v);
        return (v < static_cast<f32>(i)) ? i - 1 : i;
    }

    u32 width_ = 0u;
    u32 height_ = 0u;
    f32 cell_size_ = 1.0f;
    math::Vec3 origin_{0.0f, 0.0f, 0.0f};
    // Row-major occupancy: 0 = walkable, 1 = blocked. Sized once at resize().
    std::vector<u8> cells_;
};

// ─── NavQuery ───────────────────────────────────────────────────────────────
// Reusable, alloc-free A* scratch. Construct ONE and reuse it for every query
// (typically owned by the AI world / NavContext). All scratch is sized to the
// grid's cell count at reset(); find_path() performs ZERO heap allocation.
//
// Determinism: the open set is a binary min-heap of cell indices keyed on
// (f-score, then linear cell index). Equal-f nodes are popped lowest-index
// first, so neighbour expansion order — and therefore the final path — is
// identical across runs / threads / builds. The heuristic is octile distance
// (admissible + consistent for 8-connected movement), so A* is optimal.
class NavQuery {
public:
    NavQuery() = default;

    // Size scratch to `grid` and stamp a fresh generation (invalidates the prior
    // visited marks without clearing arrays — O(1) reuse). Call whenever the
    // grid dimensions change; otherwise find_path() reuses the existing scratch.
    void reset(const NavGrid& grid) {
        const usize n = grid.cell_count();
        if (g_score_.size() != n) {
            g_score_.assign(n, 0.0f);
            came_from_.assign(n, kNoParent);
            visited_gen_.assign(n, 0u);
            in_open_gen_.assign(n, 0u);
            // Indexed binary heap: at most ONE entry per cell (decrease-key in
            // place), so the heap can never exceed n entries — sized exactly to
            // n, no duplicate-push overflow. heap_pos_ maps cell -> heap slot
            // (kNotInHeap when absent), valid only for the current generation.
            heap_.assign(n, 0u);
            heap_f_.assign(n, 0.0f);
            heap_pos_.assign(n, kNotInHeap);
            generation_ = 0u;  // ONLY reset on (re)allocation; bumped per query.
        }
        // NOTE: generation_ is intentionally NOT reset on a same-size reset().
        // It must increase monotonically across queries so the per-query
        // visited/open/heap_pos stamps invalidate cleanly without an array
        // clear; resetting it would let a stale stamp from the prior query
        // (same gen value) leak in and corrupt the next search.
    }

    // Find an 8-connected path from `start` to `goal` over `grid`, writing
    // world-space cell-centre waypoints into `out`. Returns true on success
    // (out.count >= 1, ending at the goal cell centre), false if no path exists
    // (out is cleared). Both endpoints must be walkable; a blocked endpoint is a
    // clean "no path" (false), never a crash.
    //
    // ALLOC-FREE: assumes reset(grid) was called for the current grid size; all
    // work uses the preallocated scratch + the caller's NavPath. Diagonal moves
    // are forbidden through a blocked corner (no cutting through wall diagonals).
    bool find_path(const NavGrid& grid, NavCell start, NavCell goal, NavPath& out);

    // Number of cells the scratch is currently sized for (0 until first reset()).
    // A caller can prime sizing with reset(grid) and assert this == grid cell
    // count to prove a subsequent find_path() does ZERO heap growth; it is also
    // the cheap "is this query already sized for `grid`?" test used to skip a
    // redundant reset. The reserved heap capacity never shrinks across queries.
    [[nodiscard]] usize scratch_size() const noexcept { return g_score_.size(); }

private:
    static constexpr u32 kNoParent = 0xFFFFFFFFu;
    static constexpr u32 kNotInHeap = 0xFFFFFFFFu;

    // Octile distance heuristic (8-connected). D = 1 for orthogonal, D2 = sqrt2
    // for diagonal. Admissible + consistent => optimal A*.
    [[nodiscard]] static f32 heuristic(NavCell a, NavCell b) noexcept {
        const i32 dx = a.x > b.x ? a.x - b.x : b.x - a.x;
        const i32 dz = a.z > b.z ? a.z - b.z : b.z - a.z;
        const i32 dmin = dx < dz ? dx : dz;
        const i32 dmax = dx < dz ? dz : dx;
        constexpr f32 kSqrt2 = 1.41421356237f;
        return static_cast<f32>(dmax - dmin) + kSqrt2 * static_cast<f32>(dmin);
    }

    // ── Indexed binary min-heap over cell indices, keyed (f, then index) ──────
    // heap_[0..heap_size_) holds cell indices; heap_f_ the parallel f-keys;
    // heap_pos_[cell] the cell's current slot (kNotInHeap if absent). At most one
    // entry per cell — relaxation either pushes a new cell or DECREASES the key
    // of one already present, so heap_size_ never exceeds n. Tie-break on the
    // lower cell index keeps pops deterministic.
    void heap_push_or_decrease(u32 cell_index, f32 f) noexcept;
    [[nodiscard]] u32 heap_pop() noexcept;  // returns cell index; precondition: non-empty
    void sift_up(usize i) noexcept;
    void sift_down(usize i) noexcept;
    [[nodiscard]] bool heap_less(usize a, usize b) const noexcept {
        if (heap_f_[a] != heap_f_[b])
            return heap_f_[a] < heap_f_[b];
        return heap_[a] < heap_[b];  // deterministic tie-break: lowest cell index
    }
    void heap_swap(usize a, usize b) noexcept {
        const u32 ci = heap_[a];
        heap_[a] = heap_[b];
        heap_[b] = ci;
        const f32 fk = heap_f_[a];
        heap_f_[a] = heap_f_[b];
        heap_f_[b] = fk;
        heap_pos_[heap_[a]] = static_cast<u32>(a);
        heap_pos_[heap_[b]] = static_cast<u32>(b);
    }

    // Scratch — all sized to cell_count at reset(), reused every query.
    std::vector<f32> g_score_;      // best known cost from start to a cell
    std::vector<u32> came_from_;    // parent cell index (kNoParent = none)
    std::vector<u32> visited_gen_;  // generation a cell was closed in
    std::vector<u32> in_open_gen_;  // generation a cell entered the open set in
    std::vector<u32> heap_;         // open-set heap: cell indices
    std::vector<f32> heap_f_;       // parallel f-scores for the heap
    std::vector<u32> heap_pos_;     // cell -> heap slot (kNotInHeap if absent)
    usize heap_size_ = 0u;
    u32 generation_ = 0u;
};

// ─── String-pull smoothing (funnel-lite) ────────────────────────────────────
// Reduce a raw cell-centre waypoint list to the fewest waypoints that still
// describe the same corridor, by skipping any intermediate waypoint the agent
// can reach in a straight line (line-of-sight over walkable cells). This makes
// the agent cut corners instead of zig-zagging through cell centres. Operates
// in place on a NavPath using a grid supersample LOS test; alloc-free,
// deterministic. Returns the new waypoint count.
//
// (A true funnel algorithm over a polygon navmesh is the eventual upgrade; on a
// uniform grid this greedy LOS-skip is the standard, deterministic equivalent.)
u32 smooth_path(const NavGrid& grid, NavPath& path) noexcept;

// Grid line-of-sight: is the straight world segment a->b clear of blocked cells?
// Supersamples the segment at sub-cell resolution; exposed so the smoother and
// any caller can share one definition. Deterministic, alloc-free.
[[nodiscard]] bool grid_segment_clear(const NavGrid& grid,
                                       math::Vec3 a,
                                       math::Vec3 b) noexcept;

// NEXT STEP (documented, not shipped here):
//   * Polygon navmesh: replace the uniform grid with convex polygon cells +
//     a real funnel algorithm for exact string-pulled paths (no supersampling).
//   * Dynamic obstacle carving: per-frame stamp moving colliders into a
//     scratch overlay layer so doors / vehicles block routing without a full
//     rebuild.
//   * Jump / drop links: off-mesh connections so agents can traverse gaps the
//     2.5D grid can't express.

}  // namespace psynder::ai
