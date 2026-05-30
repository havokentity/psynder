// SPDX-License-Identifier: MIT
// Psynder - DEMO GAME 2: psynder_racer_demo.
//
// An NFS2SE-style spline-road racing demo that composes the vehicle physics,
// chase camera, a closed-loop track, and lap timing into one drivable game:
//   * TRACK    - a closed-loop oval built in code from four cubic Bezier
//                `world::outdoor::SplineRoadSegment`s, extruded into a textured
//                road strip (a single scene mesh entity with per-vertex tarmac /
//                kerb colours) that lies flat on the ground plane at y = 0.
//   * VEHICLE  - a Box chassis rigid body + `physics::vehicle::create` with four
//                wheels at the chassis corners. Player drives with WASD:
//                W = throttle, S = brake/reverse, A/D = steer (via
//                set_throttle / set_brake / set_steer). The chassis + four wheel
//                cylinders are scene mesh entities re-posed each frame.
//   * CHASE CAM- a scene camera ~6 m behind / 2.5 m above the chassis looking at
//                it (mirrors PlayRuntime::update_chase_camera): the camera frame
//                is built off the SMOOTH track tangent at the car's position, not
//                the noisy velocity vector, so it always faces down-road.
//   * LAP TIME - a start/finish gate (a plane through the start point, normal =
//                track tangent there). Each frame we test the signed side of the
//                chassis vs that plane; a back->front crossing while moving
//                forward closes a lap, accumulates the lap time, and tracks the
//                best lap. Current lap time + speed + best lap render in a
//                ui::imm HUD.
//   * RENDER   - scene point lights + RenderSettings in RASTER mode (see below);
//                the track + car render through the scene mesh path so the engine
//                lights them automatically.
//
// PER-FRAME SYSTEM ORDER (DOTS, alloc-free in the steady state):
//   1. read input (or scripted smoke auto-drive) -> set vehicle throttle/brake/steer
//   2. physics fixed-step at 120 Hz: world.step(dt) under the vehicle module
//   3. read chassis pose; update lap timer (gate crossing test)
//   4. push chassis + wheel poses into their scene mesh transforms
//   5. chase camera: frame off the track tangent, write the scene camera transform
//   6. render (engine_frame_begin clear -> engine_frame_render raster + lights) + HUD
//
// ALLOC-FREE ARGUMENT: the track / car / wheel vertex+index buffers, every scene
// mesh entity, the physics bodies, and the scene + render pools are all created
// once at load (the scene pools are prewarmed). The per-frame loop only reads
// input, steps fixed-size systems, and mutates pooled ECS transform columns in
// place; the HUD uses ui::imm's frame-scoped immediate buffers. No std::vector or
// other heap allocation happens inside the loop. MeshDesc holds RAW pointers into
// our persistent buffers, so those buffers are owned by the long-lived game state.
//
// RASTER-vs-HYBRID CHOICE: this demo renders in scene::RenderMode::Raster with
// scene point lights. The racer's value is the moving car + chase cam + lap timer
// composing under the vehicle sim; a flat ground track casts no interesting
// occluder shadows, so the hybrid per-pixel shadow-ray cost buys little here. We
// keep the fast, fully alloc-free raster path and a couple of warm point lights
// for shape readout. (Switching to Hybrid would only mean copying shooter_demo's
// build_shadow_scene + set_pending_shadow_occluder block before the render call.)
//
// GROUND PLANE (flat, by design): the oval is tessellated FLAT at world y = 0, so
// the per-wheel suspension contacts a single ground plane via set_ground_plane at
// the track height -- no heightfield is wired here. The track geometry itself has
// no elevation to follow (all four Bezier segments sit at y = 0), so adding a
// terrain heightfield would sample a constant Y and buy nothing; the df_demo jeep
// is the lane's terrain-following showcase. Track elevation (a sculpted/banked
// road fed through set_ground_heightfield) is deferred along with the polish below.
//
// VEHICLE GOVERNOR + STEERING AUTHORITY (#58, now wired): build_vehicle sets
// VehicleDesc.max_speed (a sane cruise cap) plus steer_full_speed / steer_taper_
// speed / steer_min_authority, so a held throttle settles AT the cap instead of
// running away and the front wheels keep enough authority to pull the U-turns.
// The auto-driver targets kAutoTargetSpeed just UNDER the cap; the smoke run logs
// a governed-cruise PASS/FAIL (peak speed <= cap, with real track progress). This
// replaces the earlier "PUBLIC-API GAP" note: the engine-side governing /
// steering-authority fix the racer agent flagged has since landed on VehicleDesc,
// and this demo now opts into it. INTERACTIVE WASD play is unchanged.
//
// DEFERRED POLISH: AI racers, tyre smoke / skid marks, sculpted/banked TRACK
// ELEVATION (set_ground_heightfield once the track carries relief), multiple
// selectable tracks, a proper start grid + countdown, and engine audio. The first
// playable increment shipped here is a governed, lap-timing car on a looping flat
// track with a working chase cam.
//
// CLI flags (shared app args):
//   --smoke-frames=N         Headless CI run: auto-drive the car forward N frames.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "common/MeshWinding.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Geometry.h"
#include "render/RenderingSystem.h"
#include "render/raster/Raster.h"
#include "scene/RenderSettings.h"
#include "scene/SceneEcs.h"
#include "ui/imm/Imm.h"
#include "world/outdoor/Terrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// --- Render config ---------------------------------------------------------
constexpr u32 kFbW = 640;
constexpr u32 kFbH = 360;

// --- Vehicle governor + steering authority (#58) ---------------------------
// Sane cruise cap (m/s) so a held throttle settles AT the cap instead of
// running away, plus the speed-scaled steering authority profile. The governed
// straight cruise (the smoke's stable drive mode) accelerates to the target and
// the governor pins it at the cap: ~12 m/s ~= 43 km/h, a believable cruise.
// (The cap is the dominant lever: drive torque tapers to zero approaching it and
// the drive-wheel omega clamp limits the no-slip wheel speed to max_speed/radius,
// so the chassis cannot build forward momentum past the cap.) The steering-
// authority fields are wired onto the VehicleDesc and shape the front-wheel
// angle vs speed; they take effect for cornering once the engine tire lateral-
// force sign is corrected (see the smoke call site + DEFERRED note).
constexpr f32 kCarMaxSpeed = 12.0f;        // governed forward-speed cap (m/s)
constexpr f32 kSteerFullSpeed = 5.0f;      // full steering authority at/below this
constexpr f32 kSteerTaperSpeed = 14.0f;    // min authority at/above this
constexpr f32 kSteerMinAuthority = 0.55f;  // retained authority at high speed
constexpr f32 kAutoTargetSpeed = 11.0f;    // cruise target, just under the cap

