// SPDX-License-Identifier: MIT
// Psynder — Sample 07. Rigid-body "stacks & restitution" showcase.
//
// The scene exercises the Wave-A/B rigid-body pipeline end to end through the
// public `psynder::physics` API (DESIGN.md §10.1):
//
//   * A large STATIC ground box (mass = 0) — the floor everything rests on.
//   * A PYRAMID of dynamic boxes (3 layers: 3 + 2 + 1) stacked on the ground.
//     The boxes start axis-aligned and barely overlapping so the contact
//     solver settles them into a stable stack rather than exploding — this
//     drives the SAP broadphase + box/box narrowphase + the projected
//     Gauss-Seidel (sequential-impulse) contact solver with friction.
//   * A handful of dynamic SPHERES spawned above the stack with zero initial
//     velocity and a high restitution coefficient. They free-fall under
//     gravity, impact the ground/boxes well above the solver's restitution
//     threshold (1 m/s), bounce, and over a few seconds lose energy and come
//     to rest — the restitution + damping path.
//
// Rendering is the tiled software rasterizer (`render::raster`). Every frame
// we read each body's interpolated pose back via `World::get_position` /
// `get_rotation` and submit a unit-cube DrawItem (scaled to the body's half
// extents) for boxes or a unit-icosphere DrawItem (scaled to the radius) for
// spheres. An orbit camera (mirrors sample 05's `make_orbit_camera`) frames
// the stack.
//
// Because the engine does not expose an explicit sleep flag on the public API
// (and Wave-A never sets `kFlagSleeping`), "at rest" is derived here from each
// body's per-frame world-space displacement: a body whose centre moves less
// than a small epsilon over a frame is reported as settled. The smoke log
// prints active/settled counts plus the apex box Y and a tracked sphere Y so
// the headless run is verifiable (and catches a NaN/explosion regression).
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Same, space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the last framebuffer to PATH as a PNG.

#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "physics/Physics.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <array>
#include <cmath>
#include <cstdio>
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

// Build a model matrix from a body pose (rotation quat + world position) and a
// per-axis scale that maps the unit primitive to the body's half extents.
inline math::Mat4 pose_model(math::Vec3 pos, math::Quat rot, math::Vec3 scale) noexcept {
    const math::Mat4 t = math::translate(pos);
    const math::Mat4 r = math::rotate_quat(math::quat_normalize(rot));
    const math::Mat4 s = math::scale(scale);
    return math::mul(t, math::mul(r, s));
}

// ─── Unit cube (per-face colour, spans [-0.5, +0.5]) ─────────────────────
// Layout matches sample 02 / 04 so the cube reads correctly under the
// rasterizer's CCW front-face convention. The colour is overwritten per
// instance below; the constant here is just a placeholder.
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

// Recolour a copy of the unit cube. The rasterizer reads per-vertex colour,
// so we keep one tinted vertex array per distinct body colour.
std::array<render::raster::Vertex, 24> tint_cube(u32 rgba) {
    std::array<render::raster::Vertex, 24> out = kCubeVerts;
    for (auto& v : out)
        v.color = rgba;
    return out;
}

// ─── Unit icosphere (radius 1, centred at origin) ────────────────────────
// A subdivided icosahedron gives a rounder ball than a UV sphere at low
// triangle counts. One subdivision (80 tris) is plenty for a bouncing ball.
struct Mesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

