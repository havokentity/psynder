// SPDX-License-Identifier: MIT
// Psynder — triangle setup: clipspace verts -> screen-space edge equations
// in Q24.8, plus the per-attribute gradient pack used by per-tile coverage
// walk. DESIGN.md §7.3 / §7.4. Lane 07 internal.

#pragma once

#include "Fixed.h"
#include "core/Types.h"
#include "math/Math.h"

namespace psynder::render::raster {

// Vertex after viewport transform. Position is post-perspective; w is
// retained for perspective-correct attribute interpolation. uv/color carry
// the per-vertex attributes the rasterizer interpolates today.
struct ScreenVertex {
    f32 x;          // viewport x (pixel space, sub-pixel from FxQ24_8 fp)
    f32 y;          // viewport y
    f32 z;          // depth in [0,1]
    f32 inv_w;      // 1 / clipspace w
    f32 u_over_w;   // uv.u * inv_w
    f32 v_over_w;   // uv.v * inv_w
    f32 r_over_w;   // r * inv_w (premultiplied vertex color)
    f32 g_over_w;
    f32 b_over_w;
    f32 a_over_w;
};

// Per-triangle setup data. Computed once per triangle in the binner; the
// per-tile raster walks coverage using only the cached values here.
struct PSY_CACHELINE_ALIGN TriSetup {
    // Fixed-point screen verts (sub-pixel snap)
    FxQ24_8 x0, y0;
    FxQ24_8 x1, y1;
    FxQ24_8 x2, y2;

    // Bounding box in integer pixel coords (inclusive min, exclusive max)
    i32 minx, miny, maxx, maxy;

    // Inverse of 2× signed area for barycentric normalization. NaN-safe:
    // degenerate triangles get filtered out before this is computed.
    f32 inv_area2x;

    // Fill rule biases per edge (top-left rule; 0 or -1)
    i64 bias0, bias1, bias2;

    // Per-vertex attributes (used for barycentric interpolation in the quad)
    ScreenVertex v0, v1, v2;

    // Front-facing? false ⇒ culled or back-face
    bool valid;
};

// Compute the edge function E_i at (px, py) for edge i in {0,1,2}.
// Edges are (v1→v2), (v2→v0), (v0→v1) — opposite the corresponding vertex.
PSY_FORCEINLINE i64 eval_edge0(const TriSetup& t, FxQ24_8 px, FxQ24_8 py) noexcept {
    return edge_func(t.x1, t.y1, t.x2, t.y2, px, py) + t.bias0;
}
PSY_FORCEINLINE i64 eval_edge1(const TriSetup& t, FxQ24_8 px, FxQ24_8 py) noexcept {
    return edge_func(t.x2, t.y2, t.x0, t.y0, px, py) + t.bias1;
}
PSY_FORCEINLINE i64 eval_edge2(const TriSetup& t, FxQ24_8 px, FxQ24_8 py) noexcept {
    return edge_func(t.x0, t.y0, t.x1, t.y1, px, py) + t.bias2;
}

// Setup a triangle from three post-MVP clipspace verts.
// Returns false if degenerate / back-facing / fully outside viewport.
bool setup_triangle(const math::Vec4& cp0, const math::Vec4& cp1, const math::Vec4& cp2,
                    math::Vec2 uv0, math::Vec2 uv1, math::Vec2 uv2,
                    u32 col0, u32 col1, u32 col2,
                    u32 viewport_w, u32 viewport_h,
                    TriSetup& out) noexcept;

}  // namespace psynder::render::raster
