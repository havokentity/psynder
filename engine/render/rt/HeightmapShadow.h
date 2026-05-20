// SPDX-License-Identifier: MIT
// Psynder — heightmap shadow raymarcher. Lane 08.
//
// DESIGN.md §8.2: for maps using the heightmap-raymarcher terrain backend
// (the outdoor "Voxel-Space" path, §9.2 backend B), shadow rays from
// dynamic lights bypass the BVH and march the heightmap directly. This is
// cache-friendly (a uniform 2D grid, no tree traversal, no node tests)
// and cheaper than BVH for the open-terrain shadow case.
//
// This header is NEW for Wave B and lives inside the owned `engine/render/rt/`
// directory. The public BVH header (`Bvh.h`) is FROZEN per ADR-007/the lane
// brief; we add new surface in adjacent headers instead of editing it.
//
// Coordinate convention:
//   * The heightmap is a regular 2D grid on the XZ plane.
//   * `origin_xz` is world-space (x, z) of cell (0, 0).
//   * `cell_size` is the world-space spacing between adjacent cells.
//   * `y_data[j * width + i]` gives the surface height at cell (i, j).
//   * Height interpolation: bilinear over the four nearest cells.
//
// This matches the lane-11 outdoor heightmap layout (§9.2 backend B).

#pragma once

#include "Bvh.h"

namespace psynder::render::rt {

struct Heightmap {
    const f32* y_data = nullptr;  // width*height samples, row-major (j*width + i)
    u32 width = 0;
    u32 height = 0;
    math::Vec2 origin_xz{0.0f, 0.0f};
    f32 cell_size = 1.0f;
    f32 y_min = 0.0f;  // global min Y (for slab clip)
    f32 y_max = 0.0f;  // global max Y (for slab clip)
};

// Raymarch the heightmap along `ray` and return true if the ray is
// occluded by the height-field surface before `ray.t_max`. The march
// uses a logarithmic step schedule (small near the origin, growing with
// distance) per DESIGN.md §9.2 — view distance is a step budget, not a
// polygon budget. On a hit, the ray is `occluded` and we early-out.
//
// Implementation: 2D DDA in the XZ plane combined with a per-cell Y test
// against the (bilinearly-interpolated) surface. We do up to `max_steps`
// march steps; a typical shadow ray needs ≪ 256.
bool trace_heightmap_shadow(const Heightmap& hm, const Ray& ray, u32 max_steps = 256) noexcept;

}  // namespace psynder::render::rt