Mesh build_icosphere(u32 subdivisions, u32 rgba) {
    // 12 icosahedron vertices.
    const f32 t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    std::vector<math::Vec3> pos = {
        {-1, t, 0},
        {1, t, 0},
        {-1, -t, 0},
        {1, -t, 0},
        {0, -1, t},
        {0, 1, t},
        {0, -1, -t},
        {0, 1, -t},
        {t, 0, -1},
        {t, 0, 1},
        {-t, 0, -1},
        {-t, 0, 1},
    };
    for (auto& p : pos)
        p = math::normalize(p);

    std::vector<u32> idx = {0, 11, 5,  0, 5,  1, 0, 1, 7, 0, 7,  10, 0, 10, 11, 1, 5, 9, 5, 11,
                            4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3,  9,  4, 3,  4,  2, 3, 2, 6, 3,
                            6, 8,  3,  8, 9,  4, 9, 5, 2, 4, 11, 6,  2, 10, 8,  6, 7, 9, 8, 1};

    // Subdivide: split each triangle into 4, projecting new midpoints to the
    // unit sphere. A flat midpoint cache keeps shared edges welded.
    for (u32 s = 0; s < subdivisions; ++s) {
        std::vector<u32> next;
        next.reserve(idx.size() * 4);
        // Edge-key → midpoint vertex index. Linear scan is fine at this scale.
        std::vector<std::pair<u64, u32>> midcache;
        auto midpoint = [&](u32 a, u32 b) -> u32 {
            const u64 lo = std::min(a, b);
            const u64 hi = std::max(a, b);
            const u64 key = (lo << 32) | hi;
            for (const auto& kv : midcache)
                if (kv.first == key)
                    return kv.second;
            math::Vec3 m = math::normalize(v3((pos[a].x + pos[b].x) * 0.5f,
                                              (pos[a].y + pos[b].y) * 0.5f,
                                              (pos[a].z + pos[b].z) * 0.5f));
            const u32 mi = static_cast<u32>(pos.size());
            pos.push_back(m);
            midcache.push_back({key, mi});
            return mi;
        };
        for (usize i = 0; i < idx.size(); i += 3) {
            const u32 a = idx[i + 0], b = idx[i + 1], c = idx[i + 2];
            const u32 ab = midpoint(a, b);
            const u32 bc = midpoint(b, c);
            const u32 ca = midpoint(c, a);
            const u32 tri[12] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
            for (u32 k = 0; k < 12; ++k)
                next.push_back(tri[k]);
        }
        idx.swap(next);
    }

    Mesh m;
    m.verts.reserve(pos.size());
    for (const auto& p : pos) {
        render::raster::Vertex vt{};
        vt.position = p;  // unit sphere; scaled per-instance by radius
        vt.normal = p;    // outward normal == position on a unit sphere
        vt.uv = math::Vec2{0, 0};
        vt.color = rgba;
        m.verts.push_back(vt);
    }
    m.indices = std::move(idx);
    return m;
}

// ─── Camera ──────────────────────────────────────────────────────────────
// Slow orbit around the stack (mirrors sample 05's make_orbit_camera intent
// but produces a look-at matrix for the rasterizer's ViewState).
struct OrbitCamera {
    math::Vec3 eye;
    math::Vec3 target;
};

