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

// A single face's decoded lightmap chunk: a width x height block of RGBA8
// lumels (row-major, pitch == width), ready to hand straight to the rasterizer
// as a per-draw `DrawItem::lightmap_texels`. The on-disk lumels are RGB16F
// (BspFormat.h); `load_lightmaps` decodes them to LDR RGBA8 once at load so the
// inner raster loop samples a plain RGBA8 chunk through the face base UV.
struct BspFaceLightmap {
    u32 face = 0u;        // BspFace index this chunk shades.
    u32 width = 0u;
    u32 height = 0u;
    u32 first_texel = 0u;  // index into BspGeometry::lightmap_texels (RGBA8 pool).
};

// Per-BSP geometry tables. These live next to the BspMap and are filled at
// load time from the same .psybsp blob. Kept separate from `BspMap` so the
// public ABI in Bsp.h is preserved while we add geometry tables incrementally.
struct BspGeometry {
    std::vector<render::raster::Vertex> vertices;
    std::vector<u32> indices;
    // W12-2 baked lightmaps (ADDITIVE). `face_lightmap[face_id]` indexes
    // `lightmaps` (or kNoLightmap when the face is full-bright); each entry
    // points at a contiguous RGBA8 lumel block in `lightmap_texels`. Empty for
    // an unlit blob, so callers that don't load lightmaps pay nothing.
    std::vector<BspFaceLightmap> lightmaps;
    std::vector<u32> lightmap_texels;          // RGBA8 lumel pool (all faces).
    std::vector<u32> face_lightmap;            // BspFace index -> lightmaps index.
    static constexpr u32 kNoLightmap = 0xFFFFFFFFu;
};

// Load the per-BSP geometry tables (vertices + indices) from the same on-disk
// `.psybsp` blob that `Bsp::load` consumes (W10-2). `Bsp::load` itself only
// reads nodes / leaves / faces / pvs into the `BspMap`; this companion reads the
// vertex + index chunks into a `BspGeometry` so the runtime can render the BSP
// faces via `build_leaf_draws`. Returns false (and leaves `out` empty) if the
// blob is missing / malformed / has no geometry. Purely additive: callers that
// don't need geometry never pay for it.
bool load_geometry(std::string_view virtual_path, BspGeometry& out);

// W12-2: load the baked lightmap chunks from the same `.psybsp` blob and decode
// the on-disk RGB16F lumels into the RGBA8 pool in `out` (vertices/indices left
// untouched — call after `load_geometry`). Fills `out.lightmaps`,
// `out.lightmap_texels`, and `out.face_lightmap` (sized to `face_count`, each
// entry an index into `out.lightmaps` or BspGeometry::kNoLightmap). Returns true
// when the blob is well-formed (INCLUDING the unlit case: an empty lightmap
// chunk yields an empty `lightmaps` and an all-kNoLightmap `face_lightmap`).
// Purely additive: a caller that never calls this renders full-bright as before.
bool load_lightmaps(std::string_view virtual_path, u32 face_count, BspGeometry& out);

// Build a DrawItem stream for every face in `faces`. `out` is appended to
// (so callers can accumulate across PVS callbacks). The DrawItem's
// `vertices` / `indices` pointers are aliases into `geom.vertices` /
// `geom.indices` — `geom` MUST outlive the resulting DrawItems.
//
// W12-2: when `geom` carries baked lightmaps (load_lightmaps), a face with a
// lightmap gets its decoded RGBA8 lumel chunk wired into the DrawItem
// (`lightmap_texels` / `lightmap_w` / `lightmap_h`), so the rasterizer modulates
// the face colour by the baked shade (SurfaceCached path). A face WITHOUT a
// lightmap (or when `geom` has none) keeps `lightmap_texels == nullptr` and
// renders full-bright — nothing regresses. `first_face` is the GLOBAL BspFace
// index of `faces[0]` (so the per-face lightmap lookup keys off the right id);
// it defaults to 0 for callers that pass an already-global single span.
void build_face_draws(const BspGeometry& geom,
                      std::span<const BspFace> faces,
                      const BspMaterialResolve& resolve,
                      std::vector<render::raster::DrawItem>& out,
                      u32 first_face = 0u);

// Convenience: emit DrawItems for one leaf's face range. Used by the per-leaf
// callback in `walk_visible_leaves` (see Bsp.cpp).
void build_leaf_draws(const BspMap& map,
                      const BspGeometry& geom,
                      const BspLeaf& leaf,
                      const BspMaterialResolve& resolve,
                      std::vector<render::raster::DrawItem>& out);

}  // namespace psynder::world::bsp
