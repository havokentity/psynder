// SPDX-License-Identifier: MIT
// Psynder — Sample 10. Mixed-shape rigid-body sandbox + camera physgun.
//
// The scene exercises the Wave-A rigid-body pipeline end to end through the
// public `psynder::physics` API (DESIGN.md §10.1) with a deliberately mixed
// shape set so the GJK+EPA narrowphase and the contact solver both get a
// workout:
//
//   * A large STATIC ground box (mass = 0) — the floor everything rests on.
//   * An assortment of DYNAMIC bodies dropped from varying heights:
//       - BOXES    (box/box: AABB fast-path when axis-aligned, GJK once
//                   they tumble),
//       - SPHERES  (sphere/sphere + sphere/ground analytic kernels),
//       - CAPSULES (capsule/capsule + capsule/sphere kernels),
//       - one CONVEX HULL body. Note: lane-13's GjkSupport carries no hull
//         vertex side-table yet (Narrowphase.h GjkSupport = {pos, rot, shape,
//         half_extent}), so a ConvexHull body resolves to a *bounding cube* of
//         side 2*half_extent.x through the GJK support function. We still spawn
//         one because it forces the generic GJK+EPA path (boxes with identity
//         rotation take the AABB fast-path instead), exercising the narrowphase
//         we set out to test. True authored hulls await a lane-13 side table.
//     They collide + settle into a heap on the ground.
//
//   * A camera-aimed PHYSGUN. A ray along the camera forward picks the nearest
//     body whose bounding volume it intersects (ray/AABB via the editor's
//     header-only physgun::ray_aabb_intersect). "Grab" pins the body at a hold
//     point a fixed distance in front of the camera each frame
//     (World::set_body_position); "throw" releases it with the camera-forward
//     velocity (World::set_body_velocity). These three writers (plus
//     apply_impulse) were added to physics::World for this sample — see
//     Physics.h / World.cpp and tests/unit/physics_body_mutation.cpp.
//
// Rendering is the tiled software rasterizer (`render::raster`). Each frame we
// read every body's interpolated pose back via World::get_position /
// get_rotation and submit a unit-cube / unit-icosphere / unit-capsule DrawItem
// scaled to the body's extents. An orbit camera frames the heap (a free-fly
// controller isn't needed — the physgun aims down the orbit camera's forward).
//
// Interactive controls:
//   G / left-mouse   grab the body under the crosshair (camera forward ray)
//   release G / LMB  throw it along the camera forward
//   ESC              quit
//
// Smoke mode scripts a deterministic spawn + a grab-hold-throw on a tracked
// body so the headless run is reproducible. The log prints the live body count
// and the tracked body's Y each frame (and flags any NaN / runaway).
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Same, space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the last framebuffer to PATH as a PNG.

#include "common/Lighting.h"
#include "common/MeshWinding.h"
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Physgun.h"  // header-only ray_aabb_intersect (lane 18)
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

// Model matrix from a body pose (rotation quat + world position) and a
// per-axis scale that maps the unit primitive to the body's extents.
inline math::Mat4 pose_model(math::Vec3 pos, math::Quat rot, math::Vec3 scale) noexcept {
    const math::Mat4 t = math::translate(pos);
    const math::Mat4 r = math::rotate_quat(math::quat_normalize(rot));
    const math::Mat4 s = math::scale(scale);
    return math::mul(t, math::mul(r, s));
}

// ─── Unit cube (per-face colour, spans [-0.5, +0.5]) ─────────────────────
// Layout matches sample 02 / 07 so the cube reads correctly under the
// rasterizer's CCW front-face convention. Colour is overwritten per instance.
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

// ─── Generic mesh (sphere + capsule built procedurally) ──────────────────
struct Mesh {
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;
};

// Unit icosphere (radius 1, centred at origin). One subdivision (80 tris) is a
// plenty-round ball at this scale. Mirrors sample 07's builder.
Mesh build_icosphere(u32 subdivisions, u32 rgba) {
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

    for (u32 s = 0; s < subdivisions; ++s) {
        std::vector<u32> next;
        next.reserve(idx.size() * 4);
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
        vt.normal = p;
        vt.uv = math::Vec2{0, 0};
        vt.color = rgba;
        m.verts.push_back(vt);
    }
    m.indices = std::move(idx);
    return m;
}

