// SPDX-License-Identifier: MIT
// Psynder — Sample 04 / M4 demo. NFS-style chase-cam lap.
//
// A vehicle drives a closed oval test track that is built in code from four
// cubic Bezier road segments. The track is extruded into a textured strip
// (per-vertex coloured) and submitted to the rasterizer; the car is a simple
// box chassis with cylinder wheels. An auto-driver PID aims the vehicle at a
// look-ahead point along the spline and modulates throttle to maintain a
// target speed. The chase camera sits ~5m behind and ~2m above the car and
// looks at the chassis.
//
// Architecture notes:
//   * Track geometry: cubic Bezier evaluation is inlined here so the sample
//     only depends on the public `psynder::world::outdoor::SplineRoadSegment`
//     struct from Terrain.h. The four segments share endpoints so the loop
//     is C0-continuous; control-point pairs across the join are co-linear so
//     it's also C1-continuous (visually smooth, no kinks).
//   * Vehicle physics: created through the public `psynder::physics::vehicle`
//     API. The body is a Box mass of ~1500kg with four wheels at the corners
//     of the chassis. The driver sets throttle/brake/steer each render frame;
//     the physics module reads those values and steps under Pacejka tires
//     and a 6-speed gearbox (Wave-B landed those in the engine).
//   * Fixed-step: physics runs at 120 Hz regardless of render frame rate;
//     render interpolates between the previous and current sim sample for a
//     smooth chassis pose. Smoke runs use a deterministic per-frame dt so
//     captures match across hosts.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Same, space-separated form.
//   --smoke-capture-out PATH Write the last rendered framebuffer to PATH
//                            as a 24-bit RGB PNG. Used by the golden-cell
//                            harness.

#include "common/MeshWinding.h"
#include "common/PngWriter.h"

#include "asset/Vfs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "ui/rml/DataBind.h"
#include "ui/rml/Rml.h"
#include "world/outdoor/Terrain.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// ── Wave-E HUD live binding ───────────────────────────────────────────────
//
// Wave-D plumbed the HUD via a test-only reload hack
// (`test_only::reload_with_source`) because lane 17's in-tree RML/RCSS
// subset shipped no per-element setters.  Wave-E adds a real public
// surface in `engine/ui/rml/DataBind.h` (`set_element_text` +
// `set_element_attribute`); the sample now drives the HUD through those
// instead, with no per-frame source rebuild.  The same calls route
// through upstream RmlUi when `PSYNDER_VENDOR_RMLUI=ON` flips the
// vendor bring-up on.

using namespace psynder;

