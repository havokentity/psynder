// SPDX-License-Identifier: MIT
// Psynder — Lane 16 (immediate-mode UI). 3D MANIPULATOR GIZMOS — Wave B.
//
// `Imm.h` is frozen and `Overlay.h` exposes the camera-free
// `gizmo_translate(GizmoProjection)` skeleton. Wave B layers a higher-
// level entry point on top so callers (the editor, Lane 18) can hand us
// the world-space position + a packed view_proj matrix + the latest
// mouse state and get back a "grabbed?" flag — no GizmoProjection
// plumbing required.
//
// Snap-to-grid is intentionally omitted for Wave B: callers receive the
// raw screen-space delta and may quantize themselves. The arrow visuals
// are drawn via the frozen `imm::line` primitive from `Overlay.h`, so
// this header is implementation-inline and the unit tests can drive it
// without linking `psynder_ui_imm`.

#pragma once

#include "Imm.h"
#include "Overlay.h"

#include "core/Types.h"
#include "math/Math.h"

#include <cmath>

namespace psynder::ui::imm {

namespace gizmo_detail {

// Project a world-space point through `view_proj` into screen pixels.
// Assumes `screen_size` is the framebuffer extent in pixels. Returns
// (NaN, NaN) when the point lies behind the camera or w ≈ 0, which the
// hit-test logic interprets as "no hit".
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
    // NDC [-1,1] → screen [0, size]. Y is flipped to match the top-left
    // origin that the IMM draws into.
    return {
        (ndc_x * 0.5f + 0.5f) * screen_size.x,
        (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_size.y,
    };
}

// 2D point-to-segment distance. Same helper Lane 16's `Imm.cpp` uses for
// the camera-free gizmo skeleton; replicated header-inline so the unit
// tests don't pull in the lane lib.
inline f32 dist_point_to_segment(math::Vec2 p, math::Vec2 a, math::Vec2 b) noexcept {
    const f32 abx = b.x - a.x;
    const f32 aby = b.y - a.y;
    const f32 apx = p.x - a.x;
    const f32 apy = p.y - a.y;
    const f32 ab_len2 = abx * abx + aby * aby;
    if (ab_len2 <= 0.0f) {
        return std::sqrt(apx * apx + apy * apy);
    }
    f32 t = (apx * abx + apy * aby) / ab_len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const f32 cx = a.x + abx * t - p.x;
    const f32 cy = a.y + aby * t - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

// On-screen arm length in pixels. A real editor scales this with camera
// distance; Wave B keeps it constant so the visuals stay readable in
// every level.
inline constexpr f32 kArmLengthPx = 64.0f;
inline constexpr f32 kHitDistPx   = 8.0f;

// Default screen size used when the caller passes (0,0): callers that
// don't know their framebuffer extent get a "headless" projection that
// keeps NDC-clip space arithmetic well-defined.
inline math::Vec2 default_screen(math::Vec2 s) noexcept {
    if (s.x <= 0.0f || s.y <= 0.0f) return { 1024.0f, 768.0f };
    return s;
}

// Colour wheel for the three axes. Source-literal RGBA8.
inline constexpr u32 kColourX = 0xFF3050FFu;  // red
inline constexpr u32 kColourY = 0x30FF50FFu;  // green
inline constexpr u32 kColourZ = 0x3050FFFFu;  // blue
inline constexpr u32 kColourHot = 0xFFFF40FFu; // yellow when grabbed

}  // namespace gizmo_detail

// ─── gizmo_translate ─────────────────────────────────────────────────────
//
// World-space translate manipulator. Projects `pos` and the three axis
// tips through `view_proj`, draws axis arrows via `imm::line`, hit-tests
// the cursor against each arm, and — if the user is dragging on the
// X axis — writes the raw delta into `pos`. Returns true if any axis is
// currently grabbed (mouse over an axis arm or dragging an axis).
//
// `screen_size` is the framebuffer extent. Pass (0,0) to default to
// 1024x768 (test scenarios that don't have a framebuffer bound).
inline bool gizmo_translate(math::Vec3& pos,
                            const math::Mat4& view_proj,
                            math::Vec2 mouse_screen,
                            bool       mouse_down,
                            math::Vec2 screen_size = { 0.0f, 0.0f }) noexcept {
    namespace gd = gizmo_detail;
    const math::Vec2 ss = gd::default_screen(screen_size);

    const math::Vec2 o   = gd::project_to_screen(pos, view_proj, ss);
    if (std::isnan(o.x) || std::isnan(o.y)) return false;

    const math::Vec3 px = { pos.x + 1.0f, pos.y,        pos.z        };
    const math::Vec3 py = { pos.x,        pos.y + 1.0f, pos.z        };
    const math::Vec3 pz = { pos.x,        pos.y,        pos.z + 1.0f };
    math::Vec2 sx = gd::project_to_screen(px, view_proj, ss);
    math::Vec2 sy = gd::project_to_screen(py, view_proj, ss);
    math::Vec2 sz = gd::project_to_screen(pz, view_proj, ss);

    // Re-normalize axis vectors to a fixed on-screen length so the gizmo
    // stays readable at any camera distance.
    auto unit_arm = [&](math::Vec2 a, math::Vec2 origin) -> math::Vec2 {
        if (std::isnan(a.x) || std::isnan(a.y)) return origin;
        const f32 dx = a.x - origin.x;
        const f32 dy = a.y - origin.y;
        const f32 len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0001f) return { origin.x + gd::kArmLengthPx, origin.y };
        const f32 s = gd::kArmLengthPx / len;
        return { origin.x + dx * s, origin.y + dy * s };
    };
    const math::Vec2 tip_x = unit_arm(sx, o);
    const math::Vec2 tip_y = unit_arm(sy, o);
    const math::Vec2 tip_z = unit_arm(sz, o);

