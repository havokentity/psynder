// SPDX-License-Identifier: MIT
// Psynder samples — per-vertex (Gouraud) directional lighting.
//
// The rasterizer interpolates per-vertex colour perspective-correctly, so
// baking a lighting term into vertex colours at submit time is effectively
// free and gives flat-shaded sample geometry real form/depth. This is plain
// Lambert from one key light plus a constant ambient; no per-pixel cost and no
// new rasterizer path.
//
// Lighting is computed in whatever space the vertex normals are in. For static
// meshes that's world space; for a rotating model, pass the model's rotation
// so normals are oriented correctly (apply_gouraud_rotated), otherwise the
// light appears glued to the object.

#pragma once

#include "math/Math.h"
#include "render/raster/Raster.h"

namespace psynder::samples {

struct DirLight {
    math::Vec3 direction{0.4f, -0.85f, -0.35f};  // direction the light travels
    f32 ambient = 0.35f;                         // floor so back faces aren't black
    f32 intensity = 0.85f;                       // diffuse scale
};

// Modulate a packed RGBA8 colour by a Lambert+ambient term for `normal`
// (assumed already in the light's space). Alpha is preserved.
inline u32 shade(u32 color, math::Vec3 normal, const DirLight& light) noexcept {
    const math::Vec3 n = math::normalize(normal);
    const math::Vec3 to_light = math::normalize(math::mul(light.direction, -1.0f));
    f32 ndl = math::dot(n, to_light);
    if (ndl < 0.0f)
        ndl = 0.0f;
    f32 f = light.ambient + light.intensity * ndl;
    if (f > 1.0f)
        f = 1.0f;
    auto ch = [&](u32 shift) noexcept -> u32 {
        const f32 v = static_cast<f32>((color >> shift) & 0xFFu) * f;
        const u32 iv = static_cast<u32>(v + 0.5f);
        return (iv > 255u ? 255u : iv) << shift;
    };
    return (color & 0xFF000000u) | ch(0) | ch(8) | ch(16);
}

// Bake Gouraud lighting into a mutable vertex array (normals in light space).
inline void apply_gouraud(render::raster::Vertex* verts, u32 vertex_count, const DirLight& light) noexcept {
    for (u32 i = 0; i < vertex_count; ++i)
        verts[i].color = shade(verts[i].color, verts[i].normal, light);
}

// Same, but rotate each normal by `rot` (a model rotation matrix) into world
// space first — use for moving/rotating objects so the light stays fixed.
inline void apply_gouraud_rotated(render::raster::Vertex* verts,
                                  u32 vertex_count,
                                  const math::Mat4& rot,
                                  const DirLight& light) noexcept {
    for (u32 i = 0; i < vertex_count; ++i) {
        const math::Vec4 wn =
            math::mul(rot, math::Vec4{verts[i].normal.x, verts[i].normal.y, verts[i].normal.z, 0.0f});
        verts[i].color = shade(verts[i].color, math::Vec3{wn.x, wn.y, wn.z}, light);
    }
}

}  // namespace psynder::samples
