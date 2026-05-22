// SPDX-License-Identifier: MIT
// Psynder — Sample 14 / offline-baked lightmaps. Walking-POV "Quake room"
// lit entirely by a lightmap that this sample bakes at startup with the real
// `lm_bake` library (tools/lm_bake), then renders back through the public
// hybrid renderer facade. Press B to toggle baked vs. flat (unbaked).
//
// ─── Scene: a deliberate twin of sample 13 (RT Quake) for bake-vs-RT A/B ────
//
//   The geometry, the two ceiling point lights (position/colour/intensity),
//   and the camera start pose are copied verbatim from samples/13_rt_quake so
//   the two demos frame the IDENTICAL room: sample 13 lights it with runtime
//   raytraced shadows, sample 14 with this offline-baked lightmap. Stand them
//   side by side to compare a baked atlas against a per-pixel raytrace of the
//   same world. lm_bake casts real shadow rays (Bake.cpp::occluded), so the
//   dividing wall + doorway throw a baked shadow between the two rooms, just
//   as sample 13's runtime shadow rays do. The one model difference is light
//   falloff: lm_bake is pure inverse-square (I/r²) with no range cutoff, while
//   sample 13 softens with 1/(1+0.05·r²) and clamps at range 16 — the per-face
//   chunk tonemap (build_face_chunks) absorbs the brightness difference.
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
//   1. Build the sample-13 two-room-plus-doorway box (floor/ceiling/walls,
//      with a doorway gap punched in the dividing wall) as one shared
//      vertex/index pool, sample_03 style. Each face is a quad whose two
//      triangles are also pushed into a `bake::BakeScene` with the face's
//      albedo, plus the two warm ceiling point lights. NOTE: lm_bake takes each
//      triangle's shading normal from its WINDING (cross(v1-v0, v2-v0)), not
//      the BakeTriangle::normal field — see Bake.cpp::build_basis. So
//      emit_quad orients the bake copy's winding to face the room interior
//      (see the comment there); otherwise every surface bakes to black.
//   2. Call `bake::bake(scene, opt)` in-process (link lm_bake_lib) -> a
//      `BakedAtlas`. Serialize it with `bake::write_lmlight()` to a real
//      `.lmlight` file in the OS temp dir, then load it straight back with
//      `bake::read_lmlight()`. The render path consumes ONLY the reloaded
//      atlas, so the demo genuinely round-trips through the on-disk format.
//   3. PER-PIXEL lightmap. For every face we rasterise its baked irradiance
//      into a small RGBA8 "chunk" (build_face_chunks): each chunk texel (s,t)
//      is mapped back to the quad's 3D point, the covering bake triangle is
//      picked, its barycentrics taken, and the matching BakedSurface sampled
//      bilinearly. The chunk is tonemapped to LDR. We then set the quad's
//      base `uv` to the canonical 0,0/1,0/1,1/0,1 corners, set the vertex
//      `color` to the face albedo, and hand the chunk to the rasterizer via
//      DrawItem::lightmap_texels. The stock per-pixel surface_cached path
//      then computes albedo × chunk bilinearly at every covered pixel — a
//      true per-texel lightmap fetch, no Gouraud creases. Toggling baked off
//      drops the chunk (lightmap_texels=nullptr) and shows flat albedo so the
//      lightmap's contribution is obvious.
//
// This is a 3D lit room driven end-to-end by the actual baker: the bake is
// real lm_bake output reloaded from disk through the public reader, and the
// shading is a genuine per-pixel lightmap fetch through the rasterizer's
// surface-cached sampler.
//
// CLI flags (mirror the other samples):
//   --smoke-frames=N         Run N frames then exit (CI liveness check).
//   --smoke-frames N         Space-separated form.
//   --smoke-capture-out PATH Write the final framebuffer to PATH as a PNG.

#include "common/CharacterController.h"
#include "common/MeshWinding.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "core/console/Console.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/RenderingSystem.h"
#include "render/Texture.h"
#include "render/raster/Raster.h"
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"

#include "Bake.h"  // tools/lm_bake — bake(), write_lmlight(), read_lmlight()

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace psynder;
namespace bake = psynder::tools::bake;

