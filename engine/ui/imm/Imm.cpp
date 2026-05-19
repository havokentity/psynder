// SPDX-License-Identifier: MIT
// Psynder — immediate-mode UI implementation. Lane 16.
//
// Backs the frozen `Imm.h` widget kit + the additional `Overlay.h` perf
// graph / gizmo / selection / brush surface. Stays under a few hundred
// LoC per DESIGN.md §10.6 — anything heavier belongs in RmlUi (Lane 17)
// or the React editor panels (Lane 20).

#include "Imm.h"
#include "Overlay.h"

#include "detail/Context.h"
#include "detail/Draw.h"
#include "detail/Font.h"
#include "detail/Pixel.h"
#include "detail/Widgets.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <string_view>

namespace psynder::ui::imm {

namespace d = ::psynder::ui::imm::detail;

// ─── Frame lifecycle ──────────────────────────────────────────────────────
void begin_frame(render::Framebuffer& target) {
    auto& ctx = d::context();
    ctx.target     = &target;
    ctx.hot_id     = 0;
    ctx.frame_open = true;
}

void end_frame() {
    auto& ctx = d::context();
    ctx.input.mouse_down_prev = ctx.input.mouse_down;
    ctx.target     = nullptr;
    ctx.frame_open = false;
}

void set_input(math::Vec2 mouse_pos, bool mouse_down) {
    auto& ctx = d::context();
    ctx.input.mouse      = mouse_pos;
    ctx.input.mouse_down = mouse_down;
}

// ─── Drawing primitives ───────────────────────────────────────────────────
void filled_rect(math::Vec2 origin, math::Vec2 size, u32 rgba) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    d::filled_rect(*ctx.target, origin, size, rgba);
}

void rect_outline(math::Vec2 origin, math::Vec2 size, u32 rgba) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    d::rect_outline(*ctx.target, origin, size, rgba);
}

void line(math::Vec2 a, math::Vec2 b, u32 rgba) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    d::line(*ctx.target, a, b, rgba);
}

void label(math::Vec2 position, std::string_view text, u32 rgba) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    d::draw_text(*ctx.target,
                 static_cast<i32>(std::lround(position.x)),
                 static_cast<i32>(std::lround(position.y)),
                 text,
                 rgba);
}

// ─── Button ───────────────────────────────────────────────────────────────
bool button(math::Vec2 position, math::Vec2 size, std::string_view text) {
    auto& ctx = d::context();
    const u64  id        = d::widget_id(position, size, text.data(), text.size());
    const bool triggered = d::button_logic(ctx, position, size, text);
    if (ctx.target) {
        d::draw_button(*ctx.target, position, size, text,
                       d::button_fill_colour(ctx, id));
    }
    return triggered;
}

// ─── Perf graph ───────────────────────────────────────────────────────────
namespace {

void render_graph_box(render::Framebuffer& fb,
                      math::Vec2 origin,
                      math::Vec2 size,
                      std::string_view caption) {
    d::filled_rect(fb, origin, size, d::theme().graph_bg);
    d::rect_outline(fb, origin, size, d::theme().graph_axis);
    if (!caption.empty()) {
        d::draw_text(fb,
                     static_cast<i32>(origin.x) + 2,
                     static_cast<i32>(origin.y) + 2,
                     caption,
                     d::theme().label_text);
    }
}

void plot_polyline(render::Framebuffer& fb,
                   math::Vec2 origin,
                   math::Vec2 size,
                   std::span<const f32> samples,
                   f32 max_value) {
    if (samples.empty() || size.x <= 1.0f || size.y <= 1.0f) return;
    f32 vmax = max_value;
    if (vmax <= 0.0f) {
        for (f32 s : samples) vmax = std::max(vmax, s);
    }
    if (vmax <= 0.0f) vmax = 1.0f;

    const f32 inner_h = size.y - 2.0f;
    const f32 inner_w = size.x - 2.0f;
    const u32 n       = static_cast<u32>(samples.size());
    if (n < 2U) return;

    const f32 step = inner_w / static_cast<f32>(n - 1U);
    auto to_screen = [&](u32 i) -> math::Vec2 {
        const f32 v   = std::clamp(samples[i], 0.0f, vmax);
        const f32 nrm = v / vmax;
        return { origin.x + 1.0f + step * static_cast<f32>(i),
                 origin.y + 1.0f + (1.0f - nrm) * inner_h };
    };

    math::Vec2 prev = to_screen(0);
    for (u32 i = 1; i < n; ++i) {
        const math::Vec2 cur = to_screen(i);
        d::line(fb, prev, cur, d::theme().graph_line);
        prev = cur;
    }
}

}  // namespace