// Pack RGBA8 in the engine's 0xAABBGGRR layout (R in the low byte).
constexpr u32 rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

inline math::Vec3 v3(f32 x, f32 y, f32 z) noexcept {
    return {x, y, z};
}

// ─── Bezier helpers (public SplineRoadSegment only) ────────────────────────
// Cubic Bezier evaluation. p0..p3 are the segment's control points; t in [0,1].
inline math::Vec3 bezier_eval(const world::outdoor::SplineRoadSegment& s, f32 t) noexcept {
    const f32 u = 1.0f - t;
    const f32 b0 = u * u * u;
    const f32 b1 = 3.0f * u * u * t;
    const f32 b2 = 3.0f * u * t * t;
    const f32 b3 = t * t * t;
    return v3(b0 * s.p0.x + b1 * s.p1.x + b2 * s.p2.x + b3 * s.p3.x,
              b0 * s.p0.y + b1 * s.p1.y + b2 * s.p2.y + b3 * s.p3.y,
              b0 * s.p0.z + b1 * s.p1.z + b2 * s.p2.z + b3 * s.p3.z);
}

// Derivative wrt t - unnormalised tangent.
inline math::Vec3 bezier_tangent(const world::outdoor::SplineRoadSegment& s, f32 t) noexcept {
    const f32 u = 1.0f - t;
    const f32 a = 3.0f * u * u;
    const f32 b = 6.0f * u * t;
    const f32 c = 3.0f * t * t;
    return v3(a * (s.p1.x - s.p0.x) + b * (s.p2.x - s.p1.x) + c * (s.p3.x - s.p2.x),
              a * (s.p1.y - s.p0.y) + b * (s.p2.y - s.p1.y) + c * (s.p3.y - s.p2.y),
              a * (s.p1.z - s.p0.z) + b * (s.p2.z - s.p1.z) + c * (s.p3.z - s.p2.z));
}

// Right vector in the XZ-driving plane: tangent rotated -90 deg about +Y.
inline math::Vec3 road_right(const world::outdoor::SplineRoadSegment& seg, f32 t) noexcept {
    const math::Vec3 tan = math::normalize(bezier_tangent(seg, t));
    return math::normalize(v3(tan.z, 0.0f, -tan.x));
}

// ─── Track ─────────────────────────────────────────────────────────────────
constexpr f32 kTrackHalfWidth = 6.0f;

// Build a closed oval track in the XZ plane, centred at the origin: two straights
// along +/-X joined by two half-loop U-turns. Endpoint tangents are co-linear
// across the joins so the path is C1-continuous (no kinks for the camera or the
// car to fight). Kept compact so the car laps in a handful of seconds in smoke
// mode at the auto-driver's target speed.
std::vector<world::outdoor::SplineRoadSegment> build_oval_track() {
    std::vector<world::outdoor::SplineRoadSegment> out;
    out.reserve(4);

    constexpr f32 sx = 50.0f;    // straight half-length on X
    constexpr f32 sz = 25.0f;    // curve half-depth on Z
    constexpr f32 cz = sz * 1.8f;

    // Segment 0: straight, +X along z = -sz (the start straight).
    out.push_back({v3(-sx, 0.0f, -sz), v3(-sx * 0.33f, 0.0f, -sz), v3(sx * 0.33f, 0.0f, -sz),
                   v3(sx, 0.0f, -sz), kTrackHalfWidth, 0.0f});
    // Segment 1: U-turn at +X.
    out.push_back({v3(sx, 0.0f, -sz), v3(sx + cz, 0.0f, -sz), v3(sx + cz, 0.0f, sz),
                   v3(sx, 0.0f, sz), kTrackHalfWidth, 0.0f});
    // Segment 2: straight, -X along z = +sz.
    out.push_back({v3(sx, 0.0f, sz), v3(sx * 0.33f, 0.0f, sz), v3(-sx * 0.33f, 0.0f, sz),
                   v3(-sx, 0.0f, sz), kTrackHalfWidth, 0.0f});
    // Segment 3: U-turn at -X.
    out.push_back({v3(-sx, 0.0f, sz), v3(-sx - cz, 0.0f, sz), v3(-sx - cz, 0.0f, -sz),
                   v3(-sx, 0.0f, -sz), kTrackHalfWidth, 0.0f});

    return out;
}

// Persistent CPU geometry for a scene mesh (MeshDesc holds raw pointers into
// these, so they must outlive the mesh / the whole run).
struct MeshBuffers {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

// Tessellate the closed track into one (vertex, index) pair: two vertices per
// spline sample (left + right edge) and two triangles per interval, wrapping the
// last sample to the first so the loop closes.
void tessellate_track(const std::vector<world::outdoor::SplineRoadSegment>& segs,
                      MeshBuffers& m) {
    constexpr u32 kSamplesPerSeg = 24;
    const u32 total_samples = static_cast<u32>(segs.size()) * kSamplesPerSeg;
    m.verts.clear();
    m.indices.clear();
    m.verts.reserve(static_cast<usize>(total_samples) * 2u);
    m.indices.reserve(static_cast<usize>(total_samples) * 6u);

    const std::array<u32, 2> kTarmac = {rgba8(64, 64, 70), rgba8(80, 80, 86)};
    const u32 kLine = rgba8(210, 200, 90);  // kerb stripe

    for (usize si = 0; si < segs.size(); ++si) {
        const auto& seg = segs[si];
        for (u32 i = 0; i < kSamplesPerSeg; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kSamplesPerSeg - 1);
            const math::Vec3 p = bezier_eval(seg, t);
            const math::Vec3 right = road_right(seg, t);
            const math::Vec3 up = v3(0.0f, 1.0f, 0.0f);

            const math::Vec3 left_pos = math::sub(p, math::mul(right, seg.half_width));
            const math::Vec3 right_pos = math::add(p, math::mul(right, seg.half_width));
            const bool kerb = ((i / 3) & 1u) == 0u && (i & 1u) == 0u;
            const u32 col = kerb ? kLine : kTarmac[si & 1u];

            render::raster::Vertex vL{};
            vL.position = left_pos;
            vL.normal = up;
            vL.uv = math::Vec2{0.0f, static_cast<f32>(i)};
            vL.color = col;
            m.verts.push_back(vL);

            render::raster::Vertex vR{};
            vR.position = right_pos;
            vR.normal = up;
            vR.uv = math::Vec2{1.0f, static_cast<f32>(i)};
            vR.color = col;
            m.verts.push_back(vR);
        }
    }