namespace {

constexpr f32 kWallStandoff = 0.3f;
constexpr f32 kPortalOverlap = 2.5f * kWallStandoff;

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
// Two axis-aligned box rooms joined by a doorway corridor — the exact layout
// sample 13 raytraces. Every quad is fan-triangulated into a shared
// vertex/index pool (rendered via the hybrid facade, sample_03 style). The
// SAME triangles are pushed into a bake::BakeScene so the lightmap we bake
// lines up 1:1 with what we draw. Per-vertex we store the surface albedo in
// `albedo` so the toggle can pick flat-albedo or albedo×chunk.

struct Vertex {
    render::raster::Vertex r{};  // position/normal/uv/lightmap_uv/color
    math::Vec3 albedo{0.7f, 0.7f, 0.7f};
};

// A baked per-face lightmap chunk: a small RGBA8 grid that maps across the
// face's quad through the base uv (0,0)-(1,1). Owned by World so its bytes
// outlive the render loop (the rasterizer holds the raw pointer per frame).
struct FaceChunk {
    render::Texture2D texture;  // kChunkRes × kChunkRes RGBA8, row-major
};

struct World {
    world::bsp::BspMap map;
    std::vector<Vertex> verts;
    std::vector<u32> indices;
    std::vector<u32> face_indices_offset;
    std::vector<u32> face_indices_count;
    // Parallel bake scene: triangle index t maps 1:1 to the t-th BakedSurface.
    // Every emit_quad pushes exactly two triangles, so face `fi` owns bake
    // surfaces 2*fi (the a,b,c half) and 2*fi+1 (the a,c,d half).
    bake::BakeScene scene;
    // The four world-space corners (a,b,c,d) of each face's quad, in emit
    // order; chunk texel (s,t) bilerps these to a 3D point.
    std::vector<std::array<math::Vec3, 4>> face_quad;
    // One baked chunk per face (filled after the bake round-trips).
    std::vector<FaceChunk> face_chunks;
    math::Aabb bounds{};
    // Explicit room/corridor walkable volumes for the
    // generic convex-volume slide collision — the union AABB alone lets you
    // walk through the wall strips beside the doorway.
    std::array<math::Aabb, 3> walk_volumes{};
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
    // emitted) so the baked surface lights the interior side.
    auto add_tri = [&](u32 i0, u32 i1, u32 i2) {
        math::Vec3 p0 = w.verts[i0].r.position;
        math::Vec3 p1 = w.verts[i1].r.position;
        math::Vec3 p2 = w.verts[i2].r.position;
        const math::Vec3 geo = math::cross(math::sub(p1, p0), math::sub(p2, p0));
        if (math::dot(geo, normal) < 0.0f)
            std::swap(p1, p2);
        bake::BakeTriangle t{};
        t.v0 = p0;
        t.v1 = p1;
        t.v2 = p2;
        t.normal = normal;
        t.albedo = albedo;
        w.scene.triangles.push_back(t);
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
    // Record the quad corners (emit order a,b,c,d) for per-texel chunk baking.
    w.face_quad.push_back({a, b, c, d});
}

// Linear-RGB albedo from 8-bit channels (mirrors sample 13's rgb01 so the
// baked room uses the SAME surface colours the raytraced room does).
math::Vec3 rgb01(u32 r, u32 g, u32 b) noexcept {
    return {static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f, static_cast<f32>(b) / 255.0f};
}

void build_world(World& w) {
    // ── Geometry copied verbatim from samples/13_rt_quake (build_room). ────
    // Two boxy rooms joined by a doorway corridor; floor at Y=0, ceiling Y=3.
    //
    //      Z = -8 ┌──────────────┐
    //             │              │   ROOM A
    //      Z = -2 └──┐        ┌──┘
    //                │ doorway│
    //      Z =  0 ┌──┘        └──┐
    //             │              │   ROOM B
    //      Z =  6 └──────────────┘
    //             X=-4          X=4
    constexpr f32 kFloorY = 0.0f;
    constexpr f32 kCeilY = 3.0f;
    constexpr f32 kRoomAZ0 = -8.0f;  // back wall of room A
    constexpr f32 kRoomAZ1 = -2.0f;  // front wall of room A (doorway side)
    constexpr f32 kDoorZ0 = -2.0f;   // doorway near A
    constexpr f32 kDoorZ1 = 0.0f;    // doorway near B
    constexpr f32 kRoomBZ0 = 0.0f;   // back wall of room B (doorway side)
    constexpr f32 kRoomBZ1 = 6.0f;   // front wall of room B
    constexpr f32 kRoomX0 = -4.0f;
    constexpr f32 kRoomX1 = 4.0f;
    constexpr f32 kDoorX0 = -1.0f;  // doorway corridor extent
    constexpr f32 kDoorX1 = 1.0f;

    // Logical material ids (the rasterizer doesn't resolve them today; we use
    // per-vertex colour × the baked chunk). Same per-surface colours sample 13
    // shades with: cool-blue room A, warm-orange room B, green corridor.
    constexpr u32 kMatFloor = 1, kMatCeil = 2, kMatWall = 3;
    const math::Vec3 cA_floor = rgb01(110, 140, 180);  // cool blue room A
    const math::Vec3 cA_ceil = rgb01(70, 90, 130);
    const math::Vec3 cA_wall = rgb01(150, 170, 200);
    const math::Vec3 cB_floor = rgb01(180, 140, 110);  // warm orange room B
    const math::Vec3 cB_ceil = rgb01(130, 90, 70);
    const math::Vec3 cB_wall = rgb01(200, 170, 150);
    const math::Vec3 cDoor_floor = rgb01(160, 200, 130);  // green corridor
    const math::Vec3 cDoor_ceil = rgb01(110, 150, 90);
    const math::Vec3 cDoor_wall = rgb01(180, 210, 150);

    const math::Vec3 up{0, 1, 0};
    const math::Vec3 down{0, -1, 0};
    const math::Vec3 px{1, 0, 0};
    const math::Vec3 nx{-1, 0, 0};
    const math::Vec3 pz{0, 0, 1};
    const math::Vec3 nz{0, 0, -1};

    const u32 leaf0_first = static_cast<u32>(w.map.faces.size());

    // emit_quad orients each bake triangle's winding to face `normal` and the
    // render winding is taken as given; the corner orders below match the
    // proven interior-facing convention (CCW seen from the room interior).
    // One quad (or two strips around the doorway) per face — the lightmap's
    // hotspot + 1/r^2 falloff is resolved per-pixel inside each face's baked
    // chunk (build_face_chunks), so no mesh subdivision is needed.

    // ── Room A ─────────────────────────────────────────────────────────────
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ1},
              {kRoomX0, kFloorY, kRoomAZ1},
              up,
              cA_floor,
              kMatFloor);
    emit_quad(w,
              {kRoomX0, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ0},
              down,
              cA_ceil,
              kMatCeil);
    emit_quad(w,  // -X wall (faces +X interior)
              {kRoomX0, kFloorY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX0, kFloorY, kRoomAZ0},
              px,
              cA_wall,
              kMatWall);
    emit_quad(w,  // +X wall (faces -X interior)
              {kRoomX1, kFloorY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kFloorY, kRoomAZ1},
              nx,
              cA_wall,
              kMatWall);
    emit_quad(w,  // -Z back wall (faces +Z interior)
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ0},
              pz,
              cA_wall,
              kMatWall);
    // +Z front wall (doorway side) — two strips around the door opening.
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ1},
              {kDoorX0, kCeilY, kRoomAZ1},
              {kDoorX0, kFloorY, kRoomAZ1},
              nz,
              cA_wall,
              kMatWall);
    emit_quad(w,
              {kDoorX1, kFloorY, kRoomAZ1},
              {kDoorX1, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kFloorY, kRoomAZ1},
              nz,
              cA_wall,
              kMatWall);

    // ── Doorway corridor ────────────────────────────────────────────────
    emit_quad(w,
              {kDoorX0, kFloorY, kDoorZ0},
              {kDoorX1, kFloorY, kDoorZ0},
              {kDoorX1, kFloorY, kDoorZ1},
              {kDoorX0, kFloorY, kDoorZ1},
              up,
              cDoor_floor,
              kMatFloor);
    emit_quad(w,
              {kDoorX0, kCeilY, kDoorZ1},
              {kDoorX1, kCeilY, kDoorZ1},
              {kDoorX1, kCeilY, kDoorZ0},
              {kDoorX0, kCeilY, kDoorZ0},
              down,
              cDoor_ceil,
              kMatCeil);
    emit_quad(w,  // corridor -X wall (faces +X interior)
              {kDoorX0, kFloorY, kDoorZ1},
              {kDoorX0, kCeilY, kDoorZ1},
              {kDoorX0, kCeilY, kDoorZ0},
              {kDoorX0, kFloorY, kDoorZ0},
              px,
              cDoor_wall,
              kMatWall);
    emit_quad(w,  // corridor +X wall (faces -X interior)
              {kDoorX1, kFloorY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ1},
              {kDoorX1, kFloorY, kDoorZ1},
              nx,
              cDoor_wall,
              kMatWall);

    // ── Room B ─────────────────────────────────────────────────────────────
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ1},
              {kRoomX0, kFloorY, kRoomBZ1},
              up,
              cB_floor,
              kMatFloor);
    emit_quad(w,
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ0},
              down,
              cB_ceil,
              kMatCeil);
    emit_quad(w,  // -X wall (faces +X interior)
              {kRoomX0, kFloorY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ0},
              {kRoomX0, kFloorY, kRoomBZ0},
              px,
              cB_wall,
              kMatWall);
    emit_quad(w,  // +X wall (faces -X interior)
              {kRoomX1, kFloorY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kFloorY, kRoomBZ1},
              nx,
              cB_wall,
              kMatWall);
    emit_quad(w,  // +Z far wall (faces -Z interior)
              {kRoomX0, kFloorY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kFloorY, kRoomBZ1},
              nz,
              cB_wall,
              kMatWall);
    // -Z back wall (doorway side) — two strips around the door opening.
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ0},
              {kDoorX0, kCeilY, kRoomBZ0},
              {kDoorX0, kFloorY, kRoomBZ0},
              pz,
              cB_wall,
              kMatWall);
    emit_quad(w,
              {kDoorX1, kFloorY, kRoomBZ0},
              {kDoorX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ0},
              pz,
              cB_wall,
              kMatWall);

    const u32 leaf_face_end = static_cast<u32>(w.map.faces.size());

    // One leaf containing every face (the whole two-room box reads as one
    // convex cluster for this demo — the doorway never occludes a full room
    // from the camera path), plus a solid outside leaf so `locate()` is honest
    // when the eye leaves the box.
    w.map.leaves.resize(2);
    w.map.leaves[0].cluster = 0;
    w.map.leaves[0].first_face = leaf0_first;
    w.map.leaves[0].face_count = leaf_face_end - leaf0_first;
    w.map.leaves[0].bounds.min = {kRoomX0, kFloorY, kRoomAZ0};
    w.map.leaves[0].bounds.max = {kRoomX1, kCeilY, kRoomBZ1};
    w.map.leaves[1].cluster = world::bsp::kBspSolidCluster;
    w.map.leaves[1].first_face = 0;
    w.map.leaves[1].face_count = 0;
    w.map.leaves[1].bounds.min = {0, 0, 0};
    w.map.leaves[1].bounds.max = {0, 0, 0};

    // No node tree -> locate() falls back to leaves.front(); the PVS walk
    // emits the single cluster. One cluster, visible to itself.
    w.map.pvs.assign(1, 0b0000'0001);

    // Bake lights: the two warm ceiling point lights from sample 13, copied
    // position/colour/intensity for an apples-to-apples bake-vs-RT compare.
    // lm_bake's point model is pure inverse-square (I/r²) with shadow rays and
    // no range cutoff — sample 13 additionally softens 1/(1+0.05·r²) and clamps
    // at range 16, so the falloff curve differs slightly; the chunk tonemap
    // soaks up the brightness offset.
    bake::BakeLight room_a{};  // cool-white over room A
    room_a.kind = bake::LightKind::kPoint;
    room_a.position = {0.0f, 2.7f, -5.0f};
    room_a.color = {0.95f, 0.97f, 1.0f};
    room_a.intensity = 11.0f;
    w.scene.lights.push_back(room_a);

    bake::BakeLight room_b{};  // warm-amber over room B
    room_b.kind = bake::LightKind::kPoint;
    room_b.position = {0.0f, 2.7f, 3.0f};
    room_b.color = {1.0f, 0.85f, 0.65f};
    room_b.intensity = 11.0f;
    w.scene.lights.push_back(room_b);

    w.floor_y = kFloorY;
    w.bounds.min = {kRoomX0, kFloorY, kRoomAZ0};
    w.bounds.max = {kRoomX1, kCeilY, kRoomBZ1};
    // Walkable volumes for slide collision. The corridor is stretched in Z so it
    // overlaps both rooms past the wall standoff (no dead gap at the doorways);
    // the stretch only re-covers floor already inside the rooms.
    w.walk_volumes[0] = math::Aabb{{kRoomX0, kFloorY, kRoomAZ0}, {kRoomX1, kCeilY, kRoomAZ1}};
    w.walk_volumes[1] = math::Aabb{{kDoorX0, kFloorY, kDoorZ0 - kPortalOverlap},
                                   {kDoorX1, kCeilY, kDoorZ1 + kPortalOverlap}};
    w.walk_volumes[2] = math::Aabb{{kRoomX0, kFloorY, kRoomBZ0}, {kRoomX1, kCeilY, kRoomBZ1}};
}

