// SPDX-License-Identifier: MIT
// Psynder — BSP face → DrawItem converter. Lane 10 owns; consumed by the
// rasterizer (lane 07) and the renderer's "build draw stream" pass.
//
// `lm_qbsp` writes BSP faces with material/lightmap IDs plus a flat
// (vertex, index) pool that already matches the rasterizer's Vertex layout
// (position / normal / uv / lightmap_uv / packed RGBA8). The runtime is
// therefore a *binding* pass — no vertex transformations, no material
// resolution — that emits one DrawItem per face. The caller threads through
// PVS / portal filtering before calling this converter.

#pragma once

#include <vector>  // Bsp.h uses std::vector without including <vector>; see Bsp.cpp.
#include "Bsp.h"
#include "render/raster/Raster.h"

#include <span>

namespace psynder::world::bsp {

// Optional per-face overrides resolved from the asset system at level-load.
// Maps `BspFace::material` (an opaque id baked by lm_qbsp) onto the runtime
// `MaterialId` resolved against the live material cache. A null override
// table means "pass the raw u32 through" (handy for tests).
struct BspMaterialResolve {
    const render::raster::MaterialId* table = nullptr;
    u32 count = 0;
};

// Per-BSP geometry tables. These live next to the BspMap and are filled at
// load time from the same .psybsp blob. Kept separate from `BspMap` so the
// public ABI in Bsp.h is preserved while we add geometry tables incrementally.
struct BspGeometry {
    std::vector<render::raster::Vertex> vertices;
    std::vector<u32> indices;
};

// Load the per-BSP geometry tables (vertices + indices) from the same on-disk
// `.psybsp` blob that `Bsp::load` consumes (W10-2). `Bsp::load` itself only
// reads nodes / leaves / faces / pvs into the `BspMap`; this companion reads the
// vertex + index chunks into a `BspGeometry` so the runtime can render the BSP
// faces via `build_leaf_draws`. Returns false (and leaves `out` empty) if the
// blob is missing / malformed / has no geometry. Purely additive: callers that
// don't need geometry never pay for it.
bool load_geometry(std::string_view virtual_path, BspGeometry& out);

// Build a DrawItem stream for every face in `faces`. `out` is appended to
// (so callers can accumulate across PVS callbacks). The DrawItem's
// `vertices` / `indices` pointers are aliases into `geom.vertices` /
// `geom.indices` — `geom` MUST outlive the resulting DrawItems.
void build_face_draws(const BspGeometry& geom,
                      std::span<const BspFace> faces,
                      const BspMaterialResolve& resolve,
                      std::vector<render::raster::DrawItem>& out);

// Convenience: emit DrawItems for one leaf's face range. Used by the per-leaf
// callback in `walk_visible_leaves` (see Bsp.cpp).
void build_leaf_draws(const BspMap& map,
                      const BspGeometry& geom,
                      const BspLeaf& leaf,
                      const BspMaterialResolve& resolve,
                      std::vector<render::raster::DrawItem>& out);

}  // namespace psynder::world::bsp
