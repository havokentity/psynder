// SPDX-License-Identifier: MIT
// Psynder — internal heightmap sampling helpers used by both terrain
// backends (CDLOD mesh in CdlodMesh*, per-column raymarch in Raymarch*).
//
// Header-only / inline so unit tests can pick up the implementation without
// linking the world_outdoor static lib (tests/unit/CMakeLists.txt is owned
// by the build-system maintainer and only links a fixed set of lane libs).
//
// World mapping (DESIGN.md §9.2, ADR-005):
//   World X = column_index * spacing
//   World Z = row_index    * spacing
//   World Y = sample_u16   * height_scale
//
// All maps capped at 16 km × 16 km — single-precision float coords are
// sufficient when combined with per-frame render-relative origin re-centering.

#pragma once

#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace psynder::world::outdoor::detail {

// Hard-coded constants (DESIGN.md §9.2 + ADR-008).
inline constexpr u32 kChunkDim = 64;            // CDLOD leaf chunks are 64×64 quads
inline constexpr u32 kMaxLodLevels = 8;         // 64 → 128 → 256 → … 64*2^7 = 8192 quads
inline constexpr f32 kMaxMapMetres = 16384.0f;  // 16 km map cap (§9.2)
inline constexpr u32 kSplatWeightCount = 4;     // 4-weight splatmap per vertex (§9.2)

// ─── Clamp helpers ───────────────────────────────────────────────────────
PSY_FORCEINLINE u32 clamp_u32(i32 v, u32 lo, u32 hi) noexcept {
    if (v < static_cast<i32>(lo))
        return lo;
    const u32 u = static_cast<u32>(v);
    return u > hi ? hi : u;
}