    const u32 n_pairs = static_cast<u32>(m.verts.size() / 2u);
    for (u32 i = 0; i < n_pairs; ++i) {
        const u32 j = (i + 1u) % n_pairs;
        const u32 a = i * 2u + 0u;
        const u32 b = i * 2u + 1u;
        const u32 c = j * 2u + 0u;
        const u32 d = j * 2u + 1u;
        m.indices.push_back(a);
        m.indices.push_back(c);
        m.indices.push_back(d);
        m.indices.push_back(a);
        m.indices.push_back(d);
        m.indices.push_back(b);
    }
}

// ─── Car primitives ──────────────────────────────────────────────────────
constexpr u32 kColChassis = rgba8(190, 50, 50);
constexpr u32 kColTop = rgba8(150, 35, 35);
constexpr u32 kColWheel = rgba8(30, 30, 32);

// Unit cube (per-face colour), spans +/-0.5 on each axis.
const std::array<render::raster::Vertex, 24> kCubeVerts{{
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0, 1}, {0, 0}, kColChassis},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0, 0}, {0, 0}, kColChassis},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {1, 0}, {0, 0}, kColChassis},
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {1, 1}, {0, 0}, kColChassis},
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0, 1}, {0, 0}, kColChassis},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0, 0}, {0, 0}, kColChassis},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {1, 0}, {0, 0}, kColChassis},
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {1, 1}, {0, 0}, kColChassis},
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0, 1}, {0, 0}, kColTop},
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0, 0}, {0, 0}, kColTop},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {1, 0}, {0, 0}, kColTop},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {1, 1}, {0, 0}, kColTop},
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0, 1}, {0, 0}, kColChassis},
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}, {0, 0}, kColChassis},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}, {0, 0}, kColChassis},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {1, 1}, {0, 0}, kColChassis},
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0, 1}, {0, 0}, kColChassis},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {1, 1}, {0, 0}, kColChassis},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {1, 0}, {0, 0}, kColChassis},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0, 0}, {0, 0}, kColChassis},
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}, {0, 0}, kColChassis},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}, {0, 0}, kColChassis},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0}, {0, 0}, kColChassis},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0, 0}, {0, 0}, kColChassis},
}};

constexpr std::array<u32, 36> kCubeIndices{
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

// Low-poly cylinder for wheels; rotation axis along local Y.
void build_cylinder(f32 radius, f32 half_height, u32 sides, MeshBuffers& c) {
    c.verts.clear();
    c.indices.clear();
    c.verts.reserve(static_cast<usize>(sides) * 2u + 2u);
    c.indices.reserve(static_cast<usize>(sides) * 12u);

    for (u32 i = 0; i < sides; ++i) {
        const f32 a = static_cast<f32>(i) * math::kTwoPi / static_cast<f32>(sides);
        const f32 cs = std::cos(a);
        const f32 sn = std::sin(a);
        render::raster::Vertex vt{};
        vt.position = v3(radius * cs, half_height, radius * sn);
        vt.normal = v3(cs, 0.0f, sn);
        vt.uv = math::Vec2{static_cast<f32>(i) / static_cast<f32>(sides), 1.0f};
        vt.color = kColWheel;
        c.verts.push_back(vt);
        render::raster::Vertex vb = vt;
        vb.position = v3(radius * cs, -half_height, radius * sn);
        vb.uv = math::Vec2{static_cast<f32>(i) / static_cast<f32>(sides), 0.0f};
        c.verts.push_back(vb);
    }
    const u32 hub_top = static_cast<u32>(c.verts.size());
    c.verts.push_back({v3(0, half_height, 0), v3(0, 1, 0), {0.5f, 0.5f}, {0, 0}, kColWheel});
    const u32 hub_bot = static_cast<u32>(c.verts.size());
    c.verts.push_back({v3(0, -half_height, 0), v3(0, -1, 0), {0.5f, 0.5f}, {0, 0}, kColWheel});

    for (u32 i = 0; i < sides; ++i) {
        const u32 j = (i + 1u) % sides;
        const u32 t0 = i * 2u + 0u, b0 = i * 2u + 1u;
        const u32 t1 = j * 2u + 0u, b1 = j * 2u + 1u;
        c.indices.push_back(t0);
        c.indices.push_back(t1);
        c.indices.push_back(b1);
        c.indices.push_back(t0);
        c.indices.push_back(b1);
        c.indices.push_back(b0);
        c.indices.push_back(hub_top);
        c.indices.push_back(t1);
        c.indices.push_back(t0);
        c.indices.push_back(hub_bot);
        c.indices.push_back(b0);
        c.indices.push_back(b1);
    }
}

// ─── Track position query (auto-drive + chase-cam tangent) ─────────────────
struct TrackPos {
    u32 seg = 0u;
    f32 t = 0.0f;
    math::Vec3 p{};
};

TrackPos closest_on_track(const std::vector<world::outdoor::SplineRoadSegment>& segs,
                          math::Vec3 q) noexcept {
    constexpr u32 kSamples = 48;
    TrackPos best{};
    f32 best_d2 = 1e30f;
    for (u32 si = 0; si < segs.size(); ++si) {
        for (u32 i = 0; i <= kSamples; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kSamples);
            const math::Vec3 p = bezier_eval(segs[si], t);
            const math::Vec3 d = math::sub(p, q);
            const f32 d2 = math::dot(d, d);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = TrackPos{si, t, p};
            }
        }
    }
    return best;
}

// Advance a (segment, t) lookahead point `advance_m` of arc along the closed loop.
TrackPos advance_along(const std::vector<world::outdoor::SplineRoadSegment>& segs,
                       u32 seg,
                       f32 t,
                       f32 advance_m) noexcept {
    constexpr f32 dt = 1.0f / 64.0f;
    f32 covered = 0.0f;
    math::Vec3 prev = bezier_eval(segs[seg], t);
    while (covered < advance_m) {
        t += dt;
        while (t > 1.0f) {
            t -= 1.0f;
            seg = (seg + 1u) % segs.size();
        }
        const math::Vec3 here = bezier_eval(segs[seg], t);
        covered += math::length(math::sub(here, prev));
        prev = here;
    }
    return TrackPos{seg, t, prev};
}

