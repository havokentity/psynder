// SPDX-License-Identifier: MIT
// Psynder — tiled sort-middle rasterizer public API. Lane 07 owns.
//
// Hot path: vertex transform → triangle setup → tile bin → per-tile raster
// + shade. See DESIGN.md §7 for the full pipeline.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"
#include "render/Material.h"

namespace psynder::render::raster {

// ─── Material handles (resolved against the shared material library) ─────
using MaterialId = ::psynder::render::MaterialId;

struct TextureTag {};
using TextureId = Handle<TextureTag>;

// ─── Vertex format (SoA on the rasterizer side; AoS on disk) ─────────────
struct Vertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec2 lightmap_uv;
    u32 color = 0xFFFFFFFFu;  // packed RGBA8
};

// ─── Face culling (DESIGN.md §7.3) ───────────────────────────────────────
// Front-facing is CCW after the viewport Y-flip (signed screen area > 0).
// Meshes must be wound consistently; samples/common/MeshWinding.h can fix a
// mesh's winding from its per-vertex normals. Use `None` only for genuinely
// two-sided geometry (thin walls, foliage, debug) — it costs the back-face
// pixels and disables the cull's overdraw savings.
enum class CullMode : u8 {
    Back = 0,   // cull back faces (default — closed, consistently-wound meshes)
    Front = 1,  // cull front faces (e.g. rendering interiors / inverted hulls)
    None = 2,   // two-sided: rewind back faces to front, draw both sides
};

enum class DrawBlendMode : u8 {
    Opaque = 0,
    Multiply = 1,
};

// Pre-projected blob/decal shadow input. Callers provide the receiver-space
// geometry and UVs up front; raster only interpolates UV/alpha and uses the
// existing multiplicative no-depth-write path.
struct ProjectedShadowDesc {
    const Vertex* vertices = nullptr;
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;
    math::Mat4 model;
    // Optional RGBA8 mask sampled through Vertex::uv. If omitted, vertex alpha
    // alone controls the shadow falloff.
    const u32* mask_texels = nullptr;
    u32 mask_w = 0;
    u32 mask_h = 0;
    f32 opacity = 1.0f;
    CullMode cull = CullMode::None;
};

// ─── DrawItem — the unit the binner sees ─────────────────────────────────
struct DrawItem {
    const Vertex* vertices = nullptr;
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;
    math::Mat4 model;
    MaterialId material;
    u8 flags = 0;
    CullMode cull = CullMode::Back;
    DrawBlendMode blend = DrawBlendMode::Opaque;
    f32 blend_opacity = 1.0f;
    // Optional per-draw baked lightmap chunk (RGBA8, row-major, pitch == w).
    // When set, the draw is forced onto the SurfaceCached shading path and
    // this chunk is sampled bilinearly through the base `uv` — i.e. the inner
    // loop computes vertexColor × chunk per pixel (see TileRaster's
    // surface_cached branch). Takes precedence over the SurfaceCache slab
    // lookup. Lifetime must outlive end_frame().
    const u32* lightmap_texels = nullptr;
    u32 lightmap_w = 0;
    u32 lightmap_h = 0;
};

// ─── Scene-wide rasterizer state ─────────────────────────────────────────
struct ViewState {
    math::Mat4 view;
    math::Mat4 projection;
    Framebuffer target;
    u32 tile_w = 64;
    u32 tile_h = 64;
};

// ─── Driver API ──────────────────────────────────────────────────────────
class Rasterizer {
   public:
    static Rasterizer& Get();

    void begin_frame(const ViewState& view);
    void submit(const DrawItem& draw);
    void end_frame();
};

// Compatibility alias; prefer renderer-neutral render::clear_framebuffer.
using ::psynder::render::clear_framebuffer;

// Build a multiplicative DrawItem for precomputed projected/blob shadow
// geometry. The returned draw performs no material lookup and writes no depth.
DrawItem make_projected_shadow_draw(const ProjectedShadowDesc& shadow) noexcept;

}  // namespace psynder::render::raster