// Unit capsule aligned to local +Y: a cylinder of radius 1 spanning
// [-half_h, +half_h] capped by two hemispheres of radius 1. `half_h` is the
// half-height of the cylindrical section (matching the physics capsule's
// half_extent.y); the render radius is applied per-instance via the model
// scale, so geometry here uses unit radius. `rings` per hemisphere and
// `segments` around give a smooth-enough pill.
Mesh build_capsule(u32 segments, u32 rings, f32 half_h, u32 rgba) {
    Mesh m;
    auto push_vert = [&](math::Vec3 p, math::Vec3 n) {
        render::raster::Vertex vt{};
        vt.position = p;
        vt.normal = math::normalize(n);
        vt.uv = math::Vec2{0, 0};
        vt.color = rgba;
        m.verts.push_back(vt);
    };

    // Rows of the lat/long grid: top hemisphere (rings+1 rows from pole to
    // equator), then bottom hemisphere (rings+1 rows from equator to pole).
    // The equator rows are duplicated with a +/-half_h offset so the cylinder
    // body has straight sides between them.
    const u32 cols = segments + 1;
    std::vector<u32> row_start;

    // Top hemisphere: latitude phi from +pi/2 (pole) down to 0 (equator).
    for (u32 i = 0; i <= rings; ++i) {
        const f32 phi = math::kHalfPi * (1.0f - static_cast<f32>(i) / static_cast<f32>(rings));
        const f32 cphi = std::cos(phi), sphi = std::sin(phi);
        row_start.push_back(static_cast<u32>(m.verts.size()));
        for (u32 j = 0; j <= segments; ++j) {
            const f32 th = math::kTwoPi * static_cast<f32>(j) / static_cast<f32>(segments);
            const math::Vec3 n{cphi * std::cos(th), sphi, cphi * std::sin(th)};
            push_vert(v3(n.x, n.y + half_h, n.z), n);  // top cap shifted up
        }
    }
    // Bottom hemisphere: latitude phi from 0 (equator) down to -pi/2 (pole).
    for (u32 i = 0; i <= rings; ++i) {
        const f32 phi = -math::kHalfPi * (static_cast<f32>(i) / static_cast<f32>(rings));
        const f32 cphi = std::cos(phi), sphi = std::sin(phi);
        row_start.push_back(static_cast<u32>(m.verts.size()));
        for (u32 j = 0; j <= segments; ++j) {
            const f32 th = math::kTwoPi * static_cast<f32>(j) / static_cast<f32>(segments);
            const math::Vec3 n{cphi * std::cos(th), sphi, cphi * std::sin(th)};
            push_vert(v3(n.x, n.y - half_h, n.z), n);  // bottom cap shifted down
        }
    }

    // Stitch adjacent rows into quads (two triangles each), CCW so the
    // outward face is front under the rasterizer's winding.
    const u32 total_rows = static_cast<u32>(row_start.size());
    for (u32 r = 0; r + 1 < total_rows; ++r) {
        const u32 a0 = row_start[r];
        const u32 b0 = row_start[r + 1];
        for (u32 j = 0; j < segments; ++j) {
            const u32 a = a0 + j, an = a0 + j + 1;
            const u32 b = b0 + j, bn = b0 + j + 1;
            m.indices.push_back(a);
            m.indices.push_back(b);
            m.indices.push_back(an);
            m.indices.push_back(an);
            m.indices.push_back(b);
            m.indices.push_back(bn);
        }
    }
    (void)cols;
    return m;
}

// ─── Camera ──────────────────────────────────────────────────────────────
// Slow orbit around the heap. We expose eye/target so the physgun can aim a
// ray straight down the camera forward (eye → target).
struct OrbitCamera {
    math::Vec3 eye;
    math::Vec3 target;
};