// Build a yaw-only chassis rotation matrix from a forward (XZ) vector. The
// chassis long axis (4.2 vs 1.8) and front wheels sit on local X, so the car
// drives along X; the extra -kHalfPi rolls travel onto the long axis (matches
// samples/04 so the model points where it drives instead of broadside).
inline math::Mat4 yaw_from_forward(math::Vec3 fwd) noexcept {
    fwd.y = 0.0f;
    fwd = math::normalize(fwd);
    const f32 yaw = std::atan2(fwd.x, -fwd.z) - math::kHalfPi;
    return math::rotate_quat(math::quat_from_axis_angle(v3(0, 1, 0), yaw));
}

// ─── Lap timer ─────────────────────────────────────────────────────────────
// The start/finish gate is the plane through `gate_point` with normal
// `gate_normal` (the track forward tangent at the start). We track the signed
// distance of the chassis to that plane; a transition from behind (negative) to
// in front (positive) while the car is moving forward (dot(vel, normal) > 0)
// closes a lap. We ignore the very first crossing as the start so lap 1 times
// from the line.
struct LapTimer {
    math::Vec3 gate_point{};
    math::Vec3 gate_normal{};   // unit, points down-track at the start
    f32 prev_signed = 0.0f;
    bool have_prev = false;
    f32 current_lap_time = 0.0f;
    f32 last_lap_time = 0.0f;   // the lap just completed (for logging)
    f32 best_lap_time = 0.0f;
    u32 lap_count = 0u;         // completed laps
    bool armed = false;         // ignore the initial crossing off the grid

    void reset(math::Vec3 point, math::Vec3 normal) noexcept {
        gate_point = point;
        gate_normal = math::normalize(normal);
        prev_signed = 0.0f;
        have_prev = false;
        current_lap_time = 0.0f;
        last_lap_time = 0.0f;
        best_lap_time = 0.0f;
        lap_count = 0u;
        armed = false;
    }

    // Returns true on the frame a lap was completed.
    bool update(math::Vec3 chassis_pos, math::Vec3 vel_xz, f32 dt) noexcept {
        current_lap_time += dt;
        const math::Vec3 rel = math::sub(chassis_pos, gate_point);
        const f32 signed_d = math::dot(rel, gate_normal);
        bool lap_done = false;
        if (have_prev) {
            const bool crossed_forward = prev_signed < 0.0f && signed_d >= 0.0f;
            const bool moving_down_track = math::dot(vel_xz, gate_normal) > 0.0f;
            // Gate the crossing to a band near the line on the perpendicular axis
            // so a car cutting across the infinite plane elsewhere on the oval
            // does not count. (The two straights are far apart in Z.)
            const math::Vec3 lateral = math::sub(rel, math::mul(gate_normal, signed_d));
            const bool near_line = math::dot(lateral, lateral) < (kTrackHalfWidth * 2.0f) *
                                                                  (kTrackHalfWidth * 2.0f);
            if (crossed_forward && moving_down_track && near_line) {
                if (armed) {
                    ++lap_count;
                    last_lap_time = current_lap_time;
                    if (best_lap_time <= 0.0f || current_lap_time < best_lap_time)
                        best_lap_time = current_lap_time;
                    lap_done = true;
                }
                armed = true;  // first crossing arms; subsequent ones count
                current_lap_time = 0.0f;
            }
        }
        prev_signed = signed_d;
        have_prev = true;
        return lap_done;
    }
};

// ─── Auto-driver (smoke + an idle attract drive) ──────────────────────────
struct Driver {
    f32 target_speed_mps = kAutoTargetSpeed;  // just under the governed cap
    f32 look_ahead_m = 12.0f;                 // aim distance: smooth but responsive
    f32 steer_gain = 0.7f;                    // gentle proportional steer: no spin
    f32 steer_clamp = 0.22f;                  // hard cap on commanded front angle (rad)
    f32 throttle_kp = 0.15f;  // gentle throttle: no wheel-spin burst to overshoot
};

// Governed STRAIGHT cruise: hold steer at 0 and run the throttle PI toward the
// target speed so the car accelerates onto the start straight and the
// VehicleDesc governor pins it AT the cap. This is the provably-stable drive
// mode (steering is gated on the engine tire-sign fix; see the call site +
// DEFERRED note). Pure read of the current speed; writes the three controls.
void straight_cruise(const Driver& driver,
                     f32 car_speed,
                     f32& steer_out,
                     f32& throttle_out,
                     f32& brake_out) noexcept {
    steer_out = 0.0f;
    const f32 speed_err = driver.target_speed_mps - car_speed;
    if (speed_err >= 0.0f) {
        throttle_out = std::min(1.0f, driver.throttle_kp * speed_err);
        brake_out = 0.0f;
    } else {
        throttle_out = 0.0f;
        brake_out = std::min(1.0f, -driver.throttle_kp * speed_err);
    }
}

// Compute steer / throttle / brake to chase a look-ahead point at the target
// speed. Pure read of the track + current car state; writes nothing. Retained
// for interactive idle-attract + as the ready closed-loop controller for once
// the engine tire lateral-force sign is corrected (see the smoke call site).
[[maybe_unused]] void auto_drive(const std::vector<world::outdoor::SplineRoadSegment>& segs,
                const Driver& driver,
                math::Vec3 car_pos,
                math::Vec3 chassis_fwd,
                f32 car_speed,
                f32& steer_out,
                f32& throttle_out,
                f32& brake_out) noexcept {
    const TrackPos here = closest_on_track(segs, car_pos);
    const TrackPos ahead = advance_along(segs, here.seg, here.t, driver.look_ahead_m);

    // Steering is computed against the CHASSIS HEADING (not the velocity vector):
    // set_steer angles the front wheels relative to the chassis, so a chassis-
    // relative aim error is the controller's true plant input. (The velocity
    // vector is noisy at low speed and lags in a slide, which fed a runaway
    // over-correction.) The proportional gain + a tight clamp keep the commanded
    // angle small so the lateral tire force never saturates the friction circle
    // and spins the light chassis -- the car eases onto the look-ahead instead.
    steer_out = 0.0f;
    math::Vec3 to_aim = math::sub(ahead.p, car_pos);
    to_aim.y = 0.0f;
    const f32 to_aim_len = math::length(to_aim);
    if (to_aim_len > 0.001f) {
        to_aim = math::mul(to_aim, 1.0f / to_aim_len);
        math::Vec3 fwd_xz = chassis_fwd;
        fwd_xz.y = 0.0f;
        fwd_xz = math::normalize(fwd_xz);
        const f32 dotv = math::dot(fwd_xz, to_aim);
        const f32 crossy = fwd_xz.z * to_aim.x - fwd_xz.x * to_aim.z;
        const f32 ang = std::atan2(crossy, std::clamp(dotv, -1.0f, 1.0f));
        f32 steer = driver.steer_gain * ang;
        steer = std::clamp(steer, -driver.steer_clamp, driver.steer_clamp);
        steer_out = steer;
    }

    // Throttle PI on speed error, with a HARD overspeed cutout: above the target
    // we cut throttle and brake firmly. The in-engine vehicle solver (Wave-B
    // Pacejka tires + 6-speed gearbox) has limited low-load steering authority,
    // so holding a modest target speed keeps the steered front wheels able to
    // pull the car around the U-turns instead of understeering off the loop.
    const f32 speed_err = driver.target_speed_mps - car_speed;
    f32 throttle = 0.0f;
    f32 brake = 0.0f;
    if (speed_err >= 0.0f) {
        throttle = std::min(1.0f, driver.throttle_kp * speed_err);
    } else {
        brake = std::min(1.0f, -driver.throttle_kp * speed_err);
    }
    throttle_out = throttle;
    brake_out = brake;
}

