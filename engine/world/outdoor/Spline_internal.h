// SPDX-License-Identifier: MIT
// Psynder — internal cubic Bezier spline track helpers (DESIGN.md §9.2).
//
// A racing track is a sequence of cubic Bezier road segments. Each segment
// has 4 control points (p0..p3), a half-width, and a banking (roll about
// the tangent). We extrude a textured strip along the segment by sampling
// N points along the curve and laying down a quad per pair of samples.
//
// The strip is marked as a drivable physics surface (DESIGN.md §9.2 calls
// out the contact label for lane 13). Wave-A flags it with DrawItem::flags
// bit 0; the physics lane consumes the same triangle data to build its
// triangle-mesh collider.

#pragma once

#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/raster/Raster.h"

#include <cmath>
#include <vector>

namespace psynder::world::outdoor::detail {

// DrawItem::flags bit assignments for terrain-emitted strips. The renderer
// (lane 07) ignores everything except bit 7 (skybox); we use bit 0 for
// "drivable" so the physics lane can filter on it without re-parsing the
// vertex stream.
inline constexpr u8 kDrawItemFlagDrivable = 0x01;

// Evaluate a cubic Bezier at parameter t∈[0,1]. Standard de-Casteljau.
PSY_FORCEINLINE math::Vec3 bezier_eval(const SplineRoadSegment& seg, f32 t) noexcept {
    const f32 u  = 1.0f - t;
    const f32 b0 = u * u * u;
    const f32 b1 = 3.0f * u * u * t;
    const f32 b2 = 3.0f * u * t * t;
    const f32 b3 = t * t * t;
    return math::Vec3{
        b0 * seg.p0.x + b1 * seg.p1.x + b2 * seg.p2.x + b3 * seg.p3.x,
        b0 * seg.p0.y + b1 * seg.p1.y + b2 * seg.p2.y + b3 * seg.p3.y,
        b0 * seg.p0.z + b1 * seg.p1.z + b2 * seg.p2.z + b3 * seg.p3.z,
    };
}

// Derivative wrt t (3 * Bezier' produces an unnormalized tangent).
PSY_FORCEINLINE math::Vec3 bezier_tangent(const SplineRoadSegment& seg, f32 t) noexcept {
    const f32 u   = 1.0f - t;
    // dB/dt = 3 [(p1-p0)(1-t)^2 + 2(p2-p1)(1-t)t + (p3-p2)t^2]
    const f32 a = 3.0f * u * u;
    const f32 b = 6.0f * u * t;
    const f32 c = 3.0f * t * t;
    return math::Vec3{
        a * (seg.p1.x - seg.p0.x) + b * (seg.p2.x - seg.p1.x) + c * (seg.p3.x - seg.p2.x),
        a * (seg.p1.y - seg.p0.y) + b * (seg.p2.y - seg.p1.y) + c * (seg.p3.y - seg.p2.y),
        a * (seg.p1.z - seg.p0.z) + b * (seg.p2.z - seg.p1.z) + c * (seg.p3.z - seg.p2.z),
    };
}

// Bezier arc length estimate via Simpson's rule (N samples). We don't need
// a perfect length for the renderer (a few percent is fine — affects UV
// rate only), but the physics lane will want a tighter estimate later.
inline f32 bezier_arc_length(const SplineRoadSegment& seg, u32 samples = 16) noexcept {
    if (samples < 4) samples = 4;
    if ((samples & 1u) == 1u) ++samples;          // need even count for Simpson
    f32       total = 0.0f;
    const f32 inv   = 1.0f / static_cast<f32>(samples);
    f32       prev_speed = math::length(bezier_tangent(seg, 0.0f));
    for (u32 i = 1; i <= samples; ++i) {
        const f32 t   = static_cast<f32>(i) * inv;
        const f32 spd = math::length(bezier_tangent(seg, t));
        // Trapezoidal — good enough for Wave A. The Simpson rule is the
        // same shape and another half-line; we can lift later.
        total += 0.5f * (prev_speed + spd) * inv;
        prev_speed = spd;
    }
    return total;
}

// Frenet-ish frame: pick a "right" vector perpendicular to the tangent in
// the XZ plane, then rotate it about the tangent by `banking_rad` to roll
// the road. Up is +Y (DESIGN convention).
PSY_FORCEINLINE void frame_at(const SplineRoadSegment& seg,
                              f32                      t,
                              math::Vec3&              right_out,
                              math::Vec3&              up_out) noexcept {
    math::Vec3 tan = math::normalize(bezier_tangent(seg, t));
    // Project the tangent into XZ, build a right vector by 90° rotation.
    math::Vec3 right{ tan.z, 0.0f, -tan.x };
    right = math::normalize(right);
    math::Vec3 up{ 0.0f, 1.0f, 0.0f };
    // Apply banking: rotate (right, up) about the tangent by `banking_rad`.
    const f32 c = std::cos(seg.banking_rad);
    const f32 s = std::sin(seg.banking_rad);
    math::Vec3 new_right{
         c * right.x + s * up.x,
         c * right.y + s * up.y,
         c * right.z + s * up.z,
    };
    math::Vec3 new_up{
        -s * right.x + c * up.x,
        -s * right.y + c * up.y,
        -s * right.z + c * up.z,
    };
    right_out = new_right;
    up_out    = new_up;
}

// One DrawItem-worth of vertices + indices for an extruded strip. The
// caller owns the vectors; we append to them.
struct ExtrudedStrip {
    std::vector<render::raster::Vertex> vertices;
    std::vector<u32>                    indices;
    u8                                  flags = kDrawItemFlagDrivable;
};

// Extrude a single Bezier segment into a textured strip. `samples` is the
// number of along-curve subdivisions; each subdivision adds 2 verts (left
// + right) and 2 triangles. UVs run [0,1] across width and [0, length/uv_repeat]
// along length so the road texture tiles naturally.
inline ExtrudedStrip extrude_segment(const SplineRoadSegment& seg,
                                     u32                      samples = 16,
                                     f32                      uv_repeat_metres = 8.0f) {
    ExtrudedStrip strip;
    if (samples < 2) samples = 2;
    strip.vertices.reserve(static_cast<usize>(samples) * 2u);
    strip.indices.reserve(static_cast<usize>(samples - 1u) * 6u);

    const f32 arc = bezier_arc_length(seg, samples * 2u);
    const f32 v_scale = uv_repeat_metres > 0.0f ? (arc / uv_repeat_metres) : 1.0f;

    for (u32 i = 0; i < samples; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(samples - 1u);
        const math::Vec3 c = bezier_eval(seg, t);
        math::Vec3 r{}, u{};
        frame_at(seg, t, r, u);
        const math::Vec3 left {
            c.x - r.x * seg.half_width,
            c.y - r.y * seg.half_width,
            c.z - r.z * seg.half_width,
        };
        const math::Vec3 right {
            c.x + r.x * seg.half_width,
            c.y + r.y * seg.half_width,
            c.z + r.z * seg.half_width,
        };

        render::raster::Vertex vL{};
        vL.position    = left;
        vL.normal      = u;
        vL.uv          = math::Vec2{ 0.0f, t * v_scale };
        vL.lightmap_uv = vL.uv;
        vL.color       = 0xFFFFFFFFu;

        render::raster::Vertex vR{};
        vR.position    = right;
        vR.normal      = u;
        vR.uv          = math::Vec2{ 1.0f, t * v_scale };
        vR.lightmap_uv = vR.uv;
        vR.color       = 0xFFFFFFFFu;

        strip.vertices.push_back(vL);
        strip.vertices.push_back(vR);
    }

    // Index two triangles per (i, i+1) pair: (L_i, R_i, L_{i+1}) and
    // (L_{i+1}, R_i, R_{i+1}). CCW from above given the right-vector flip.
    for (u32 i = 0; i + 1 < samples; ++i) {
        const u32 L0 = i * 2u;
        const u32 R0 = L0 + 1u;
        const u32 L1 = L0 + 2u;
        const u32 R1 = L0 + 3u;
        strip.indices.push_back(L0);
        strip.indices.push_back(R0);
        strip.indices.push_back(L1);
        strip.indices.push_back(L1);
        strip.indices.push_back(R0);
        strip.indices.push_back(R1);
    }

    strip.flags = kDrawItemFlagDrivable;
    return strip;
}

}  // namespace psynder::world::outdoor::detail
