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

#include "world/outdoor/Heightmap_internal.h"   // for detail::clamp_f32
#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"
#include "render/raster/Raster.h"

#include <cmath>
#include <utility>
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

// ─── Wave-B: SplineEditor — track-authoring data ops ─────────────────────
//
// The editor (lane 18) drives the spline track through a thin set of data
// operations: insert / move / delete a control point, and set banking at a
// given t along the entire track. We keep the model independent of any
// UI/IPC plumbing — lane 18 wires its inspector / gizmo events to these
// ops, and the test exercises them directly.
//
// A SplineEditor holds the same `std::vector<SplineRoadSegment>` the
// runtime reads. Editing operations preserve the curve's C1 continuity
// where possible (insertion splits a segment using de Casteljau, so the
// curve is bitwise identical before and after the split until the new
// control point is moved). Banking is stored per-segment, but the editor
// exposes "set banking at parameter t along the full track" by mapping t
// onto (segment_index, local_t) and writing the segment's banking_rad.
//
// Control-point indexing convention:
//   - Each segment has 4 control points (p0, p1, p2, p3). For a track with
//     N segments, the total control-point count is 4*N.
//   - Global index `idx` maps to `(segment_index = idx / 4, local = idx % 4)`.
//   - Adjacent segments share an endpoint visually (segment[i].p3 ≈
//     segment[i+1].p0 for a connected track) but the editor doesn't enforce
//     this — the level designer can pull them apart for sharp corners.
//     "Move control point" can therefore touch a single segment's endpoint,
//     leaving the neighbor independent. If the caller wants to keep two
//     segments connected they call `move_control_point` once on each side
//     of the seam.

class SplineEditor {
public:
    // Initial track is empty; the caller appends segments first via
    // `append_segment`. This is the path the editor's "new track" command
    // takes; load-from-disk fills the vector directly and constructs the
    // editor around it.
    SplineEditor() noexcept = default;

    // Build over an existing track (e.g. one loaded via `load_spline_track`).
    explicit SplineEditor(std::vector<SplineRoadSegment> initial) noexcept
        : segments_(std::move(initial)) {}

    // ── Access ─────────────────────────────────────────────────────────
    std::vector<SplineRoadSegment>&       segments()       noexcept { return segments_; }
    const std::vector<SplineRoadSegment>& segments() const noexcept { return segments_; }
    u32 segment_count() const noexcept { return static_cast<u32>(segments_.size()); }
    u32 control_point_count() const noexcept {
        return static_cast<u32>(segments_.size()) * 4u;
    }

    // Append a new segment to the end of the track. Returns its index.
    u32 append_segment(const SplineRoadSegment& seg) {
        segments_.push_back(seg);
        return static_cast<u32>(segments_.size() - 1u);
    }

    // ── Insert control point ───────────────────────────────────────────
    // Insert a NEW control point that splits an existing segment in two,
    // at parameter `t` along that segment. The pre-split curve geometry is
    // exactly preserved: de Casteljau splits a cubic Bezier into two cubics
    // whose union reproduces the original. Banking and half-width are
    // inherited (the editor can then adjust them per-half).
    //
    // Returns `true` on success. `t` is clamped to (0, 1) — endpoints are
    // already control points; trying to insert at t=0 or t=1 is a no-op.
    bool insert_control_point(u32 segment_index, f32 t) {
        if (segment_index >= segments_.size()) return false;
        if (!(t > 0.0f && t < 1.0f)) return false;

        // de Casteljau: given p0..p3, produce two cubics A(t) ∪ B(t) that
        // reproduce the original curve.
        //   q0 = lerp(p0, p1, t)
        //   q1 = lerp(p1, p2, t)
        //   q2 = lerp(p2, p3, t)
        //   r0 = lerp(q0, q1, t)
        //   r1 = lerp(q1, q2, t)
        //   s  = lerp(r0, r1, t)        ← split point on the curve
        //
        // Segment A: (p0, q0, r0, s)
        // Segment B: (s,  r1, q2, p3)
        const SplineRoadSegment& src = segments_[segment_index];
        const math::Vec3 q0 = lerp_v3(src.p0, src.p1, t);
        const math::Vec3 q1 = lerp_v3(src.p1, src.p2, t);
        const math::Vec3 q2 = lerp_v3(src.p2, src.p3, t);
        const math::Vec3 r0 = lerp_v3(q0,     q1,     t);
        const math::Vec3 r1 = lerp_v3(q1,     q2,     t);
        const math::Vec3 s  = lerp_v3(r0,     r1,     t);

        SplineRoadSegment A{};
        A.p0 = src.p0; A.p1 = q0; A.p2 = r0; A.p3 = s;
        A.half_width  = src.half_width;
        A.banking_rad = src.banking_rad;

        SplineRoadSegment B{};
        B.p0 = s; B.p1 = r1; B.p2 = q2; B.p3 = src.p3;
        B.half_width  = src.half_width;
        B.banking_rad = src.banking_rad;

        segments_[segment_index] = A;
        segments_.insert(segments_.begin() + segment_index + 1, B);
        return true;
    }