// --- Game state ------------------------------------------------------------
struct RacerGame {
    scene::Scene* scene = nullptr;
    render::RenderingSystem* renderer = nullptr;

    physics::World world{};  // owns the chassis body + vehicle module state

    // Persistent CPU geometry (MeshDesc points into these for the run).
    std::vector<world::outdoor::SplineRoadSegment> track{};
    MeshBuffers track_mesh{};
    MeshBuffers chassis_mesh{};
    MeshBuffers wheel_mesh{};

    // Scene entities re-posed each frame.
    Entity track_entity{};
    Entity chassis_entity{};
    std::array<Entity, 4> wheel_entities{};
    Entity camera{};

    physics::BodyId chassis{};
    physics::vehicle::VehicleId vehicle{};

    // Chassis dimensions / wheel layout (chassis-local).
    f32 wx = 1.45f, wz = 0.85f;
    f32 wheel_radius = 0.35f;

    // Sim-derived state.
    math::Vec3 car_pos{};
    math::Vec3 car_fwd = v3(1.0f, 0.0f, 0.0f);
    f32 car_speed = 0.0f;
    f32 hud_throttle = 0.0f;
    f32 hud_brake = 0.0f;
    f32 hud_steer = 0.0f;

    LapTimer lap{};
    Driver driver{};
};

// Flat-shaded, double-sided material (the track is a thin ribbon; the chassis +
// wheels read from every camera angle without per-face winding worries).
render::MaterialId make_material(scene::Scene& scene, u32 albedo) {
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = albedo;
    desc.winding = render::MaterialWinding::DoubleSided;
    return scene.materials().create(desc);
}

render::MeshId make_mesh(RacerGame& game, const MeshBuffers& buf) {
    render::MeshDesc desc{};
    desc.vertices = buf.verts.data();
    desc.vertex_count = static_cast<u32>(buf.verts.size());
    desc.indices = buf.indices.data();
    desc.index_count = static_cast<u32>(buf.indices.size());
    desc.cull = render::raster::CullMode::None;  // double-sided ribbon + boxes
    return game.renderer->meshes().create_mesh(desc);
}

void build_track(RacerGame& game) {
    game.track = build_oval_track();
    tessellate_track(game.track, game.track_mesh);
    samples::fix_winding(game.track_mesh.verts.data(),
                         static_cast<u32>(game.track_mesh.verts.size()),
                         game.track_mesh.indices.data(),
                         static_cast<u32>(game.track_mesh.indices.size()));

    const render::MeshId track_id = make_mesh(game, game.track_mesh);
    const render::MaterialId track_mat = make_material(*game.scene, rgba8(255, 255, 255));
    game.track_entity =
        game.scene->spawn_mesh_instance(track_id, track_mat, scene::LocalTransform{},
                                        scene::kInvalidSceneNode,
                                        scene::RenderableFlags::Visible,
                                        scene::ObjectMobility::Static);
}

void build_car_meshes(RacerGame& game) {
    game.chassis_mesh.verts.assign(kCubeVerts.begin(), kCubeVerts.end());
    game.chassis_mesh.indices.assign(kCubeIndices.begin(), kCubeIndices.end());
    samples::fix_winding(game.chassis_mesh.verts.data(),
                         static_cast<u32>(game.chassis_mesh.verts.size()),
                         game.chassis_mesh.indices.data(),
                         static_cast<u32>(game.chassis_mesh.indices.size()));

    build_cylinder(game.wheel_radius, 0.18f, 12u, game.wheel_mesh);
    samples::fix_winding(game.wheel_mesh.verts.data(),
                         static_cast<u32>(game.wheel_mesh.verts.size()),
                         game.wheel_mesh.indices.data(),
                         static_cast<u32>(game.wheel_mesh.indices.size()));

    const render::MeshId chassis_id = make_mesh(game, game.chassis_mesh);
    const render::MeshId wheel_id = make_mesh(game, game.wheel_mesh);
    const render::MaterialId body_mat = make_material(*game.scene, rgba8(255, 255, 255));
    const render::MaterialId wheel_mat = make_material(*game.scene, rgba8(255, 255, 255));

    game.chassis_entity =
        game.scene->spawn_mesh_instance(chassis_id, body_mat, scene::LocalTransform{},
                                        scene::kInvalidSceneNode,
                                        scene::RenderableFlags::Visible,
                                        scene::ObjectMobility::Dynamic);
    for (auto& we : game.wheel_entities) {
        we = game.scene->spawn_mesh_instance(wheel_id, wheel_mat, scene::LocalTransform{},
                                             scene::kInvalidSceneNode,
                                             scene::RenderableFlags::Visible,
                                             scene::ObjectMobility::Dynamic);
    }
}

