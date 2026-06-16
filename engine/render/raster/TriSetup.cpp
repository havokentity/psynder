// SPDX-License-Identifier: MIT
// Psynder — triangle setup: clipspace → screen-space + Q24.8 edges.
// DESIGN.md §7.3 — edge functions in Q24.8, sub-pixel precision 1/256 px.

#include "EdgeEq.h"
#include "Raster.h"

#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace psynder::render::raster {

namespace {

PSY_FORCEINLINE u8 unpack_r(u32 c) noexcept {
    return static_cast<u8>(c & 0xFFu);
}
PSY_FORCEINLINE u8 unpack_g(u32 c) noexcept {
    return static_cast<u8>((c >> 8) & 0xFFu);
}
PSY_FORCEINLINE u8 unpack_b(u32 c) noexcept {
    return static_cast<u8>((c >> 16) & 0xFFu);
}
PSY_FORCEINLINE u8 unpack_a(u32 c) noexcept {
    return static_cast<u8>((c >> 24) & 0xFFu);
}

}  // namespace

bool setup_triangle(const math::Vec4& cp0_in,
                    const math::Vec4& cp1_in,
                    const math::Vec4& cp2_in,
                    math::Vec2 uv0,
                    math::Vec2 uv1,
                    math::Vec2 uv2,
                    u32 col0,
                    u32 col1,
                    u32 col2,
                    u32 viewport_w,
                    u32 viewport_h,
                    TriSetup& out,
                    u8 cull_mode) noexcept {
    return setup_triangle_lit(cp0_in,
                              cp1_in,
                              cp2_in,
                              {0.0f, 0.0f, 0.0f},
                              {0.0f, 0.0f, 0.0f},
                              {0.0f, 0.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f},
                              {0.0f, 1.0f, 0.0f},
                              uv0,
                              uv1,
                              uv2,
                              col0,
                              col1,
                              col2,
                              viewport_w,
                              viewport_h,
                              out,
                              cull_mode);
}