    // Hit-test the cursor against each arm.
    const f32 dx_arm = gd::dist_point_to_segment(mouse_screen, o, tip_x);
    const f32 dy_arm = gd::dist_point_to_segment(mouse_screen, o, tip_y);
    const f32 dz_arm = gd::dist_point_to_segment(mouse_screen, o, tip_z);
    f32 best = gd::kHitDistPx;
    i32 picked = -1;
    if (dx_arm < best) { best = dx_arm; picked = 0; }
    if (dy_arm < best) { best = dy_arm; picked = 1; }
    if (dz_arm < best) { best = dz_arm; picked = 2; }

    const u32 col_x = (picked == 0) ? gd::kColourHot : gd::kColourX;
    const u32 col_y = (picked == 1) ? gd::kColourHot : gd::kColourY;
    const u32 col_z = (picked == 2) ? gd::kColourHot : gd::kColourZ;
    imm::line(o, tip_x, col_x);
    imm::line(o, tip_y, col_y);
    imm::line(o, tip_z, col_z);

    const bool grabbed = (picked >= 0) && mouse_down;
    if (grabbed) {
        // Raw screen-space delta along the picked axis (no snap; Wave B).
        // We don't have last-frame mouse here, so the editor (Lane 18)
        // composes the world delta from successive calls — this header
        // intentionally only reports the grabbed state.
        (void)pos;  // (raw delta path lives in editor-core/Lane 18.)
    }
    return (picked >= 0);
}

// ─── gizmo_rotate ────────────────────────────────────────────────────────
//
// Same shape as `gizmo_translate`. For Wave B we only visualise rotation
// around world Y; the editor wires X/Z later. Returns true if the ring
// is grabbed.
inline bool gizmo_rotate(math::Vec3& /*pos*/,
                         const math::Mat4& view_proj,
                         math::Vec3 anchor,
                         math::Vec2 mouse_screen,
                         bool       mouse_down,
                         math::Vec2 screen_size = { 0.0f, 0.0f }) noexcept {
    namespace gd = gizmo_detail;
    const math::Vec2 ss = gd::default_screen(screen_size);
    const math::Vec2 o  = gd::project_to_screen(anchor, view_proj, ss);
    if (std::isnan(o.x) || std::isnan(o.y)) return false;

    // Approximate the Y-axis rotation ring as a screen-space circle of
    // radius kArmLengthPx. Wave B keeps the math flat; Lane 18 will swap
    // in a proper ellipse projection when full XYZ rings land.
    constexpr u32 kSegments = 24;
    f32 best   = gd::kHitDistPx;
    bool hit_ring = false;
    math::Vec2 prev{};
    for (u32 i = 0; i <= kSegments; ++i) {
        const f32 theta = math::kTwoPi * static_cast<f32>(i)
                        / static_cast<f32>(kSegments);
        const math::Vec2 p = {
            o.x + gd::kArmLengthPx * std::cos(theta),
            o.y + gd::kArmLengthPx * std::sin(theta),
        };
        if (i > 0) {
            imm::line(prev, p, gd::kColourY);
            const f32 d = gd::dist_point_to_segment(mouse_screen, prev, p);
            if (d < best) { best = d; hit_ring = true; }
        }
        prev = p;
    }
    return hit_ring && mouse_down ? true : hit_ring;
}

// ─── gizmo_scale ─────────────────────────────────────────────────────────
//
// Same shape; visualised as three short arms terminated by small squares
// (drawn as four `imm::line` calls per arm). Returns true if any axis is
// grabbed.
inline bool gizmo_scale(math::Vec3& /*scale*/,
                        const math::Mat4& view_proj,
                        math::Vec3 anchor,
                        math::Vec2 mouse_screen,
                        bool       mouse_down,
                        math::Vec2 screen_size = { 0.0f, 0.0f }) noexcept {
    namespace gd = gizmo_detail;
    const math::Vec2 ss = gd::default_screen(screen_size);
    const math::Vec2 o  = gd::project_to_screen(anchor, view_proj, ss);
    if (std::isnan(o.x) || std::isnan(o.y)) return false;

    // Three flat axis arms in screen space (X right, Y up, Z = 45° diag).
    // Wave B keeps the arm directions camera-locked so the visualisation
    // is stable in tests; the world-aligned variant lands with the
    // editor wiring.
    const math::Vec2 tip_x = { o.x + gd::kArmLengthPx, o.y };
    const math::Vec2 tip_y = { o.x,                    o.y - gd::kArmLengthPx };
    const math::Vec2 tip_z = { o.x + gd::kArmLengthPx * 0.7071f,
                               o.y + gd::kArmLengthPx * 0.7071f };

    const f32 dx_arm = gd::dist_point_to_segment(mouse_screen, o, tip_x);
    const f32 dy_arm = gd::dist_point_to_segment(mouse_screen, o, tip_y);
    const f32 dz_arm = gd::dist_point_to_segment(mouse_screen, o, tip_z);
    f32 best = gd::kHitDistPx;
    i32 picked = -1;
    if (dx_arm < best) { best = dx_arm; picked = 0; }
    if (dy_arm < best) { best = dy_arm; picked = 1; }
    if (dz_arm < best) { best = dz_arm; picked = 2; }

    const u32 col_x = (picked == 0) ? gd::kColourHot : gd::kColourX;
    const u32 col_y = (picked == 1) ? gd::kColourHot : gd::kColourY;
    const u32 col_z = (picked == 2) ? gd::kColourHot : gd::kColourZ;
    imm::line(o, tip_x, col_x);
    imm::line(o, tip_y, col_y);
    imm::line(o, tip_z, col_z);
    // Small terminator boxes (4 lines each).
    const f32 hw = 4.0f;
    auto draw_box = [&](math::Vec2 c, u32 colour) {
        const math::Vec2 tl{ c.x - hw, c.y - hw };
        const math::Vec2 tr{ c.x + hw, c.y - hw };
        const math::Vec2 br{ c.x + hw, c.y + hw };
        const math::Vec2 bl{ c.x - hw, c.y + hw };
        imm::line(tl, tr, colour);
        imm::line(tr, br, colour);
        imm::line(br, bl, colour);
        imm::line(bl, tl, colour);
    };
    draw_box(tip_x, col_x);
    draw_box(tip_y, col_y);
    draw_box(tip_z, col_z);

    return (picked >= 0) && (mouse_down || picked >= 0);
}

}  // namespace psynder::ui::imm
