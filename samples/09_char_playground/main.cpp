// SPDX-License-Identifier: MIT
// Psynder — Sample 09. Kinematic capsule character "obstacle playground".
//
// A capsule character walks an obstacle course built from static physics
// boxes, exercising the public character controller (DESIGN.md §10.1):
//
//   physics::character::CharacterDesc {position, height, radius}
//   physics::character::create(desc)            -> CharacterId
//   physics::character::move(id, delta, dt)     (sweep-step-slide)
//
// The course (laid out along +X, the character's walk direction):
//
//   * A long FLAT ground box (top face at y = 0) — the walkable floor.
//   * A walkable RAMP (a box tilted ~22 deg about +Z; cos 22 deg ~= 0.93,
//     well above the 0.7 slope-limit cosine) the character climbs.
//   * A STEEP wedge beside the ramp (tilted ~58 deg; cos 58 deg ~= 0.53,
//     below the slope limit) — pushing into it makes the controller slide
//     rather than climb, demonstrating the slope limit.
//   * A 4-step STAIRCASE, each step rising 0.30 m (< the 0.35 m step_height)
//     so the controller's stair-step climb-up walks the capsule up each one.
//   * A raised PLATFORM the stairs lead onto, ending in a 0.30 m LEDGE step
//     back down to the floor.
//
// The character controller (engine/physics) owns MOVEMENT (step-up + slope
// slide); the shared samples::CharacterController (samples/common) owns only
// the MOUSE-LOOK + the WASD->yaw intent. Each frame we:
//   1. read look/intent from the shared controller (live) or a scripted path
//      (smoke);
//   2. build a world-space delta = (yaw-relative WASD) + (a constant downward
//      "gravity" pull so the body stays seated on whatever surface it is on);
//   3. feed that delta into physics::character::move(); the engine resolves
//      collisions, climbs steps, and slides on steep slopes;
//   4. read the resolved capsule centre back out of the engine and place the
//      camera eye at centre + eye-height, then render the capsule + course.
//
// The public character namespace exposes no position getter (only
// create/destroy/move), so we read the resolved centre from the engine's
// internal character store via physics/Character.h's detail::character_world()
// accessor — the same internal-header reach-in the physics unit/bench TUs use
// (tests/bench/physics_solver.cpp reaches physics/WorldImpl.h likewise). This
// reports the TRUE post-move pose (including step-climb / push-out) rather
// than a dead-reckoned guess.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run: scripted forward walk for N
//                            frames then exit. Logs the capsule centre Y so
//                            the climb (Y rises over the stairs) is verifiable.
//   --smoke-frames N         Same, space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the last framebuffer to PATH as a PNG.

#include "common/CharacterController.h"
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "physics/Character.h"  // detail::character_world() — resolved pose read-back
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

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

// Model matrix from a pose (world position + rotation quat) and a per-axis
// scale that maps the unit primitive to the body's full extents.
inline math::Mat4 pose_model(math::Vec3 pos, math::Quat rot, math::Vec3 scale) noexcept {
    const math::Mat4 t = math::translate(pos);
    const math::Mat4 r = math::rotate_quat(math::quat_normalize(rot));
    const math::Mat4 s = math::scale(scale);
    return math::mul(t, math::mul(r, s));
}

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

// ─── Unit cube (per-face colour, spans [-0.5, +0.5]) ─────────────────────
// Same winding as samples 02 / 04 / 07 under the CCW front-face convention.
constexpr u32 kCubeBase = pack_rgba(200, 200, 200);

