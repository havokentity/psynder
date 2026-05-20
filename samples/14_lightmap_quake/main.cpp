// SPDX-License-Identifier: MIT
// Psynder — Sample 14 / offline-baked lightmaps. Walking-POV "Quake room"
// lit entirely by a lightmap that this sample bakes at startup with the real
// `lm_bake` library (tools/lm_bake), then renders back through the public
// Rasterizer::submit() API. Press B to toggle baked vs. flat (unbaked).
//
// ─── What lm_bake actually bakes (verified against tools/lm_bake/Bake.cpp) ──
//
//   lm_bake is a CPU lightmap baker. Its scene model is fully 3D:
//     - `bake::BakeScene` = a list of 3D `BakeTriangle` (v0/v1/v2 + albedo)
//       plus a list of `BakeLight` (point or directional, linear-RGB).
//     - The CLI's `.psyscene` text grammar is NOT the 2D golden grammar
//       (size/clear/rect/triangle). It is a 3D shim: `tri x0..z2 [aR aG aB]`,
//       `light_point x y z r g b I`, `light_dir dx dy dz r g b I`.
//   Output (`.lmlight`, magic "LMLT" v1) is a per-triangle planar atlas:
//     one `BakedSurface` per triangle, each an NxN grid of f16 RGB lumels
//     (irradiance). A texel (i,j) maps to triangle barycentrics
//     u=(i+0.5)/N, v=(j+0.5)/N and is valid when u+v<=1, so each surface
//     fills the lower-left triangular half of its NxN chart.
//   The lumel value is the incident diffuse irradiance at that point
//   (Lambertian direct light + optional `--bounces` indirect). Final
//   surface radiance = albedo * lumel.
//
// ─── How this sample round-trips through the baker ──────────────────────────
//
//   1. Build a small 3D room (floor/ceiling/4 walls) as one shared
//      vertex/index pool, sample_03 style, but each face is subdivided into a
//      grid of sub-quads so per-vertex shading can resolve the light's
//      hotspot + falloff. Each sub-quad's two triangles are also pushed into a
//      `bake::BakeScene` with the face's albedo, plus one warm ceiling point
//      light. NOTE: lm_bake takes each triangle's shading normal from its
//      WINDING (cross(v1-v0, v2-v0)), not the BakeTriangle::normal field, so
//      emit_quad orients the bake copy's winding to face the room interior
//      (see the comment there) — otherwise every surface bakes to black.
//   2. Call `bake::bake(scene, opt)` in-process (link lm_bake_lib) -> a
//      `BakedAtlas`. Serialize it with `bake::write_lmlight()` to a real
//      `.lmlight` file in the OS temp dir, then load it straight back with
//      `bake::read_lmlight()`. The render path consumes ONLY the reloaded
//      atlas, so the demo genuinely round-trips through the on-disk format.
//   3. The rasterizer's first-class lightmap-atlas sampling path is still a
//      lane-10 stub (SurfaceCache is keyed but the base*lightmap product is
//      not cooked yet — see engine/render/raster/TileRaster.cpp:454). The
//      rasterizer DOES do perspective-correct per-vertex Gouraud colour with
//      no texture, so this sample does the lightmap lookup itself: for every
//      vertex it inverts the baker's texel mapping, samples the reloaded
//      atlas at that lumel, and folds `albedo * lumel` into the vertex
//      colour. The result is a 3D room lit by the baked lightmap, shaded by
//      the stock rasterizer. Toggling baked off restores flat albedo so the
//      lightmap's contribution is obvious.
//
// This is a 3D lit room driven end-to-end by the actual baker, not a faked
// approximation. The only honesty caveat is that shading is per-vertex
// (Gouraud over the subdivided mesh) rather than a true per-lumel texture
// fetch — forced by the missing lane-10 surface cooker. The bake itself is
// the real lm_bake output, reloaded from disk through the public reader.
//
// CLI flags (mirror the other samples):
//   --smoke-frames=N         Run N frames then exit (CI liveness check).
//   --smoke-frames N         Space-separated form.
//   --smoke-capture-out PATH Write the final framebuffer to PATH as a PNG.