namespace {

// ─── CLI ─────────────────────────────────────────────────────────────────
struct Args {
    u32 smoke_frames = 0;
    std::string capture_out;
};

u32 parse_uint(std::string_view v) noexcept {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9')
            return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kSmoke = "--smoke-frames=";
    constexpr std::string_view kSmokeSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kSmoke)) {
            a.smoke_frames = parse_uint(s.substr(kSmoke.size()));
        } else if (s == kSmokeSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

// ─── Math helpers ────────────────────────────────────────────────────────
constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

inline math::Vec3 v3(f32 x, f32 y, f32 z) noexcept {
    return {x, y, z};
}

inline math::Vec3 lerp_v3(math::Vec3 a, math::Vec3 b, f32 t) noexcept {
    return v3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

// Cubic Bezier evaluation. p0..p3 are the segment's control points; t∈[0,1].
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

// Derivative wrt t — unnormalised tangent.
inline math::Vec3 bezier_tangent(const world::outdoor::SplineRoadSegment& s, f32 t) noexcept {
    const f32 u = 1.0f - t;
    const f32 a = 3.0f * u * u;
    const f32 b = 6.0f * u * t;
    const f32 c = 3.0f * t * t;
    return v3(a * (s.p1.x - s.p0.x) + b * (s.p2.x - s.p1.x) + c * (s.p3.x - s.p2.x),
              a * (s.p1.y - s.p0.y) + b * (s.p2.y - s.p1.y) + c * (s.p3.y - s.p2.y),
              a * (s.p1.z - s.p0.z) + b * (s.p2.z - s.p1.z) + c * (s.p3.z - s.p2.z));
}

// Frenet-ish frame in the XZ-driving plane. Right is the tangent rotated -90°
// about +Y; up is +Y rotated about the tangent by `banking_rad`.
inline void frame_at(const world::outdoor::SplineRoadSegment& seg,
                     f32 t,
                     math::Vec3& right_out,
                     math::Vec3& up_out) noexcept {
    const math::Vec3 tan = math::normalize(bezier_tangent(seg, t));
    math::Vec3 right = math::normalize(v3(tan.z, 0.0f, -tan.x));
    math::Vec3 up = v3(0.0f, 1.0f, 0.0f);
    const f32 cs = std::cos(seg.banking_rad);
    const f32 sn = std::sin(seg.banking_rad);
    right_out = math::normalize(
        v3(cs * right.x + sn * up.x, cs * right.y + sn * up.y, cs * right.z + sn * up.z));
    up_out = math::normalize(math::cross(right_out, tan));
}

// ─── Track ───────────────────────────────────────────────────────────────
constexpr f32 kTrackHalfWidth = 6.0f;

// Build a closed oval track in the XZ plane, centered at the origin.
// The loop is four cubic Beziers: two straights along +X and -X with two
// half-loop curves at the ends. Endpoint tangents are co-linear across the
// joins so the path is C1-continuous and the auto-driver can chain segments
// without a step in steering input.
std::vector<world::outdoor::SplineRoadSegment> build_oval_track() {
    std::vector<world::outdoor::SplineRoadSegment> out;
    out.reserve(4);

    // Geometry parameters — keep the loop compact so the car laps quickly in
    // smoke-run modes (we want a few segment transitions in a handful of
    // seconds at the auto-driver's target speed).
    constexpr f32 sx = 50.0f;    // straight half-length on X
    constexpr f32 sz = 25.0f;    // curve half-depth on Z
    constexpr f32 bank = 0.10f;  // ~6° outward banking on the turns

    // Segment 0: straight, +X direction along z = -sz.
    out.push_back({
        v3(-sx, 0.0f, -sz),
        v3(-sx * 0.33f, 0.0f, -sz),
        v3(sx * 0.33f, 0.0f, -sz),
        v3(sx, 0.0f, -sz),
        kTrackHalfWidth,
        0.0f,
    });

    // Segment 1: U-turn at +X. p1 keeps the +X tangent from seg0; p2 sets up
    // the -X tangent for seg2. Control points pushed in Z to bulge out into
    // a half-circle.
    const f32 cz = sz * 1.8f;
    out.push_back({
        v3(sx, 0.0f, -sz),
        v3(sx + cz, 0.0f, -sz),
        v3(sx + cz, 0.0f, sz),
        v3(sx, 0.0f, sz),
        kTrackHalfWidth,
        bank,
    });

    // Segment 2: straight, -X direction along z = +sz.
    out.push_back({
        v3(sx, 0.0f, sz),
        v3(sx * 0.33f, 0.0f, sz),
        v3(-sx * 0.33f, 0.0f, sz),
        v3(-sx, 0.0f, sz),
        kTrackHalfWidth,
        0.0f,
    });

    // Segment 3: U-turn at -X.
    out.push_back({
        v3(-sx, 0.0f, sz),
        v3(-sx - cz, 0.0f, sz),
        v3(-sx - cz, 0.0f, -sz),
        v3(-sx, 0.0f, -sz),
        kTrackHalfWidth,
        bank,
    });

    return out;
}

// Tessellate the closed track into a single (vertex, index) pair the
// rasterizer can submit. Two vertices per spline sample (left + right edge)
// and two triangles per spline interval.
struct TrackMesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

TrackMesh tessellate_track(const std::vector<world::outdoor::SplineRoadSegment>& segs) {
    TrackMesh m;
    constexpr u32 kSamplesPerSeg = 24;
    const u32 total_samples = static_cast<u32>(segs.size()) * kSamplesPerSeg;
    m.verts.reserve(static_cast<usize>(total_samples) * 2u);
    m.indices.reserve(static_cast<usize>(total_samples) * 6u);

    // Tarmac shade alternates per segment between two near-blacks to give the
    // viewer a "section change" cue across the oval.
    const std::array<u32, 2> kTarmac = {
        pack_rgba(64, 64, 70, 255),  // base asphalt
        pack_rgba(80, 80, 86, 255),  // slightly lighter band
    };
    const u32 kLine = pack_rgba(210, 200, 90, 255);  // racing line (kerb)

    for (usize si = 0; si < segs.size(); ++si) {
        const auto& seg = segs[si];
        for (u32 i = 0; i < kSamplesPerSeg; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kSamplesPerSeg - 1);
            const math::Vec3 p = bezier_eval(seg, t);
            math::Vec3 right, up;
            frame_at(seg, t, right, up);

            const math::Vec3 left_pos = math::sub(p, math::mul(right, seg.half_width));
            const math::Vec3 right_pos = math::add(p, math::mul(right, seg.half_width));

            // Vertex colour: kerb stripe on the outer edge every few samples
            // (decorative — gives the camera something to flicker past).
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

    // Indices: two triangles per quad between consecutive sample pairs. The
    // last sample of one segment shares an index slot but not a vertex with
    // the first sample of the next; we still emit a quad bridging them so
    // the track surface closes around the loop. The very last sample wraps
    // to the very first (closed loop).
    const u32 n_pairs = static_cast<u32>(m.verts.size() / 2u);
    for (u32 i = 0; i < n_pairs; ++i) {
        const u32 j = (i + 1u) % n_pairs;  // next pair, wraps at end
        const u32 a = i * 2u + 0u;         // left  this
        const u32 b = i * 2u + 1u;         // right this
        const u32 c = j * 2u + 0u;         // left  next
        const u32 d = j * 2u + 1u;         // right next
        m.indices.push_back(a);
        m.indices.push_back(c);
        m.indices.push_back(d);
        m.indices.push_back(a);
        m.indices.push_back(d);
        m.indices.push_back(b);
    }
    return m;
}

// ─── Auto-driver ─────────────────────────────────────────────────────────
//
// Holds the current segment index and the parameter t along that segment.
// The driver picks a look-ahead point a fixed arc-distance ahead of the
// car and steers toward it (proportional-only — derivative is implicit in
// the steady advance of the look-ahead point). Throttle is a P controller
// on a target speed.
struct Driver {
    f32 target_speed_mps = 20.0f;
    f32 look_ahead_m = 14.0f;
    f32 steer_gain = 1.4f;  // rad/rad heading error
    f32 throttle_kp = 0.25f;
};

// Find the segment index + t closest to a world-space query point. We scan a
// small window forward from the previous best to keep this O(N) per tick;
// the loop is short (4 segments × 24 samples) so a full scan is also fine.
struct TrackPos {
    u32 seg;
    f32 t;
    math::Vec3 p;
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

// Advance a (segment, t, distance) lookahead point along the closed loop.
TrackPos advance_along(const std::vector<world::outdoor::SplineRoadSegment>& segs,
                       u32 seg,
                       f32 t,
                       f32 advance_m) noexcept {
    // Walk forward in small parameter increments until we have covered
    // `advance_m` of arc. Crude but cheap and good enough for steering.
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

// ─── Simple primitives for the car ───────────────────────────────────────
constexpr u32 kColChassis = pack_rgba(190, 50, 50);  // red body
constexpr u32 kColTop = pack_rgba(150, 35, 35);      // darker roof
constexpr u32 kColWheel = pack_rgba(30, 30, 32);     // tyre black

// Unit cube (per-face colour). Identical to sample 02's layout — kept local
// so this sample stays self-contained.
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

// Low-poly cylinder for wheels. Built once at startup; oriented so its
// rotation axis is along local Y (so the wheel's spin axis aligns with the
// chassis local X after a 90° model rotation).
struct CylinderMesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

CylinderMesh build_cylinder(f32 radius, f32 half_height, u32 sides = 12) {
    CylinderMesh c;
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
    // Endcaps: top & bottom hub vertices.
    const u32 hub_top = static_cast<u32>(c.verts.size());
    c.verts.push_back({v3(0, half_height, 0), v3(0, 1, 0), {0.5f, 0.5f}, {0, 0}, kColWheel});
    const u32 hub_bot = static_cast<u32>(c.verts.size());
    c.verts.push_back({v3(0, -half_height, 0), v3(0, -1, 0), {0.5f, 0.5f}, {0, 0}, kColWheel});

    for (u32 i = 0; i < sides; ++i) {
        const u32 j = (i + 1u) % sides;
        const u32 t0 = i * 2u + 0u, b0 = i * 2u + 1u;
        const u32 t1 = j * 2u + 0u, b1 = j * 2u + 1u;
        // Side quad (two triangles)
        c.indices.push_back(t0);
        c.indices.push_back(t1);
        c.indices.push_back(b1);
        c.indices.push_back(t0);
        c.indices.push_back(b1);
        c.indices.push_back(b0);
        // Top cap fan
        c.indices.push_back(hub_top);
        c.indices.push_back(t1);
        c.indices.push_back(t0);
        // Bottom cap fan
        c.indices.push_back(hub_bot);
        c.indices.push_back(b0);
        c.indices.push_back(b1);
    }
    return c;
}

// ─── Camera + framebuffer helpers ────────────────────────────────────────
void clear_depth_far(render::Framebuffer& fb) noexcept {
    if (!fb.depth)
        return;
    u32 packed = 0;
    const f32 one = 1.0f;
    std::memcpy(&packed, &one, sizeof(packed));
    packed &= 0xFFFFFF00u;
    const usize n = static_cast<usize>(fb.width) * fb.height;
    for (usize i = 0; i < n; ++i)
        fb.depth[i] = packed;
}

// ─── HUD live-binding helpers ────────────────────────────────────────────
//
// Vehicle telemetry pushed into hud.rml each frame.  Speed comes from the
// chassis position delta the sim already computes (`car_speed`, m/s).  The
// public physics API doesn't expose `engine_rpm()` so we derive an RPM
// estimate from wheel ω: wheel ω ≈ v / r, and engine ω ≈ wheel ω × gear ×
// final, mapped to rpm.  Numbers are calibrated to feel right against the
// in-engine 6-speed gearbox (idle 800, redline 7000); they don't need to be
// physically exact — the M7 milestone is the *binding wiring*, not a
// dyno-accurate cluster.
struct HudTelemetry {
    f32 speed_mps = 0.0f;
    f32 throttle = 0.0f;       // 0..1
    f32 brake = 0.0f;          // 0..1
    f32 steer = 0.0f;          // rad (unused for visuals, logged)
    f32 wheel_radius = 0.35f;  // m
};

// Derive an engine-RPM estimate from forward speed.  Picks a sensible gear
// from the current speed (1st ~0-12 m/s, 2nd ~12-20, 3rd ~20-30, ...) so
// the readout climbs and resets across shifts the way a real cluster does.
// Returns (rpm, gear_index_1to6) — gear_index 0 is neutral (idle), -1 reverse.
struct EngineEstimate {
    f32 rpm;
    i32 gear;
};

EngineEstimate estimate_engine(f32 speed_mps, f32 throttle, f32 wheel_radius) noexcept {
    constexpr f32 kIdle = 800.0f;
    constexpr f32 kRedline = 7000.0f;
    constexpr f32 kFinal = 3.7f;
    // Gear ratios: gentle slope, 6-speed pattern.
    constexpr std::array<f32, 6> kGearRatio = {3.5f, 2.4f, 1.8f, 1.4f, 1.1f, 0.9f};

    if (speed_mps < 0.5f) {
        const f32 rpm = kIdle + throttle * 1200.0f;
        return EngineEstimate{rpm, throttle > 0.05f ? 1 : 0};
    }

    // Pick gear by speed band so each gear holds ~3000-rpm window before a
    // visual shift up.  Bands chosen so 6th is reached above ~50 m/s.
    i32 gear = 1;
    if (speed_mps > 50.0f)
        gear = 6;
    else if (speed_mps > 38.0f)
        gear = 5;
    else if (speed_mps > 28.0f)
        gear = 4;
    else if (speed_mps > 19.0f)
        gear = 3;
    else if (speed_mps > 11.0f)
        gear = 2;
    else
        gear = 1;

    const f32 wheel_omega = speed_mps / wheel_radius;  // rad/s
    const f32 engine_omega = wheel_omega * kGearRatio[gear - 1] * kFinal;
    f32 rpm = engine_omega * 60.0f / math::kTwoPi;
    if (rpm < kIdle)
        rpm = kIdle;
    if (rpm > kRedline)
        rpm = kRedline;
    return EngineEstimate{rpm, gear};
}

// Push current telemetry into the HUD document via the public DataBind
// setters.  The static structure (speed panel, rpm bar, pedal indicators)
// is defined in `assets/hud.rml` and loaded once at startup; every frame
// we update only the fields that change — the text run on the speed +
// gear elements, and the `style="..."` attribute on the bars whose
// height/width represent the value.  No per-frame source rebuild,
// no test-layer hooks.
void push_hud_telemetry(f32 speed_mps, f32 rpm, i32 gear, f32 throttle, f32 brake) {
    const u32 speed_kmh = static_cast<u32>(std::round(speed_mps * 3.6f));
    // RPM bar: scale 800..7000 → 0..340 px (matches width in hud.rcss).
    constexpr f32 kIdle = 800.0f;
    constexpr f32 kRedline = 7000.0f;
    constexpr u32 kBarW = 340u;
    f32 rpm_norm = (rpm - kIdle) / (kRedline - kIdle);
    if (rpm_norm < 0.0f)
        rpm_norm = 0.0f;
    if (rpm_norm > 1.0f)
        rpm_norm = 1.0f;
    const u32 rpm_fill_px = static_cast<u32>(rpm_norm * static_cast<f32>(kBarW));

    constexpr u32 kPedalH = 170u;
    const u32 thr_fill_px = static_cast<u32>(std::min(1.0f, throttle) * static_cast<f32>(kPedalH));
    const u32 brk_fill_px = static_cast<u32>(std::min(1.0f, brake) * static_cast<f32>(kPedalH));
    // Throttle/brake bars grow from the bottom: anchor top = 30 + (H - fill).
    const u32 thr_top = 30u + (kPedalH - thr_fill_px);
    const u32 brk_top = 30u + (kPedalH - brk_fill_px);

    char gear_ch = 'N';
    if (gear == -1)
        gear_ch = 'R';
    else if (gear == 0)
        gear_ch = 'N';
    else if (gear >= 1 && gear <= 6)
        gear_ch = static_cast<char>('0' + gear);

    // Format the numeric readouts into small stack buffers so we don't
    // allocate on the hot path.  `set_element_text` copies the bytes.
    char speed_buf[16];
    std::snprintf(speed_buf, sizeof(speed_buf), "%u", speed_kmh);
    const char gear_str[2] = {gear_ch, '\0'};

    psynder::ui::rml::set_element_text("hud", "speed-value", speed_buf);
    psynder::ui::rml::set_element_text("hud", "gear-letter", gear_str);

    // Bars drive their geometry via the `style` attribute.  The setter
    // re-parses the inline style and re-cascades the document so
    // `computed_style.width` / `top` / `height` reflect the new pixel
    // count for the next render() pass.
    char rpm_style[32];
    std::snprintf(rpm_style, sizeof(rpm_style), "width:%u", rpm_fill_px);
    psynder::ui::rml::set_element_attribute("hud", "rpm-fill", "style", rpm_style);

    char thr_style[48];
    std::snprintf(thr_style, sizeof(thr_style), "top:%u; height:%u", thr_top, thr_fill_px);
    psynder::ui::rml::set_element_attribute("hud", "thr-fill", "style", thr_style);

    char brk_style[48];
    std::snprintf(brk_style, sizeof(brk_style), "top:%u; height:%u", brk_top, brk_fill_px);
    psynder::ui::rml::set_element_attribute("hud", "brk-fill", "style", brk_style);
}

// Build a chassis-local frame from a forward (XZ) vector.
inline math::Mat4 yaw_from_forward(math::Vec3 fwd) noexcept {
    fwd.y = 0.0f;
    fwd = math::normalize(fwd);
    // The chassis's long axis (4.2 vs 1.8) and its front wheels both sit on
    // local X, so the car must drive along X — not -Z. atan2(fwd.x, -fwd.z)
    // aligns local -Z with travel; the extra quarter turn rolls that onto the
    // long -X axis so the chassis points where it drives instead of broadside
    // (front wheels, at local -X, lead). The wheels share this matrix, so the
    // whole assembly rotates together and stays attached.
    const f32 yaw = std::atan2(fwd.x, -fwd.z) - math::kHalfPi;
    return math::rotate_quat(math::quat_from_axis_angle(v3(0, 1, 0), yaw));
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    // ─── Platform / framebuffer ─────────────────────────────────────────
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 04 (NFS track lap)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_04: failed to create window");
        return EXIT_FAILURE;
    }

    std::vector<u32> pixels(static_cast<usize>(desc.render_width) * desc.render_height, 0);
    std::vector<u32> depth(static_cast<usize>(desc.render_width) * desc.render_height, 0);

    render::Framebuffer fb{};
    fb.width = desc.render_width;
    fb.height = desc.render_height;
    fb.pitch = desc.render_width * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(pixels.data());
    fb.depth = depth.data();

    auto& rasterizer = render::raster::Rasterizer::Get();

    // ─── Track build ────────────────────────────────────────────────────
    const auto track_segs = build_oval_track();
    TrackMesh track_mesh = tessellate_track(track_segs);
    // The rasterizer back-face culls by default; the track ribbon, chassis cube
    // and wheel cylinders are wound from their per-vertex normals so none of
    // them drop out (the track in particular faces straight up and was being
    // culled to the sky colour). The cube winding is shared, so rewind it once.
    samples::fix_winding(track_mesh.verts.data(),
                         track_mesh.indices.data(),
                         static_cast<u32>(track_mesh.indices.size()));
    std::array<u32, kCubeIndices.size()> cube_idx = kCubeIndices;
    samples::fix_winding(kCubeVerts.data(), cube_idx.data(), static_cast<u32>(cube_idx.size()));

    // ─── Vehicle ────────────────────────────────────────────────────────
    auto& world = physics::World::Get();
    world.set_gravity(v3(0.0f, -9.81f, 0.0f));

    // Starting pose: midway along segment 0 (the back straight), facing +X.
    const f32 start_t = 0.05f;
    const math::Vec3 start_p = bezier_eval(track_segs[0], start_t);

    physics::BodyDesc chassis_desc{};
    chassis_desc.shape = physics::Shape::Box;
    chassis_desc.mass = 1500.0f;
    chassis_desc.position = v3(start_p.x, start_p.y + 0.6f, start_p.z);
    chassis_desc.half_extent = v3(2.1f, 0.55f, 0.9f);  // length × height × width
    chassis_desc.friction = 0.5f;
    const physics::BodyId chassis = world.create_body(chassis_desc);

    // Four wheels — locations are in chassis-local space. Front wheels are
    // toward -X in chassis-local because the chassis faces local -Z and the
    // cube model is rolled so its long axis is X; we keep "front" semantic
    // for the drive flag (rear-wheel drive).
    std::array<physics::vehicle::WheelDesc, 4> wheels{};
    const f32 wx = 1.45f, wz = 0.85f, wy = -0.35f;
    wheels[0].local_position = v3(-wx, wy, wz);   // front-left
    wheels[1].local_position = v3(-wx, wy, -wz);  // front-right
    wheels[2].local_position = v3(wx, wy, wz);    // rear-left
    wheels[3].local_position = v3(wx, wy, -wz);   // rear-right
    for (auto& w : wheels) {
        w.radius = 0.35f;
        w.suspension = 0.30f;
        w.stiffness = 40000.0f;
        w.damping = 4800.0f;
    }
    physics::vehicle::VehicleDesc vd{};
    vd.chassis = chassis;
    vd.wheels = std::span<const physics::vehicle::WheelDesc>(wheels.data(), wheels.size());
    vd.engine_max_torque = 450.0f;
    vd.drag_coefficient = 0.30f;
    const physics::vehicle::VehicleId veh = physics::vehicle::create(vd);

    // The oval track lies flat at y = 0 (every Bezier control point has
    // y = 0), so the per-wheel suspension rays contact a single flat ground
    // plane at the track height. Without this the wheels never find ground
    // and the chassis falls straight through under gravity alone.
    physics::vehicle::set_ground_plane(veh, start_p.y);

    // ─── Wheel mesh ─────────────────────────────────────────────────────
    CylinderMesh wheel_mesh = build_cylinder(0.35f, 0.18f);
    samples::fix_winding(wheel_mesh.verts.data(),
                         wheel_mesh.indices.data(),
                         static_cast<u32>(wheel_mesh.indices.size()));

    // ─── Sim state for interp ───────────────────────────────────────────
    math::Vec3 prev_pos = chassis_desc.position;
    math::Quat prev_rot = chassis_desc.rotation;
    math::Vec3 cur_pos = prev_pos;
    math::Quat cur_rot = prev_rot;
    math::Vec3 car_fwd = v3(1.0f, 0.0f, 0.0f);  // initial heading: +X
    f32 car_speed = 0.0f;
    // Last-tick driver outputs — surfaced to the HUD so the pedal indicators
    // reflect what the auto-driver is asking for, not the noisy chassis
    // accelerations.
    f32 hud_throttle = 0.0f;
    f32 hud_brake = 0.0f;
    f32 hud_steer = 0.0f;

    Driver driver{};

    // ─── HUD / RmlUi (Wave-D M7) ────────────────────────────────────────
    // Mount the sample's `assets/` directory under the asset VFS so the
    // engine-side RmlUi binding can read hud.rml and hud.rcss through the
    // same surface a future .lmpak distribution would use.  The compile-time
    // PSY_SAMPLE_04_ASSET_DIR define is wired by CMakeLists.txt.
    bool hud_active = false;
    if (psynder::ui::rml::initialize()) {
#ifdef PSY_SAMPLE_04_ASSET_DIR
        const std::string_view kAssetDir = PSY_SAMPLE_04_ASSET_DIR;
        const bool mounted = psynder::asset::Vfs::Get().mount_directory(kAssetDir);
        if (!mounted) {
            PSY_LOG_WARN("sample_04: failed to mount HUD asset dir {}", kAssetDir);
        }
#endif
        if (psynder::ui::rml::load_document("hud.rml", "hud")) {
            psynder::ui::rml::show("hud");
            hud_active = true;
            PSY_LOG_INFO("sample_04: HUD loaded (hud.rml + hud.rcss)");
        } else {
            PSY_LOG_WARN(
                "sample_04: RmlUi load_document(\"hud.rml\") failed; "
                "HUD disabled");
        }
    } else {
        PSY_LOG_WARN("sample_04: RmlUi initialize() failed; HUD disabled");
    }

    PSY_LOG_INFO("Psynder sample 04 running{}",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{});

    // ─── Fixed timestep ─────────────────────────────────────────────────
    constexpr f32 kSimHz = 120.0f;
    constexpr f32 kSimDt = 1.0f / kSimHz;
    f32 accumulator = 0.0f;

    const u64 t0_ticks = platform::Clock::ticks_now();
    u64 last_ticks = t0_ticks;
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();

        // Quit on ESC outside smoke mode.
        if (args.smoke_frames == 0 && platform::input() != nullptr &&
            platform::input()->key_down(platform::KeyCode::Escape)) {
            break;
        }

        // Frame dt — fixed for smoke runs so captures are reproducible.
        f32 dt;
        if (args.smoke_frames > 0) {
            dt = 1.0f / 60.0f;
        } else {
            const u64 now = platform::Clock::ticks_now();
            dt = static_cast<f32>(platform::Clock::seconds(now - last_ticks));
            last_ticks = now;
            if (dt > 0.10f)
                dt = 0.10f;  // clamp on hitch
        }

        // ── Physics fixed-step ─────────────────────────────────────────
        accumulator += dt;
        // Cap so a paused window can't burn an unbounded number of steps.
        if (accumulator > 0.5f)
            accumulator = 0.5f;
        while (accumulator >= kSimDt) {
            prev_pos = cur_pos;
            prev_rot = cur_rot;

            // ── Auto-driver: aim ahead, hold target speed ─────────────
            const TrackPos here = closest_on_track(track_segs, cur_pos);
            const TrackPos ahead = advance_along(track_segs, here.seg, here.t, driver.look_ahead_m);

            // Heading error: angle between current forward and (ahead - pos),
            // signed so positive means "steer right" in world XZ.
            math::Vec3 to_aim = math::sub(ahead.p, cur_pos);
            to_aim.y = 0.0f;
            const f32 to_aim_len = math::length(to_aim);
            if (to_aim_len > 0.001f) {
                to_aim = math::mul(to_aim, 1.0f / to_aim_len);
                // Signed angle between car_fwd (XZ) and to_aim (XZ).
                math::Vec3 fwd_xz = car_fwd;
                fwd_xz.y = 0.0f;
                fwd_xz = math::normalize(fwd_xz);
                const f32 dotv = math::dot(fwd_xz, to_aim);
                // cross.y carries the sign (right-handed, +Y up).
                const f32 crossy = fwd_xz.z * to_aim.x - fwd_xz.x * to_aim.z;
                const f32 ang = std::atan2(crossy, std::max(-1.0f, std::min(1.0f, dotv)));
                f32 steer = driver.steer_gain * ang;
                // Clamp to ~30° physical steering range.
                const f32 kSteerMax = 0.55f;
                if (steer > kSteerMax)
                    steer = kSteerMax;
                if (steer < -kSteerMax)
                    steer = -kSteerMax;
                physics::vehicle::set_steer(veh, steer);
            }

            // Throttle PI on speed error.
            const f32 speed_err = driver.target_speed_mps - car_speed;
            f32 throttle = driver.throttle_kp * speed_err;
            f32 brake = 0.0f;
            if (throttle < 0.0f) {
                brake = std::min(1.0f, -throttle);
                throttle = 0.0f;
            }
            if (throttle > 1.0f)
                throttle = 1.0f;
            physics::vehicle::set_throttle(veh, throttle);
            physics::vehicle::set_brake(veh, brake);
            // Capture for the HUD — the last driver decision per render
            // frame is the one the dashboard reflects (matches a real car's
            // pedal throw, smoothed by the sim's 120 Hz under-sampling).
            hud_throttle = throttle;
            hud_brake = brake;

            // ── Step ──────────────────────────────────────────────────
            world.step(kSimDt);

            const math::Vec3 new_pos = world.get_position(chassis);
            const math::Quat new_rot = world.get_rotation(chassis);
            const math::Vec3 dp = math::sub(new_pos, cur_pos);
            const f32 step_dist = math::length(v3(dp.x, 0.0f, dp.z));
            car_speed = step_dist / kSimDt;
            if (step_dist > 0.001f) {
                car_fwd = math::normalize(v3(dp.x, 0.0f, dp.z));
            }
            cur_pos = new_pos;
            cur_rot = new_rot;

            accumulator -= kSimDt;
        }

        // Interpolation alpha for render.
        const f32 alpha = accumulator / kSimDt;
        const math::Vec3 render_pos = lerp_v3(prev_pos, cur_pos, alpha);

        // ── Camera: chase ────────────────────────────────────────────
        // car_fwd is the velocity heading, which is noisy at low speed (it can
        // point across the road). Frame the chase — and orient the chassis —
        // off the *track tangent* at the car's position instead: smooth and
        // always down-road, independent of how fast the car is actually going.
        const TrackPos cam_tp = closest_on_track(track_segs, render_pos);
        math::Vec3 road_fwd = bezier_tangent(track_segs[cam_tp.seg], cam_tp.t);
        road_fwd.y = 0.0f;
        road_fwd = math::normalize(road_fwd);
        // Classic NFS chase: eye ~8.5m behind the car along -road_fwd and ~3.2m
        // up (the 4.2m-long car then frames in the lower third with road
        // visible ahead), aiming ~12m down-track and near ground level so the
        // camera tilts into the oncoming tarmac.
        const math::Vec3 eye =
            math::add(render_pos, math::add(math::mul(road_fwd, -8.5f), v3(0.0f, 3.2f, 0.0f)));
        const math::Vec3 tgt =
            math::add(render_pos, math::add(math::mul(road_fwd, 12.0f), v3(0.0f, 0.6f, 0.0f)));

        // ── Clear + frame setup ──────────────────────────────────────
        render::raster::clear_framebuffer(fb, 0xFF8FA7C8u);  // dusk sky
        clear_depth_far(fb);

        render::raster::ViewState view{};
        view.target = fb;
        view.view = math::look_at_rh(eye, tgt, v3(0.0f, 1.0f, 0.0f));
        view.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                               static_cast<f32>(desc.render_width) /
                                                   static_cast<f32>(desc.render_height),
                                               0.2f,
                                               400.0f);
        view.tile_w = 64;
        view.tile_h = 64;
        rasterizer.begin_frame(view);

        // ── Track submit ────────────────────────────────────────────
        {
            render::raster::DrawItem item{};
            item.vertices = track_mesh.verts.data();
            item.vertex_count = static_cast<u32>(track_mesh.verts.size());
            item.indices = track_mesh.indices.data();
            item.index_count = static_cast<u32>(track_mesh.indices.size());
            item.model = math::identity4();
            rasterizer.submit(item);
        }

        // ── Chassis submit ──────────────────────────────────────────
        // Orient the car (and its wheels, which share this matrix) along the
        // smooth track heading rather than the jittery velocity vector.
        const math::Mat4 yaw_mat = yaw_from_forward(road_fwd);
        {
            // Scale the unit cube to chassis dimensions, raise to wheel hub.
            const math::Mat4 scl = math::scale(v3(4.2f, 1.1f, 1.8f));
            const math::Mat4 trs = math::translate(v3(render_pos.x, render_pos.y, render_pos.z));
            render::raster::DrawItem item{};
            item.vertices = kCubeVerts.data();
            item.vertex_count = static_cast<u32>(kCubeVerts.size());
            item.indices = cube_idx.data();
            item.index_count = static_cast<u32>(cube_idx.size());
            item.model = math::mul(trs, math::mul(yaw_mat, scl));
            rasterizer.submit(item);
        }

        // ── Wheel submits — four cylinders, oriented so the spin axis
        //    runs along the chassis-local +Z direction (world cross of
        //    car_fwd and +Y).
        {
            // Rotate cylinder local Y → chassis local Z (so cylinder caps
            // become the wheel disc faces). That's a 90° rotation about +X.
            const math::Mat4 axis_align =
                math::rotate_quat(math::quat_from_axis_angle(v3(1, 0, 0), math::kHalfPi));
            const std::array<math::Vec3, 4> wheel_local = {
                v3(-wx, 0.0f, wz),
                v3(-wx, 0.0f, -wz),
                v3(wx, 0.0f, wz),
                v3(wx, 0.0f, -wz),
            };
            for (const auto& wl : wheel_local) {
                // Express chassis-local offset in world via yaw_mat.
                const math::Vec4 wl4{wl.x, wl.y, wl.z, 0.0f};
                const math::Vec4 worldoff = math::mul(yaw_mat, wl4);
                const math::Vec3 wp = v3(render_pos.x + worldoff.x,
                                         render_pos.y + worldoff.y - 0.2f,
                                         render_pos.z + worldoff.z);
                const math::Mat4 trs = math::translate(wp);
                render::raster::DrawItem item{};
                item.vertices = wheel_mesh.verts.data();
                item.vertex_count = static_cast<u32>(wheel_mesh.verts.size());
                item.indices = wheel_mesh.indices.data();
                item.index_count = static_cast<u32>(wheel_mesh.indices.size());
                item.model = math::mul(trs, math::mul(yaw_mat, axis_align));
                rasterizer.submit(item);
            }
        }

        rasterizer.end_frame();

        // ── HUD update + render (Wave-E) ────────────────────────────────
        //
        // Push current telemetry directly into the live DOM via the public
        // DataBind setters (`set_element_text` + `set_element_attribute`)
        // — no per-frame source rebuild, no test-layer reload hack.  The
        // engine-side render() walks the cascaded layout boxes and draws
        // them on top of the 3D scene without disturbing depth.
        if (hud_active) {
            const EngineEstimate ee = estimate_engine(car_speed, hud_throttle, 0.35f);
            push_hud_telemetry(car_speed, ee.rpm, ee.gear, hud_throttle, hud_brake);
            psynder::ui::rml::update(dt);
            psynder::ui::rml::render(fb);
        }

        window->present(fb);

        // In smoke mode, log the chassis position each frame so CI (and the
        // suspension fix verification) can confirm the car rests on the track
        // instead of sinking through it.
        if (args.smoke_frames > 0) {
            PSY_LOG_INFO("sample_04: frame {} car_pos=({:.3f}, {:.3f}, {:.3f}) speed={:.2f}",
                         frame,
                         cur_pos.x,
                         cur_pos.y,
                         cur_pos.z,
                         car_speed);
        }

        ++frame;
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_04: smoke target reached ({}); exiting", args.smoke_frames);
            break;
        }
    }

    // ─── Capture ────────────────────────────────────────────────────────
    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_04: failed to write capture to {}", args.capture_out);
            if (hud_active)
                psynder::ui::rml::hide("hud");
            psynder::ui::rml::shutdown();
            physics::vehicle::destroy(veh);
            world.destroy_body(chassis);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_04: wrote capture to {}", args.capture_out);
    }

    if (hud_active) {
        psynder::ui::rml::hide("hud");
    }
    psynder::ui::rml::shutdown();

    physics::vehicle::destroy(veh);
    world.destroy_body(chassis);
    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