OrbitCamera make_orbit_camera(f32 t_seconds) noexcept {
    const f32 radius = 11.0f;
    const f32 height = 5.0f;
    const f32 angle = t_seconds * 0.25f;
    OrbitCamera c{};
    c.eye = v3(std::cos(angle) * radius, height, std::sin(angle) * radius);
    c.target = v3(0.0f, 1.4f, 0.0f);
    return c;
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

// ─── Scene bodies ─────────────────────────────────────────────────────────
struct BoxBody {
    physics::BodyId id;
    math::Vec3 half;
    u32 color;
    std::array<render::raster::Vertex, 24> mesh;  // tinted cube
};

struct SphereBody {
    physics::BodyId id;
    f32 radius;
    u32 color;
};

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    // ─── Platform / framebuffer ─────────────────────────────────────────
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 07 (stacks & restitution)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_07: failed to create window");
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

    // ─── Physics world ──────────────────────────────────────────────────
    auto& world = physics::World::Get();
    world.set_gravity(v3(0.0f, -9.81f, 0.0f));

    // Static ground: a wide, thin box whose TOP surface sits at y = 0. mass=0
    // marks it static (infinite mass; integrator skips it, narrowphase still
    // collides against it).
    constexpr f32 kGroundHalfY = 0.5f;
    BoxBody ground{};
    {
        physics::BodyDesc d{};
        d.shape = physics::Shape::Box;
        d.mass = 0.0f;  // static
        d.half_extent = v3(12.0f, kGroundHalfY, 12.0f);
        d.position = v3(0.0f, -kGroundHalfY, 0.0f);  // top face at y = 0
        d.friction = 0.8f;
        d.restitution = 0.2f;
        ground.id = world.create_body(d);
        ground.half = d.half_extent;
        ground.color = pack_rgba(70, 74, 82);
        ground.mesh = tint_cube(ground.color);
    }

    // Pyramid of dynamic boxes: 3 layers (3 + 2 + 1). Each box is a 0.5 m
    // cube (half extent 0.25). We seat each layer a hair (1 mm) below the
    // exact resting height so the solver starts with a tiny positive
    // penetration and pushes the stack apart into a stable tower rather than
    // leaving a gap that lets the upper boxes free-fall and jitter.
    constexpr f32 kBoxHalf = 0.25f;
    constexpr f32 kBoxStep = kBoxHalf * 2.0f;  // full box height
    constexpr f32 kSeat = 0.001f;              // initial overlap seed
    const std::array<u32, 3> kLayerColors = {
        pack_rgba(210, 90, 70),   // bottom — warm red
        pack_rgba(90, 170, 210),  // middle — blue
        pack_rgba(225, 200, 90),  // apex   — gold
    };

    std::vector<BoxBody> boxes;
    {
        // Layer L has (3 - L) boxes, centred on x = 0, spaced one box apart.
        for (u32 layer = 0; layer < 3; ++layer) {
            const u32 count = 3u - layer;
            const f32 y =
                kBoxHalf + static_cast<f32>(layer) * kBoxStep - static_cast<f32>(layer) * kSeat;
            const f32 x0 = -static_cast<f32>(count - 1) * 0.5f * kBoxStep;
            for (u32 i = 0; i < count; ++i) {
                physics::BodyDesc d{};
                d.shape = physics::Shape::Box;
                d.mass = 1.0f;
                d.half_extent = v3(kBoxHalf, kBoxHalf, kBoxHalf);
                d.position = v3(x0 + static_cast<f32>(i) * kBoxStep, y, 0.0f);
                d.friction = 0.8f;
                d.restitution = 0.0f;  // boxes shouldn't bounce — they stack
                BoxBody b{};
                b.id = world.create_body(d);
                b.half = d.half_extent;
                b.color = kLayerColors[layer];
                b.mesh = tint_cube(b.color);
                boxes.push_back(b);
            }
        }
    }

    // Bouncing spheres: spawned high above with zero velocity and high
    // restitution. They impact well above the 1 m/s restitution threshold so
    // the solver reflects them, then damp out over a few seconds.
    constexpr f32 kSphereRadius = 0.35f;
    std::vector<SphereBody> spheres;
    {
        const std::array<math::Vec3, 3> spawn = {
            v3(-2.6f, 5.0f, 1.2f),
            v3(2.4f, 6.2f, -1.0f),
            v3(0.4f, 7.4f, 2.6f),
        };
        const std::array<u32, 3> cols = {
            pack_rgba(120, 220, 120),
            pack_rgba(220, 120, 200),
            pack_rgba(230, 230, 240),
        };
        for (usize i = 0; i < spawn.size(); ++i) {
            physics::BodyDesc d{};
            d.shape = physics::Shape::Sphere;
            d.mass = 1.2f;
            d.half_extent = v3(kSphereRadius, kSphereRadius, kSphereRadius);  // x = radius
            d.position = spawn[i];
            d.friction = 0.4f;
            d.restitution = 0.7f;  // lively bounce
            SphereBody s{};
            s.id = world.create_body(d);
            s.radius = kSphereRadius;
            s.color = cols[i];
            spheres.push_back(s);
        }
    }

    // Sphere render mesh — one shared unit icosphere; colour is per-vertex so
    // we rebuild a tinted copy per sphere (cheap, done once).
    std::vector<Mesh> sphere_meshes;
    sphere_meshes.reserve(spheres.size());
    for (const auto& s : spheres)
        sphere_meshes.push_back(build_icosphere(1, s.color));

    PSY_LOG_INFO("Psynder sample 07 running{} — {} stacked boxes, {} spheres",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{},
                 boxes.size(),
                 spheres.size());

    // ─── Fixed timestep ─────────────────────────────────────────────────
    // World::step() carries its own internal 1/120 s accumulator, so we feed
    // it the frame dt once per frame and it sub-steps deterministically. Smoke
    // runs pin dt to 1/60 (== 2 internal sub-ticks, zero residual) so poses
    // are reproducible across hosts.
    const u64 t0_ticks = platform::Clock::ticks_now();
    u64 last_ticks = t0_ticks;
    u32 frame = 0;

    // Per-body previous-frame centre, to classify active vs. settled.
    std::vector<math::Vec3> box_prev(boxes.size());
    std::vector<math::Vec3> sph_prev(spheres.size());
    for (usize i = 0; i < boxes.size(); ++i)
        box_prev[i] = world.get_position(boxes[i].id);
    for (usize i = 0; i < spheres.size(); ++i)
        sph_prev[i] = world.get_position(spheres[i].id);
    // A body that drifts under this per-frame distance is "at rest".
    constexpr f32 kRestEps = 0.0015f;  // m / frame

    while (!window->should_close()) {
        window->poll_events();

        // ESC quits outside smoke mode.
        if (args.smoke_frames == 0 && platform::input() != nullptr &&
            platform::input()->key_down(platform::KeyCode::Escape)) {
            break;
        }

        // Frame dt — fixed for smoke runs so the sim is reproducible.
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

        // ── Step the world (internal 120 Hz sub-stepping). ──────────────
        world.step(dt);

        // ── Read poses + classify active/settled. ──────────────────────
        u32 active = 0;
        u32 settled = 0;
        f32 apex_y = 0.0f;     // gold apex box centre Y
        f32 sphere0_y = 0.0f;  // first sphere centre Y
        bool any_bad = false;  // NaN / runaway guard for the smoke log

        auto classify = [&](math::Vec3 cur, math::Vec3& prev) {
            const math::Vec3 d = math::sub(cur, prev);
            const f32 dist = std::sqrt(math::dot(d, d));
            prev = cur;
            if (!std::isfinite(cur.x) || !std::isfinite(cur.y) || !std::isfinite(cur.z) ||
                std::fabs(cur.x) > 1e4f || std::fabs(cur.y) > 1e4f || std::fabs(cur.z) > 1e4f)
                any_bad = true;
            if (dist < kRestEps)
                ++settled;
            else
                ++active;
        };

        // ── Clear + view setup. ─────────────────────────────────────────
        const f64 t = args.smoke_frames > 0
                          ? static_cast<f64>(frame) * (1.0 / 60.0)
                          : platform::Clock::seconds(platform::Clock::ticks_now() - t0_ticks);
        const OrbitCamera cam = make_orbit_camera(static_cast<f32>(t));

        render::raster::clear_framebuffer(fb, 0xFF705038u);  // warm dusk sky
        clear_depth_far(fb);

        render::raster::ViewState view{};
        view.target = fb;
        view.view = math::look_at_rh(cam.eye, cam.target, v3(0.0f, 1.0f, 0.0f));
        view.projection = math::perspective_rh(55.0f * math::kDegToRad,
                                               static_cast<f32>(desc.render_width) /
                                                   static_cast<f32>(desc.render_height),
                                               0.1f,
                                               200.0f);
        view.tile_w = 64;
        view.tile_h = 64;
        rasterizer.begin_frame(view);

        // ── Ground (static; identity pose). ────────────────────────────
        {
            render::raster::DrawItem item{};
            item.vertices = ground.mesh.data();
            item.vertex_count = static_cast<u32>(ground.mesh.size());
            item.indices = kCubeIndices.data();
            item.index_count = static_cast<u32>(kCubeIndices.size());
            item.model = pose_model(world.get_position(ground.id),
                                    world.get_rotation(ground.id),
                                    math::mul(ground.half, 2.0f));  // unit cube → full extent
            rasterizer.submit(item);
        }

        // ── Stacked boxes. ──────────────────────────────────────────────
        for (usize i = 0; i < boxes.size(); ++i) {
            const math::Vec3 p = world.get_position(boxes[i].id);
            const math::Quat r = world.get_rotation(boxes[i].id);
            classify(p, box_prev[i]);
            if (boxes[i].color == kLayerColors[2])
                apex_y = p.y;

            render::raster::DrawItem item{};
            item.vertices = boxes[i].mesh.data();
            item.vertex_count = static_cast<u32>(boxes[i].mesh.size());
            item.indices = kCubeIndices.data();
            item.index_count = static_cast<u32>(kCubeIndices.size());
            item.model = pose_model(p, r, math::mul(boxes[i].half, 2.0f));
            rasterizer.submit(item);
        }

        // ── Spheres. ────────────────────────────────────────────────────
        for (usize i = 0; i < spheres.size(); ++i) {
            const math::Vec3 p = world.get_position(spheres[i].id);
            const math::Quat r = world.get_rotation(spheres[i].id);
            classify(p, sph_prev[i]);
            if (i == 0)
                sphere0_y = p.y;

            render::raster::DrawItem item{};
            item.vertices = sphere_meshes[i].verts.data();
            item.vertex_count = static_cast<u32>(sphere_meshes[i].verts.size());
            item.indices = sphere_meshes[i].indices.data();
            item.index_count = static_cast<u32>(sphere_meshes[i].indices.size());
            item.model = pose_model(p, r, v3(spheres[i].radius, spheres[i].radius, spheres[i].radius));
            rasterizer.submit(item);
        }

        rasterizer.end_frame();
        window->present(fb);

        if (args.smoke_frames > 0) {
            PSY_LOG_INFO(
                "sample_07: frame {} — active {}, settled {}, apex_box_y {:.3f}, sphere0_y "
                "{:.3f}{}",
                frame,
                active,
                settled,
                apex_y,
                sphere0_y,
                any_bad ? " [WARN: non-finite/runaway body]" : "");
        }

        ++frame;
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_07: smoke target reached ({}); exiting", args.smoke_frames);
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
            PSY_LOG_ERROR("sample_07: failed to write capture to {}", args.capture_out);
            for (const auto& s : spheres)
                world.destroy_body(s.id);
            for (const auto& b : boxes)
                world.destroy_body(b.id);
            world.destroy_body(ground.id);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_07: wrote capture to {}", args.capture_out);
    }

    for (const auto& s : spheres)
        world.destroy_body(s.id);
    for (const auto& b : boxes)
        world.destroy_body(b.id);
    world.destroy_body(ground.id);
    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