#include "common/CharacterController.h"
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "core/console/Console.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "ui/imm/DebugHud.h"
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"

#include "Bake.h"  // tools/lm_bake — bake(), write_lmlight(), read_lmlight()

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;
namespace bake = psynder::tools::bake;

namespace {

// ─── CLI parsing ─────────────────────────────────────────────────────────
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
    constexpr std::string_view kFlag = "--smoke-frames=";
    constexpr std::string_view kFlagSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kFlag)) {
            a.smoke_frames = parse_uint(s.substr(kFlag.size()));
        } else if (s == kFlagSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

u8 to_u8(f32 v) noexcept {
    const f32 c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<u8>(c * 255.0f + 0.5f);
}

void clear_depth_far(render::Framebuffer& fb) noexcept {
    if (!fb.depth)
        return;
    u32 packed_far = 0;
    const f32 one = 1.0f;
    std::memcpy(&packed_far, &one, sizeof(packed_far));
    packed_far &= 0xFFFFFF00u;
    const usize n = static_cast<usize>(fb.width) * fb.height;
    for (usize i = 0; i < n; ++i)
        fb.depth[i] = packed_far;
}

// ─── Room geometry + parallel bake scene ─────────────────────────────────
//
// One axis-aligned box room. Every quad is fan-triangulated into a shared
// vertex/index pool (rendered via Rasterizer::submit, sample_03 style). The
// SAME triangles are pushed into a bake::BakeScene so the lightmap we bake
// lines up 1:1 with what we draw. Per-vertex we store the surface albedo in
// `albedo_rgb` so the toggle can pick flat-albedo or albedo*lumel.

struct Vertex {
    render::raster::Vertex r{};  // position/normal/uv/lightmap_uv/color
    math::Vec3 albedo{0.7f, 0.7f, 0.7f};
};

struct World {
    world::bsp::BspMap map;
    std::vector<Vertex> verts;
    std::vector<u32> indices;
    std::vector<u32> face_indices_offset;
    std::vector<u32> face_indices_count;
    // Parallel bake scene: triangle index t maps 1:1 to the t-th BakedSurface.
    // `tri_render_verts[t]` are the three render-vertex indices that triangle
    // covers, in the SAME winding order as the bake triangle's v0/v1/v2 — so a
    // texel lookup for any of those vertices uses matching barycentrics.
    bake::BakeScene scene;
    std::vector<std::array<u32, 3>> tri_render_verts;
    math::Aabb bounds{};
    f32 floor_y = 0.0f;
};

// Push one CCW quad (a,b,c,d viewed from `normal`) into the render pool AND
// the bake scene (two triangles), as its own BspFace. UVs span the quad.
void emit_quad(World& w,
               math::Vec3 a,
               math::Vec3 b,
               math::Vec3 c,
               math::Vec3 d,
               math::Vec3 normal,
               math::Vec3 albedo,
               u32 material_id) {
    const u32 base = static_cast<u32>(w.verts.size());
    auto push = [&](math::Vec3 p, math::Vec2 uv) {
        Vertex v{};
        v.r.position = p;
        v.r.normal = normal;
        v.r.uv = uv;
        v.r.lightmap_uv = {0, 0};
        v.r.color = pack_rgba(to_u8(albedo.x), to_u8(albedo.y), to_u8(albedo.z));
        v.albedo = albedo;
        w.verts.push_back(v);
    };
    push(a, {0, 0});
    push(b, {1, 0});
    push(c, {1, 1});
    push(d, {0, 1});

    const u32 idx_base = static_cast<u32>(w.indices.size());
    const std::array<u32, 6> fan = {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3};
    for (u32 idx : fan)
        w.indices.push_back(idx);

    // Mirror the two triangles into the bake scene. IMPORTANT: lm_bake derives
    // each triangle's shading normal from its WINDING (cross(v1-v0, v2-v0)),
    // NOT from the `BakeTriangle::normal` field — see Bake.cpp::build_basis.
    // So we orient the bake triangle so its geometric normal faces the room
    // interior (the `normal` arg). If the render winding's geometric normal
    // disagrees, we swap v1/v2 for the bake copy (the render winding stays as
    // emitted). `tri_render_verts` records the render-vertex indices in the
    // bake winding order so the texel lookup uses matching barycentrics.
    auto add_tri = [&](u32 i0, u32 i1, u32 i2) {
        math::Vec3 p0 = w.verts[i0].r.position;
        math::Vec3 p1 = w.verts[i1].r.position;
        math::Vec3 p2 = w.verts[i2].r.position;
        const math::Vec3 geo = math::cross(math::sub(p1, p0), math::sub(p2, p0));
        if (math::dot(geo, normal) < 0.0f) {
            std::swap(i1, i2);
            std::swap(p1, p2);
        }
        bake::BakeTriangle t{};
        t.v0 = p0;
        t.v1 = p1;
        t.v2 = p2;
        t.normal = normal;
        t.albedo = albedo;
        w.scene.triangles.push_back(t);
        w.tri_render_verts.push_back({i0, i1, i2});
    };
    add_tri(base + 0, base + 1, base + 2);
    add_tri(base + 0, base + 2, base + 3);

    world::bsp::BspFace face{};
    face.first_vertex = base;
    face.vertex_count = 4;
    face.material = material_id;
    face.lightmap = 0xFFFFFFFFu;
    w.map.faces.push_back(face);
    w.face_indices_offset.push_back(idx_base);
    w.face_indices_count.push_back(6);
}

// Emit a CCW quad subdivided into an `n x n` grid of sub-quads (each its own
// face / lightmap surface). Bilinear-interpolating the four corners gives the
// sub-quad vertices. Subdivision matters here: per-vertex Gouraud shading only
// captures the lightmap's curvature where the mesh has vertices, so a single
// big wall would show just a linear ramp. A modest grid resolves the point
// light's hotspot + 1/r^2 falloff so the bake actually looks baked.
void emit_wall(World& w,
               math::Vec3 a,
               math::Vec3 b,
               math::Vec3 c,
               math::Vec3 d,
               math::Vec3 normal,
               math::Vec3 albedo,
               u32 material_id,
               u32 n) {
    const auto bilerp = [&](f32 s, f32 t) {
        // s along a->b (and d->c), t along a->d (and b->c).
        const math::Vec3 ab = math::add(math::mul(a, 1.0f - s), math::mul(b, s));
        const math::Vec3 dc = math::add(math::mul(d, 1.0f - s), math::mul(c, s));
        return math::add(math::mul(ab, 1.0f - t), math::mul(dc, t));
    };
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f32 s0 = static_cast<f32>(i) / static_cast<f32>(n);
            const f32 s1 = static_cast<f32>(i + 1) / static_cast<f32>(n);
            const f32 t0 = static_cast<f32>(j) / static_cast<f32>(n);
            const f32 t1 = static_cast<f32>(j + 1) / static_cast<f32>(n);
            emit_quad(w, bilerp(s0, t0), bilerp(s1, t0), bilerp(s1, t1), bilerp(s0, t1), normal, albedo, material_id);
        }
    }
}