// ─── Bake round-trip ──────────────────────────────────────────────────────
//
// Bake the scene in-process, write a real .lmlight, read it straight back.
// Returns the RELOADED atlas (so the render path only ever sees bytes that
// survived the on-disk format). On any failure we return an empty atlas and
// the render falls back to flat albedo.
bake::BakedAtlas bake_and_roundtrip(const bake::BakeScene& scene, u32 resolution, u32 bounces) {
    bake::BakeOptions opt{};
    opt.lightmap_resolution = resolution;
    opt.max_indirect_bounces = bounces;
    opt.indirect_samples_per_bounce = 8;

    const bake::BakedAtlas baked = bake::bake(scene, opt);

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

struct AsyncBakeState {
    std::atomic<bool> done{false};
    bool ok = false;
    bake::BakedAtlas atlas;
};

struct AsyncBakePayload {
    std::shared_ptr<AsyncBakeState> state;
    bake::BakeScene scene;
    u32 resolution = 0;
    u32 bounces = 0;
};

void bake_and_roundtrip_job(void* user) noexcept {
    std::unique_ptr<AsyncBakePayload> payload{static_cast<AsyncBakePayload*>(user)};
    try {
        payload->state->atlas =
            bake_and_roundtrip(payload->scene, payload->resolution, payload->bounces);
        payload->state->ok = !payload->state->atlas.surfaces.empty();
    } catch (...) {
        payload->state->atlas = {};
        payload->state->ok = false;
    }
    payload->state->done.store(true, std::memory_order_release);
}

std::shared_ptr<AsyncBakeState> kick_async_bake(bake::BakeScene scene, u32 resolution, u32 bounces) {
    auto state = std::make_shared<AsyncBakeState>();
    auto* payload = new AsyncBakePayload{state, std::move(scene), resolution, bounces};

    jobs::JobDesc desc{};
    desc.fn = &bake_and_roundtrip_job;
    desc.user = payload;
    desc.name = "sample_14.bake_lightmap";
    jobs::JobSystem::Get().submit(desc);
    return state;
}

// Chunk resolution: each face's baked irradiance is rasterised into a
// kChunkRes × kChunkRes RGBA8 grid that maps across the quad through the base
// uv. 32 is plenty to resolve a single point light's hotspot smoothly across a
// ~6 m wall while staying cache-friendly (32×32×4 = 4 KiB per face).
constexpr u32 kChunkRes = 32;

// Barycentrics of point `p` w.r.t. triangle (v0,v1,v2). Returns false on a
// degenerate triangle. (u,v) are the v1/v2 weights; the v0 weight is 1-u-v.
bool barycentric(math::Vec3 p, math::Vec3 v0, math::Vec3 v1, math::Vec3 v2, f32& u, f32& v) noexcept {
    const math::Vec3 e1 = math::sub(v1, v0);
    const math::Vec3 e2 = math::sub(v2, v0);
    const math::Vec3 q = math::sub(p, v0);
    const f32 d00 = math::dot(e1, e1);
    const f32 d01 = math::dot(e1, e2);
    const f32 d11 = math::dot(e2, e2);
    const f32 d20 = math::dot(q, e1);
    const f32 d21 = math::dot(q, e2);
    const f32 denom = d00 * d11 - d01 * d01;
    if (std::fabs(denom) < 1e-12f)
        return false;
    const f32 inv = 1.0f / denom;
    u = (d11 * d20 - d01 * d21) * inv;
    v = (d00 * d21 - d01 * d20) * inv;
    return true;
}

// Bilinearly sample a BakedSurface at triangle barycentrics (bu,bv). The baker
// maps texel (i,j) -> barycentric ((i+0.5)/W, (j+0.5)/H) and only fills the
// u+v<=1 half, so we sample with the half-texel bias and clamp to in-range
// texels (an out-of-half corner clamps to its nearest filled neighbour rather
// than reading the zeroed upper-right). Returns linear RGB irradiance.
math::Vec3 sample_surface_bilinear(const bake::BakedSurface& surf, f32 bu, f32 bv) noexcept {
    if (surf.width == 0 || surf.height == 0 || surf.pixels.empty())
        return {0, 0, 0};
    const f32 wf = static_cast<f32>(surf.width);
    const f32 hf = static_cast<f32>(surf.height);
    const f32 tx = std::clamp(bu, 0.0f, 1.0f) * wf - 0.5f;
    const f32 ty = std::clamp(bv, 0.0f, 1.0f) * hf - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(tx));
    const i32 y0 = static_cast<i32>(std::floor(ty));
    const f32 fx = tx - static_cast<f32>(x0);
    const f32 fy = ty - static_cast<f32>(y0);
    const auto fetch = [&](i32 x, i32 y) -> math::Vec3 {
        x = std::clamp(x, 0, static_cast<i32>(surf.width) - 1);
        y = std::clamp(y, 0, static_cast<i32>(surf.height) - 1);
        // The baker leaves the upper-right (u+v>1) half at zero; nudge such a
        // sample back onto the diagonal so corners read the lit edge texel.
        if (static_cast<u32>(x) + static_cast<u32>(y) >= surf.width) {
            const i32 m = static_cast<i32>(surf.width) - 1;
            const i32 s = x + y;
            if (s > m && s > 0) {
                x = std::clamp(x * m / s, 0, m);
                y = std::clamp(m - x, 0, m);
            }
        }
        const usize px = (static_cast<usize>(y) * surf.width + static_cast<usize>(x)) * 3u;
        return {surf.pixels[px + 0], surf.pixels[px + 1], surf.pixels[px + 2]};
    };
    const math::Vec3 c00 = fetch(x0, y0);
    const math::Vec3 c10 = fetch(x0 + 1, y0);
    const math::Vec3 c01 = fetch(x0, y0 + 1);
    const math::Vec3 c11 = fetch(x0 + 1, y0 + 1);
    const math::Vec3 a = math::add(math::mul(c00, 1.0f - fx), math::mul(c10, fx));
    const math::Vec3 b = math::add(math::mul(c01, 1.0f - fx), math::mul(c11, fx));
    return math::add(math::mul(a, 1.0f - fy), math::mul(b, fy));
}