bool setup_triangle_lit(const math::Vec4& cp0_in,
                        const math::Vec4& cp1_in,
                        const math::Vec4& cp2_in,
                        math::Vec3 world0,
                        math::Vec3 world1,
                        math::Vec3 world2,
                        math::Vec3 normal0,
                        math::Vec3 normal1,
                        math::Vec3 normal2,
                        math::Vec2 uv0,
                        math::Vec2 uv1,
                        math::Vec2 uv2,
                        u32 col0,
                        u32 col1,
                        u32 col2,
                        u32 viewport_w,
                        u32 viewport_h,
                        TriSetup& out,
                        u8 cull_mode) noexcept {
    // Reject triangles with any vertex on or behind the eye plane (w <= 0),
    // where the perspective divide is invalid. This is the eye-plane test, not
    // the near-plane test (z + w >= 0): near-plane straddlers are already split
    // upstream by Raster.cpp's Sutherland-Hodgman clipper, so by the time setup
    // runs every vertex has w > 0.
    if (cp0_in.w <= 0.0f || cp1_in.w <= 0.0f || cp2_in.w <= 0.0f) {
        out.valid = false;
        return false;
    }

    // Face culling. screen_area2x measures the signed area AFTER the viewport
    // Y-flip (screen is Y-down). The engine's front-facing convention is a
    // POSITIVE signed screen area — i.e. CCW after the Y-flip (which is CW in
    // NDC, Y-up); back faces are negative; zero is degenerate. We decide
    // front/back from the float screen area here (cheap, and avoids the Q24.8
    // snap affecting the cull). Meshes must be wound consistently — see
    // samples/common/MeshWinding.h for a normal-based rewind helper.
    //   Back  (default): keep front faces (area > 0)
    //   Front:           keep back faces  (area < 0), rewound to front for setup
    //   None:            two-sided — rewind back faces to front, draw both sides
    math::Vec4 cp0 = cp0_in;
    math::Vec4 cp1 = cp1_in;
    math::Vec4 cp2 = cp2_in;

    auto screen_area2x = [&](const math::Vec4& a, const math::Vec4& b, const math::Vec4& c) noexcept {
        const f32 vwf = static_cast<f32>(viewport_w);
        const f32 vhf = static_cast<f32>(viewport_h);
        const f32 iwa = 1.0f / a.w, iwb = 1.0f / b.w, iwc = 1.0f / c.w;
        const f32 ax = (a.x * iwa * 0.5f + 0.5f) * vwf;
        const f32 ay = (1.0f - (a.y * iwa * 0.5f + 0.5f)) * vhf;
        const f32 bx = (b.x * iwb * 0.5f + 0.5f) * vwf;
        const f32 by = (1.0f - (b.y * iwb * 0.5f + 0.5f)) * vhf;
        const f32 cx = (c.x * iwc * 0.5f + 0.5f) * vwf;
        const f32 cy = (1.0f - (c.y * iwc * 0.5f + 0.5f)) * vhf;
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    };

    const f32 sa = screen_area2x(cp0, cp1, cp2);
    const CullMode mode = static_cast<CullMode>(cull_mode);
    if (mode == CullMode::None) {  // two-sided
        if (sa == 0.0f) {          // degenerate
            out.valid = false;
            return false;
        }
        if (sa < 0.0f) {
            std::swap(cp1, cp2);
            std::swap(uv1, uv2);
            std::swap(col1, col2);
            std::swap(world1, world2);
            std::swap(normal1, normal2);
        }
    } else if (mode == CullMode::Front) {
        if (sa >= 0.0f) {  // front face or degenerate
            out.valid = false;
            return false;
        }
        std::swap(cp1, cp2);  // rewind the kept back face to front for setup
        std::swap(uv1, uv2);
        std::swap(col1, col2);
        std::swap(world1, world2);
        std::swap(normal1, normal2);
    } else {               // Back (default) — also the safe fallback for any
                           // unexpected cull_mode value: cull, never silently
                           // disable culling.
        if (sa <= 0.0f) {  // back face or degenerate
            out.valid = false;
            return false;
        }
    }

    const f32 inv_w0 = 1.0f / cp0.w;
    const f32 inv_w1 = 1.0f / cp1.w;
    const f32 inv_w2 = 1.0f / cp2.w;

    // NDC ∈ [-1,1] → viewport pixel coordinates.
    const f32 vw = static_cast<f32>(viewport_w);
    const f32 vh = static_cast<f32>(viewport_h);

    const f32 sx0 = (cp0.x * inv_w0 * 0.5f + 0.5f) * vw;
    const f32 sy0 = (1.0f - (cp0.y * inv_w0 * 0.5f + 0.5f)) * vh;
    const f32 sx1 = (cp1.x * inv_w1 * 0.5f + 0.5f) * vw;
    const f32 sy1 = (1.0f - (cp1.y * inv_w1 * 0.5f + 0.5f)) * vh;
    const f32 sx2 = (cp2.x * inv_w2 * 0.5f + 0.5f) * vw;
    const f32 sy2 = (1.0f - (cp2.y * inv_w2 * 0.5f + 0.5f)) * vh;

    // Snap to Q24.8 sub-pixel grid
    const FxQ24_8 x0 = FxQ24_8::from_float(sx0);
    const FxQ24_8 y0 = FxQ24_8::from_float(sy0);
    const FxQ24_8 x1 = FxQ24_8::from_float(sx1);
    const FxQ24_8 y1 = FxQ24_8::from_float(sy1);
    const FxQ24_8 x2 = FxQ24_8::from_float(sx2);
    const FxQ24_8 y2 = FxQ24_8::from_float(sy2);

    // Signed 2× area in Q48.16; after the swap above this is > 0 for any
    // non-degenerate triangle. Only true degenerates (zero area) are dropped.
    const i64 area2x = tri_area_2x(x0, y0, x1, y1, x2, y2);
    if (area2x <= 0) {  // degenerate (zero-area / collinear)
        out.valid = false;
        return false;
    }

    out.x0 = x0;
    out.y0 = y0;
    out.x1 = x1;
    out.y1 = y1;
    out.x2 = x2;
    out.y2 = y2;

    // Bounding box, clipped to viewport.
    const i32 vw_i = static_cast<i32>(viewport_w);
    const i32 vh_i = static_cast<i32>(viewport_h);
    const i32 minx_raw = std::min({x0.floor_to_int(), x1.floor_to_int(), x2.floor_to_int()});
    const i32 miny_raw = std::min({y0.floor_to_int(), y1.floor_to_int(), y2.floor_to_int()});
    const i32 maxx_raw = std::max({x0.ceil_to_int(), x1.ceil_to_int(), x2.ceil_to_int()});
    const i32 maxy_raw = std::max({y0.ceil_to_int(), y1.ceil_to_int(), y2.ceil_to_int()});
    out.minx = std::max(0, minx_raw);
    out.miny = std::max(0, miny_raw);
    out.maxx = std::min(vw_i, maxx_raw);
    out.maxy = std::min(vh_i, maxy_raw);
    if (out.minx >= out.maxx || out.miny >= out.maxy) {
        out.valid = false;
        return false;
    }

    // 2x area in float for barycentric normalization
    out.inv_area2x = 1.0f / static_cast<f32>(area2x);

    // Top-left bias per edge so the inside test is `E + bias >= 0`.
    out.bias0 = top_left_bias(x1, y1, x2, y2);
    out.bias1 = top_left_bias(x2, y2, x0, y0);
    out.bias2 = top_left_bias(x0, y0, x1, y1);

    // Pack ScreenVerts with 1/w + attributes premultiplied by 1/w for
    // perspective-correct interpolation (DESIGN.md §7.4).
    auto make_screen_vert = [&](f32 sx,
                                f32 sy,
                                f32 z_div_w,
                                f32 inv_w,
                                math::Vec2 uv,
                                u32 col,
                                math::Vec3 world,
                                math::Vec3 normal) noexcept {
        ScreenVertex v;
        v.x = sx;
        v.y = sy;
        v.z = z_div_w;  // NDC depth in [-1,1]
        v.inv_w = inv_w;
        v.u_over_w = uv.x * inv_w;
        v.v_over_w = uv.y * inv_w;
        v.r_over_w = static_cast<f32>(unpack_r(col)) * (1.0f / 255.0f) * inv_w;
        v.g_over_w = static_cast<f32>(unpack_g(col)) * (1.0f / 255.0f) * inv_w;
        v.b_over_w = static_cast<f32>(unpack_b(col)) * (1.0f / 255.0f) * inv_w;
        v.a_over_w = static_cast<f32>(unpack_a(col)) * (1.0f / 255.0f) * inv_w;
        v.wx_over_w = world.x * inv_w;
        v.wy_over_w = world.y * inv_w;
        v.wz_over_w = world.z * inv_w;
        v.nx_over_w = normal.x * inv_w;
        v.ny_over_w = normal.y * inv_w;
        v.nz_over_w = normal.z * inv_w;
        return v;
    };

    // Map NDC z (-1..1) to depth (0..1) — the framebuffer stores 24-bit
    // float Z (DESIGN.md §7.4) but for early-Z + tile compare we want a
    // 0..1 range. The actual far/near remapping happened in the projection
    // matrix; here we just shift.
    const f32 z0 = cp0.z * inv_w0 * 0.5f + 0.5f;
    const f32 z1 = cp1.z * inv_w1 * 0.5f + 0.5f;
    const f32 z2 = cp2.z * inv_w2 * 0.5f + 0.5f;

    out.v0 = make_screen_vert(sx0, sy0, z0, inv_w0, uv0, col0, world0, normal0);
    out.v1 = make_screen_vert(sx1, sy1, z1, inv_w1, uv1, col1, world1, normal1);
    out.v2 = make_screen_vert(sx2, sy2, z2, inv_w2, uv2, col2, world2, normal2);

    out.valid = true;
    return true;
}

}  // namespace psynder::render::raster