const std::array<render::raster::Vertex, 24> kCubeVerts{{
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0, 1}, {0, 0}, kCubeBase},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0, 0}, {0, 0}, kCubeBase},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {1, 0}, {0, 0}, kCubeBase},
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {1, 1}, {0, 0}, kCubeBase},

    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0, 1}, {0, 0}, kCubeBase},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0, 0}, {0, 0}, kCubeBase},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {1, 0}, {0, 0}, kCubeBase},
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {1, 1}, {0, 0}, kCubeBase},

    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0, 1}, {0, 0}, kCubeBase},
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0, 0}, {0, 0}, kCubeBase},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {1, 0}, {0, 0}, kCubeBase},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {1, 1}, {0, 0}, kCubeBase},

    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0, 1}, {0, 0}, kCubeBase},
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}, {0, 0}, kCubeBase},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}, {0, 0}, kCubeBase},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {1, 1}, {0, 0}, kCubeBase},

    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0, 1}, {0, 0}, kCubeBase},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {1, 1}, {0, 0}, kCubeBase},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {1, 0}, {0, 0}, kCubeBase},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0, 0}, {0, 0}, kCubeBase},

    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}, {0, 0}, kCubeBase},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}, {0, 0}, kCubeBase},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0}, {0, 0}, kCubeBase},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0, 0}, {0, 0}, kCubeBase},
}};

