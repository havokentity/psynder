// SPDX-License-Identifier: MIT
// Psynder — Hierarchical-Z (HiZ) buffer for per-tile early-reject. 8×8
// blocks per tile, storing the *max* (farthest) depth in the block. A
// triangle whose minimum depth is greater than every block's max depth
// is fully occluded and can be skipped before the inner loop. DESIGN.md
// §7.3 ("Hierarchical Z (HiZ) buffer at 8×8 per tile for early reject").
//
// Lane-07 internal — not in any public header. The HiZ block grid is
// implicitly sized by the tile dimensions: a 64×64 tile carries an 8×8
// HiZ at 8-pixel cells, a 32×32 tile carries 4×4, a 128×128 carries 16×16.

#pragma once

#include "core/Types.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <cstring>

namespace psynder::render::raster {

inline constexpr u32 kHiZCellPx = 8;  // 8×8 per cell — ADR-002 ties this
                                      // to the tile size being a multiple
                                      // of 8.

// HiZ buffer for one tile. Cells store the max (farthest) NDC z in
// [0,1]; cleared to 1.0f (= far plane).
template <u32 TILE_W, u32 TILE_H>
struct HiZTile {
    static constexpr u32 kCols = TILE_W / kHiZCellPx;
    static constexpr u32 kRows = TILE_H / kHiZCellPx;
    static constexpr u32 kCells = kCols * kRows;

    f32 max_z[kCells];

    // Cell index from a pixel position inside the tile.
    PSY_FORCEINLINE u32 cell_idx(u32 tile_local_x, u32 tile_local_y) const noexcept {
        const u32 cx = tile_local_x / kHiZCellPx;
        const u32 cy = tile_local_y / kHiZCellPx;
        return cy * kCols + cx;
    }

    void clear() noexcept {
        for (u32 i = 0; i < kCells; ++i)
            max_z[i] = 1.0f;
    }

    // Build the HiZ from a framebuffer depth slice for the tile. Used at
    // the start of a tile's rasterize pass — the depth buffer carries
    // last frame's content if not cleared, but for the within-frame
    // refinement we instead update HiZ incrementally as quads finalize.
    void rebuild_from_fb(const Framebuffer& fb, u32 tile_x0, u32 tile_y0) noexcept {
        clear();
        if (!fb.depth)
            return;
        const u32 fb_w = fb.width;
        const u32 fb_h = fb.height;
        for (u32 cy = 0; cy < kRows; ++cy) {
            const u32 y0 = tile_y0 + cy * kHiZCellPx;
            const u32 y1 = std::min(y0 + kHiZCellPx, fb_h);
            if (y0 >= fb_h)
                continue;
            for (u32 cx = 0; cx < kCols; ++cx) {
                const u32 x0 = tile_x0 + cx * kHiZCellPx;
                const u32 x1 = std::min(x0 + kHiZCellPx, fb_w);
                if (x0 >= fb_w)
                    continue;
                f32 max_seen = 0.0f;
                for (u32 y = y0; y < y1; ++y) {
                    const u32* row = fb.depth + static_cast<usize>(y) * fb_w;
                    for (u32 x = x0; x < x1; ++x) {
                        u32 packed = row[x] & 0xFFFFFF00u;
                        f32 z;
                        std::memcpy(&z, &packed, sizeof(z));
                        if (z > max_seen)
                            max_seen = z;
                    }
                }
                max_z[cy * kCols + cx] = max_seen;
            }
        }
    }

    // Conservative test: does any cell touching the rect [x0..x1] × [y0..y1]
    // (in tile-local coords) have max_z >= test_z? Used for triangle-AABB
    // early reject — return false ⇒ the triangle can be skipped.
    PSY_FORCEINLINE bool any_cell_passes(i32 x0, i32 y0, i32 x1, i32 y1, f32 test_z) const noexcept {
        // Convert pixel rect → cell rect.
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > static_cast<i32>(TILE_W))
            x1 = static_cast<i32>(TILE_W);
        if (y1 > static_cast<i32>(TILE_H))
            y1 = static_cast<i32>(TILE_H);
        if (x0 >= x1 || y0 >= y1)
            return false;
        const u32 cx0 = static_cast<u32>(x0) / kHiZCellPx;
        const u32 cy0 = static_cast<u32>(y0) / kHiZCellPx;
        const u32 cx1 = (static_cast<u32>(x1 - 1) / kHiZCellPx) + 1;
        const u32 cy1 = (static_cast<u32>(y1 - 1) / kHiZCellPx) + 1;
        for (u32 cy = cy0; cy < cy1 && cy < kRows; ++cy) {
            for (u32 cx = cx0; cx < cx1 && cx < kCols; ++cx) {
                // `>=`, not `>`: the early-reject is conservative — a triangle
                // whose nearest depth EQUALS a cell's farthest stored depth is
                // still potentially visible (e.g. the second triangle of a
                // coplanar, camera-facing quad, whose z matches what the first
                // triangle just wrote). Rejecting on equality dropped that
                // second triangle entirely, leaving half the quad as a hole —
                // visible on face-on walls/floors while angled geometry, with
                // its varying depth, slipped past. The precise per-pixel depth
                // test downstream is the real authority.
                if (max_z[cy * kCols + cx] >= test_z)
                    return true;
            }
        }
        return false;
    }

    // Tighten a single cell's max_z after a quad finishes. The depth-buffer
    // already carries the current z; HiZ keeps a conservative upper bound.
    PSY_FORCEINLINE void update_cell(u32 cell, f32 new_max_z) noexcept {
        if (new_max_z < max_z[cell])
            max_z[cell] = new_max_z;
    }
};

}  // namespace psynder::render::raster