// Reinhard-style tonemap of linear irradiance into an LDR [0,1] channel:
// c = 1 - exp(-irradiance * exposure). Soft-clips so the hotspot doesn't blow
// to flat white while still lifting the dim corners.
u8 tonemap_channel(f32 irradiance, f32 exposure) noexcept {
    const f32 c = 1.0f - std::exp(-std::max(0.0f, irradiance) * exposure);
    return to_u8(c);
}

// Build one RGBA8 chunk per face from the reloaded atlas. For each chunk texel
// (s,t) in [0,1]^2 we bilerp the quad's 3D point, pick which of the quad's two
// bake triangles covers it (the a,b,c half is t<=s, the a,c,d half t>=s — the
// fan diagonal a->c is s==t), take that triangle's barycentrics and sample its
// BakedSurface, then tonemap to RGBA8. The chunk maps across the quad through
// the base uv, so the rasterizer's surface_cached path fetches it per pixel.
void build_face_chunks(World& w, const bake::BakedAtlas& atlas, f32 exposure) {
    const usize face_count = w.map.faces.size();
    w.face_chunks.assign(face_count, FaceChunk{});

    const bool atlas_ok = atlas.surfaces.size() == w.scene.triangles.size();
    if (!atlas_ok) {
        PSY_LOG_WARN("sample_14: atlas/scene triangle mismatch ({} vs {}); chunks left empty",
                     atlas.surfaces.size(),
                     w.scene.triangles.size());
    }

    for (usize fi = 0; fi < face_count; ++fi) {
        FaceChunk& chunk = w.face_chunks[fi];
        std::vector<u32> texels(static_cast<usize>(kChunkRes) * kChunkRes, 0xFFFFFFFFu);
        if (!atlas_ok)
            continue;

        const std::array<math::Vec3, 4>& quad = w.face_quad[fi];  // a,b,c,d
        const math::Vec3 a = quad[0], b = quad[1], c = quad[2], d = quad[3];
        // The two bake triangles for this face (emit_quad pushes them in order
        // (a,b,c) then (a,c,d), so they sit at 2*fi and 2*fi+1).
        const usize st0 = fi * 2u + 0u;  // covers a,b,c
        const usize st1 = fi * 2u + 1u;  // covers a,c,d
        const bake::BakeTriangle& bt0 = w.scene.triangles[st0];
        const bake::BakeTriangle& bt1 = w.scene.triangles[st1];
        const bake::BakedSurface& bs0 = atlas.surfaces[st0];
        const bake::BakedSurface& bs1 = atlas.surfaces[st1];

        for (u32 j = 0; j < kChunkRes; ++j) {
            const f32 t = (static_cast<f32>(j) + 0.5f) / static_cast<f32>(kChunkRes);
            for (u32 i = 0; i < kChunkRes; ++i) {
                const f32 s = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kChunkRes);
                // Bilerp the quad: s along a->b (and d->c), t along a->d.
                const math::Vec3 ab = math::add(math::mul(a, 1.0f - s), math::mul(b, s));
                const math::Vec3 dc = math::add(math::mul(d, 1.0f - s), math::mul(c, s));
                const math::Vec3 p = math::add(math::mul(ab, 1.0f - t), math::mul(dc, t));

                // Pick the covering triangle and sample its surface. We sample
                // with the bake triangle's own v0/v1/v2 barycentrics so the
                // texel mapping matches what the baker filled.
                math::Vec3 irr{0, 0, 0};
                f32 bu = 0.0f, bv = 0.0f;
                if (t <= s) {
                    if (barycentric(p, bt0.v0, bt0.v1, bt0.v2, bu, bv))
                        irr = sample_surface_bilinear(bs0, bu, bv);
                } else {
                    if (barycentric(p, bt1.v0, bt1.v1, bt1.v2, bu, bv))
                        irr = sample_surface_bilinear(bs1, bu, bv);
                }

                const u8 r = tonemap_channel(irr.x, exposure);
                const u8 g = tonemap_channel(irr.y, exposure);
                const u8 bl = tonemap_channel(irr.z, exposure);
                texels[static_cast<usize>(j) * kChunkRes + i] = pack_rgba(r, g, bl);
            }
        }
        chunk.texture = render::Texture2D::from_rgba8(kChunkRes, kChunkRes, std::move(texels));
    }
}

