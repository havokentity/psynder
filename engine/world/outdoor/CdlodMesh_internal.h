// SPDX-License-Identifier: MIT
// Psynder — internal CDLOD chunked mesh (Backend A, DESIGN.md §9.2).
//
// Continuous Distance-Dependent LOD: each leaf chunk is a 64×64 quad patch
// with a vertex morph from level L → L+1 driven by a normalized distance
// parameter. Adjacent chunks share an edge of vertices that morph in the
// same direction, so the seam is closed by construction — no skirts and
// no T-junctions when the morph is computed consistently.
//
// We emit the leaf-level grid only in Wave A (no inter-level morph yet —
// that comes with the inner-frame budget pass in Wave B). The leaf mesh is
// fully sufficient to drive the per-tile DrawItem queue (lane 07) and to
// validate stitching watertightness — the §9.2 invariant the test pins.
//
// Watertight invariant: two chunks sharing an edge produce IDENTICAL world
// positions for the vertices on that edge. We achieve this by sampling
// vertices at integer texel coords (no per-chunk offset), so float reps
// are bitwise identical between neighbors.

#pragma once

#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/raster/Raster.h"

#include <cstddef>
#include <vector>

namespace psynder::world::outdoor::detail {

// A single CDLOD chunk's mesh. Stored in the same Vertex / index layout the
// rasterizer's DrawItem expects (lane 07).
struct CdlodChunk {
    u32                                       chunk_x = 0;   // grid coord
    u32                                       chunk_z = 0;
    u32                                       lod     = 0;   // 0 = leaf
    std::vector<render::raster::Vertex>       vertices;
    std::vector<u32>                          indices;
    math::Aabb                                bounds;
};

// Build a single leaf-level chunk. The chunk covers texels
// [chunk_x*kChunkDim … chunk_x*kChunkDim + kChunkDim] (inclusive on both
// sides) along X, similarly along Z — that is, (kChunkDim+1)² verts and
// kChunkDim² × 2 triangles. Edge vertices are shared with neighbors by
// position (verbatim integer texel coords; no per-chunk float drift).
inline CdlodChunk build_chunk(const HeightmapDesc& h,
                              u32                  chunk_x,
                              u32                  chunk_z) noexcept {
    CdlodChunk c{};
    c.chunk_x = chunk_x;
    c.chunk_z = chunk_z;
    c.lod     = 0;

    if (h.size_x < 2 || h.size_z < 2) return c;

    const i32 x0 = static_cast<i32>(chunk_x * kChunkDim);
    const i32 z0 = static_cast<i32>(chunk_z * kChunkDim);
    const i32 x1 = static_cast<i32>(x0) + static_cast<i32>(kChunkDim);
    const i32 z1 = static_cast<i32>(z0) + static_cast<i32>(kChunkDim);

    // Last chunk along an axis is clipped to the map edge — emit only the
    // verts inside the map. (kChunkDim+1 normally; less at the boundary.)
    const i32 vx_max = static_cast<i32>(h.size_x) - 1;
    const i32 vz_max = static_cast<i32>(h.size_z) - 1;
    const i32 xN = (x1 > vx_max) ? vx_max : x1;
    const i32 zN = (z1 > vz_max) ? vz_max : z1;
    if (xN <= x0 || zN <= z0) return c;

    const u32 verts_x = static_cast<u32>(xN - x0 + 1);
    const u32 verts_z = static_cast<u32>(zN - z0 + 1);

    c.vertices.reserve(static_cast<usize>(verts_x) * verts_z);

    f32 y_min =  1e30f;
    f32 y_max = -1e30f;

    for (u32 vz = 0; vz < verts_z; ++vz) {
        const i32 z = z0 + static_cast<i32>(vz);
        for (u32 vx = 0; vx < verts_x; ++vx) {
            const i32 x = x0 + static_cast<i32>(vx);
            // World-space position. Multiplying integer texel by spacing
            // gives the SAME float for the same (x,z) pair in any chunk —
            // the watertight invariant lives here.
            const f32 wx = static_cast<f32>(x) * h.spacing;
            const f32 wz = static_cast<f32>(z) * h.spacing;
            const f32 wy = height_at_texel(h, x, z);

            if (wy < y_min) y_min = wy;
            if (wy > y_max) y_max = wy;

            render::raster::Vertex v{};
            v.position     = math::Vec3{ wx, wy, wz };
            v.normal       = normal_at_texel(h, x, z);
            // UV is texel-relative for the colormap.
            v.uv           = math::Vec2{
                static_cast<f32>(x) / static_cast<f32>(h.size_x),
                static_cast<f32>(z) / static_cast<f32>(h.size_z),
            };
            // Lightmap UV uses the same parameterization as `uv` — the
            // light channel lives next to the colormap (DESIGN §9.2).
            v.lightmap_uv  = v.uv;
            v.color        = pack_splat(splat_at_texel(h, x, z));

            c.vertices.push_back(v);
        }
    }

    // Two triangles per quad. Winding: counterclockwise for the
    // upward-facing normal (RH coords; we look down at +Y from above).
    const u32 quads_x = verts_x - 1;
    const u32 quads_z = verts_z - 1;
    c.indices.reserve(static_cast<usize>(quads_x) * quads_z * 6u);

    auto idx = [verts_x](u32 vx, u32 vz) {
        return vz * verts_x + vx;
    };

    for (u32 qz = 0; qz < quads_z; ++qz) {
        for (u32 qx = 0; qx < quads_x; ++qx) {
            const u32 i00 = idx(qx,     qz);
            const u32 i10 = idx(qx + 1, qz);
            const u32 i01 = idx(qx,     qz + 1);
            const u32 i11 = idx(qx + 1, qz + 1);
            // Triangle 1: (00, 01, 10)
            c.indices.push_back(i00);
            c.indices.push_back(i01);
            c.indices.push_back(i10);
            // Triangle 2: (10, 01, 11)
            c.indices.push_back(i10);
            c.indices.push_back(i01);
            c.indices.push_back(i11);
        }
    }

    if (y_min > y_max) { y_min = 0.0f; y_max = 0.0f; }
    c.bounds.min = math::Vec3{
        static_cast<f32>(x0) * h.spacing, y_min, static_cast<f32>(z0) * h.spacing
    };
    c.bounds.max = math::Vec3{
        static_cast<f32>(xN) * h.spacing, y_max, static_cast<f32>(zN) * h.spacing
    };
    return c;
}

// Build every leaf chunk for the heightmap.
inline std::vector<CdlodChunk> build_all_chunks(const HeightmapDesc& h) {
    std::vector<CdlodChunk> out;
    const u32 ncx = chunk_count_x(h);
    const u32 ncz = chunk_count_z(h);
    out.reserve(static_cast<usize>(ncx) * ncz);
    for (u32 cz = 0; cz < ncz; ++cz) {
        for (u32 cx = 0; cx < ncx; ++cx) {
            out.push_back(build_chunk(h, cx, cz));
        }
    }
    return out;
}

// CDLOD distance morph: per-vertex t∈[0,1] morph toward the next-coarser LOD.
// At the leaf level (no parent), morph is identity.
//
// `eye` is in world space; the morph band runs between `near_band` and
// `far_band` metres from the eye, inside the chunk. This is the standard
// CDLOD morph and is exposed here so unit tests can pin the value.
PSY_FORCEINLINE f32 cdlod_morph_t(math::Vec3 vertex_world,
                                  math::Vec3 eye,
                                  f32        near_band,
                                  f32        far_band) noexcept {
    if (far_band <= near_band) return 0.0f;
    const f32 dx = vertex_world.x - eye.x;
    const f32 dy = vertex_world.y - eye.y;
    const f32 dz = vertex_world.z - eye.z;
    const f32 d  = std::sqrt(dx * dx + dy * dy + dz * dz);
    const f32 t  = (d - near_band) / (far_band - near_band);
    return clamp_f32(t, 0.0f, 1.0f);
}

}  // namespace psynder::world::outdoor::detail
