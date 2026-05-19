// SPDX-License-Identifier: MIT
// Psynder — Lane 16 (immediate-mode UI). 3D BRUSH PREVIEW WIREFRAMES.
//
// The editor (Lane 18) brush CSG / sculpt tools want a 3D-shaped preview
// in the viewport that follows the cursor. `Overlay.h::brush_preview()`
// covers the 2D disk case; this header covers the 3D primitives (box,
// cylinder, sphere) projected to screen space and rendered as wireframe
// outlines via the frozen `imm::line` primitive.
//
// All three calls take `origin` as the box / cylinder-base / sphere-
// centre in world space and `size` as the extents (axis-aligned for the
// box, x = radius / z = height for the cylinder, x = radius for the
// sphere). For Wave B the projection is **camera-free**: the caller
// supplies the screen-space mapping by passing in a `view_proj` matrix
// directly. Tests can drive the helpers headlessly via a `Vec2` offset.

#pragma once

#include "Imm.h"
#include "Overlay.h"

#include "core/Types.h"
#include "math/Math.h"

#include <array>
#include <cmath>

namespace psynder::ui::imm {

namespace brush_detail {

// Same screen projection helper as Gizmo.h, copied so each header is
// self-contained (header-only inline lets tests pull either in without
// linking the lane lib).
inline math::Vec2 project_to_screen(math::Vec3      world,
                                    const math::Mat4& view_proj,
                                    math::Vec2      screen_size) noexcept {
    const math::Vec4 clip = math::mul(view_proj,
                                      math::Vec4{ world.x, world.y, world.z, 1.0f });
    if (clip.w <= 0.0001f) {
        return { std::nanf(""), std::nanf("") };
    }
    const f32 ndc_x = clip.x / clip.w;
    const f32 ndc_y = clip.y / clip.w;
    return {
        (ndc_x * 0.5f + 0.5f) * screen_size.x,
        (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_size.y,
    };
}

inline math::Vec2 default_screen(math::Vec2 s) noexcept {
    if (s.x <= 0.0f || s.y <= 0.0f) return { 1024.0f, 768.0f };
    return s;
}

// Identity projection helper used by tests: returns x/y of the world
// point shifted by the screen-centre offset, ignoring z. Useful for
// off-screen pixel-accurate verification when no view matrix is passed.
inline math::Vec2 identity_xy(math::Vec3 p) noexcept { return { p.x, p.y }; }

// A stable identity Mat4 with static-storage duration so call sites can
// take a const reference without depending on default-argument temporary
// lifetime extension (which has been observed to mis-emit under some
// optimizer + inlining combinations).
inline const math::Mat4& identity_mat4() noexcept {
    static const math::Mat4 m = math::identity4();
    return m;
}

}  // namespace brush_detail

// ─── Box brush ───────────────────────────────────────────────────────────
//
// Draws the 12 edges of an axis-aligned box. `origin` is the box centre
// in world space; `size` is the full extents along X/Y/Z. The world
// vertices are projected via `view_proj` (defaults to identity, which
// makes the box screen-aligned for tests).
inline void brush_preview_box(math::Vec3      origin,
                              math::Vec3      size,
                              u32             rgba,
                              const math::Mat4& view_proj = brush_detail::identity_mat4(),
                              math::Vec2      screen_size = { 0.0f, 0.0f }) noexcept {
    namespace bd = brush_detail;
    const math::Vec2 ss = bd::default_screen(screen_size);

    const f32 hx = size.x * 0.5f;
    const f32 hy = size.y * 0.5f;
    const f32 hz = size.z * 0.5f;
    const std::array<math::Vec3, 8> v = {
        math::Vec3{ origin.x - hx, origin.y - hy, origin.z - hz },  // 0 bbb
        math::Vec3{ origin.x + hx, origin.y - hy, origin.z - hz },  // 1 fbb
        math::Vec3{ origin.x + hx, origin.y + hy, origin.z - hz },  // 2 fub
        math::Vec3{ origin.x - hx, origin.y + hy, origin.z - hz },  // 3 bub
        math::Vec3{ origin.x - hx, origin.y - hy, origin.z + hz },  // 4 bbf
        math::Vec3{ origin.x + hx, origin.y - hy, origin.z + hz },  // 5 fbf
        math::Vec3{ origin.x + hx, origin.y + hy, origin.z + hz },  // 6 fuf
        math::Vec3{ origin.x - hx, origin.y + hy, origin.z + hz },  // 7 buf
    };
    std::array<math::Vec2, 8> s{};
    for (usize i = 0; i < 8; ++i) {
        s[i] = bd::project_to_screen(v[i], view_proj, ss);
    }
    // 12 cube edges: bottom (0-1-2-3), top (4-5-6-7), four verticals.
    constexpr std::array<std::pair<u8, u8>, 12> kEdges = {{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},   // bottom face
        {4, 5}, {5, 6}, {6, 7}, {7, 4},   // top face
        {0, 4}, {1, 5}, {2, 6}, {3, 7},   // verticals
    }};
    for (const auto& e : kEdges) {
        const math::Vec2 a = s[e.first];
        const math::Vec2 b = s[e.second];
        if (std::isnan(a.x) || std::isnan(b.x)) continue;
        imm::line(a, b, rgba);
    }
}

// ─── Cylinder brush ──────────────────────────────────────────────────────
//
// Two horizontal rings (top + bottom) connected by 4 vertical struts.
// `origin` is the base centre; `size.x` is the radius, `size.z` is the
// height along world Z. Wave B uses a coarse 16-segment ring.
inline void brush_preview_cylinder(math::Vec3      origin,
                                   math::Vec3      size,
                                   u32             rgba,
                                   const math::Mat4& view_proj = brush_detail::identity_mat4(),
                                   math::Vec2      screen_size = { 0.0f, 0.0f }) noexcept {
    namespace bd = brush_detail;
    const math::Vec2 ss = bd::default_screen(screen_size);
    const f32 r  = size.x;
    const f32 h  = size.z;
    constexpr u32 kSeg = 16;

    auto ring = [&](f32 z_off) {
        math::Vec2 prev{};
        for (u32 i = 0; i <= kSeg; ++i) {
            const f32 theta = math::kTwoPi * static_cast<f32>(i)
                            / static_cast<f32>(kSeg);
            const math::Vec3 w = {
                origin.x + r * std::cos(theta),
                origin.y + r * std::sin(theta),
                origin.z + z_off,
            };
            const math::Vec2 p = bd::project_to_screen(w, view_proj, ss);
            if (i > 0 && !std::isnan(prev.x) && !std::isnan(p.x)) {
                imm::line(prev, p, rgba);
            }
            prev = p;
        }
    };
    ring(0.0f);
    ring(h);

    // 4 vertical struts at 0, 90, 180, 270 degrees.
    for (u32 i = 0; i < 4; ++i) {
        const f32 theta = math::kHalfPi * static_cast<f32>(i);
        const f32 cx = origin.x + r * std::cos(theta);
        const f32 cy = origin.y + r * std::sin(theta);
        const math::Vec3 a{ cx, cy, origin.z };
        const math::Vec3 b{ cx, cy, origin.z + h };
        const math::Vec2 sa = bd::project_to_screen(a, view_proj, ss);
        const math::Vec2 sb = bd::project_to_screen(b, view_proj, ss);
        if (std::isnan(sa.x) || std::isnan(sb.x)) continue;
        imm::line(sa, sb, rgba);
    }
}

// ─── Sphere brush ────────────────────────────────────────────────────────
//
// Three orthogonal rings (XY, YZ, XZ) — the classic "wire sphere" look.
inline void brush_preview_sphere(math::Vec3      origin,
                                 math::Vec3      size,
                                 u32             rgba,
                                 const math::Mat4& view_proj = brush_detail::identity_mat4(),
                                 math::Vec2      screen_size = { 0.0f, 0.0f }) noexcept {
    namespace bd = brush_detail;
    const math::Vec2 ss = bd::default_screen(screen_size);
    const f32 r = size.x;
    constexpr u32 kSeg = 24;

    auto wire_ring = [&](math::Vec3 ux, math::Vec3 uy) {
        math::Vec2 prev{};
        for (u32 i = 0; i <= kSeg; ++i) {
            const f32 theta = math::kTwoPi * static_cast<f32>(i)
                            / static_cast<f32>(kSeg);
            const f32 c = r * std::cos(theta);
            const f32 s = r * std::sin(theta);
            const math::Vec3 w = {
                origin.x + ux.x * c + uy.x * s,
                origin.y + ux.y * c + uy.y * s,
                origin.z + ux.z * c + uy.z * s,
            };
            const math::Vec2 p = bd::project_to_screen(w, view_proj, ss);
            if (i > 0 && !std::isnan(prev.x) && !std::isnan(p.x)) {
                imm::line(prev, p, rgba);
            }
            prev = p;
        }
    };
    wire_ring({1, 0, 0}, {0, 1, 0});  // XY
    wire_ring({0, 1, 0}, {0, 0, 1});  // YZ
    wire_ring({1, 0, 0}, {0, 0, 1});  // XZ
}

}  // namespace psynder::ui::imm