// Set every vertex colour to its flat (unbaked) albedo. The base uv is reset
// to the quad's canonical 0,0/1,0/1,1/0,1 corners so the per-face chunk maps
// across the quad when baked rendering is on.
void set_quad_uvs_and_albedo(World& w) {
    for (usize fi = 0; fi < w.map.faces.size(); ++fi) {
        const u32 base = w.map.faces[fi].first_vertex;
        const math::Vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (u32 k = 0; k < 4; ++k) {
            Vertex& v = w.verts[base + k];
            v.r.uv = uvs[k];
            v.r.color = pack_rgba(to_u8(v.albedo.x), to_u8(v.albedo.y), to_u8(v.albedo.z));
        }
    }
}

// ─── Visibility callback ──────────────────────────────────────────────────
struct DrawCtx {
    const World* world = nullptr;
    render::RenderingSystem* renderer = nullptr;
    // Render-vertex buffer (positions/uv/normal/color all constant — colour is
    // always the face albedo; the lightmap modulates it per pixel via the
    // chunk). Submitted as the vertex buffer for every face.
    const render::raster::Vertex* verts = nullptr;
    u32 vert_count = 0;
    bool show_baked = false;  // attach each face's lightmap chunk when true
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
        // Baked mode: hand this face's lightmap chunk to the rasterizer so the
        // surface_cached path computes albedo × chunk per pixel. Flat mode:
        // leave it null so the room renders as plain albedo (toggle proof).
        if (ctx->show_baked && fi < w.face_chunks.size() && w.face_chunks[fi].texture.valid()) {
            const FaceChunk& chunk = w.face_chunks[fi];
            const render::TextureView chunk_view = chunk.texture.view();
            item.lightmap_texels = chunk_view.texels;
            item.lightmap_w = chunk_view.width;
            item.lightmap_h = chunk_view.height;
        }
        ctx->renderer->submit_raster_draw(item);
        ++ctx->draw_count;
        ctx->tri_count += idx_cnt / 3u;
    }
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 14 (baked lightmap room)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;
    return desc;
}

