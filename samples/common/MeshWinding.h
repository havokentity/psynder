// SPDX-License-Identifier: MIT
// Psynder samples — fix triangle winding from per-vertex normals.
//
// The rasterizer's default cull mode is Back: a triangle is front-facing when
// its signed screen area is positive (CCW after the viewport Y-flip). Meshes
// must therefore be wound consistently. Hand-built sample meshes (icospheres,
// cubes, BSP-room quads) often aren't, which under back-face culling drops the
// wrongly-wound triangles and leaves holes.
//
// fix_winding() makes every triangle wound so that its geometric normal agrees
// with the (averaged) per-vertex normal — i.e. front-facing when the camera is
// on the normal's side. Call it once on a mesh after building it, before
// submitting, and back-face culling renders cleanly.

#pragma once

#include "math/Math.h"
#include "render/raster/Raster.h"

namespace psynder::samples {

// In-place: swaps the 2nd/3rd index of any triangle whose geometric winding
// disagrees with its vertex normals. `indices` length must be a multiple of 3.
// `vertex_count` bounds the index range — triangles referencing an out-of-range
// vertex are skipped (no OOB reads), matching the rasterizer's own guard.
inline void fix_winding(const render::raster::Vertex* verts,
                        u32 vertex_count,
                        u32* indices,
                        u32 index_count) noexcept {
    for (u32 i = 0; i + 3 <= index_count; i += 3) {
        const u32 a = indices[i + 0];
        const u32 b = indices[i + 1];
        const u32 c = indices[i + 2];
        if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
            continue;  // out-of-range triple — skip rather than read OOB
        const math::Vec3 p0 = verts[a].position;
        const math::Vec3 p1 = verts[b].position;
        const math::Vec3 p2 = verts[c].position;
        const math::Vec3 geo = math::cross(math::sub(p1, p0), math::sub(p2, p0));
        const math::Vec3 n = math::add(math::add(verts[a].normal, verts[b].normal), verts[c].normal);
        // The rasterizer's Back cull keeps the winding whose geometric normal
        // points OPPOSITE the shading normal (a consequence of the viewport
        // Y-flip — front is CW-from-outside in this engine). So swap when the
        // geometric and vertex normals agree, leaving every triangle wound to
        // render from its normal's side. (Calibrated against sample geometry.)
        if (math::dot(geo, n) > 0.0f) {
            indices[i + 1] = c;
            indices[i + 2] = b;
        }
    }
}

}  // namespace psynder::samples
