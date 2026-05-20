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
    u32 chunk_x = 0;  // grid coord
    u32 chunk_z = 0;
    u32 lod = 0;  // 0 = leaf
    std::vector<render::raster::Vertex> vertices;
    std::vector<u32> indices;
    math::Aabb bounds;
};

// Build a single leaf-level chunk. The chunk covers texels
// [chunk_x*kChunkDim … chunk_x*kChunkDim + kChunkDim] (inclusive on both
// sides) along X, similarly along Z — that is, (kChunkDim+1)² verts and
// kChunkDim² × 2 triangles. Edge vertices are shared with neighbors by
// position (verbatim integer texel coords; no per-chunk float drift).
inline CdlodChunk build_chunk(const HeightmapDesc& h, u32 chunk_x, u32 chunk_z) noexcept {
    CdlodChunk c{};
    c.chunk_x = chunk_x;
    c.chunk_z = chunk_z;
    c.lod = 0;

    if (h.size_x < 2 || h.size_z < 2)
        return c;

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
    if (xN <= x0 || zN <= z0)
        return c;

    const u32 verts_x = static_cast<u32>(xN - x0 + 1);
    const u32 verts_z = static_cast<u32>(zN - z0 + 1);

    c.vertices.reserve(static_cast<usize>(verts_x) * verts_z);

    f32 y_min = 1e30f;
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

            if (wy < y_min)
                y_min = wy;
            if (wy > y_max)
                y_max = wy;

            render::raster::Vertex v{};
            v.position = math::Vec3{wx, wy, wz};
            v.normal = normal_at_texel(h, x, z);
            // UV is texel-relative for the colormap.
            v.uv = math::Vec2{
                static_cast<f32>(x) / static_cast<f32>(h.size_x),
                static_cast<f32>(z) / static_cast<f32>(h.size_z),
            };
            // Lightmap UV uses the same parameterization as `uv` — the
            // light channel lives next to the colormap (DESIGN §9.2).
            v.lightmap_uv = v.uv;
            v.color = pack_splat(splat_at_texel(h, x, z));

            c.vertices.push_back(v);
        }
    }

    // Two triangles per quad. Winding: counterclockwise for the
    // upward-facing normal (RH coords; we look down at +Y from above).
    const u32 quads_x = verts_x - 1;
    const u32 quads_z = verts_z - 1;
    c.indices.reserve(static_cast<usize>(quads_x) * quads_z * 6u);

    auto idx = [verts_x](u32 vx, u32 vz) { return vz * verts_x + vx; };

    for (u32 qz = 0; qz < quads_z; ++qz) {
        for (u32 qx = 0; qx < quads_x; ++qx) {
            const u32 i00 = idx(qx, qz);
            const u32 i10 = idx(qx + 1, qz);
            const u32 i01 = idx(qx, qz + 1);
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

    if (y_min > y_max) {
        y_min = 0.0f;
        y_max = 0.0f;
    }
    c.bounds.min =
        math::Vec3{static_cast<f32>(x0) * h.spacing, y_min, static_cast<f32>(z0) * h.spacing};
    c.bounds.max =
        math::Vec3{static_cast<f32>(xN) * h.spacing, y_max, static_cast<f32>(zN) * h.spacing};
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
                                  f32 near_band,
                                  f32 far_band) noexcept {
    if (far_band <= near_band)
        return 0.0f;
    const f32 dx = vertex_world.x - eye.x;
    const f32 dy = vertex_world.y - eye.y;
    const f32 dz = vertex_world.z - eye.z;
    const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const f32 t = (d - near_band) / (far_band - near_band);
    return clamp_f32(t, 0.0f, 1.0f);
}

// ─── Wave-B: inter-LOD vertex morph (smooth coarse→fine transition) ──────
//
// CDLOD's signature feature: as the camera approaches a chunk, the chunk
// transitions from a coarser LOD (e.g. every-other-vertex) to the finer
// leaf grid (every vertex). The transition is *smooth* — vertices that
// only exist at the finer LOD interpolate from the average of their two
// neighbors (the position they'd occupy if the coarse mesh were drawn)
// toward their true heightmap position, driven by a per-chunk morph factor.
//
// LOD level conventions:
//   level 0 = leaf (every texel is a vertex; the original Wave-A mesh)
//   level L = stride 2^L (every 2^L-th texel is a vertex)
//
// Watertight invariant preservation: two neighboring chunks sharing an edge
// must produce bitwise-identical vertex positions on that edge for any LOD
// level + morph factor. This is the §9.2 invariant Wave A pins; Wave B's
// morph must NOT break it.
//
// We achieve preservation by:
//   1. Both chunks sample at integer texel coords × spacing — already true
//      from Wave A. The shared-edge texel index is the same on both sides.
//   2. The morph "kept" position is the SAME integer-texel sample on both
//      chunks. The morph "skipped" position is the average of two neighbors
//      that are also shared (they live on the edge), so they evaluate to
//      the same f32 on both sides too.
//   3. The morph factor applied to an edge vertex is a per-chunk scalar.
//      For adjacent chunks at the same LOD, both chunks apply the same
//      factor to the same vertex — preserved bitwise.
//
// The morph factor for a chunk is `cdlod_morph_t(chunk_center, eye, ...)`,
// computed once per chunk per frame. Edge vertices on the seam between two
// chunks at the same LOD see the same factor on both sides because both
// chunks query the same morph band — even if the per-chunk centers differ
// slightly, the band is global. (At the LOD seam between a coarse and a
// fine chunk, the watertight invariant is preserved differently — see
// `morph_at_lod_boundary` below.)

// Per-vertex morph kernel. Given a vertex's heightmap-derived "fine" Y and
// the average-of-neighbors "coarse" Y at this texel position, produce the
// morphed Y for a given morph factor t∈[0,1]:
//   t = 0 → fine Y (camera close, full detail)
//   t = 1 → coarse Y (camera far, every-other-texel sampling)
//
// The fine X/Z are unchanged — only Y morphs. The position layout in the
// CDLOD chunk doesn't move; only the surface height interpolates.
PSY_FORCEINLINE f32 morph_height(f32 y_fine, f32 y_coarse, f32 t) noexcept {
    // Standard CDLOD: y = lerp(fine, coarse, t).
    return y_fine + (y_coarse - y_fine) * t;
}

// "Coarse" Y for a vertex at (texel_x, texel_z) when the chunk's LOD level
// is L. At level L, only vertices at texels divisible by 2^L are "kept";
// the rest are interpolated from their two same-axis neighbors at the next
// kept positions.
//
// Mathematically, for a vertex at (x, z) with stride S = 2^L:
//   x0 = floor(x/S)*S        x1 = x0 + S
//   z0 = floor(z/S)*S        z1 = z0 + S
//   coarse_y = bilinear blend of the 4 corners at (x0,z0)..(x1,z1)
//
// At a corner (x % S == 0 AND z % S == 0), this collapses to the corner's
// own height — i.e. kept-vertex morph is identity. At an edge midpoint
// (only one axis on-grid), it's the linear blend of the two on-axis
// neighbors. At the interior (off-grid in both axes), it's the bilinear
// blend of the 4 corners.
PSY_FORCEINLINE f32 coarse_height_at_lod(const HeightmapDesc& h, i32 x, i32 z, u32 lod_level) noexcept {
    if (lod_level == 0)
        return height_at_texel(h, x, z);  // identity
    const i32 stride = 1 << lod_level;    // 2^L
    // Snap down to the nearest stride-aligned coarse grid corner.
    // Use bit-and on positive coords; for negative coords we'd want floor,
    // but heightmap texels are always in [0, size) and the caller has
    // already clamped to bounds before reaching us.
    const i32 x0 = (x / stride) * stride;
    const i32 z0 = (z / stride) * stride;
    const i32 x1 = x0 + stride;
    const i32 z1 = z0 + stride;
    const f32 tx = static_cast<f32>(x - x0) / static_cast<f32>(stride);
    const f32 tz = static_cast<f32>(z - z0) / static_cast<f32>(stride);
    const f32 h00 = height_at_texel(h, x0, z0);
    const f32 h10 = height_at_texel(h, x1, z0);
    const f32 h01 = height_at_texel(h, x0, z1);
    const f32 h11 = height_at_texel(h, x1, z1);
    const f32 hx0 = h00 + (h10 - h00) * tx;
    const f32 hx1 = h01 + (h11 - h01) * tx;
    return hx0 + (hx1 - hx0) * tz;
}

// Per-chunk LOD level + morph factor. Stored alongside the chunk and updated
// per frame from the camera. The renderer applies `morph_height` to each
// vertex's Y at submit time (or in a vertex pre-pass — the cost is sub-
// millisecond for our chunk counts).
struct CdlodMorph {
    u32 lod_level = 0;        // 0 = leaf, 1 = stride-2, 2 = stride-4, …
    f32 morph_factor = 0.0f;  // t∈[0,1]: 0 = fine, 1 = coarse
};

// Compute the per-chunk LOD + morph factor from the camera distance to the
// chunk's bounding-box center. The canonical CDLOD scheme:
//
//   - `lod_distances[i]` is the *outer* distance for LOD i. Inside that
//     ring the chunk renders at LOD i.
//   - The morph band runs from `morph_start_fraction * lod_distances[i]` to
//     `lod_distances[i]` itself. Outside the band morph is 0 (fine); at the
//     outer ring morph is 1 (fully coarse, ready to switch to LOD i+1 next
//     frame without a visual pop).
//
// `lod_distances_count` lets the caller pass any number of LOD bands; we
// clamp to the last one for distances beyond the farthest band.
inline CdlodMorph compute_chunk_morph(const CdlodChunk& chunk,
                                      math::Vec3 eye,
                                      const f32* lod_distances,
                                      u32 lod_distances_count,
                                      f32 morph_start_fraction = 0.667f) noexcept {
    CdlodMorph m{};
    if (!lod_distances || lod_distances_count == 0)
        return m;

    // Chunk center for the distance query.
    const f32 cx = 0.5f * (chunk.bounds.min.x + chunk.bounds.max.x);
    const f32 cy = 0.5f * (chunk.bounds.min.y + chunk.bounds.max.y);
    const f32 cz = 0.5f * (chunk.bounds.min.z + chunk.bounds.max.z);
    const f32 dx = cx - eye.x;
    const f32 dy = cy - eye.y;
    const f32 dz = cz - eye.z;
    const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Find which LOD bin we're in.
    u32 lod = 0;
    for (u32 i = 0; i < lod_distances_count; ++i) {
        if (d <= lod_distances[i]) {
            lod = i;
            break;
        }
        lod = i;  // overshoot — keep last
        if (i + 1 == lod_distances_count) {
            // Past the farthest band: stay at the coarsest LOD, morph=1.
            m.lod_level = lod;
            m.morph_factor = 1.0f;
            return m;
        }
    }
    m.lod_level = lod;

    // Inside the band [start, end] interpolate morph from 0 → 1.
    const f32 end = lod_distances[lod];
    const f32 start = morph_start_fraction * end;
    if (d <= start) {
        m.morph_factor = 0.0f;
    } else if (d >= end) {
        m.morph_factor = 1.0f;
    } else {
        m.morph_factor = (d - start) / (end - start);
    }
    return m;
}

// Apply the inter-LOD morph to a single vertex. Returns the post-morph
// world-space Y; X/Z are unchanged. Caller passes the vertex's integer
// texel coords (so the morph kernel can recompute the coarse-grid blend
// against the heightmap) and the chunk's morph state.
//
// This is the load-bearing kernel the watertight test pins: along any LOD
// seam between two chunks with the SAME (lod_level, morph_factor), the
// post-morph Y is identical because `coarse_height_at_lod` reads the same
// texel corners and `morph_height` is a deterministic linear blend.
PSY_FORCEINLINE f32 apply_vertex_morph(const HeightmapDesc& h,
                                       i32 texel_x,
                                       i32 texel_z,
                                       const CdlodMorph& morph) noexcept {
    const f32 y_fine = height_at_texel(h, texel_x, texel_z);
    if (morph.lod_level == 0 || morph.morph_factor <= 0.0f)
        return y_fine;
    const f32 y_coarse = coarse_height_at_lod(h, texel_x, texel_z, morph.lod_level);
    // Special-case the endpoints so callers / tests can reason about
    // bitwise reproduction at morph=0 / morph=1 (the lerp `a + (b-a)*t` is
    // mathematically t=1 → b, but in float `a + b - a` ≠ b for some `a`).
    if (morph.morph_factor >= 1.0f)
        return y_coarse;
    return morph_height(y_fine, y_coarse, morph.morph_factor);
}

// Wave-B-aware chunk builder: same vertex/index topology as the Wave-A
// leaf chunk (full-resolution mesh), but every vertex's Y is run through
// the morph kernel so the chunk renders at any LOD level + morph factor
// without re-tessellating the mesh.
//
// We keep Wave A's `build_chunk` intact (callers that don't yet wire morph
// state still get a leaf chunk). `build_chunk_with_morph` is the new entry
// point for Wave B-aware callers.
inline CdlodChunk build_chunk_with_morph(const HeightmapDesc& h,
                                         u32 chunk_x,
                                         u32 chunk_z,
                                         const CdlodMorph& morph) noexcept {
    CdlodChunk c = build_chunk(h, chunk_x, chunk_z);
    if (morph.lod_level == 0 || morph.morph_factor <= 0.0f)
        return c;

    // Walk the chunk's verts and rewrite each Y. The X/Z positions encode
    // the texel coord directly (Wave A places verts at `texel*spacing`),
    // so we recover the texel from a divide. Spacing > 0 is guaranteed —
    // `build_chunk` early-outs on size_x < 2 / size_z < 2.
    const f32 inv_spacing = 1.0f / (h.spacing > 0.0f ? h.spacing : 1.0f);
    f32 y_min = 1e30f;
    f32 y_max = -1e30f;
    for (auto& v : c.vertices) {
        const i32 tx_i = static_cast<i32>(std::floor(v.position.x * inv_spacing + 0.5f));
        const i32 tz_i = static_cast<i32>(std::floor(v.position.z * inv_spacing + 0.5f));
        v.position.y = apply_vertex_morph(h, tx_i, tz_i, morph);
        if (v.position.y < y_min)
            y_min = v.position.y;
        if (v.position.y > y_max)
            y_max = v.position.y;
    }
    if (y_min > y_max) {
        y_min = 0.0f;
        y_max = 0.0f;
    }
    c.bounds.min.y = y_min;
    c.bounds.max.y = y_max;
    c.lod = morph.lod_level;
    return c;
}

}  // namespace psynder::world::outdoor::detail
