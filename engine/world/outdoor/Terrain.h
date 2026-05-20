// SPDX-License-Identifier: MIT
// Psynder — outdoor heightmap world. Two backends (CDLOD mesh + heightmap
// raymarch) per ADR-008. Lane 11 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string_view>
#include <vector>

namespace psynder::world::outdoor {

enum class TerrainBackend : u8 {
    PolygonCDLOD,       // default for NFS-style racing tracks
    HeightmapRaymarch,  // default for IGI / Delta Force-style tactical FPS
};

struct HeightmapDesc {
    u32 size_x = 0;                // texels along world X
    u32 size_z = 0;                // texels along world Z
    f32 spacing = 1.0f;            // metres per texel
    f32 height_scale = 1.0f;       // metres per heightmap unit
    const u16* heights = nullptr;  // 16-bit height samples
};

struct SplineRoadSegment {
    math::Vec3 p0, p1, p2, p3;  // cubic Bezier control points
    f32 half_width = 4.0f;
    f32 banking_rad = 0.0f;
};

class TerrainMesh {
   public:
    void build(const HeightmapDesc& desc);
    void render_cdlod(const math::Mat4& view, const math::Mat4& proj) const;
};

class TerrainRaymarch {
   public:
    void set_heightmap(const HeightmapDesc& desc);
    // Lane 11 implements per-column ray march into the framebuffer + Z.
    void render(const math::Mat4& view, const math::Mat4& proj) const;
};

void load_spline_track(std::string_view virtual_path, std::vector<SplineRoadSegment>& segments_out);

}  // namespace psynder::world::outdoor