OrbitCamera make_orbit_camera(f32 t_seconds) noexcept {
    const f32 radius = 12.0f;
    const f32 height = 6.0f;
    const f32 angle = t_seconds * 0.22f;
    OrbitCamera c{};
    c.eye = v3(std::cos(angle) * radius, height, std::sin(angle) * radius);
    c.target = v3(0.0f, 1.0f, 0.0f);
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
enum class Kind : u8 { Box, Sphere, Capsule, Hull };

struct Body {
    physics::BodyId id;
    Kind kind;
    math::Vec3 half;  // box: half extents; sphere/hull: x = radius; capsule: x=r, y=half_h
    u32 color;
    u32 mesh_index;  // index into the per-kind mesh table
};

// Conservative pick radius for the camera-ray bounding test, per kind.
f32 pick_radius(const Body& b) noexcept {
    switch (b.kind) {
        case Kind::Sphere:
            return b.half.x;
        case Kind::Capsule:
            return b.half.x + b.half.y;
        case Kind::Hull:
            return b.half.x;
        case Kind::Box:
        default:
            return std::sqrt(b.half.x * b.half.x + b.half.y * b.half.y + b.half.z * b.half.z);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const bool smoke = args.smoke_frames > 0;

    // ─── Platform / framebuffer ─────────────────────────────────────────
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 10 (shape sandbox + physgun)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_10: failed to create window");
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

    // Static ground: a wide, thin box whose TOP surface sits at y = 0.
    constexpr f32 kGroundHalfY = 0.5f;
    Body ground{};
    {
        physics::BodyDesc d{};
        d.shape = physics::Shape::Box;
        d.mass = 0.0f;
        d.half_extent = v3(14.0f, kGroundHalfY, 14.0f);
        d.position = v3(0.0f, -kGroundHalfY, 0.0f);
        d.friction = 0.8f;
        d.restitution = 0.1f;
        ground.id = world.create_body(d);
        ground.kind = Kind::Box;
        ground.half = d.half_extent;
        ground.color = pack_rgba(64, 68, 76);
    }
    auto ground_mesh = tint_cube(ground.color);

    // ── Mixed-shape drop set. Each entry seeds one dynamic body. Heights are
    // staggered so they don't all land at once; lateral offsets keep them in a
    // loose cluster that piles onto the ground.
    struct Spawn {
        Kind kind;
        math::Vec3 pos;
        math::Vec3 half;  // see Body::half layout
        f32 mass;
        f32 restitution;
        u32 color;
    };
    const std::array<Spawn, 9> spawns = {{
        {Kind::Box, v3(-1.6f, 3.2f, 0.0f), v3(0.45f, 0.45f, 0.45f), 1.0f, 0.0f, pack_rgba(210, 90, 70)},
        {Kind::Box, v3(0.2f, 5.0f, 0.6f), v3(0.40f, 0.40f, 0.40f), 1.0f, 0.05f, pack_rgba(225, 150, 60)},
        {Kind::Sphere, v3(1.5f, 4.0f, -0.4f), v3(0.45f, 0.45f, 0.45f), 1.2f, 0.55f, pack_rgba(110, 210, 120)},
        {Kind::Sphere, v3(-0.8f, 6.4f, 1.1f), v3(0.38f, 0.38f, 0.38f), 1.0f, 0.6f, pack_rgba(120, 200, 230)},
        {Kind::Capsule, v3(0.9f, 5.8f, -1.4f), v3(0.32f, 0.5f, 0.32f), 1.1f, 0.05f, pack_rgba(200, 120, 210)},
        {Kind::Capsule, v3(-1.9f, 7.2f, -0.7f), v3(0.30f, 0.45f, 0.30f), 1.0f, 0.0f, pack_rgba(230, 200, 90)},
        {Kind::Hull, v3(0.4f, 8.0f, 0.2f), v3(0.45f, 0.45f, 0.45f), 1.0f, 0.0f, pack_rgba(150, 160, 235)},
        {Kind::Sphere, v3(2.1f, 7.0f, 1.5f), v3(0.42f, 0.42f, 0.42f), 1.3f, 0.5f, pack_rgba(235, 235, 245)},
        {Kind::Box, v3(-0.3f, 9.0f, -1.0f), v3(0.5f, 0.3f, 0.5f), 1.0f, 0.0f, pack_rgba(180, 90, 150)},
    }};

    std::vector<Body> bodies;
    bodies.reserve(spawns.size());

    // Per-kind render meshes. Spheres + hull share the icosphere (the hull is a
    // cube physically but we draw it as a tinted cube so its facets read).
    std::vector<Mesh> sphere_meshes;   // one per sphere (per-vertex colour)
    std::vector<Mesh> capsule_meshes;  // one per capsule (half_h varies)
    std::vector<std::array<render::raster::Vertex, 24>> box_meshes;  // boxes + hull

    for (const auto& sp : spawns) {
        physics::BodyDesc d{};
        d.mass = sp.mass;
        d.position = sp.pos;
        d.half_extent = sp.half;
        d.friction = 0.6f;
        d.restitution = sp.restitution;

        Body b{};
        b.kind = sp.kind;
        b.half = sp.half;
        b.color = sp.color;

        switch (sp.kind) {
            case Kind::Box:
                d.shape = physics::Shape::Box;
                b.mesh_index = static_cast<u32>(box_meshes.size());
                box_meshes.push_back(tint_cube(sp.color));
                break;
            case Kind::Hull:
                d.shape = physics::Shape::ConvexHull;  // GJK+EPA path (cube support)
                b.mesh_index = static_cast<u32>(box_meshes.size());
                box_meshes.push_back(tint_cube(sp.color));
                break;
            case Kind::Sphere:
                d.shape = physics::Shape::Sphere;
                b.mesh_index = static_cast<u32>(sphere_meshes.size());
                sphere_meshes.push_back(build_icosphere(1, sp.color));
                break;
            case Kind::Capsule:
                d.shape = physics::Shape::Capsule;
                b.mesh_index = static_cast<u32>(capsule_meshes.size());
                capsule_meshes.push_back(build_capsule(16, 6, sp.half.y / sp.half.x, sp.color));
                break;
        }
        b.id = world.create_body(d);
        bodies.push_back(b);
    }

    // Back-face culling is on by default, so every mesh must be wound to agree
    // with its outward normals (procedural sphere/capsule especially), and a
    // directional key light is baked into the vertex colours so the flat-tinted
    // primitives gain real form (samples/common/{MeshWinding,Lighting}.h). The
    // cube winding is shared, so rewind it once against the unit-cube template.
    const samples::DirLight kLight{};
    std::array<u32, kCubeIndices.size()> cube_idx = kCubeIndices;
    samples::fix_winding(kCubeVerts.data(), cube_idx.data(), static_cast<u32>(cube_idx.size()));
    samples::apply_gouraud(ground_mesh.data(), static_cast<u32>(ground_mesh.size()), kLight);
    for (auto& m : box_meshes)
        samples::apply_gouraud(m.data(), static_cast<u32>(m.size()), kLight);
    for (auto& m : sphere_meshes) {
        samples::fix_winding(m.verts.data(), m.indices.data(), static_cast<u32>(m.indices.size()));
        samples::apply_gouraud(m.verts.data(), static_cast<u32>(m.verts.size()), kLight);
    }
    for (auto& m : capsule_meshes) {
        samples::fix_winding(m.verts.data(), m.indices.data(), static_cast<u32>(m.indices.size()));
        samples::apply_gouraud(m.verts.data(), static_cast<u32>(m.verts.size()), kLight);
    }

    // Track one body (the convex-hull, spawn index 6) for the smoke log + the
    // scripted physgun. Find its slot in `bodies`.
    usize tracked = 0;
    for (usize i = 0; i < bodies.size(); ++i)
        if (bodies[i].kind == Kind::Hull)
            tracked = i;

    PSY_LOG_INFO(
        "Psynder sample 10 running{} — {} dynamic bodies (box/sphere/capsule/hull) + ground",
        smoke ? fmt::format(" — smoke mode, {} frames", args.smoke_frames) : std::string{},
        bodies.size());
    PSY_LOG_INFO(
        "sample_10: note — ConvexHull has no vertex side-table in GjkSupport yet; it collides as "
        "a bounding cube via the GJK support fn (drives the GJK+EPA path).");

    // ─── Physgun state ──────────────────────────────────────────────────
    struct Physgun {
        bool active = false;
        usize body = 0;  // index into `bodies` of the grabbed body
        f32 hold_distance = 6.0f;
        f32 throw_speed = 14.0f;
    } gun;

    // Camera forward (eye → target), normalised. Filled each frame.
    math::Vec3 cam_eye{}, cam_fwd{};

    // Grab the nearest body the camera ray hits (ray/AABB via the editor's
    // header-only intersector). Returns true on a hit.
    auto try_grab = [&]() -> bool {
        f32 best_t = 1e30f;
        usize best = bodies.size();
        for (usize i = 0; i < bodies.size(); ++i) {
            const math::Vec3 c = world.get_position(bodies[i].id);
            const f32 r = pick_radius(bodies[i]);
            f32 t = 0.0f;
            if (editor::physgun::ray_aabb_intersect(cam_eye, cam_fwd, c, v3(r, r, r), t) &&
                t < best_t && t > 0.0f) {
                best_t = t;
                best = i;
            }
        }
        if (best == bodies.size())
            return false;
        gun.active = true;
        gun.body = best;
        gun.hold_distance = std::max(3.0f, best_t);
        return true;
    };

    auto release_and_throw = [&]() {
        if (!gun.active)
            return;
        world.set_body_velocity(bodies[gun.body].id, math::mul(cam_fwd, gun.throw_speed));
        gun.active = false;
    };

    // ─── Fixed timestep ─────────────────────────────────────────────────
    // World::step() carries its own 1/120 s accumulator. Smoke runs pin dt to
    // 1/60 (== 2 internal sub-ticks, zero residual) so poses are reproducible.
    const u64 t0_ticks = platform::Clock::ticks_now();
    u64 last_ticks = t0_ticks;
    u32 frame = 0;

    // Scripted physgun timeline for smoke mode (frame indices). We grab the
    // tracked body, hold it at the camera point for a few frames, then throw.
    constexpr u32 kScriptGrab = 2;
    constexpr u32 kScriptThrow = 6;

    while (!window->should_close()) {
        window->poll_events();

        auto* in = platform::input();

        // ESC quits outside smoke mode.
        if (!smoke && in != nullptr && in->key_down(platform::KeyCode::Escape))
            break;

        // Frame dt — fixed for smoke runs so the sim is reproducible.
        f32 dt;
        if (smoke) {
            dt = 1.0f / 60.0f;
        } else {
            const u64 now = platform::Clock::ticks_now();
            dt = static_cast<f32>(platform::Clock::seconds(now - last_ticks));
            last_ticks = now;
            if (dt > 0.10f)
                dt = 0.10f;
        }

        // ── Camera (also defines the physgun aim ray). ──────────────────
        const f64 t = smoke ? static_cast<f64>(frame) * (1.0 / 60.0)
                            : platform::Clock::seconds(platform::Clock::ticks_now() - t0_ticks);
        const OrbitCamera cam = make_orbit_camera(static_cast<f32>(t));
        cam_eye = cam.eye;
        cam_fwd = math::normalize(math::sub(cam.target, cam.eye));

        // ── Physgun input. ──────────────────────────────────────────────
        if (smoke) {
            if (frame == kScriptGrab) {
                gun.active = true;
                gun.body = tracked;
                gun.hold_distance = 6.0f;
                PSY_LOG_INFO("sample_10: [script] grab tracked body at frame {}", frame);
            } else if (frame == kScriptThrow && gun.active) {
                PSY_LOG_INFO("sample_10: [script] throw tracked body at frame {}", frame);
                release_and_throw();
            }
        } else if (in != nullptr) {
            const bool want = in->key_pressed(platform::KeyCode::G) || in->mouse().left;
            const bool grab_edge =
                in->key_pressed(platform::KeyCode::G) || (in->mouse().left && !gun.active);
            if (!gun.active && grab_edge) {
                try_grab();
            } else if (gun.active && !want) {
                release_and_throw();
            }
        }

        // Hold the grabbed body at the point in front of the camera. Done
        // *before* the step so the solver sees the held pose this tick.
        if (gun.active) {
            const math::Vec3 hold = math::add(cam_eye, math::mul(cam_fwd, gun.hold_distance));
            world.set_body_position(bodies[gun.body].id, hold);
        }

        // ── Step the world (internal 120 Hz sub-stepping). ──────────────
        world.step(dt);

        // ── Clear + view setup. ─────────────────────────────────────────
        render::raster::clear_framebuffer(fb, 0xFF40484Eu);  // cool grey sky
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

        // ── Ground. ─────────────────────────────────────────────────────
        {
            render::raster::DrawItem item{};
            item.vertices = ground_mesh.data();
            item.vertex_count = static_cast<u32>(ground_mesh.size());
            item.indices = cube_idx.data();
            item.index_count = static_cast<u32>(cube_idx.size());
            item.model = pose_model(world.get_position(ground.id),
                                    world.get_rotation(ground.id),
                                    math::mul(ground.half, 2.0f));
            rasterizer.submit(item);
        }

        // ── Dynamic bodies + NaN/runaway guard + tracked-Y readout. ─────
        bool any_bad = false;
        f32 tracked_y = 0.0f;
        for (usize i = 0; i < bodies.size(); ++i) {
            const math::Vec3 p = world.get_position(bodies[i].id);
            const math::Quat r = world.get_rotation(bodies[i].id);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                std::fabs(p.x) > 1e4f || std::fabs(p.y) > 1e4f || std::fabs(p.z) > 1e4f)
                any_bad = true;
            if (i == tracked)
                tracked_y = p.y;

            render::raster::DrawItem item{};
            switch (bodies[i].kind) {
                case Kind::Box:
                case Kind::Hull: {
                    const auto& mesh = box_meshes[bodies[i].mesh_index];
                    item.vertices = mesh.data();
                    item.vertex_count = static_cast<u32>(mesh.size());
                    item.indices = cube_idx.data();
                    item.index_count = static_cast<u32>(cube_idx.size());
                    item.model = pose_model(p, r, math::mul(bodies[i].half, 2.0f));
                    break;
                }
                case Kind::Sphere: {
                    const Mesh& mesh = sphere_meshes[bodies[i].mesh_index];
                    item.vertices = mesh.verts.data();
                    item.vertex_count = static_cast<u32>(mesh.verts.size());
                    item.indices = mesh.indices.data();
                    item.index_count = static_cast<u32>(mesh.indices.size());
                    const f32 rad = bodies[i].half.x;
                    item.model = pose_model(p, r, v3(rad, rad, rad));
                    break;
                }
                case Kind::Capsule: {
                    const Mesh& mesh = capsule_meshes[bodies[i].mesh_index];
                    item.vertices = mesh.verts.data();
                    item.vertex_count = static_cast<u32>(mesh.verts.size());
                    item.indices = mesh.indices.data();
                    item.index_count = static_cast<u32>(mesh.indices.size());
                    // Capsule geometry is built with unit radius and the body's
                    // half_h baked in along Y; scale uniformly by the radius.
                    const f32 rad = bodies[i].half.x;
                    item.model = pose_model(p, r, v3(rad, rad, rad));
                    break;
                }
            }
            rasterizer.submit(item);
        }

        rasterizer.end_frame();
        window->present(fb);

        if (smoke) {
            PSY_LOG_INFO("sample_10: frame {} — bodies {}, tracked_y {:.3f}, gun {}{}",
                         frame,
                         bodies.size(),
                         tracked_y,
                         gun.active ? "held" : "free",
                         any_bad ? " [WARN: non-finite/runaway body]" : "");
        }

        ++frame;
        if (smoke && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_10: smoke target reached ({}); exiting", args.smoke_frames);
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
            PSY_LOG_ERROR("sample_10: failed to write capture to {}", args.capture_out);
            for (const auto& b : bodies)
                world.destroy_body(b.id);
            world.destroy_body(ground.id);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_10: wrote capture to {}", args.capture_out);
    }

    for (const auto& b : bodies)
        world.destroy_body(b.id);
    world.destroy_body(ground.id);
    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
