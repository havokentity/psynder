// SPDX-License-Identifier: MIT
// Psynder — tile binner + per-tile rasterizer. Templated over <TILE_W,TILE_H>
// per ADR-002. Specializations for 32×32 / 64×64 / 128×128 are emitted from
// the .cpp; the function pointers are swapped behind the r_tile_size cvar.
// DESIGN.md §7.3 / §7.4.

#pragma once

#include "EdgeEq.h"
#include "RasterLighting.h"
#include "core/Types.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <cstddef>

namespace psynder::render::raster {

// One DrawItem-level draw command after vertex shading. Holds a pointer
// to a contiguous array of TriSetup in the frame arena, plus a few flags.
// 128 bytes / cache line per DESIGN.md §7.3 (give or take padding).
//
// Wave-B adds the surface-cache dispatch tag (DESIGN.md §7.6 / ADR-001).
// The `shading_path` byte is set by Rasterizer::end_frame() from the
// per-draw eligibility check; the tile rasterizer dispatches on it once
// per draw, never per pixel.
struct PSY_CACHELINE_ALIGN DrawCmd {
    const TriSetup* tris = nullptr;
    u32 tri_count = 0;
    u32 material_id = 0;
    // Material albedo tint (0xAABBGGRR). Copied from DrawItem; the fragment loop
    // modulates base colour by this so authored material colours show in raster
    // (was previously never read). 0xFFFFFFFF = identity (white).
    u32 albedo_rgba8 = 0xFFFFFFFFu;
    DrawFlags flags = DrawFlags::None;
    // Surface-cache dispatch (DESIGN.md §7.6).
    u8 shading_path = 0;  // ShadingPath enum
    // Per-draw EWA anisotropy cap (1/2/4/8/16). 1 ⇒ no anisotropy; the
    // inner loop falls back to bilinear / trilinear. Set by Rasterizer
    // end_frame from the r_anisotropy cvar (DESIGN.md §7.5). Clamped to
    // the canonical set so the inner-loop dispatch is a small switch.
    u8 aniso_max = 1;
    u8 blend_mode = 0;  // DrawBlendMode
    u8 blend_opacity = 255;
    u8 _spad[1] = {};
    RasterMaterialInputs material_lighting{};
    RasterLightPacket light_packet{};
    const u32* surface_cache_payload = nullptr;  // pre-multiplied chunk
    u32 surface_cache_width = 0;
    u32 surface_cache_height = 0;
    u32 _pad[2] = {};
};

// A bin entry — (draw, triangle index) pair for one tile. Stored
// SoA-ish in the per-tile vector (also living in the frame arena).
struct BinEntry {
    u32 draw_idx;
    u32 tri_idx;
};

// Per-frame binning state, allocated from the frame arena.
struct TileGrid {
    u32 tile_w = 64;
    u32 tile_h = 64;
    u32 cols = 0;
    u32 rows = 0;
    BinEntry* entries = nullptr;  // contiguous pool
    u32 entries_capacity = 0;
    u32 entries_used = 0;
    // Per-tile offset + count into entries[]
    u32* tile_offset = nullptr;
    u32* tile_count = nullptr;
};

// Texture descriptor handed to the bilinear sampler. The lane 07-local
// texture is the minimum the rasterizer needs to shade — full asset
// integration arrives with lane 05's Vfs / lane 24's lm_pak.
//
// Mip support (Wave B / DESIGN.md §7.5):
//   `mip_count` ≥ 1; mip 0 is `texels`+`width`/`height`. Higher mips
//   sit at the contiguous pointer chain in `mips[]`. When `mip_count`
//   is 1, trilinear collapses to bilinear and `mips[]` is unused.
struct Texture {
    const u32* texels = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 pitch = 0;  // texels per row

    // Optional mip chain. mips[0] aliases the (texels, width, height,
    // pitch) above; mips[1..mip_count-1] are the half-res successors.
    static constexpr u32 kMaxMipLevels = 12;
    struct MipLevel {
        const u32* texels = nullptr;
        u32 width = 0;
        u32 height = 0;
        u32 pitch = 0;
    };
    MipLevel mips[kMaxMipLevels] = {};
    u32 mip_count = 1;
};

// Rasterize one tile's slice of triangles into the framebuffer. The
// implementation is templated over tile dimensions; the .cpp emits the
// 32 / 64 / 128 specializations.
template <u32 TILE_W, u32 TILE_H>
void rasterize_tile(const Framebuffer& fb,
                    const DrawCmd* draws,
                    u32 draw_count,
                    const TileGrid& grid,
                    const Texture* tex,
                    u32 tile_x,
                    u32 tile_y,
                    bool affine_mode) noexcept;

// Pick the right specialization based on the configured tile size.
// Returns nullptr for sizes we don't ship a specialization for.
using TileRasterFn = void (*)(
    const Framebuffer&, const DrawCmd*, u32, const TileGrid&, const Texture*, u32, u32, bool);
TileRasterFn select_tile_raster_fn(u32 tile_w, u32 tile_h) noexcept;

// Binner — assign one triangle to every tile it overlaps. Append-only into
// the tile pool. Caller bumps draw_idx between submissions.
void bin_triangle(TileGrid& grid, u32 draw_idx, u32 tri_idx, const TriSetup& tri) noexcept;

}  // namespace psynder::render::raster