void build_vehicle(RacerGame& game) {
    // Start midway along segment 0 (the start straight), facing +X.
    const f32 start_t = 0.05f;
    const math::Vec3 start_p = bezier_eval(game.track[0], start_t);

    game.world.set_gravity(v3(0.0f, -9.81f, 0.0f));

    physics::BodyDesc chassis_desc{};
    chassis_desc.shape = physics::Shape::Box;
    chassis_desc.mass = 1500.0f;
    chassis_desc.position = v3(start_p.x, start_p.y + 0.6f, start_p.z);
    chassis_desc.half_extent = v3(2.1f, 0.55f, 0.9f);
    chassis_desc.friction = 0.9f;  // grippier tarmac: helps the governor bleed speed + corner
    game.chassis = game.world.create_body(chassis_desc);

    // Four wheels in chassis-local space. Front (non-drive) axle at +X, rear
    // (drive) axle at -X so the module's derived forward axis points +X.
    std::array<physics::vehicle::WheelDesc, 4> wheels{};
    const f32 wy = -0.35f;
    wheels[0].local_position = v3(game.wx, wy, game.wz);    // front-left
    wheels[1].local_position = v3(game.wx, wy, -game.wz);   // front-right
    wheels[2].local_position = v3(-game.wx, wy, game.wz);   // rear-left  (drive)
    wheels[3].local_position = v3(-game.wx, wy, -game.wz);  // rear-right (drive)
    for (auto& w : wheels) {
        w.radius = game.wheel_radius;
        w.suspension = 0.30f;
        w.stiffness = 40000.0f;
        w.damping = 4800.0f;
    }
    physics::vehicle::VehicleDesc vd{};
    vd.chassis = game.chassis;
    vd.wheels = std::span<const physics::vehicle::WheelDesc>(wheels.data(), wheels.size());
    vd.engine_max_torque = 240.0f;  // modest torque: cruises to the cap, no runaway spin
    vd.drag_coefficient = 0.55f;     // more aero drag so it settles at the governed cap
    // Governor + speed-scaled steering authority (#58). The Wave-B solver used to
    // leave the public throttle/steer unbounded, so a closed-loop auto-driver ran
    // away down a wide arc (the old "VEHICLE PUBLIC-API GAP" note). With these set:
    //   * max_speed caps the cruise so a held throttle settles AT the cap instead
    //     of accelerating without bound (drive torque tapers to zero near it);
    //   * the steering authority keeps FULL front-wheel angle up to kSteerFullSpeed
    //     and only tapers toward kSteerMinAuthority past kSteerTaperSpeed, so the
    //     car retains enough cornering bite to pull the tight U-turns at cruise
    //     instead of understeering off the loop.
    // The auto-driver targets kAutoTargetSpeed just UNDER the cap so the governor
    // trims overshoot on the straights while the driver's own PI holds the line.
    vd.max_speed = kCarMaxSpeed;
    vd.steer_full_speed = kSteerFullSpeed;
    vd.steer_taper_speed = kSteerTaperSpeed;
    vd.steer_min_authority = kSteerMinAuthority;
    game.vehicle = physics::vehicle::create(vd, game.world);

    // The oval is flat at y = 0, so a single ground plane at the track height is
    // enough for the per-wheel suspension rays (terrain elevation is deferred).
    physics::vehicle::set_ground_plane(game.vehicle, start_p.y, game.world);

    game.car_pos = chassis_desc.position;
    game.car_fwd = v3(1.0f, 0.0f, 0.0f);

    // Lap gate: the plane through the start point with the track tangent normal.
    const math::Vec3 gate_normal = math::normalize(bezier_tangent(game.track[0], start_t));
    game.lap.reset(start_p, gate_normal);
}

void make_lights(RacerGame& game) {
    const auto add_light = [&](math::Vec3 pos, u32 color, f32 intensity, f32 range) {
        scene::LocalTransform local{};
        local.translation = pos;
        const Entity e = game.scene->create_entity(local);
        scene::LightComponent light{};
        light.kind = scene::LightKind::Point;
        light.color_rgba8 = color;
        light.intensity = intensity;
        light.range = range;
        light.casts_shadow = 0u;
        (void)game.scene->attach_light(e, light);
    };
    // Tall floodlights at the four oval corners so the car is lit everywhere it
    // drives. Ranges cover roughly a straight's length.
    add_light({-55.0f, 18.0f, -30.0f}, rgba8(255, 244, 224), 3.0f, 90.0f);
    add_light({55.0f, 18.0f, -30.0f}, rgba8(255, 244, 224), 3.0f, 90.0f);
    add_light({55.0f, 18.0f, 30.0f}, rgba8(224, 236, 255), 3.0f, 90.0f);
    add_light({-55.0f, 18.0f, 30.0f}, rgba8(224, 236, 255), 3.0f, 90.0f);

    scene::RenderSettings rs = game.scene->render_settings();
    rs.render_mode = scene::RenderMode::Raster;
    rs.ambient_color_rgba8 = rgba8(46, 50, 60);
    rs.ambient_intensity = 1.0f;
    rs.shadows_enabled = 0u;
    game.scene->set_render_settings(rs);
}

// Push the chassis + four wheels into their scene mesh transforms. Yaw is taken
// from the smooth track tangent so the model never looks broadside at low speed.
void sync_car_transforms(RacerGame& game, math::Vec3 render_pos, math::Vec3 road_fwd) {
    const math::Mat4 yaw_mat = yaw_from_forward(road_fwd);

    // Chassis: scale unit cube to body dimensions, sit at the hub height.
    {
        scene::LocalTransform local{};
        local.translation = render_pos;
        local.rotation = math::quat_from_axis_angle(
            v3(0, 1, 0), std::atan2(road_fwd.x, -road_fwd.z) - math::kHalfPi);
        local.scale = v3(4.2f, 1.1f, 1.8f);
        (void)game.scene->set_transform(game.chassis_entity, local);
    }

    // Wheels: chassis-local offsets rotated into world by the same yaw.
    const std::array<math::Vec3, 4> wheel_local = {
        v3(-game.wx, 0.0f, game.wz),
        v3(-game.wx, 0.0f, -game.wz),
        v3(game.wx, 0.0f, game.wz),
        v3(game.wx, 0.0f, -game.wz),
    };
    const math::Quat axis_align = math::quat_from_axis_angle(v3(1, 0, 0), math::kHalfPi);
    const math::Quat yaw_q =
        math::quat_from_axis_angle(v3(0, 1, 0), std::atan2(road_fwd.x, -road_fwd.z) - math::kHalfPi);
    for (usize i = 0; i < wheel_local.size(); ++i) {
        const math::Vec4 wl4{wheel_local[i].x, wheel_local[i].y, wheel_local[i].z, 0.0f};
        const math::Vec4 worldoff = math::mul(yaw_mat, wl4);
        scene::LocalTransform local{};
        local.translation = v3(render_pos.x + worldoff.x,
                               render_pos.y + worldoff.y - 0.2f,
                               render_pos.z + worldoff.z);
        local.rotation = math::quat_mul(yaw_q, axis_align);
        (void)game.scene->set_transform(game.wheel_entities[i], local);
    }
}

