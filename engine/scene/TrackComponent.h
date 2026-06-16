// SPDX-License-Identifier: MIT
// Psynder - SCENE-LEVEL authoring TRACK component (LANE W11-1 racer DoD, Wave 11).
//
// A TrackComponent makes an entity carry an authored closed-loop RACE TRACK a
// designer lays out in the editor: a small FIXED-CAPACITY set of cubic Bezier
// segments (control-point quads) the engine evaluates, plus a track half-width
// and a start/finish lap gate. Attached alongside a VehicleComponent, it lets
// PlayRuntime run a TRACK-FOLLOW auto-driver each tick so the car LAPS the
// authored loop with NO bespoke racer C++ -- the no-code racing sibling of the
// no-code drivable VehicleComponent (Wave 8) and the no-code AI proxies (Wave 10).
//
// LAYERING: like PhysicsComponents.h, this POD lives in engine/scene and must NOT
// depend on engine/physics or engine/world. The Bezier control points mirror
// world::outdoor::SplineRoadSegment's shape (p0..p3 + half_width) but are a
// SELF-CONTAINED scene type so the serializer + editor Inspector see them and the
// scene layer stays dependency-free. The track-follow math (bezier_eval / tangent
// / closest-on-track / advance-along) is ported into PlayRuntime at the physics
// boundary, exactly like vehicle_terrain_height feeds the suspension probe.
//
// BOUNDED: the segments live in a fixed C array (no std::vector in the component)
// so the type stays trivially copyable and serializes as a fixed-stride record.
// segment_count selects how many of the kMaxSegments slots are live.
//
// RUNTIME-ONLY fields: the auto-driver's progress cursor (cursor_seg / cursor_t)
// and the lap bookkeeping (lap_count / prev_gate_signed / gate_armed) are filled
// by PlayRuntime::begin()/tick() and cleared by end(); they are NEVER serialized
// (they reset to their POD defaults on load, exactly like the physics runtime
// handles). Only the authored geometry + width + gate persist with the scene.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"

#include <algorithm>
#include <cmath>

namespace psynder::scene {

// One cubic Bezier road segment: four control points in the XZ driving plane
// (y is the road height) + the segment's road half-width. Mirrors
// world::outdoor::SplineRoadSegment field-for-field so the proven racer_demo
// Bezier math ports verbatim, but kept scene-self-contained (no world:: dep).
struct TrackSegment {
    math::Vec3 p0{0.0f, 0.0f, 0.0f};
    math::Vec3 p1{0.0f, 0.0f, 0.0f};
    math::Vec3 p2{0.0f, 0.0f, 0.0f};
    math::Vec3 p3{0.0f, 0.0f, 0.0f};
    f32 half_width = 6.0f;
    u32 _pad = 0u;  // keep 16-byte alignment-friendly + a stable serialized stride
};

// A closed-loop authored race track. The live segments [0, segment_count) form a
// closed Bezier loop (segment i's p3 should equal segment i+1's p0, and the last
// segment's p3 the first segment's p0, so the path closes C0; the racer authors
// co-linear join tangents for C1). lap_gate_* describes the start/finish plane
// PlayRuntime's lap timer crosses. The auto-driver targets target_speed (just
// under the vehicle governor cap) and chases a look_ahead point on the spline.
PSYNDER_COMPONENT(TrackComponent) {
    static constexpr u32 kMaxSegments = 8u;

    TrackSegment segments[kMaxSegments] = {};
    u32 segment_count = 0u;
    // Auto-driver tuning (mirrors racer_demo's Driver; sane defaults so an
    // un-tuned authored track still laps under PlayRuntime's track-follow driver).
    f32 target_speed = 11.0f;    // m/s cruise target (keep just under the governor)
    f32 look_ahead = 12.0f;      // m: how far down-spline the driver aims
    f32 steer_gain = 0.7f;       // proportional steer gain against the heading error
    f32 steer_clamp = 0.22f;     // rad: hard cap on the commanded front-wheel angle
    f32 throttle_kp = 0.5f;      // throttle/brake P gain on the speed error
    // Start/finish gate: a plane through lap_gate_point with unit normal
    // lap_gate_normal (the track forward tangent at the start). A back->front
    // crossing while moving down-track closes a lap.
    math::Vec3 lap_gate_point{0.0f, 0.0f, 0.0f};
    math::Vec3 lap_gate_normal{1.0f, 0.0f, 0.0f};
    // --- RUNTIME-only (never serialized; reset to defaults on load) ---------
    u32 cursor_seg = 0u;          // auto-driver progress cursor: current segment
    f32 cursor_t = 0.0f;          // auto-driver progress cursor: param within seg
    u32 lap_count = 0u;           // completed gate laps this Play session
    f32 prev_gate_signed = 0.0f;  // last-tick signed distance to the gate plane
    u8 gate_have_prev = 0u;       // 1 once prev_gate_signed is valid
    u8 gate_armed = 0u;           // 1 after the first crossing arms the timer
    u8 _pad[2] = {};
};

// Clamp an authored TrackComponent to sane ranges: bound the segment count, force
// a positive per-segment half-width, sane positive driver gains, and renormalise
// the gate normal (defaulting to +X if degenerate). Idempotent; leaves the
// authored geometry untouched save the clamps; zeroes the runtime cursor/lap
// fields so a freshly-authored or freshly-loaded track always starts clean.
[[nodiscard]] inline TrackComponent sanitize_track_component(TrackComponent t) noexcept {
    if (t.segment_count > TrackComponent::kMaxSegments)
        t.segment_count = TrackComponent::kMaxSegments;
    for (u32 i = 0u; i < TrackComponent::kMaxSegments; ++i) {
        if (!(t.segments[i].half_width > 0.0f))
            t.segments[i].half_width = 6.0f;
        t.segments[i]._pad = 0u;
    }
    if (!(t.target_speed >= 0.0f)) t.target_speed = 11.0f;
    if (!(t.look_ahead > 0.0f)) t.look_ahead = 12.0f;
    if (!(t.steer_gain >= 0.0f)) t.steer_gain = 0.7f;
    if (!(t.steer_clamp >= 0.0f)) t.steer_clamp = 0.22f;
    if (!(t.throttle_kp >= 0.0f)) t.throttle_kp = 0.5f;
    const f32 n2 = t.lap_gate_normal.x * t.lap_gate_normal.x +
                   t.lap_gate_normal.y * t.lap_gate_normal.y +
                   t.lap_gate_normal.z * t.lap_gate_normal.z;
    if (n2 > 1e-12f) {
        const f32 inv = 1.0f / std::sqrt(n2);
        t.lap_gate_normal.x *= inv;
        t.lap_gate_normal.y *= inv;
        t.lap_gate_normal.z *= inv;
    } else {
        t.lap_gate_normal = math::Vec3{1.0f, 0.0f, 0.0f};
    }
    // Runtime bookkeeping never persists / never carries across a sanitize.
    t.cursor_seg = 0u;
    t.cursor_t = 0.0f;
    t.lap_count = 0u;
    t.prev_gate_signed = 0.0f;
    t.gate_have_prev = 0u;
    t.gate_armed = 0u;
    t._pad[0] = t._pad[1] = 0u;
    return t;
}

static_assert(sizeof(TrackSegment) == 56u, "TrackSegment must match the cooked stride");

}  // namespace psynder::scene
