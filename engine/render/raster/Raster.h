// SPDX-License-Identifier: MIT
// Psynder — tiled sort-middle rasterizer public API. Lane 07 owns.
//
// Hot path: vertex transform → triangle setup → tile bin → per-tile raster
// + shade. See DESIGN.md §7 for the full pipeline.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

namespace psynder::render::raster {

// ─── Material handles (resolved against the asset cache) ─────────────────
struct MaterialTag {};
using MaterialId = Handle<MaterialTag>;

struct TextureTag {};
using TextureId = Handle<TextureTag>;

// ─── Vertex format (SoA on the rasterizer side; AoS on disk) ─────────────
struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec2 lightmap_uv;
    u32        color = 0xFFFFFFFFu;   // packed RGBA8
};

// ─── DrawItem — the unit the binner sees ─────────────────────────────────
struct DrawItem {
    const Vertex*    vertices    = nullptr;
    u32              vertex_count= 0;
    const u32*       indices     = nullptr;
    u32              index_count = 0;
    math::Mat4       model;
    MaterialId       material;
    u8               flags       = 0;
};

// ─── Scene-wide rasterizer state ─────────────────────────────────────────
struct ViewState {
    math::Mat4   view;
    math::Mat4   projection;
    Framebuffer  target;
    u32          tile_w = 64;
    u32          tile_h = 64;
};

// ─── Driver API ──────────────────────────────────────────────────────────
class Rasterizer {
public:
    static Rasterizer& Get();

    void begin_frame(const ViewState& view);
    void submit(const DrawItem& draw);
    void end_frame();
};

// Convenience: clear a framebuffer to the given RGBA8.
void clear_framebuffer(Framebuffer& fb, u32 rgba) noexcept;

}  // namespace psynder::render::raster