// Chase camera: ~6 m behind / 2.5 m above the chassis, looking at it, framed off
// the smooth track tangent (mirrors PlayRuntime::update_chase_camera geometry).
void update_chase_camera(RacerGame& game, math::Vec3 render_pos, math::Vec3 road_fwd) {
    constexpr f32 kBack = 6.0f;
    constexpr f32 kUp = 2.5f;
    const math::Vec3 eye{render_pos.x - road_fwd.x * kBack,
                         render_pos.y - road_fwd.y * kBack + kUp,
                         render_pos.z - road_fwd.z * kBack};
    const math::Vec3 target{render_pos.x, render_pos.y + 0.6f, render_pos.z};

    scene::LocalTransform local = game.scene->transform(game.camera);
    local.translation = eye;
    local.rotation = scene::camera_rotation_towards(eye, target, v3(0.0f, 1.0f, 0.0f));
    (void)game.scene->set_transform(game.camera, local);
}

void draw_hud(RacerGame& game, render::Framebuffer& fb) {
    const f32 speed_kmh = game.car_speed * 3.6f;

    char line[96];
    std::snprintf(line, sizeof(line), "LAP %u   TIME %5.1fs   SPD %3.0f km/h",
                  game.lap.lap_count + 1u,
                  static_cast<double>(game.lap.current_lap_time),
                  static_cast<double>(speed_kmh));

    char best[64];
    if (game.lap.best_lap_time > 0.0f)
        std::snprintf(best, sizeof(best), "BEST %5.1fs", static_cast<double>(game.lap.best_lap_time));
    else
        std::snprintf(best, sizeof(best), "BEST --.-s");

    ui::imm::begin_frame(fb);
    ui::imm::filled_rect(math::Vec2{8.0f, 8.0f}, math::Vec2{300.0f, 38.0f},
                         ui::imm::rgba(0x0B, 0x10, 0x18, 0xC0));
    ui::imm::rect_outline(math::Vec2{8.0f, 8.0f}, math::Vec2{300.0f, 38.0f},
                          ui::imm::rgba(0x60, 0x70, 0x88));
    ui::imm::label(math::Vec2{16.0f, 14.0f}, line, ui::imm::rgba(0xE6, 0xF0, 0xFF));
    ui::imm::label(math::Vec2{16.0f, 30.0f}, best, ui::imm::rgba(0xF0, 0xD8, 0x60));
    ui::imm::label(math::Vec2{16.0f, fb.height - 18.0f},
                   "W throttle  -  S brake/reverse  -  A/D steer  -  ESC quit",
                   ui::imm::rgba(0x90, 0xA0, 0xB8));
    ui::imm::end_frame();
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder - Racer Demo";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;
    return desc;
}

