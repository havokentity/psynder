// SPDX-License-Identifier: MIT
// Psynder — M-AI navigation grid pathfinder implementation (deterministic,
// pooled, alloc-free A* + grid string-pull smoothing).

#include "ai/NavGrid.h"

#include <cmath>

namespace psynder::ai {

namespace {

// 8-connected neighbour offsets. Ordered so deterministic iteration is stable
// (orthogonal first, then diagonal); ties in the open set are broken by cell
// index regardless, so this order only affects which equal-cost node is touched
// first — itself deterministic.
struct Offset {
    i32 dx;
    i32 dz;
    f32 cost;
};
constexpr f32 kSqrt2 = 1.41421356237f;
constexpr Offset kNeighbours[8] = {
    {1, 0, 1.0f},  {-1, 0, 1.0f}, {0, 1, 1.0f},      {0, -1, 1.0f},
    {1, 1, kSqrt2}, {1, -1, kSqrt2}, {-1, 1, kSqrt2}, {-1, -1, kSqrt2},
};

}  // namespace

// ─── Indexed binary min-heap ──────────────────────────────────────────────
void NavQuery::sift_up(usize i) noexcept {
    while (i > 0u) {
        const usize parent = (i - 1u) / 2u;
        if (heap_less(i, parent)) {
            heap_swap(i, parent);
            i = parent;
        } else {
            break;
        }
    }
}

void NavQuery::sift_down(usize i) noexcept {
    for (;;) {
        const usize l = 2u * i + 1u;
        const usize r = 2u * i + 2u;
        usize smallest = i;
        if (l < heap_size_ && heap_less(l, smallest))
            smallest = l;
        if (r < heap_size_ && heap_less(r, smallest))
            smallest = r;
        if (smallest == i)
            break;
        heap_swap(i, smallest);
        i = smallest;
    }
}

void NavQuery::heap_push_or_decrease(u32 cell_index, f32 f) noexcept {
    const u32 slot = heap_pos_[cell_index];
    if (slot != kNotInHeap && slot < heap_size_ && heap_[slot] == cell_index) {
        // Already present: decrease-key in place (f only ever drops here).
        heap_f_[slot] = f;
        sift_up(slot);
        return;
    }
    const usize i = heap_size_++;
    heap_[i] = cell_index;
    heap_f_[i] = f;
    heap_pos_[cell_index] = static_cast<u32>(i);
    sift_up(i);
}

u32 NavQuery::heap_pop() noexcept {
    const u32 top = heap_[0];
    heap_pos_[top] = kNotInHeap;
    const usize last = --heap_size_;
    if (last > 0u) {
        heap_[0] = heap_[last];
        heap_f_[0] = heap_f_[last];
        heap_pos_[heap_[0]] = 0u;
        sift_down(0u);
    }
    return top;
}

// ─── A* ─────────────────────────────────────────────────────────────────────
bool NavQuery::find_path(const NavGrid& grid, NavCell start, NavCell goal, NavPath& out) {
    out.clear();

    // Lazily size scratch if the caller forgot to reset() for this grid size.
    // After this, NO allocation happens for the rest of the query.
    if (g_score_.size() != grid.cell_count())
        reset(grid);

    // A blocked or out-of-bounds endpoint is a clean "no path" — never a crash,
    // never an allocation.
    if (!grid.walkable(start) || !grid.walkable(goal))
        return false;

    const u32 width = grid.width();
    const auto cell_index = [width](NavCell c) -> u32 {
        return static_cast<u32>(c.z) * width + static_cast<u32>(c.x);
    };

    // Fresh generation: invalidates all prior visited / in-open marks in O(1)
    // (no array clear). Wrap-safe: on overflow, hard-clear the stamps once.
    if (generation_ == 0xFFFFFFFFu) {
        for (u32& v : visited_gen_)
            v = 0u;
        for (u32& v : in_open_gen_)
            v = 0u;
        for (u32& v : heap_pos_)
            v = kNotInHeap;
        generation_ = 0u;
    }
    ++generation_;
    const u32 gen = generation_;
    heap_size_ = 0u;

    const u32 start_idx = cell_index(start);
    const u32 goal_idx = cell_index(goal);

    g_score_[start_idx] = 0.0f;
    came_from_[start_idx] = kNoParent;
    in_open_gen_[start_idx] = gen;
    heap_pos_[start_idx] = kNotInHeap;  // fresh for this generation
    heap_push_or_decrease(start_idx, heuristic(start, goal));

    bool found = false;
    while (heap_size_ > 0u) {
        // Indexed heap: each cell appears at most once, so a pop is FINAL — close
        // it immediately (no lazy-delete re-pops to skip).
        const u32 current = heap_pop();
        if (current == goal_idx) {
            found = true;
            break;
        }
        visited_gen_[current] = gen;

        const NavCell cc{static_cast<i32>(current % width),
                         static_cast<i32>(current / width)};
        for (const Offset& off : kNeighbours) {
            const NavCell nb{cc.x + off.dx, cc.z + off.dz};
            if (!grid.walkable(nb))
                continue;
            // Forbid cutting a blocked diagonal corner: a diagonal step is only
            // legal if BOTH orthogonal cells it squeezes between are walkable.
            if (off.dx != 0 && off.dz != 0) {
                if (grid.blocked(NavCell{cc.x + off.dx, cc.z}) ||
                    grid.blocked(NavCell{cc.x, cc.z + off.dz}))
                    continue;
            }
            const u32 nidx = cell_index(nb);
            if (visited_gen_[nidx] == gen)
                continue;  // already closed — consistent heuristic => optimal
            const f32 tentative = g_score_[current] + off.cost;
            const bool seen = (in_open_gen_[nidx] == gen);
            if (seen && tentative >= g_score_[nidx])
                continue;  // not an improvement
            if (!seen)
                heap_pos_[nidx] = kNotInHeap;  // first touch this generation
            g_score_[nidx] = tentative;
            came_from_[nidx] = current;
            in_open_gen_[nidx] = gen;
            heap_push_or_decrease(nidx, tentative + heuristic(nb, goal));
        }
    }

    if (!found)
        return false;

    // ── Reconstruct ───────────────────────────────────────────────────────
    // Walk came_from from goal back to start, counting first, then fill the
    // NavPath forward (start -> goal). If the raw chain exceeds the fixed
    // buffer we keep the tail nearest the goal and flag truncation (still no
    // allocation; smoothing normally collapses real paths well under the cap).
    u32 chain_len = 0u;
    for (u32 c = goal_idx; c != kNoParent; c = came_from_[c])
        ++chain_len;

    const u32 cap = NavPath::kMaxWaypoints;
    const u32 keep = chain_len <= cap ? chain_len : cap;
    out.truncated = chain_len > cap ? 1u : 0u;
    out.count = keep;

    // Fill back-to-front: index (keep-1) is the goal, 0 is the earliest kept.
    u32 write = keep;
    for (u32 c = goal_idx; c != kNoParent && write > 0u; c = came_from_[c]) {
        --write;
        const NavCell cell{static_cast<i32>(c % width), static_cast<i32>(c / width)};
        out.points[write] = grid.cell_to_world(cell);
    }
    return true;
}

// ─── Grid line-of-sight (supersampled) ──────────────────────────────────────
bool grid_segment_clear(const NavGrid& grid, math::Vec3 a, math::Vec3 b) noexcept {
    const f32 cell = grid.cell_size();
    if (!(cell > 0.0f))
        return false;
    const f32 dx = b.x - a.x;
    const f32 dz = b.z - a.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    // Sample at twice the cell resolution so we cannot tunnel through a 1-cell
    // wall between two samples. +1 guarantees the endpoint is checked.
    const i32 steps = static_cast<i32>(dist / (cell * 0.5f)) + 1;
    for (i32 s = 0; s <= steps; ++s) {
        const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
        const math::Vec3 p{a.x + dx * t, a.y, a.z + dz * t};
        if (grid.blocked(grid.world_to_cell(p)))
            return false;
    }
    return true;
}

// ─── String-pull smoothing (greedy LOS-skip) ────────────────────────────────
u32 smooth_path(const NavGrid& grid, NavPath& path) noexcept {
    if (path.count <= 2u)
        return path.count;

    // Greedy funnel-lite: from the current anchor, advance as far along the raw
    // path as a straight line stays clear, then commit that waypoint as the new
    // anchor. Done in place: we OVERWRITE the front of the buffer with the kept
    // waypoints (write index never exceeds read index, so this is safe). Keeps
    // the agent cutting corners instead of zig-zagging cell centres.
    const u32 n = path.count;
    u32 write = 0u;
    u32 anchor = 0u;
    path.points[write++] = path.points[anchor];  // always keep the start

    while (anchor < n - 1u) {
        // Find the farthest j > anchor with a clear straight line from anchor.
        u32 farthest = anchor + 1u;
        for (u32 j = anchor + 2u; j < n; ++j) {
            if (grid_segment_clear(grid, path.points[anchor], path.points[j]))
                farthest = j;
            // Note: we keep scanning past the first blocked j — a later j can be
            // visible again around a thin corner — to maximise the skip. This is
            // deterministic (fixed order) and alloc-free.
        }
        path.points[write++] = path.points[farthest];
        anchor = farthest;
    }

    path.count = write;
    return write;
}

}  // namespace psynder::ai