void graph(math::Vec2 origin,
           math::Vec2 size,
           f32        sample_ms,
           f32        max_ms,
           std::string_view caption) {
    auto& ctx = d::context();
    if (!ctx.target) return;

    // Push the new sample onto the ring.
    ctx.perf_samples[ctx.perf_head] = sample_ms;
    ctx.perf_head = (ctx.perf_head + 1U) % d::kPerfGraphSamples;
    if (ctx.perf_count < d::kPerfGraphSamples) ++ctx.perf_count;

    // Linearise the ring into a temporary span for plotting. We can't
    // dynamically allocate per frame (DESIGN.md §3.4), so we copy onto a
    // stack buffer sized for the maximum sample count.
    std::array<f32, d::kPerfGraphSamples> linear{};
    const u32 count = ctx.perf_count;
    const u32 start = (ctx.perf_head + d::kPerfGraphSamples - count) % d::kPerfGraphSamples;
    for (u32 i = 0; i < count; ++i) {
        linear[i] = ctx.perf_samples[(start + i) % d::kPerfGraphSamples];
    }
    render_graph_box(*ctx.target, origin, size, caption);
    plot_polyline(*ctx.target,
                  origin,
                  size,
                  std::span<const f32>{linear.data(), count},
                  max_ms);
}

void graph_series(math::Vec2 origin,
                  math::Vec2 size,
                  std::span<const f32> samples,
                  f32 max_value,
                  std::string_view caption) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    render_graph_box(*ctx.target, origin, size, caption);
    plot_polyline(*ctx.target, origin, size, samples, max_value);
}

// ─── Selection highlight ──────────────────────────────────────────────────
namespace {
u32 g_frame_tick = 0;  // Drives the marching-dash phase.
}

void selection_highlight(math::Vec2 origin, math::Vec2 size) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    ++g_frame_tick;
    const u32 phase = g_frame_tick / 4U;     // ~15 FPS dash speed at 60 FPS.
    const u32 colour = d::theme().selection;

    auto plot_dashed_h = [&](i32 x0, i32 x1, i32 y) {
        const i32 fw = static_cast<i32>(ctx.target->width);
        for (i32 x = x0; x <= x1; ++x) {
            if (((static_cast<u32>(x) + phase) >> 2U) & 1U) {
                d::plot(*ctx.target, x, y, colour);
                if (y + 1 < static_cast<i32>(ctx.target->height)) {
                    d::plot(*ctx.target, x, y + 1, colour);
                }
            }
            (void)fw;
        }
    };
    auto plot_dashed_v = [&](i32 x, i32 y0, i32 y1) {
        for (i32 y = y0; y <= y1; ++y) {
            if (((static_cast<u32>(y) + phase) >> 2U) & 1U) {
                d::plot(*ctx.target, x, y, colour);
                if (x + 1 < static_cast<i32>(ctx.target->width)) {
                    d::plot(*ctx.target, x + 1, y, colour);
                }
            }
        }
    };
    const i32 x0 = static_cast<i32>(std::floor(origin.x));
    const i32 y0 = static_cast<i32>(std::floor(origin.y));
    const i32 x1 = static_cast<i32>(std::floor(origin.x + size.x)) - 1;
    const i32 y1 = static_cast<i32>(std::floor(origin.y + size.y)) - 1;
    if (x1 < x0 || y1 < y0) return;
    plot_dashed_h(x0, x1, y0);
    plot_dashed_h(x0, x1, y1);
    plot_dashed_v(x0, y0, y1);
    plot_dashed_v(x1, y0, y1);
}

// ─── Brush preview ────────────────────────────────────────────────────────
void brush_preview(math::Vec2 centre, f32 radius, f32 falloff_radius) {
    auto& ctx = d::context();
    if (!ctx.target) return;
    d::blended_disk(*ctx.target, centre, radius, d::theme().brush_fill);
    d::circle(*ctx.target, centre, radius, d::theme().brush_outline, false);
    if (falloff_radius > radius) {
        d::circle(*ctx.target, centre, falloff_radius,
                  d::theme().brush_outline, false);
    }
}

// ─── Gizmos ───────────────────────────────────────────────────────────────
namespace {

f32 dist_point_to_segment_2d(math::Vec2 p, math::Vec2 a, math::Vec2 b) noexcept {
    const f32 abx = b.x - a.x;
    const f32 aby = b.y - a.y;
    const f32 apx = p.x - a.x;
    const f32 apy = p.y - a.y;
    const f32 ab_len2 = abx * abx + aby * aby;
    if (ab_len2 <= 0.0f) {
        return std::sqrt(apx * apx + apy * apy);
    }
    f32 t = (apx * abx + apy * aby) / ab_len2;
    t = std::clamp(t, 0.0f, 1.0f);
    const f32 cx = a.x + abx * t - p.x;
    const f32 cy = a.y + aby * t - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

math::Vec2 scale_v2(math::Vec2 v, f32 s) noexcept { return { v.x * s, v.y * s }; }
math::Vec2 add_v2  (math::Vec2 a, math::Vec2 b) noexcept { return { a.x + b.x, a.y + b.y }; }

// Render an arrowhead at `tip` pointing along `dir` (unit-length-ish).
void draw_arrow(render::Framebuffer& fb,
                math::Vec2 base,
                math::Vec2 tip,
                u32 colour) {
    d::line(fb, base, tip, colour);
    const f32 dx = tip.x - base.x;
    const f32 dy = tip.y - base.y;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.001f) return;
    const f32 inv = 1.0f / len;
    const f32 ux = dx * inv;
    const f32 uy = dy * inv;
    const f32 head = 8.0f;
    // Two arrowhead lines, 25° each side.
    constexpr f32 kCos = 0.906308f;  // cos(25°)
    constexpr f32 kSin = 0.422618f;  // sin(25°)
    const math::Vec2 back{ tip.x - ux * head, tip.y - uy * head };
    const math::Vec2 left {
        back.x + (-ux * kCos + uy * kSin) * (head * 0.5f),
        back.y + (-uy * kCos - ux * kSin) * (head * 0.5f),
    };
    const math::Vec2 right{
        back.x + (-ux * kCos - uy * kSin) * (head * 0.5f),
        back.y + (-uy * kCos + ux * kSin) * (head * 0.5f),
    };
    d::line(fb, tip, left,  colour);
    d::line(fb, tip, right, colour);
}

}  // namespace