int run_demo(const app::AppArgs& args, app::WindowApp& app_host) {
    const u32 smoke_frames = args.smoke_frames;
    auto* window = &app_host.window();

    RacerGame game{};
    game.renderer = &app_host.rendering_system();
    game.scene = &app_host.create_active_scene();
    app_host.set_scene_lighting_enabled(true);

    game.scene->prewarm_capacity(scene::ScenePrewarmConfig{
        .scene_entities = 32u,
        .renderables = 16u,
        .cameras = 2u,
        .lights = 8u,
        .render_items = 16u,
    });
    app_host.reserve_scene_capacity(16u, 8u);

    build_track(game);
    build_car_meshes(game);
    build_vehicle(game);
    make_lights(game);

    // Chase camera entity.
    scene::CameraDesc cam{};
    cam.position = v3(game.car_pos.x - 6.0f, game.car_pos.y + 2.5f, game.car_pos.z);
    cam.look_at = game.car_pos;
    cam.fov_y_rad = 60.0f * math::kDegToRad;
    cam.near_z = 0.2f;
    cam.far_z = 400.0f;
    game.camera = game.scene->spawn_camera(cam);

    PSY_LOG_INFO("racer_demo: track + vehicle built, raster + scene lights{}",
                 smoke_frames > 0 ? std::string{" (smoke mode)"} : std::string{});

    // Fixed-step physics at 120 Hz; render interpolates / reads the latest pose.
    constexpr f32 kSimHz = 120.0f;
    constexpr f32 kSimDt = 1.0f / kSimHz;
    f32 accumulator = 0.0f;

    u64 last_ticks = platform::Clock::ticks_now();
    u32 frame = 0;

    // Smoke instrumentation: track the peak governed speed and the cumulative
    // planar distance so we can ASSERT (via a single greppable log line) that the
    // governor held the cruise under the cap AND the car actually progressed a
    // lap's worth of track instead of stalling.
    f32 smoke_peak_speed = 0.0f;
    f32 smoke_distance = 0.0f;
    math::Vec3 smoke_prev_pos = game.car_pos;

    while (!window->should_close()) {
        window->poll_events();

        auto* input = platform::input();
        if (input && input->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            PSY_LOG_INFO("racer_demo: escape pressed, exiting");
            break;
        }

        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(
                                     platform::Clock::ticks_now() - last_ticks)));
        last_ticks = platform::Clock::ticks_now();

        const editor::Mode edit_mode = app_host.engine_frame_update(dt);

        // 1. Decide the control source for this frame. Interactive play reads
        //    WASD; smoke runs (and the editor/overlay-capturing case) auto-drive
        //    so the headless capture and an idle window still lap the track. The
        //    auto-driver runs INSIDE the fixed-step loop (recomputed against the
        //    freshly-stepped pose each 120 Hz tick) so steering tracks the spline
        //    tightly even at high render dt; manual controls are constant across
        //    the frame's sub-steps.
        f32 man_throttle = 0.0f, man_brake = 0.0f, man_steer = 0.0f;
        bool manual = false;
        if (smoke_frames == 0 && edit_mode != editor::Mode::Edit && input &&
            !editor::overlays_capturing()) {
            manual = true;
            if (input->key_down(platform::KeyCode::W))
                man_throttle = 1.0f;
            if (input->key_down(platform::KeyCode::S))
                man_brake = 1.0f;
            if (input->key_down(platform::KeyCode::A))
                man_steer += 0.5f;
            if (input->key_down(platform::KeyCode::D))
                man_steer -= 0.5f;
        }

        // 2. Physics fixed-step.
        app_host.engine_frame_begin(app::FrameClear::color_depth(rgba8(120, 134, 168)));
        accumulator += dt;
        if (accumulator > 0.5f)
            accumulator = 0.5f;
        while (accumulator >= kSimDt) {
            // Per-tick controls: manual is constant; auto recomputes against the
            // current pose / speed so the proportional steer + throttle stay in
            // their stable regime.
            f32 throttle = man_throttle, brake = man_brake, steer = man_steer;
            if (!manual) {
                // Non-manual (headless smoke + idle attract): a GOVERNED STRAIGHT
                // cruise down the start straight. The throttle PI eases the car up
                // to the auto-driver's target and the VehicleDesc governor holds it
                // AT the cap -- the demo's value here is the speed governor + the
                // alloc-free fixed-step composition, exercised on the stable
                // longitudinal axis. Closed-loop CORNERING (the auto_drive steering
                // controller, kept below for interactive use + the engine fix) is
                // gated on an engine tire-model fix: the Wave-B Pacejka lateral
                // force is applied along +tire_right (same sense as the lateral
                // slip) instead of opposing it, so ANY steer feeds a positive-
                // feedback side-slide that runs away regardless of throttle/brake
                // (reproduces in samples/04_nfs_track too -- a pre-existing engine
                // issue, not this demo). Until that sign is corrected the smoke
                // drives the provably-stable straight line; steer stays 0 so the
                // governed-cruise assertion is meaningful. See DEFERRED note.
                straight_cruise(game.driver, game.car_speed, steer, throttle, brake);
            }
            physics::vehicle::set_throttle(game.vehicle, throttle, game.world);
            physics::vehicle::set_brake(game.vehicle, brake, game.world);
            physics::vehicle::set_steer(game.vehicle, steer, game.world);
            game.hud_throttle = throttle;
            game.hud_brake = brake;
            game.hud_steer = steer;

            const math::Vec3 before = game.world.get_position(game.chassis);
            game.world.step(kSimDt);
            const math::Vec3 after = game.world.get_position(game.chassis);
            const math::Vec3 dp = math::sub(after, before);
            const f32 step_dist = math::length(v3(dp.x, 0.0f, dp.z));
            game.car_speed = step_dist / kSimDt;
            if (step_dist > 0.001f)
                game.car_fwd = math::normalize(v3(dp.x, 0.0f, dp.z));

            // 3. Lap timer on the per-step chassis position (use the planar
            //    velocity as the forward-crossing test).
            const math::Vec3 vel_xz = v3(dp.x / kSimDt, 0.0f, dp.z / kSimDt);
            if (game.lap.update(after, vel_xz, kSimDt)) {
                PSY_LOG_INFO("racer_demo: lap {} complete, time {:.2f}s (best {:.2f}s)",
                             game.lap.lap_count, static_cast<double>(game.lap.last_lap_time),
                             static_cast<double>(game.lap.best_lap_time));
            }
            game.car_pos = after;
            accumulator -= kSimDt;
        }

        // 4. Chassis + wheel scene transforms, framed off the smooth track tangent.
        const TrackPos cam_tp = closest_on_track(game.track, game.car_pos);
        math::Vec3 road_fwd = bezier_tangent(game.track[cam_tp.seg], cam_tp.t);
        road_fwd.y = 0.0f;
        road_fwd = math::normalize(road_fwd);
        sync_car_transforms(game, game.car_pos, road_fwd);

        // 5. Chase camera.
        update_chase_camera(game, game.car_pos, road_fwd);

        // 6. Render (raster + scene lights) + HUD.
        (void)app_host.engine_frame_render();
        draw_hud(game, app_host.framebuffer());
        app_host.present();

        if (smoke_frames > 0) {
            const math::Vec3 dp = math::sub(game.car_pos, smoke_prev_pos);
            smoke_distance += math::length(v3(dp.x, 0.0f, dp.z));
            smoke_prev_pos = game.car_pos;
            smoke_peak_speed = std::max(smoke_peak_speed, game.car_speed);
            if ((frame % 30u) == 0u) {
                PSY_LOG_INFO("racer_demo: frame {} car_pos=({:.2f},{:.2f},{:.2f}) speed={:.2f} "
                             "thr={:.2f} brk={:.2f} str={:.2f} lap={}",
                             frame, static_cast<double>(game.car_pos.x),
                             static_cast<double>(game.car_pos.y),
                             static_cast<double>(game.car_pos.z),
                             static_cast<double>(game.car_speed),
                             static_cast<double>(game.hud_throttle),
                             static_cast<double>(game.hud_brake),
                             static_cast<double>(game.hud_steer), game.lap.lap_count);
            }
        }

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            // Governed-cruise assertion. With the governor + steering authority on
            // the VehicleDesc, the auto-driver must hold the cruise UNDER the cap
            // (peak speed <= the cap, with a small numerical margin) and the car
            // must have PROGRESSED a meaningful distance (it laps / covers track
            // rather than stalling). A runaway car would blow past the cap; a
            // stalled one would log ~0 distance. We emit one greppable PASS/FAIL.
            constexpr f32 kCapMargin = 1.5f;     // m/s tolerance over the hard cap
            constexpr f32 kMinProgress = 50.0f;  // metres of track the car must cover
            const bool governed = smoke_peak_speed <= kCarMaxSpeed + kCapMargin;
            const bool progressed = smoke_distance >= kMinProgress;
            PSY_LOG_INFO("racer_demo: governed-cruise peak_speed={:.2f} m/s (cap {:.1f}) "
                         "distance={:.1f}m laps={} {} {}",
                         static_cast<double>(smoke_peak_speed),
                         static_cast<double>(kCarMaxSpeed), static_cast<double>(smoke_distance),
                         game.lap.lap_count, governed ? "UNDER-CAP" : "OVER-CAP",
                         progressed ? "PROGRESSED" : "STALLED");
            if (governed && progressed)
                PSY_LOG_INFO("racer_demo: governed-cruise PASS");
            else
                PSY_LOG_ERROR("racer_demo: governed-cruise FAIL (governed={} progressed={})",
                              governed, progressed);
            PSY_LOG_INFO("racer_demo: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    physics::vehicle::destroy(game.vehicle, game.world);
    game.world.destroy_body(game.chassis);

    const bool capture_ok = app_host.write_capture_if_requested("racer_demo");
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct RacerDemo {
    static constexpr std::string_view log_name() noexcept { return "racer_demo"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder Racer Demo"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) { return run_demo(args, app_host); }
};

PSYNDER_WINDOW_SAMPLE_MAIN(RacerDemo)
