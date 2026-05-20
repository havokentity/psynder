// SPDX-License-Identifier: MIT
// Psynder — internal header. Constraint-graph debug visualization.
//
// Renders each Wave-B constraint kind (weld / axis / slider / ball-socket
// / rope / elastic) as a set of 2D line segments using lane 16's
// `ui::imm::line` primitive. Lane 18 owns the projection (camera math is
// in `math::look_at_rh` + `math::perspective_rh`); lane 16 stays
// camera-free.
//
// Header-only so unit tests can drive the projection helpers without
// linking against psynder_editor_core (the line emit goes to a caller-
// supplied sink in those tests).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include "editor/core/Constraints.h"

#include <cmath>
#include <vector>

namespace psynder::editor::viz {

// ─── Per-kind palette (rgba) ─────────────────────────────────────────────
// Matches the swatch designers picked in the Wave-B spec: weld=red,
// axis=cyan, slider=yellow, ball=magenta, rope=tan, elastic=lime.
PSY_FORCEINLINE u32 color_for_kind(constraints::Kind k) noexcept {
    switch (k) {
        case constraints::Kind::Weld:
            return 0xE03333FFu;
        case constraints::Kind::Axis:
            return 0x33CCEEFFu;
        case constraints::Kind::Slider:
            return 0xEED633FFu;
        case constraints::Kind::BallSocket:
            return 0xCC55CCFFu;
        case constraints::Kind::Rope:
            return 0xBBAA77FFu;
        case constraints::Kind::Elastic:
            return 0x77EE55FFu;
    }
    return 0xFFFFFFFFu;
}

// ─── Camera ──────────────────────────────────────────────────────────────
// Editor-side camera params. The renderer (lane 07) owns the final
// view/projection; the editor synthesises its own matrices for the in-
// viewport overlay so the lane can stay independent (and so unit tests can
// drive deterministic projections).
struct Camera {
    math::Vec3 eye{0.0f, 4.0f, 8.0f};
    math::Vec3 target{0.0f, 0.0f, 0.0f};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    f32 fov_y_rad = 1.0472f;  // 60°
    f32 aspect = 16.0f / 9.0f;
    f32 near_z = 0.1f;
    f32 far_z = 200.0f;
    f32 viewport_w = 1280.0f;
    f32 viewport_h = 720.0f;
};

struct Projected {
    math::Vec2 screen{0.0f, 0.0f};
    f32 clip_w = 0.0f;
    bool visible = false;
};

// World → clip → NDC → screen pixels. Returns visible=false if the point is
// behind the eye (w <= 0) or outside the NDC near/far range.
inline Projected project_point(const Camera& cam, math::Vec3 p) noexcept {
    const math::Mat4 view = math::look_at_rh(cam.eye, cam.target, cam.up);
    const math::Mat4 proj = math::perspective_rh(cam.fov_y_rad, cam.aspect, cam.near_z, cam.far_z);
    const math::Mat4 mvp = math::mul(proj, view);

    const math::Vec4 clip = math::mul(mvp, math::Vec4{p.x, p.y, p.z, 1.0f});
    Projected out;
    out.clip_w = clip.w;
    if (clip.w <= 0.0f)
        return out;
    const f32 inv_w = 1.0f / clip.w;
    const f32 ndc_x = clip.x * inv_w;
    const f32 ndc_y = clip.y * inv_w;
    const f32 ndc_z = clip.z * inv_w;
    if (ndc_z < -1.0f || ndc_z > 1.0f)
        return out;
    // NDC (-1,1) -> screen (0,w) / (0,h) with Y flipped (image-space origin
    // at top-left).
    out.screen.x = (ndc_x * 0.5f + 0.5f) * cam.viewport_w;
    out.screen.y = (1.0f - (ndc_y * 0.5f + 0.5f)) * cam.viewport_h;
    out.visible = true;
    return out;
}

// Same camera, but pre-multiplies a model-space anchor by its body's pose.
// `anchor_local` is in body local space (relative to body_pos + body_rot).
inline Projected project_anchor(const Camera& cam,
                                math::Vec3 body_pos,
                                math::Quat body_rot,
                                math::Vec3 anchor_local) noexcept {
    const math::Mat4 m = math::translate(body_pos);
    const math::Mat4 r = math::rotate_quat(body_rot);
    const math::Mat4 mr = math::mul(m, r);
    const math::Vec4 w =
        math::mul(mr, math::Vec4{anchor_local.x, anchor_local.y, anchor_local.z, 1.0f});
    return project_point(cam, math::Vec3{w.x, w.y, w.z});
}

// ─── Line-segment buffer ─────────────────────────────────────────────────
// Lane 16's `ui::imm::line` consumes one segment at a time. We build a
// caller-owned buffer so the unit tests can introspect what got emitted
// without booting the imm-mode framebuffer.
struct ScreenSegment {
    math::Vec2 a{0, 0};
    math::Vec2 b{0, 0};
    u32 rgba = 0xFFFFFFFFu;
};

struct WorldBodyPose {
    u32 id{0};
    math::Vec3 position{0, 0, 0};
    math::Quat rotation{0, 0, 0, 1};
};

// Look up the world transform for a body id. Returns identity if not
// present — keeps the visualization robust to dangling constraint refs.
PSY_FORCEINLINE WorldBodyPose find_pose(const std::vector<WorldBodyPose>& bodies, u32 id) noexcept {
    for (const auto& p : bodies) {
        if (p.id == id)
            return p;
    }
    WorldBodyPose def;
    def.id = id;
    return def;
}

// Emit a screen-space line; clips to the viewport on miss / behind-camera.
PSY_FORCEINLINE void push_line_if_visible(std::vector<ScreenSegment>& out,
                                          const Projected& a,
                                          const Projected& b,
                                          u32 rgba) {
    if (!a.visible || !b.visible)
        return;
    ScreenSegment s;
    s.a = a.screen;
    s.b = b.screen;
    s.rgba = rgba;
    out.push_back(s);
}

// Build line segments for one constraint. Each kind has its own glyph:
//
//   Weld       : single segment a → b, double-width (emitted twice with
//                a one-pixel y offset on the second line)
//   Axis       : segment a → b, plus an axis tick (a + axis*0.5)
//   Slider     : segment a → b, plus min/max marker brackets at the ends
//   BallSocket : segment a → b, plus a small cross at anchor a
//   Rope       : segment a → b only (rope-length viz)
//   Elastic    : segment a → b in a dashed style (4 alternating dashes)
inline void build_constraint_lines(std::vector<ScreenSegment>& out,
                                   const Camera& cam,
                                   const constraints::Constraint& c,
                                   const std::vector<WorldBodyPose>& bodies) {
    const WorldBodyPose pa = find_pose(bodies, c.body_a);
    const WorldBodyPose pb = find_pose(bodies, c.body_b);

    const Projected A = project_anchor(cam, pa.position, pa.rotation, c.anchor_a);
    const Projected B = project_anchor(cam, pb.position, pb.rotation, c.anchor_b);

    const u32 rgba = color_for_kind(c.kind);
    push_line_if_visible(out, A, B, rgba);

    switch (c.kind) {
        case constraints::Kind::Weld: {
            // A second segment, y-offset by 1 px, gives a clear "thick"
            // weld glyph at any resolution.
            if (A.visible && B.visible) {
                ScreenSegment s;
                s.a = {A.screen.x, A.screen.y + 1.0f};
                s.b = {B.screen.x, B.screen.y + 1.0f};
                s.rgba = rgba;
                out.push_back(s);
            }
            break;
        }
        case constraints::Kind::Axis: {
            // Tick: from anchor A, half a metre along the axis (world frame).
            const math::Vec3 ax = math::normalize(c.axis);
            const math::Vec3 tip_w =
                math::add(math::add(pa.position, c.anchor_a), math::mul(ax, 0.5f));
            const Projected T = project_point(cam, tip_w);
            push_line_if_visible(out, A, T, rgba);
            break;
        }
        case constraints::Kind::Slider: {
            // Two short tick-marks at anchor a / b perpendicular to the axis,
            // showing slider min/max travel.
            const math::Vec3 ax = math::normalize(c.axis);
            // Pick an arbitrary perpendicular (cross with world-up unless
            // the axis IS up, then cross with X).
            math::Vec3 perp = std::fabs(ax.y) > 0.99f ? math::cross(ax, math::Vec3{1, 0, 0})
                                                      : math::cross(ax, math::Vec3{0, 1, 0});
            perp = math::normalize(perp);
            const math::Vec3 a_w = math::add(pa.position, c.anchor_a);
            const math::Vec3 b_w = math::add(pb.position, c.anchor_b);
            const f32 tick = 0.15f;
            const Projected Ap = project_point(cam, math::add(a_w, math::mul(perp, tick)));
            const Projected An = project_point(cam, math::add(a_w, math::mul(perp, -tick)));
            const Projected Bp = project_point(cam, math::add(b_w, math::mul(perp, tick)));
            const Projected Bn = project_point(cam, math::add(b_w, math::mul(perp, -tick)));
            push_line_if_visible(out, Ap, An, rgba);
            push_line_if_visible(out, Bp, Bn, rgba);
            break;
        }
        case constraints::Kind::BallSocket: {
            // Small X at anchor a.
            const f32 r = 4.0f;  // pixels
            if (A.visible) {
                ScreenSegment x1;
                x1.a = {A.screen.x - r, A.screen.y - r};
                x1.b = {A.screen.x + r, A.screen.y + r};
                x1.rgba = rgba;
                out.push_back(x1);
                ScreenSegment x2;
                x2.a = {A.screen.x - r, A.screen.y + r};
                x2.b = {A.screen.x + r, A.screen.y - r};
                x2.rgba = rgba;
                out.push_back(x2);
            }
            break;
        }
        case constraints::Kind::Rope:
            // Just the spine segment (already emitted above).
            break;
        case constraints::Kind::Elastic: {
            // Replace the spine with a 4-segment dashed line by emitting
            // additional staggered segments at 25/50/75% along the line.
            if (A.visible && B.visible) {
                auto mix = [](math::Vec2 p, math::Vec2 q, f32 t) {
                    return math::Vec2{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t};
                };
                const math::Vec2 p25 = mix(A.screen, B.screen, 0.25f);
                const math::Vec2 p75 = mix(A.screen, B.screen, 0.75f);
                ScreenSegment d;
                d.a = p25;
                d.b = p75;
                d.rgba = rgba;
                out.push_back(d);
            }
            break;
        }
    }
}

// Build segments for an entire graph; pass-through to per-kind builder.
inline void build_graph_lines(std::vector<ScreenSegment>& out,
                              const Camera& cam,
                              const constraints::Graph& g,
                              const std::vector<WorldBodyPose>& bodies) {
    for (usize i = 0; i < g.size(); ++i) {
        build_constraint_lines(out, cam, g.at(i), bodies);
    }
}

}  // namespace psynder::editor::viz
