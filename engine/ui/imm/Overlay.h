// SPDX-License-Identifier: MIT
// Psynder — Lane 16 (immediate-mode UI). EDITOR / DEBUG OVERLAY EXTRAS.
//
// The frozen public surface in `Imm.h` covers the minimal widget kit. Lane
// 16 also exposes a small additional surface for in-viewport overlays
// that the editor (Lane 18) and the debug HUD will consume:
//
//   • `graph()`             — scrolling line plot for per-frame metrics.
//   • `selection_highlight` — animated rect outline that pulses around a
//                             selected entity's screen-space bounds.
//   • `brush_preview`       — translucent disk + outline used by the
//                             heightmap sculpt + brush CSG tools.
//   • `gizmo_translate`     — 3-axis arrow manipulator (XYZ).
//   • `gizmo_rotate`        — 3-axis ring manipulator (XYZ).
//
// These are *not* in `Imm.h` because that header was frozen before the
// gizmo + perf-graph deliverables were carved off; they live here, in a
// new header inside the lane-owned directory, so other lanes can include
// `<ui/imm/Overlay.h>` without touching the frozen contract.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

#include <span>
#include <string_view>

namespace psynder::ui::imm {

// ─── Perf graph ───────────────────────────────────────────────────────────
//
// Pushes `sample_ms` onto the rolling history and renders the last
// `kPerfGraphSamples` entries inside `rect`. `max_ms` controls the
// vertical scale; pass 0 to autoscale to the current window's peak.
void graph(math::Vec2 origin,
           math::Vec2 size,
           f32        sample_ms,
           f32        max_ms = 0.0f,
           std::string_view caption = {});

// One-shot version: render a caller-owned series rather than the global
// rolling buffer. Useful for plotting CPU vs GPU vs island-physics times
// side by side.
void graph_series(math::Vec2 origin,
                  math::Vec2 size,
                  std::span<const f32> samples,
                  f32 max_value = 0.0f,
                  std::string_view caption = {});

// ─── Selection highlight ──────────────────────────────────────────────────
//
// Draws a 2-pixel dashed outline around a screen-space rect. The dash
// phase is driven by a free-running frame counter so the highlight
// "marches" — same UX convention every Quake/UE-derived editor ships.
void selection_highlight(math::Vec2 origin, math::Vec2 size);

// ─── Brush preview ────────────────────────────────────────────────────────
//
// Used by Lane 18's heightmap sculpt + indoor brush CSG tools. Renders a
// translucent disk centred at `centre` with `radius` pixels of solid fill
// blended onto the framebuffer, plus a hard outline. `falloff_radius`
// draws a second outer outline for falloff visualization (0 → skip).
void brush_preview(math::Vec2 centre, f32 radius, f32 falloff_radius = 0.0f);

// ─── 3D manipulator gizmos ────────────────────────────────────────────────
//
// Wave-A ships these as skeletons: world-space axis vectors are projected
// onto the framebuffer by the caller (the editor knows the camera; Lane
// 16 stays camera-free). Lane 18 wires the actual entity-transform write
// path through these projections. The `picked_axis` output reports which
// axis (if any) the cursor is currently over: 0=X, 1=Y, 2=Z, -1=none.
enum class GizmoAxis : i8 { None = -1, X = 0, Y = 1, Z = 2 };

struct GizmoProjection {
    math::Vec2 origin{};
    math::Vec2 axis_x{};
    math::Vec2 axis_y{};
    math::Vec2 axis_z{};
    // Logical length of each axis arm in pixels (post-projection). The
    // editor sets this so the gizmo stays a constant on-screen size.
    f32        arm_length = 64.0f;
};

GizmoAxis gizmo_translate(const GizmoProjection& proj);
GizmoAxis gizmo_rotate   (const GizmoProjection& proj);

// ─── Input plumbing ───────────────────────────────────────────────────────
//
// The frozen `button()` doesn't take a mouse argument; the host (Lane 18
// editor, or whoever owns the platform window) feeds pointer state into
// the IMM via this entry point exactly once per frame between
// `begin_frame()` and the first widget call.
void set_input(math::Vec2 mouse_pos, bool mouse_down);

}  // namespace psynder::ui::imm