int sample_main(const app::AppArgs& base_args, app::WindowApp& app_host) {
    const app::AppArgs& args = base_args;
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();
    auto* input = platform::input();

    // Build the room + parallel bake scene once.
    World w;
    build_world(w);
    // Back-face culling is on by default; the room faces carry interior-facing
    // normals, so rewind every render triangle to agree with them — otherwise
    // mis-wound walls are culled and you see through to black. (Our Vertex wraps
    // a raster vertex, so copy the raster verts out for the winding decision;
    // fix_winding only reorders indices, never the verts, and per-vertex uv is
    // untouched so the baked chunks still map correctly.)
    {
        std::vector<render::raster::Vertex> rverts;
        rverts.reserve(w.verts.size());
        for (const auto& v : w.verts)
            rverts.push_back(v.r);
        samples::fix_winding(rverts.data(),
                             static_cast<u32>(rverts.size()),
                             w.indices.data(),
                             static_cast<u32>(w.indices.size()));
    }
    PSY_LOG_INFO("sample_14: room built — {} faces, {} verts, {} bake triangles, {} lights",
                 w.map.faces.size(),
                 w.verts.size(),
                 w.scene.triangles.size(),
                 w.scene.lights.size());

    // Exposure for the per-texel tonemap: chunk channel = 1 - exp(-irr * k).
    // Tuned for this room + the two 11 W/sr ceiling lights so each room's
    // hotspot is bright without flat-clipping to white and the shadowed
    // doorway / far corners stay readable.
    constexpr f32 kExposure = 2.2f;

    // Bake the lightmap and round-trip it through .lmlight on a worker. The
    // render loop starts immediately in flat mode, then atomically adopts the
    // reloaded atlas once the job is done. The scene is ~36 triangles, several
    // of them large (8x6 m floors, 8x3 m walls), so we bake 40 lumels per
    // triangle and one indirect bounce for readable corners.
    std::shared_ptr<AsyncBakeState> bake_state =
        kick_async_bake(w.scene, /*resolution=*/40, /*bounces=*/1);
    bool bake_integrated = false;
    bool have_bake = false;
    PSY_LOG_INFO("sample_14: queued async lightmap bake");

    // Baked vs. unbaked toggle. Mirrors CharacterController's lazy-cvar
    // pattern: `r_lightmap_baked` (bool, starts flat until async bake completes)
    // plus the B key for an edge-triggered runtime flip. The cvar is the source
    // of truth; B just flips it.
    auto& console = console::Console::Get();
    console::CVar* baked_cvar = console.FindCVar("r_lightmap_baked");
    if (!baked_cvar) {
        baked_cvar =
            console.RegisterCVar("r_lightmap_baked",
                                 "0",
                                 "Render the room lit by the baked lightmap (1) or flat unbaked "
                                 "albedo (0). Toggle with the B key in sample_14.");
    }

    // Set canonical quad UVs (0,0..1,1) + flat-albedo vertex colours once, then
    // rasterise the reloaded atlas into one RGBA8 chunk per face. The vertex
    // buffer is constant across the toggle — only DrawItem::lightmap_texels
    // differs (the surface_cached path multiplies albedo × chunk per pixel).
    set_quad_uvs_and_albedo(w);
    std::vector<render::raster::Vertex> verts(w.verts.size());
    for (usize i = 0; i < w.verts.size(); ++i)
        verts[i] = w.verts[i].r;

    // Camera: shared FPS/free-cam controller, started inside the room.
    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.floor_y = w.floor_y;
    cc_cfg.eye_height = 1.6f;
    // Wall standoff > the camera near plane so you can't poke the near clip
    // through a wall and see the void/next room when standing close.
    cc_cfg.bounds_skin = kWallStandoff;
    samples::CharacterController controller{cc_cfg};
    // Generic slide collision against the explicit room/corridor volumes; the
    // union AABB alone let you walk through the wall strips beside
    // the doorway.
    controller.set_volumes(w.walk_volumes.data(), static_cast<u32>(w.walk_volumes.size()));
    controller.set_mode(samples::ControllerMode::Fps);
    // SAME start pose as sample 13: stand deep in Room A looking straight down
    // +Z toward the doorway and Room B, so the two demos frame the identical
    // room for an A/B compare. yaw=0,pitch=0 faces +Z.
    controller.set_position({0.0f, w.floor_y + cc_cfg.eye_height, -5.0f});
    controller.set_look(0.0f, 0.0f);

    // CPU framebuffer + depth.
    render::Framebuffer& fb = app_host.framebuffer();

    render::RenderingSystem renderer;

    PSY_LOG_INFO("Psynder sample 14 running{} — B toggles baked/unbaked",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();
        const u64 now = platform::Clock::ticks_now();
        const f32 dt =
            (args.smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now - last_ticks)));
        last_ticks = now;

        // Run overlay input before gameplay hotkeys so toggle/escape/B frames
        // never leak into game controls.
        (void)app_host.engine_frame_update(dt);

        if (!bake_integrated && bake_state && bake_state->done.load(std::memory_order_acquire)) {
            bake_integrated = true;
            have_bake = bake_state->ok;
            if (have_bake) {
                build_face_chunks(w, bake_state->atlas, kExposure);
                if (baked_cvar)
                    console.SetCVarOverride("r_lightmap_baked", "1");
                PSY_LOG_INFO("sample_14: async lightmap ready ({} surfaces)",
                             bake_state->atlas.surfaces.size());
            } else {
                if (baked_cvar)
                    console.SetCVarOverride("r_lightmap_baked", "0");
                PSY_LOG_WARN("sample_14: async lightmap bake failed; staying flat");
            }
            bake_state->atlas = {};
        }

        // ESC quits unless the overlays consumed this input frame.
        if (input->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            PSY_LOG_INFO("sample_14: escape pressed, exiting");
            break;
        }
        // B toggles the baked cvar (edge-triggered). Disabled when no bake.
        if (have_bake && input->key_pressed(platform::KeyCode::B) && baked_cvar &&
            !editor::overlays_capturing()) {
            const bool now_on = !baked_cvar->GetBool();
            console.SetCVarOverride("r_lightmap_baked", now_on ? "1" : "0");
            PSY_LOG_INFO("sample_14: lightmap {}", now_on ? "BAKED" : "FLAT");
        }

        if (args.smoke_frames > 0) {
            // Deterministic camera path identical to sample 13: walk from deep
            // in Room A through the doorway into Room B over the first 60
            // frames (z: -5 -> +3), looking straight down +Z. Host-stable, and
            // it frames the same transition the raytraced demo captures.
            const f32 t01 = std::clamp(static_cast<f32>(frame) / 60.0f, 0.0f, 1.0f);
            controller.set_position({0.0f, w.floor_y + cc_cfg.eye_height, -5.0f + 8.0f * t01});
            controller.set_look(0.0f, 0.0f);
            // Exercise the toggle path mid-smoke so both branches run in CI.
            if (frame == 2 && baked_cvar)
                console.SetCVarOverride("r_lightmap_baked", "0");
            if (frame == 3 && baked_cvar)
                console.SetCVarOverride("r_lightmap_baked", "1");
        } else if (!editor::overlays_capturing()) {
            // Frozen while the console owns input so typing doesn't walk the cam.
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
        renderer.begin_raster_frame(view);

        DrawCtx ctx{};
        ctx.world = &w;
        ctx.renderer = &renderer;
        ctx.verts = verts.data();
        ctx.vert_count = static_cast<u32>(w.verts.size());
        ctx.show_baked = show_baked;
        world::bsp::walk_visible_leaves(w.map, eye, &emit_leaf_faces, &ctx);

        renderer.end_raster_frame();

        app_host.engine_frame_post();
        app_host.present();

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

    const bool capture_ok = app_host.write_capture_if_requested("sample_14");

    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct LightmapQuakeSample {
    static constexpr std::string_view log_name() noexcept { return "sample_14"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 14"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    static app::WindowAppOptions window_options(const app::AppArgs&) noexcept {
        return {.depth_buffer = true};
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return sample_main(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(LightmapQuakeSample)