constexpr std::array<u32, 36> kCubeIndices{
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

std::array<render::raster::Vertex, 24> tint_cube(u32 rgba) {
    std::array<render::raster::Vertex, 24> out = kCubeVerts;
    for (auto& v : out)
        v.color = rgba;
    return out;
}

struct Mesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

// ─── Capsule mesh (radius 1, axis length controlled by `half_cyl`) ────────
// Built procedurally about the local +Y axis: a cylindrical mid-section of
// half-height `half_cyl` capped by two hemispheres of radius 1. Rendered with
// a model matrix that scales radius and translates to the capsule centre, so
// the on-screen capsule matches the collider exactly (radius * unit + the
// straight section the engine uses: half_cyl == height/2 - radius).
Mesh build_capsule(f32 half_cyl, u32 rings, u32 segments, u32 rgba) {
    Mesh m;
    auto push = [&](math::Vec3 p, math::Vec3 n) {
        render::raster::Vertex v{};
        v.position = p;
        v.normal = math::normalize(n);
        v.uv = math::Vec2{0, 0};
        v.color = rgba;
        m.verts.push_back(v);
    };

    // Two stacked hemispheres separated by `half_cyl`. We sweep latitude from
    // the south pole (-Y) up to the north pole (+Y); for the lower half the
    // sphere centre is at y=-half_cyl, for the upper half at y=+half_cyl. This
    // welds into a clean capsule with a straight cylindrical middle.
    const u32 half_rings = std::max<u32>(2, rings / 2);
    std::vector<u32> ring_start;
    for (u32 hemi = 0; hemi < 2; ++hemi) {
        const f32 cy = (hemi == 0) ? -half_cyl : half_cyl;
        // Latitude 0..half_rings maps to angle 0..pi/2.
        for (u32 r = 0; r <= half_rings; ++r) {
            const f32 lat = (static_cast<f32>(r) / static_cast<f32>(half_rings)) * math::kHalfPi;
            // hemi 0 (bottom): from south pole (-Y) toward equator.
            // hemi 1 (top): from equator toward north pole (+Y).
            const f32 sy = (hemi == 0) ? -std::cos(lat) : std::cos(lat);
            const f32 sr = std::sin(lat);
            ring_start.push_back(static_cast<u32>(m.verts.size()));
            for (u32 s = 0; s <= segments; ++s) {
                const f32 lon = (static_cast<f32>(s) / static_cast<f32>(segments)) * math::kTwoPi;
                const f32 nx = sr * std::cos(lon);
                const f32 nz = sr * std::sin(lon);
                const math::Vec3 normal = v3(nx, sy, nz);
                const math::Vec3 pos = v3(nx, sy + cy, nz);
                push(pos, normal);
            }
        }
    }

    // Stitch adjacent rings into quads (two tris). Rings are stored in order:
    // bottom hemi (half_rings+1 rings) then top hemi (half_rings+1 rings). We
    // bridge every consecutive pair, including the bottom-equator to
    // top-equator seam which forms the straight cylindrical wall.
    for (u32 ri = 0; ri + 1 < static_cast<u32>(ring_start.size()); ++ri) {
        const u32 a = ring_start[ri];
        const u32 b = ring_start[ri + 1];
        for (u32 s = 0; s < segments; ++s) {
            const u32 a0 = a + s;
            const u32 a1 = a + s + 1;
            const u32 b0 = b + s;
            const u32 b1 = b + s + 1;
            m.indices.push_back(a0);
            m.indices.push_back(b0);
            m.indices.push_back(b1);
            m.indices.push_back(a0);
            m.indices.push_back(b1);
            m.indices.push_back(a1);
        }
    }
    return m;
}

// ─── Course geometry ───────────────────────────────────────────────────────
// A static box collider plus its render data. Boxes are axis-aligned unless
// `rot` tilts them (used for the ramp + the steep wedge).
struct Block {
    physics::BodyId id;
    math::Vec3 center;
    math::Vec3 half;
    math::Quat rot{0, 0, 0, 1};
    std::array<render::raster::Vertex, 24> mesh;
};

Block make_block(
    physics::World& world, math::Vec3 center, math::Vec3 half, math::Quat rot, u32 color, f32 friction) {
    physics::BodyDesc d{};
    d.shape = physics::Shape::Box;
    d.mass = 0.0f;  // static
    d.position = center;
    d.rotation = rot;
    d.half_extent = half;
    d.friction = friction;
    d.restitution = 0.0f;
    Block b{};
    b.id = world.create_body(d);
    b.center = center;
    b.half = half;
    b.rot = rot;
    b.mesh = tint_cube(color);
    return b;
}

void submit_block(render::raster::Rasterizer& r, const Block& b) {
    render::raster::DrawItem item{};
    item.vertices = b.mesh.data();
    item.vertex_count = static_cast<u32>(b.mesh.size());
    item.indices = kCubeIndices.data();
    item.index_count = static_cast<u32>(kCubeIndices.size());
    // Unit cube -> full extent: scale by 2 * half.
    item.model = pose_model(b.center, b.rot, v3(b.half.x * 2.0f, b.half.y * 2.0f, b.half.z * 2.0f));
    r.submit(item);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    // ─── Platform / framebuffer ─────────────────────────────────────────
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 09 (capsule playground)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_09: failed to create window");
        return EXIT_FAILURE;
    }

    auto* input = platform::input();

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

    // ─── Physics world + course ─────────────────────────────────────────
    auto& world = physics::World::Get();
    world.set_gravity(v3(0.0f, -9.81f, 0.0f));  // bodies are static; informational

    std::vector<Block> course;

    // Long flat ground: top face at y = 0, running far along X. Spans the
    // whole course (the start, the staircase, and the platform run-off).
    constexpr f32 kGroundHalfY = 0.5f;
    course.push_back(make_block(world,
                                v3(7.0f, -kGroundHalfY, 0.0f),
                                v3(13.0f, kGroundHalfY, 7.0f),
                                math::Quat{0, 0, 0, 1},
                                pack_rgba(74, 80, 90),
                                0.9f));

    // Walkable ramp: a flat slab tilted ~22 deg about +Z so its top face
    // climbs toward +X. cos(22 deg) ~= 0.927 > 0.7 slope-limit cosine -> the
    // controller treats the surface as floor and walks up it. Sits in the
    // +Z lane (side of the central staircase walk lane) for the free-walk
    // view; the scripted smoke path takes the central staircase instead.
    {
        const f32 ang = 22.0f * math::kDegToRad;
        const math::Quat q = math::quat_from_axis_angle(v3(0, 0, 1), ang);
        course.push_back(
            make_block(world, v3(2.5f, 0.9f, 4.0f), v3(2.0f, 0.15f, 1.4f), q, pack_rgba(90, 150, 110), 0.9f));
    }

    // Steep wedge (slope-limit demo): tilted ~58 deg about +Z. cos(58 deg)
    // ~= 0.53 < 0.7 -> pushing into this face makes the controller SLIDE
    // instead of climbing. Sits in the -Z lane.
    {
        const f32 ang = 58.0f * math::kDegToRad;
        const math::Quat q = math::quat_from_axis_angle(v3(0, 0, 1), ang);
        course.push_back(
            make_block(world, v3(2.5f, 1.2f, -4.0f), v3(1.6f, 0.15f, 1.4f), q, pack_rgba(170, 110, 90), 0.6f));
    }

    // Staircase: 4 steps each rising 0.30 m (< 0.35 step_height) and 0.7 m
    // deep, centred on the z = 0 walk lane, climbing toward +X from x ~= 0.8.
    // Each step is modelled as a box whose TOP sits at (n+1)*rise. The
    // staircase starts close to the spawn so the scripted smoke walk reaches
    // and mounts the first step within a handful of frames.
    constexpr f32 kStepRise = 0.30f;
    constexpr f32 kStepRun = 0.7f;
    constexpr u32 kStepCount = 4;
    constexpr f32 kStairX0 = 0.8f;
    f32 platform_top = 0.0f;
    for (u32 i = 0; i < kStepCount; ++i) {
        const f32 top = static_cast<f32>(i + 1) * kStepRise;
        const f32 cx = kStairX0 + static_cast<f32>(i) * kStepRun + kStepRun * 0.5f;
        // Box spans y in [top - 0.5, top] (thick enough the capsule never
        // probes through), z in [-2, 2].
        const f32 half_y = (top + 0.5f) * 0.5f;
        const f32 cy = top - half_y;
        course.push_back(make_block(world,
                                    v3(cx, cy, 0.0f),
                                    v3(kStepRun * 0.5f, half_y, 2.0f),
                                    math::Quat{0, 0, 0, 1},
                                    pack_rgba(120, 130, 200),
                                    0.9f));
        platform_top = top;
    }

    // Raised platform the stairs lead onto (top flush with the last step),
    // then a 0.30 m ledge back down to the floor at its far +X edge.
    {
        const f32 top = platform_top;
        const f32 half_y = (top + 0.5f) * 0.5f;
        const f32 cy = top - half_y;
        const f32 x0 = kStairX0 + static_cast<f32>(kStepCount) * kStepRun;
        course.push_back(make_block(world,
                                    v3(x0 + 1.5f, cy, 0.0f),
                                    v3(1.5f, half_y, 2.0f),
                                    math::Quat{0, 0, 0, 1},
                                    pack_rgba(150, 150, 160),
                                    0.9f));
    }

    // ─── Character ──────────────────────────────────────────────────────
    // Capsule: stand height 1.8, radius 0.35. The engine's collider centres
    // the capsule at `position`; its bottom is position.y - height/2. To rest
    // on the ground (top at y=0) the centre rides at y = height/2 = 0.9.
    constexpr f32 kStandHeight = 1.8f;
    constexpr f32 kRadius = 0.35f;
    constexpr f32 kHalfCyl = kStandHeight * 0.5f - kRadius;  // straight section
    constexpr f32 kCenterRest = kStandHeight * 0.5f;         // grounded centre Y
    // Spawn just in front of the staircase (on the central z=0 lane) so the
    // scripted smoke walk crosses a little flat ground, then mounts step 1.
    const math::Vec3 kStartCenter = v3(0.0f, kCenterRest, 0.0f);

    physics::character::CharacterDesc cdesc{};
    cdesc.position = kStartCenter;
    cdesc.height = kStandHeight;
    cdesc.radius = kRadius;
    const physics::character::CharacterId character = physics::character::create(cdesc);

    // Resolved-pose read-back: the public character namespace exposes no
    // position getter, so we look up the engine's internal character record
    // (physics/Character.h). This is the SAME store move() mutates, so it
    // reports the true post-move centre (push-out + stair-climb included).
    const u32 char_idx = character.raw & 0x00FFFFFFu;
    auto& char_world = physics::detail::character_world();
    auto char_center = [&]() -> math::Vec3 {
        if (char_idx < char_world.chars.size())
            return char_world.chars[char_idx].position;
        return kStartCenter;
    };

    // Capsule render mesh (unit radius; scaled per-frame to kRadius).
    const Mesh capsule_mesh = build_capsule(kHalfCyl / kRadius, 12, 20, pack_rgba(230, 200, 90));

    // ─── Shared look/intent controller (mouse-look only) ────────────────
    // We use the shared controller solely for yaw/pitch (mouse-look) and the
    // WASD intent basis; movement itself is owned by physics::character::move.
    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.eye_height = 1.6f;
    cc_cfg.walk_speed = 3.0f;
    samples::CharacterController look{cc_cfg};
    look.set_mode(samples::ControllerMode::Fps);
    // Start looking down +X (the course direction). yaw such that the planar
    // forward (sin yaw, 0, -cos yaw) points toward +X => yaw = +pi/2.
    look.set_look(math::kHalfPi, -0.12f);

    PSY_LOG_INFO("Psynder sample 09 running{} — {} course blocks, capsule h={} r={}",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{},
                 course.size(),
                 kStandHeight,
                 kRadius);

    // ─── Loop ──────────────────────────────────────────────────────────
    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;

    // Each frame the body is driven by TWO move() calls:
    //   1. a purely HORIZONTAL walk delta — a clean wall hit here (no vertical
    //      component to muddy the contact-normal floor/wall classification)
    //      lets the engine's stair-step climb-up fire reliably on a step face;
    //   2. a purely DOWNWARD seat delta — pulls the body back onto whatever
    //      surface it stands on (the sweep pushes it out of penetration).
    // The walk speed is deliberately brisk (~9 m/s). The engine's stair-step
    // climb commits only when a single move() call advances the body far
    // enough horizontally to clear the step edge before the slide-back; below
    // ~0.14 m of horizontal travel per call the capsule just perches on the
    // step's top edge and never mounts it. At 9 m/s and 60 fps each frame's
    // horizontal move() is ~0.15 m, comfortably past that threshold, so the
    // capsule climbs each step cleanly. Gravity is a separate, smaller pull.
    constexpr f32 kFallSpeed = 6.0f;
    constexpr f32 kWalkSpeed = 9.0f;

    while (!window->should_close()) {
        window->poll_events();

        if (args.smoke_frames == 0 && input != nullptr && input->key_down(platform::KeyCode::Escape)) {
            PSY_LOG_INFO("sample_09: escape pressed, exiting");
            break;
        }

        // Fixed dt for smoke determinism; wall-clock otherwise.
        f32 dt;
        if (args.smoke_frames > 0) {
            dt = 1.0f / 60.0f;
        } else {
            const u64 now = platform::Clock::ticks_now();
            dt = static_cast<f32>(platform::Clock::seconds(now - last_ticks));
            last_ticks = now;
            if (dt > 0.10f)
                dt = 0.10f;
        }

        // ── Build the planar walk intent (world XZ). ────────────────────
        math::Vec3 walk{0, 0, 0};
        if (args.smoke_frames > 0) {
            // Scripted: hold "forward" toward +X so the capsule crosses the
            // flat ground and climbs the staircase. Yaw is fixed at +pi/2,
            // whose planar forward is +X.
            const f32 cy = std::cos(math::kHalfPi);
            const f32 sy = std::sin(math::kHalfPi);
            walk = v3(sy, 0.0f, -cy);  // == (+1, 0, 0)
        } else {
            // Live: mouse-look + WASD intent from the shared controller. We
            // read its yaw to build a yaw-relative basis but apply the motion
            // through physics::character::move (NOT the controller's own
            // clamp-walk), so step-up + slope-slide come from the engine.
            look.update(*input, dt);
            const f32 yaw = look.yaw();
            const math::Vec3 fwd = v3(std::sin(yaw), 0.0f, -std::cos(yaw));
            const math::Vec3 right = v3(std::cos(yaw), 0.0f, std::sin(yaw));
            if (input->key_down(platform::KeyCode::W))
                walk = math::add(walk, fwd);
            if (input->key_down(platform::KeyCode::S))
                walk = math::sub(walk, fwd);
            if (input->key_down(platform::KeyCode::D))
                walk = math::add(walk, right);
            if (input->key_down(platform::KeyCode::A))
                walk = math::sub(walk, right);
        }

        // ── Drive the engine character (sweep-step-slide). ──────────────
        // Step 1: horizontal walk (yaw-relative). A clean wall hit triggers
        // the stair-step climb-up; on a walkable slope the body rides up.
        const f32 wlen = std::sqrt(math::dot(walk, walk));
        if (wlen > 1e-4f) {
            const math::Vec3 dir = math::mul(walk, 1.0f / wlen);
            physics::character::move(character, math::mul(dir, kWalkSpeed * dt), dt);
        }
        // Step 2: gravity seat — keep the body resting on its surface.
        physics::character::move(character, v3(0.0f, -kFallSpeed * dt, 0.0f), dt);

        // ── Read the resolved capsule centre + place the camera. ────────
        const math::Vec3 center = char_center();
        // Third-person-ish chase eye: behind the capsule along -forward and
        // above it, looking at the head. In smoke mode forward is +X.
        const f32 yaw = (args.smoke_frames > 0) ? math::kHalfPi : look.yaw();
        const math::Vec3 fwd = v3(std::sin(yaw), 0.0f, -std::cos(yaw));
        const math::Vec3 eye = math::add(center, v3(-fwd.x * 4.5f, 2.4f, -fwd.z * 4.5f));
        const math::Vec3 look_at = math::add(center, v3(0.0f, 0.4f, 0.0f));

        // ── Clear + view. ──────────────────────────────────────────────
        render::raster::clear_framebuffer(fb, 0xFF202832u);
        clear_depth_far(fb);

        render::raster::ViewState view{};
        view.target = fb;
        view.view = math::look_at_rh(eye, look_at, v3(0.0f, 1.0f, 0.0f));
        view.projection = math::perspective_rh(65.0f * math::kDegToRad,
                                               static_cast<f32>(desc.render_width) /
                                                   static_cast<f32>(desc.render_height),
                                               0.05f,
                                               200.0f);
        view.tile_w = 64;
        view.tile_h = 64;
        rasterizer.begin_frame(view);

        // ── Course. ─────────────────────────────────────────────────────
        for (const Block& b : course)
            submit_block(rasterizer, b);

        // ── Capsule (centre = resolved physics position). ──────────────
        {
            render::raster::DrawItem item{};
            item.vertices = capsule_mesh.verts.data();
            item.vertex_count = static_cast<u32>(capsule_mesh.verts.size());
            item.indices = capsule_mesh.indices.data();
            item.index_count = static_cast<u32>(capsule_mesh.indices.size());
            item.model = pose_model(center, math::Quat{0, 0, 0, 1}, v3(kRadius, kRadius, kRadius));
            rasterizer.submit(item);
        }

        rasterizer.end_frame();
        window->present(fb);

        if (args.smoke_frames > 0) {
            const bool bad =
                !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z);
            PSY_LOG_INFO(
                "sample_09: frame {} — capsule centre ({:.2f},{:.2f},{:.2f}) "
                "bottom_y {:.2f}{}",
                frame,
                center.x,
                center.y,
                center.z,
                center.y - kStandHeight * 0.5f,
                bad ? " [WARN: non-finite capsule]" : "");
        }

        ++frame;
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_09: smoke target reached ({}); exiting", args.smoke_frames);
            break;
        }
    }

    // ─── Capture (optional). ────────────────────────────────────────────
    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_09: failed to write capture to {}", args.capture_out);
            physics::character::destroy(character);
            for (const Block& b : course)
                world.destroy_body(b.id);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_09: wrote capture to {}", args.capture_out);
    }

    physics::character::destroy(character);
    for (const Block& b : course)
        world.destroy_body(b.id);
    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