    // ── Move control point ─────────────────────────────────────────────
    // Move a control point by GLOBAL index (0..control_point_count()-1).
    // For convenience the editor exposes `move_control_point(segment, local)`
    // too. The 'set' variant assigns an absolute world position; the
    // 'translate' variant adds a delta. Returns false on out-of-range.
    bool move_control_point(u32 global_index, math::Vec3 new_position) {
        const u32 seg = global_index / 4u;
        const u32 loc = global_index % 4u;
        return move_control_point(seg, loc, new_position);
    }

    bool move_control_point(u32 segment_index, u32 local_index, math::Vec3 new_position) {
        if (segment_index >= segments_.size() || local_index > 3u) return false;
        math::Vec3* cps[4] = {
            &segments_[segment_index].p0,
            &segments_[segment_index].p1,
            &segments_[segment_index].p2,
            &segments_[segment_index].p3,
        };
        *cps[local_index] = new_position;
        return true;
    }

    bool translate_control_point(u32 global_index, math::Vec3 delta) {
        if (global_index >= control_point_count()) return false;
        const u32 seg = global_index / 4u;
        const u32 loc = global_index % 4u;
        math::Vec3* cps[4] = {
            &segments_[seg].p0,
            &segments_[seg].p1,
            &segments_[seg].p2,
            &segments_[seg].p3,
        };
        cps[loc]->x += delta.x;
        cps[loc]->y += delta.y;
        cps[loc]->z += delta.z;
        return true;
    }

    // ── Delete control point ───────────────────────────────────────────
    // Delete a control point. Because each segment carries 4 control
    // points, "deleting" a single control point inside a segment is
    // ambiguous — the operation we expose is "delete the segment that
    // contains this control point". This matches the editor's intent: the
    // designer selects a node on the track and presses Delete; the segment
    // around that node disappears, and the neighbors auto-stitch end-to-end.
    //
    // After deletion, if the deleted segment had both a predecessor and a
    // successor, the predecessor's p3 is moved to the successor's p0
    // (preserving the successor's endpoint, so the next segment doesn't
    // jump). This is the cheapest stitching policy and what the editor's
    // undo records.
    //
    // Returns false on out-of-range. If the track ends up empty, fine —
    // the editor's UI handles that case.
    bool delete_control_point(u32 global_index) {
        if (global_index >= control_point_count()) return false;
        return delete_segment(global_index / 4u);
    }

    bool delete_segment(u32 segment_index) {
        if (segment_index >= segments_.size()) return false;

        const bool has_prev = segment_index > 0u;
        const bool has_next = (segment_index + 1u) < segments_.size();

        if (has_prev && has_next) {
            // Stitch: move predecessor's p3 to the successor's p0.
            segments_[segment_index - 1u].p3 = segments_[segment_index + 1u].p0;
        }
        segments_.erase(segments_.begin() + segment_index);
        return true;
    }

    // ── Banking ────────────────────────────────────────────────────────
    // Set banking at a parameter t along the *full* track. We map t∈[0,1]
    // onto (segment_index, local_t) by uniform parameterization across
    // segments (i.e. each segment covers an equal slice of the global t).
    //
    // This is the cheapest mapping; a true arc-length-based mapping would
    // be more visually uniform but requires summing per-segment arc
    // lengths and locating the slice — Wave B's editor signal is
    // "designer drags a control on the t-line", and uniform-by-segment is
    // sufficient for the editor preview. The bake step (Wave C) will
    // re-parameterize.
    //
    // Returns the segment index whose banking was written, or -1 on an
    // empty track.
    i32 set_banking_at_t(f32 t, f32 banking_rad) {
        if (segments_.empty()) return -1;
        const f32 tc = clamp_f32(t, 0.0f, 1.0f);
        // Map t into [0, N) and pick the segment index, clamping the
        // upper bound so t==1.0 lands on the LAST segment (not one past).
        const u32 N = static_cast<u32>(segments_.size());
        u32 seg = static_cast<u32>(tc * static_cast<f32>(N));
        if (seg >= N) seg = N - 1u;
        segments_[seg].banking_rad = banking_rad;
        return static_cast<i32>(seg);
    }

    // Read the banking at a given t (same parameterization as `set_banking_at_t`).
    f32 banking_at_t(f32 t) const noexcept {
        if (segments_.empty()) return 0.0f;
        const f32 tc = clamp_f32(t, 0.0f, 1.0f);
        const u32 N = static_cast<u32>(segments_.size());
        u32 seg = static_cast<u32>(tc * static_cast<f32>(N));
        if (seg >= N) seg = N - 1u;
        return segments_[seg].banking_rad;
    }

private:
    std::vector<SplineRoadSegment> segments_;

    PSY_FORCEINLINE static math::Vec3 lerp_v3(math::Vec3 a, math::Vec3 b, f32 t) noexcept {
        return math::Vec3{
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
        };
    }
};

}  // namespace psynder::world::outdoor::detail