GizmoAxis gizmo_translate(const GizmoProjection& proj) {
    auto& ctx = d::context();
    if (!ctx.target) return GizmoAxis::None;
    const f32 L = proj.arm_length;
    const math::Vec2 ex = add_v2(proj.origin, scale_v2(proj.axis_x, L));
    const math::Vec2 ey = add_v2(proj.origin, scale_v2(proj.axis_y, L));
    const math::Vec2 ez = add_v2(proj.origin, scale_v2(proj.axis_z, L));

    constexpr f32 kHitDist = 6.0f;
    GizmoAxis picked = GizmoAxis::None;
    f32 best = kHitDist;
    auto try_pick = [&](GizmoAxis axis, math::Vec2 tip) {
        const f32 d = dist_point_to_segment_2d(ctx.input.mouse, proj.origin, tip);
        if (d < best) { best = d; picked = axis; }
    };
    try_pick(GizmoAxis::X, ex);
    try_pick(GizmoAxis::Y, ey);
    try_pick(GizmoAxis::Z, ez);

    auto axis_colour = [&](GizmoAxis ax, u32 base) -> u32 {
        return (picked == ax) ? d::theme().button_active : base;
    };
    draw_arrow(*ctx.target, proj.origin, ex,
               axis_colour(GizmoAxis::X, d::theme().gizmo_x));
    draw_arrow(*ctx.target, proj.origin, ey,
               axis_colour(GizmoAxis::Y, d::theme().gizmo_y));
    draw_arrow(*ctx.target, proj.origin, ez,
               axis_colour(GizmoAxis::Z, d::theme().gizmo_z));
    return picked;
}

GizmoAxis gizmo_rotate(const GizmoProjection& proj) {
    auto& ctx = d::context();
    if (!ctx.target) return GizmoAxis::None;
    const f32 L = proj.arm_length;

    // Three rings, one per axis. The ring axis is the projected axis
    // vector; we draw a flat circle in the plane perpendicular to it,
    // approximated as an ellipse via the two remaining axes.
    struct Ring { GizmoAxis ax; math::Vec2 u; math::Vec2 v; u32 colour; };
    const Ring rings[3] = {
        { GizmoAxis::X, proj.axis_y, proj.axis_z, d::theme().gizmo_x },
        { GizmoAxis::Y, proj.axis_z, proj.axis_x, d::theme().gizmo_y },
        { GizmoAxis::Z, proj.axis_x, proj.axis_y, d::theme().gizmo_z },
    };

    constexpr u32 kSegments = 24;
    constexpr f32 kHitDist  = 5.0f;
    GizmoAxis picked = GizmoAxis::None;
    f32 best = kHitDist;

    for (const Ring& r : rings) {
        math::Vec2 prev{};
        for (u32 i = 0; i <= kSegments; ++i) {
            const f32 theta = math::kTwoPi * static_cast<f32>(i)
                            / static_cast<f32>(kSegments);
            const f32 c = std::cos(theta);
            const f32 s = std::sin(theta);
            const math::Vec2 p = {
                proj.origin.x + L * (r.u.x * c + r.v.x * s),
                proj.origin.y + L * (r.u.y * c + r.v.y * s),
            };
            if (i > 0) {
                d::line(*ctx.target, prev, p, r.colour);
                const f32 d = dist_point_to_segment_2d(ctx.input.mouse, prev, p);
                if (d < best) { best = d; picked = r.ax; }
            }
            prev = p;
        }
    }

    if (picked != GizmoAxis::None) {
        // Highlight by overdrawing the picked ring in active colour.
        const Ring& r = rings[static_cast<u32>(picked)];
        math::Vec2 prev{};
        for (u32 i = 0; i <= kSegments; ++i) {
            const f32 theta = math::kTwoPi * static_cast<f32>(i)
                            / static_cast<f32>(kSegments);
            const f32 c = std::cos(theta);
            const f32 s = std::sin(theta);
            const math::Vec2 p = {
                proj.origin.x + L * (r.u.x * c + r.v.x * s),
                proj.origin.y + L * (r.u.y * c + r.v.y * s),
            };
            if (i > 0) {
                d::line(*ctx.target, prev, p, d::theme().button_active);
            }
            prev = p;
        }
    }
    return picked;
}

}  // namespace psynder::ui::imm