PSY_FORCEINLINE f32 clamp_f32(f32 v, f32 lo, f32 hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ─── Raw heightmap sample (no interpolation) ─────────────────────────────
// Returns 0 for out-of-bounds (border behavior). This matches a "skybox"
// horizon for the raymarcher and a "flat" border for the mesh.
PSY_FORCEINLINE u16 sample_raw(const HeightmapDesc& h, i32 x, i32 z) noexcept {
    if (!h.heights || h.size_x == 0 || h.size_z == 0)
        return 0;
    if (x < 0 || z < 0)
        return 0;
    const u32 ux = static_cast<u32>(x);
    const u32 uz = static_cast<u32>(z);
    if (ux >= h.size_x || uz >= h.size_z)
        return 0;
    return h.heights[static_cast<usize>(uz) * h.size_x + ux];
}

// World-Y in metres for an integer texel sample.
PSY_FORCEINLINE f32 height_at_texel(const HeightmapDesc& h, i32 x, i32 z) noexcept {
    return static_cast<f32>(sample_raw(h, x, z)) * h.height_scale;
}

// Bilinear height sample at world (wx, wz) in metres. Used by both backends
// and by heightfield physics (lane 13 collides against the same data).
PSY_FORCEINLINE f32 sample_bilinear(const HeightmapDesc& h, f32 wx, f32 wz) noexcept {
    if (!h.heights || h.size_x == 0 || h.size_z == 0 || h.spacing <= 0.0f) {
        return 0.0f;
    }
    const f32 fx = wx / h.spacing;
    const f32 fz = wz / h.spacing;
    const i32 x0 = static_cast<i32>(std::floor(fx));
    const i32 z0 = static_cast<i32>(std::floor(fz));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 tz = fz - static_cast<f32>(z0);

    const f32 h00 = height_at_texel(h, x0, z0);
    const f32 h10 = height_at_texel(h, x0 + 1, z0);
    const f32 h01 = height_at_texel(h, x0, z0 + 1);
    const f32 h11 = height_at_texel(h, x0 + 1, z0 + 1);

    const f32 hx0 = h00 + (h10 - h00) * tx;
    const f32 hx1 = h01 + (h11 - h01) * tx;
    return hx0 + (hx1 - hx0) * tz;
}

// Central-difference normal at a texel (cheap; no interpolation).
PSY_FORCEINLINE math::Vec3 normal_at_texel(const HeightmapDesc& h, i32 x, i32 z) noexcept {
    const f32 hl = height_at_texel(h, x - 1, z);
    const f32 hr = height_at_texel(h, x + 1, z);
    const f32 hd = height_at_texel(h, x, z - 1);
    const f32 hu = height_at_texel(h, x, z + 1);
    // World-space gradient. Spacing cancels in the normalization but we
    // keep it for numerical stability when spacing is non-unity.
    const f32 s = h.spacing > 0.0f ? h.spacing : 1.0f;
    math::Vec3 n{(hl - hr) * 0.5f, 2.0f * s, (hd - hu) * 0.5f};
    return math::normalize(n);
}

// 4-weight per-vertex splatmap. Derived from slope + altitude (deterministic;
// the editor will replace this with painted weights in Wave B). The four
// channels are: 0=grass, 1=rock, 2=sand, 3=snow. Weights sum to 1.
struct SplatWeights {
    f32 w[kSplatWeightCount];
};

PSY_FORCEINLINE SplatWeights splat_at_texel(const HeightmapDesc& h, i32 x, i32 z) noexcept {
    const math::Vec3 n = normal_at_texel(h, x, z);
    const f32 wy = clamp_f32(n.y, 0.0f, 1.0f);  // 1 = flat, 0 = cliff
    const f32 hy = height_at_texel(h, x, z);

    // Slope-based split: grass on flat, rock on steep.
    const f32 slope = 1.0f - wy;  // 0 = flat
    SplatWeights s{};
    s.w[0] = wy * 0.85f;     // grass
    s.w[1] = slope * 0.95f;  // rock

    // Altitude-based split: sand at low Y, snow at high Y.
    // Mapped against (h.size_z * spacing) as a stand-in for the map's
    // altitude scale — gives stable behavior across map sizes.
    const f32 altScale = h.height_scale * 32768.0f;  // ~half of u16 range
    if (altScale > 0.0f) {
        const f32 a = clamp_f32(hy / altScale, 0.0f, 1.0f);
        s.w[2] = (1.0f - a) * 0.1f;  // sand
        s.w[3] = a * 0.2f;           // snow
    }

    // Renormalize to sum=1 (defensive, in case the heuristic underflows).
    f32 sum = s.w[0] + s.w[1] + s.w[2] + s.w[3];
    if (sum <= 0.0f) {
        s.w[0] = 1.0f;
        sum = 1.0f;
    }
    const f32 inv = 1.0f / sum;
    for (u32 i = 0; i < kSplatWeightCount; ++i)
        s.w[i] *= inv;
    return s;
}

// Pack the 4 splat weights into a u32 RGBA8 (the same format the rasterizer
// reads from Vertex::color). This is the on-the-wire format consumed by the
// per-tile shader.
PSY_FORCEINLINE u32 pack_splat(const SplatWeights& s) noexcept {
    auto toU8 = [](f32 v) -> u32 {
        const f32 c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<u32>(c * 255.0f + 0.5f) & 0xFFu;
    };
    return toU8(s.w[0]) | (toU8(s.w[1]) << 8) | (toU8(s.w[2]) << 16) | (toU8(s.w[3]) << 24);
}

// CDLOD chunk grid dims for a given heightmap. We tile the map into kChunkDim
// quad chunks; the last column / row may be partial — we still emit it,
// clipped to the map edge, so the renderer always covers the full map.
PSY_FORCEINLINE u32 chunk_count_x(const HeightmapDesc& h) noexcept {
    if (h.size_x < 2)
        return 0;
    // chunks of kChunkDim quads = kChunkDim+1 verts. Vertex count = size_x.
    // Number of quad rows = size_x - 1. Chunks tile quads, not verts.
    const u32 quads = h.size_x - 1;
    return (quads + kChunkDim - 1) / kChunkDim;
}
PSY_FORCEINLINE u32 chunk_count_z(const HeightmapDesc& h) noexcept {
    if (h.size_z < 2)
        return 0;
    const u32 quads = h.size_z - 1;
    return (quads + kChunkDim - 1) / kChunkDim;
}

}  // namespace psynder::world::outdoor::detail