void build_world(World& w) {
    constexpr f32 kFloorY = 0.0f;
    constexpr f32 kCeilY = 3.0f;
    constexpr f32 kX0 = -3.0f;
    constexpr f32 kX1 = 3.0f;
    constexpr f32 kZ0 = -3.5f;
    constexpr f32 kZ1 = 3.5f;
    // Per-face subdivision. 6 faces * kSub^2 quads * 2 tris each. At kSub=5
    // that's 300 triangles — small enough that the brute-force baker finishes
    // a 10-lumel/1-bounce bake in a fraction of a second, but dense enough
    // that per-vertex Gouraud resolves the light's hotspot + falloff.
    constexpr u32 kSub = 5;

    // Logical material ids (the rasterizer doesn't resolve them today; we use
    // per-vertex colour). Albedos are mid-grey with subtle tints so the baked
    // falloff reads clearly against a single ceiling light.
    constexpr u32 kMatFloor = 1, kMatCeil = 2, kMatWall = 3;
    const math::Vec3 kAlbFloor{0.72f, 0.70f, 0.66f};
    const math::Vec3 kAlbCeil{0.62f, 0.64f, 0.70f};
    const math::Vec3 kAlbWallX{0.78f, 0.70f, 0.64f};  // warm side walls
    const math::Vec3 kAlbWallZ{0.66f, 0.70f, 0.78f};  // cool end walls

    const u32 leaf0_first = static_cast<u32>(w.map.faces.size());

    // Floor (+Y up normal, interior-facing).
    emit_wall(w,
              {kX0, kFloorY, kZ0},
              {kX1, kFloorY, kZ0},
              {kX1, kFloorY, kZ1},
              {kX0, kFloorY, kZ1},
              {0, 1, 0},
              kAlbFloor,
              kMatFloor,
              kSub);
    // Ceiling (-Y).
    emit_wall(w,
              {kX0, kCeilY, kZ1},
              {kX1, kCeilY, kZ1},
              {kX1, kCeilY, kZ0},
              {kX0, kCeilY, kZ0},
              {0, -1, 0},
              kAlbCeil,
              kMatCeil,
              kSub);
    // -X wall.
    emit_wall(w,
              {kX0, kFloorY, kZ1},
              {kX0, kCeilY, kZ1},
              {kX0, kCeilY, kZ0},
              {kX0, kFloorY, kZ0},
              {1, 0, 0},
              kAlbWallX,
              kMatWall,
              kSub);
    // +X wall.
    emit_wall(w,
              {kX1, kFloorY, kZ0},
              {kX1, kCeilY, kZ0},
              {kX1, kCeilY, kZ1},
              {kX1, kFloorY, kZ1},
              {-1, 0, 0},
              kAlbWallX,
              kMatWall,
              kSub);
    // -Z wall.
    emit_wall(w,
              {kX0, kFloorY, kZ0},
              {kX0, kCeilY, kZ0},
              {kX1, kCeilY, kZ0},
              {kX1, kFloorY, kZ0},
              {0, 0, 1},
              kAlbWallZ,
              kMatWall,
              kSub);
    // +Z wall.
    emit_wall(w,
              {kX1, kFloorY, kZ1},
              {kX1, kCeilY, kZ1},
              {kX0, kCeilY, kZ1},
              {kX0, kFloorY, kZ1},
              {0, 0, -1},
              kAlbWallZ,
              kMatWall,
              kSub);

    const u32 leaf_face_end = static_cast<u32>(w.map.faces.size());

    // One leaf containing every face (single convex room), plus a solid
    // outside leaf so `locate()` is honest when the eye leaves the box.
    w.map.leaves.resize(2);
    w.map.leaves[0].cluster = 0;
    w.map.leaves[0].first_face = leaf0_first;
    w.map.leaves[0].face_count = leaf_face_end - leaf0_first;
    w.map.leaves[0].bounds.min = {kX0, kFloorY, kZ0};
    w.map.leaves[0].bounds.max = {kX1, kCeilY, kZ1};
    w.map.leaves[1].cluster = world::bsp::kBspSolidCluster;
    w.map.leaves[1].first_face = 0;
    w.map.leaves[1].face_count = 0;
    w.map.leaves[1].bounds.min = {0, 0, 0};
    w.map.leaves[1].bounds.max = {0, 0, 0};

    // No node tree -> locate() falls back to leaves.front(); the PVS walk
    // emits the single room. One cluster, visible to itself.
    w.map.pvs.assign(1, 0b0000'0001);

    // Bake lights: a warm point light hung near the ceiling, off-centre to
    // +Z so the hotspot lands on the floor/walls asymmetrically and the
    // 1/r^2 falloff is obvious across the room. Intensity tuned for the
    // ~3-4 m room scale.
    bake::BakeLight key{};
    key.kind = bake::LightKind::kPoint;
    key.position = {0.0f, kCeilY - 0.5f, 1.4f};
    key.color = {1.0f, 0.92f, 0.78f};
    key.intensity = 11.0f;
    w.scene.lights.push_back(key);

    w.floor_y = kFloorY;
    w.bounds.min = {kX0, kFloorY, kZ0};
    w.bounds.max = {kX1, kCeilY, kZ1};
}

// ─── Bake round-trip ──────────────────────────────────────────────────────
//
// Bake the scene in-process, write a real .lmlight, read it straight back.
// Returns the RELOADED atlas (so the render path only ever sees bytes that
// survived the on-disk format). On any failure we return an empty atlas and
// the render falls back to flat albedo.
bake::BakedAtlas bake_and_roundtrip(const World& w, u32 resolution, u32 bounces) {
    bake::BakeOptions opt{};
    opt.lightmap_resolution = resolution;
    opt.max_indirect_bounces = bounces;
    opt.indirect_samples_per_bounce = 8;

    const bake::BakedAtlas baked = bake::bake(w.scene, opt);

    std::vector<u8> bytes;
    bake::write_lmlight(baked, bytes);

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path out = fs::temp_directory_path(ec) / "psynder_sample14_room.lmlight";
    {
        std::ofstream os(out, std::ios::binary | std::ios::trunc);
        if (os && !bytes.empty()) {
            os.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        }
        if (!os) {
            PSY_LOG_ERROR("sample_14: failed to write {}", out.string());
            return {};
        }
    }
    PSY_LOG_INFO("sample_14: baked {} surfaces ({}x{} lumels, {} bounces) -> {} ({} bytes)",
                 baked.surfaces.size(),
                 resolution,
                 resolution,
                 bounces,
                 out.string(),
                 bytes.size());

    // Reload from disk through the public reader.
    std::ifstream is(out, std::ios::binary);
    std::vector<u8> reread((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    bake::BakedAtlas loaded;
    std::string err;
    if (!bake::read_lmlight(reread, loaded, &err)) {
        PSY_LOG_ERROR("sample_14: read_lmlight failed: {}", err);
        return {};
    }
    PSY_LOG_INFO("sample_14: reloaded {} surfaces from .lmlight", loaded.surfaces.size());
    return loaded;
}

// Sample the baked lumel for a world-space point on triangle `tri`. Inverts
// the baker's texel mapping: barycentrics (u,v) of `p` on the triangle, then
// nearest lumel (i,j) = clamp(u*W), clamp(v*H). Mirrors Bake.cpp's
// sample_surface_radiance so we read the same texel the baker filled.
math::Vec3 sample_lumel(
    const bake::BakedSurface& surf, math::Vec3 v0, math::Vec3 v1, math::Vec3 v2, math::Vec3 p) {
    if (surf.width == 0 || surf.height == 0)
        return {0, 0, 0};
    const math::Vec3 e1 = math::sub(v1, v0);
    const math::Vec3 e2 = math::sub(v2, v0);
    const math::Vec3 q = math::sub(p, v0);
    const f32 d00 = math::dot(e1, e1);
    const f32 d01 = math::dot(e1, e2);
    const f32 d11 = math::dot(e2, e2);
    const f32 d20 = math::dot(q, e1);
    const f32 d21 = math::dot(q, e2);
    const f32 denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-9f)
        return {0, 0, 0};
    const f32 inv = 1.0f / denom;
    f32 u = (d11 * d20 - d01 * d21) * inv;
    f32 v = (d00 * d21 - d01 * d20) * inv;
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const i32 i = std::clamp(static_cast<i32>(u * static_cast<f32>(surf.width)),
                             0,
                             static_cast<i32>(surf.width) - 1);
    const i32 j = std::clamp(static_cast<i32>(v * static_cast<f32>(surf.height)),
                             0,
                             static_cast<i32>(surf.height) - 1);
    const usize px = (static_cast<usize>(j) * surf.width + static_cast<usize>(i)) * 3u;
    return {surf.pixels[px + 0], surf.pixels[px + 1], surf.pixels[px + 2]};
}

// Fold the reloaded lightmap into per-vertex colours. For each triangle in
// the bake scene we look up the lumel nearest to each of its three corners
// and accumulate `albedo * lumel` into the corresponding render vertices,
// averaging across the (up to two) triangles that share a vertex. A small
// ambient term keeps unlit corners from going pure black. `exposure` maps
// linear irradiance into the 0..1 LDR framebuffer.
void apply_baked_colors(World& w, const bake::BakedAtlas& atlas, f32 exposure, f32 ambient) {
    if (atlas.surfaces.size() != w.scene.triangles.size()) {
        PSY_LOG_WARN("sample_14: atlas/scene triangle mismatch ({} vs {}); using flat albedo",
                     atlas.surfaces.size(),
                     w.scene.triangles.size());
        for (Vertex& v : w.verts)
            v.r.color = pack_rgba(to_u8(v.albedo.x), to_u8(v.albedo.y), to_u8(v.albedo.z));
        return;
    }

    std::vector<math::Vec3> accum(w.verts.size(), math::Vec3{0, 0, 0});
    std::vector<u32> hits(w.verts.size(), 0);

    // Iterate the bake scene triangles directly. Surface t was baked for
    // scene.triangles[t]; tri_render_verts[t] are the render vertices it
    // covers, in the bake winding order, so sample_lumel reads the same texel
    // the baker filled. A corner texel can fall just outside the valid
    // u+v<=1 half (the baker leaves the upper-right of each chart at 0), so we
    // pull each sample point a little toward the triangle centroid to land on
    // a covered lumel.
    for (usize t = 0; t < w.scene.triangles.size(); ++t) {
        const bake::BakedSurface& surf = atlas.surfaces[t];
        const bake::BakeTriangle& tri = w.scene.triangles[t];
        const std::array<u32, 3>& rv = w.tri_render_verts[t];
        const math::Vec3 centroid =
            math::mul(math::add(math::add(tri.v0, tri.v1), tri.v2), 1.0f / 3.0f);
        for (u32 c = 0; c < 3; ++c) {
            const u32 vi = rv[c];
            const math::Vec3 p =
                math::add(math::mul(w.verts[vi].r.position, 0.8f), math::mul(centroid, 0.2f));
            const math::Vec3 lumel = sample_lumel(surf, tri.v0, tri.v1, tri.v2, p);
            const math::Vec3 alb = w.verts[vi].albedo;
            const math::Vec3 lit{(ambient + lumel.x * exposure) * alb.x,
                                 (ambient + lumel.y * exposure) * alb.y,
                                 (ambient + lumel.z * exposure) * alb.z};
            accum[vi] = math::add(accum[vi], lit);
            ++hits[vi];
        }
    }

    for (usize i = 0; i < w.verts.size(); ++i) {
        math::Vec3 c = w.verts[i].albedo;  // fallback: flat albedo
        if (hits[i] > 0)
            c = math::mul(accum[i], 1.0f / static_cast<f32>(hits[i]));
        w.verts[i].r.color = pack_rgba(to_u8(c.x), to_u8(c.y), to_u8(c.z));
    }
}

// Reset every vertex to its flat (unbaked) albedo.
void apply_flat_colors(World& w) {
    for (Vertex& v : w.verts)
        v.r.color = pack_rgba(to_u8(v.albedo.x), to_u8(v.albedo.y), to_u8(v.albedo.z));
}

// ─── Visibility callback ──────────────────────────────────────────────────
struct DrawCtx {
    const World* world = nullptr;
    render::raster::Rasterizer* rasterizer = nullptr;
    // Flat snapshot of render vertices (positions/uv/normal constant; only
    // `color` differs between baked/flat). Submitted as the vertex buffer.
    const render::raster::Vertex* verts = nullptr;
    u32 vert_count = 0;
    u32 draw_count = 0;
    u32 tri_count = 0;
};

void emit_leaf_faces(const world::bsp::BspLeaf& leaf, void* user) {
    auto* ctx = static_cast<DrawCtx*>(user);
    const World& w = *ctx->world;
    if (leaf.face_count == 0)
        return;
    const usize face_lo = leaf.first_face;
    const usize face_hi = std::min<usize>(face_lo + leaf.face_count, w.map.faces.size());
    for (usize fi = face_lo; fi < face_hi; ++fi) {
        const u32 idx_off = w.face_indices_offset[fi];
        const u32 idx_cnt = w.face_indices_count[fi];
        if (idx_cnt == 0)
            continue;
        render::raster::DrawItem item{};
        item.vertices = ctx->verts;
        item.vertex_count = ctx->vert_count;
        item.indices = w.indices.data() + idx_off;
        item.index_count = idx_cnt;
        item.model = math::identity4();
        item.material = render::raster::MaterialId{w.map.faces[fi].material};
        ctx->rasterizer->submit(item);
        ++ctx->draw_count;
        ctx->tri_count += idx_cnt / 3u;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 14 (baked lightmap room)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_14: failed to create window");
        return EXIT_FAILURE;
    }
    auto* input = platform::input();

    // Build the room + parallel bake scene once.
    World w;
    build_world(w);
    PSY_LOG_INFO("sample_14: room built — {} faces, {} verts, {} bake triangles, {} lights",
                 w.map.faces.size(),
                 w.verts.size(),
                 w.scene.triangles.size(),
                 w.scene.lights.size());

    // Bake the lightmap and round-trip it through .lmlight. Tiny resolution +
    // a single bounce keeps --smoke-frames=4 well under the 30 s test budget
    // (the scene is 12 triangles). Direct + 1 bounce so indirect fill is
    // visible in the corners.
    const bake::BakedAtlas atlas = bake_and_roundtrip(w, /*resolution=*/10, /*bounces=*/1);
    const bool have_bake = !atlas.surfaces.empty();

    // Exposure/ambient tuned for this room + light intensity so the baked
    // gradient lands in a pleasant LDR range.
    constexpr f32 kExposure = 0.85f;
    constexpr f32 kAmbient = 0.06f;

    // Baked vs. unbaked toggle. Mirrors CharacterController's lazy-cvar
    // pattern: `r_lightmap_baked` (bool, default 1) plus the B key for an
    // edge-triggered runtime flip. The cvar is the source of truth; B just
    // flips it.
    auto& console = console::Console::Get();
    console::CVar* baked_cvar = console.FindCVar("r_lightmap_baked");
    if (!baked_cvar) {
        baked_cvar =
            console.RegisterCVar("r_lightmap_baked",
                                 have_bake ? "1" : "0",
                                 "Render the room lit by the baked lightmap (1) or flat unbaked "
                                 "albedo (0). Toggle with the B key in sample_14.");
    }

    // Pre-bake both colour sets once; the toggle just swaps the active vertex
    // buffer (cheap, no per-frame relight). `verts_baked`/`verts_flat` hold
    // the render-vertex view (color differs) of `w.verts`.
    apply_flat_colors(w);
    std::vector<render::raster::Vertex> verts_flat(w.verts.size());
    for (usize i = 0; i < w.verts.size(); ++i)
        verts_flat[i] = w.verts[i].r;

    if (have_bake)
        apply_baked_colors(w, atlas, kExposure, kAmbient);
    std::vector<render::raster::Vertex> verts_baked(w.verts.size());
    for (usize i = 0; i < w.verts.size(); ++i)
        verts_baked[i] = w.verts[i].r;

    // Camera: shared FPS/free-cam controller, started inside the room.
    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.floor_y = w.floor_y;
    cc_cfg.eye_height = 1.6f;
    samples::CharacterController controller{cc_cfg};
    controller.set_bounds(w.bounds);
    controller.set_mode(samples::ControllerMode::Fps);
    // Stand in the -Z/+X corner looking diagonally toward +Z/-X (toward the
    // far corner past the ceiling light), so the floor, the far wall and a
    // side wall all fill the frame and the baked hotspot + falloff are
    // visible at once. yaw=pi faces +Z; +0.42 swings the view toward -X.
    controller.set_position({2.2f, w.floor_y + cc_cfg.eye_height, -2.8f});
    controller.set_look(math::kPi + 0.42f, -0.16f);

    // CPU framebuffer + depth.
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

    PSY_LOG_INFO("Psynder sample 14 running{} — B toggles baked/unbaked",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;
    f32 frame_ms_ring[60] = {0.0f};

    while (!window->should_close()) {
        window->poll_events();
        const u64 now = platform::Clock::ticks_now();
        const f32 frame_ms = static_cast<f32>(platform::Clock::seconds(now - last_ticks) * 1000.0);
        const f32 dt =
            (args.smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now - last_ticks)));
        last_ticks = now;
        frame_ms_ring[frame % 60u] = frame_ms;

        if (input->key_down(platform::KeyCode::Escape)) {
            PSY_LOG_INFO("sample_14: escape pressed, exiting");
            break;
        }
        // B toggles the baked cvar (edge-triggered). Disabled when no bake.
        if (have_bake && input->key_pressed(platform::KeyCode::B) && baked_cvar) {
            const bool now_on = !baked_cvar->GetBool();
            console.SetCVarOverride("r_lightmap_baked", now_on ? "1" : "0");
            PSY_LOG_INFO("sample_14: lightmap {}", now_on ? "BAKED" : "FLAT");
        }

        if (args.smoke_frames > 0) {
            // Deterministic pose looking diagonally across the lit room so the
            // capture is host-stable. A gentle dolly toward the centre keeps
            // every smoke frame inside the room bounds.
            const f32 t01 = std::clamp(static_cast<f32>(frame) / 60.0f, 0.0f, 1.0f);
            controller.set_position(
                {2.2f - 1.2f * t01, w.floor_y + cc_cfg.eye_height, -2.8f + 1.6f * t01});
            controller.set_look(math::kPi + 0.42f, -0.16f);
            // Exercise the toggle path mid-smoke so both branches run in CI.
            if (frame == 2 && baked_cvar)
                console.SetCVarOverride("r_lightmap_baked", "0");
            if (frame == 3 && baked_cvar)
                console.SetCVarOverride("r_lightmap_baked", "1");
        } else {
            controller.update(*input, dt);
        }

        const math::Vec3 eye = controller.eye();
        const bool show_baked = have_bake && baked_cvar && baked_cvar->GetBool();

        render::raster::clear_framebuffer(fb, 0xFF0E0C10u);
        clear_depth_far(fb);

        render::raster::ViewState view{};
        view.target = fb;
        view.view = controller.view_matrix();
        view.projection = math::perspective_rh(70.0f * math::kDegToRad,
                                               static_cast<f32>(desc.render_width) /
                                                   static_cast<f32>(desc.render_height),
                                               0.05f,
                                               200.0f);
        view.tile_w = 64;
        view.tile_h = 64;
        rasterizer.begin_frame(view);

        DrawCtx ctx{};
        ctx.world = &w;
        ctx.rasterizer = &rasterizer;
        ctx.verts = show_baked ? verts_baked.data() : verts_flat.data();
        ctx.vert_count = static_cast<u32>(w.verts.size());
        world::bsp::walk_visible_leaves(w.map, eye, &emit_leaf_faces, &ctx);

        rasterizer.end_frame();

        // Editor F2/~ toggle + PLAY/EDIT badge.
        (void)editor::sample_step(*input, fb);

        // Debug HUD overlay (`r_debug_hud full`).
        {
            ui::imm::DebugHudStats stats{};
            stats.frame_ms = frame_ms;
            const u32 n = std::min<u32>(frame + 1u, 60u);
            f32 sum = 0.0f;
            for (u32 i = 0; i < n; ++i)
                sum += frame_ms_ring[i];
            stats.avg_frame_ms = n ? sum / static_cast<f32>(n) : frame_ms;
            stats.draw_calls = ctx.draw_count;
            stats.triangles = ctx.tri_count;
            ui::imm::draw_debug_hud(fb, stats);
        }

        window->present(fb);

        if (args.smoke_frames > 0) {
            PSY_LOG_INFO("sample_14: frame {} — eye ({:.2f},{:.2f},{:.2f}) baked={} draws={}",
                         frame,
                         eye.x,
                         eye.y,
                         eye.z,
                         show_baked ? 1 : 0,
                         ctx.draw_count);
        }

        ++frame;
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_14: smoke target reached ({}); exiting", args.smoke_frames);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_14: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_14: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
