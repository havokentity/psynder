// SPDX-License-Identifier: MIT
// Psynder — Sample 03 / M3 demo. Walking-POV "Quake room": a small BSP map
// built in-memory at startup, walked first-person with WASD + mouse-look,
// rendered face-by-face through the public Rasterizer::submit() API.
//
// Wave A's `lm_qbsp` compiler exists, but its output isn't wired into a
// shippable .psybsp file under VFS yet. So we synthesise a tiny 4-leaf map
// here using the `psynder::world::bsp::BspMap` runtime struct directly:
// two rooms joined by a doorway, axis-aligned, each face split into a
// unique-coloured n-gon so the BSP structure reads at a glance.
//
// Pipeline per frame:
//   1. Read keys + mouse via platform::input(); integrate camera.
//   2. walk_visible_leaves(map, eye, emit, ...) -> set of visible leaves.
//   3. For each visible leaf, look up its face range -> push DrawItems
//      into the rasterizer.
//   4. Present.
//
// CLI flags:
//   --smoke-frames=N         Run N frames then exit (CI liveness check).
//   --smoke-frames N         Space-separated form.
//   --smoke-capture-out PATH Write the final framebuffer to PATH as a PNG.

#include "common/CharacterController.h"
#include "common/Lighting.h"
#include "common/MeshWinding.h"
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "ui/console/ConsoleOverlay.h"
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"

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

// ─── Synthetic BSP world ─────────────────────────────────────────────────
// Two rooms joined by a doorway, axis-aligned, walked first-person.
//
//   World axes: +X right, +Y up, +Z toward the camera (right-handed).
//
//      Z = -8 ┌──────────────┐
//             │              │   ROOM A  (cluster 0)
//             │              │
//             │              │
//      Z = -2 └──┐        ┌──┘
//                │ doorway│       (cluster 1, narrow corridor)
//      Z =  0 ┌──┘        └──┐
//             │              │   ROOM B  (cluster 2)
//             │              │
//      Z =  6 └──────────────┘
//             X=-4          X=4
//
// We define 4 BSP leaves (room A floor+walls+ceiling, doorway, room B,
// solid outside) and a PVS that says room A sees the doorway + itself,
// the doorway sees both rooms + itself, room B sees the doorway + itself.
//
// Each face is a fan-triangulated quad emitted into our own vertex / index
// pool. BspFace::first_vertex indexes into the pool, vertex_count says how
// long the fan is; for a quad that's 4 verts + 6 indices.

struct World {
    world::bsp::BspMap map;
    std::vector<render::raster::Vertex> verts;
    std::vector<u32> indices;              // 1:1 with verts; fan
    std::vector<u32> face_indices_offset;  // per BspFace
    std::vector<u32> face_indices_count;
    // Axis-aligned union of every room/corridor — the character controller
    // clamps the eye inside this so walls block you (unless `noclip`).
    math::Aabb bounds{};
    f32 floor_y = 0.0f;  // ground plane the FPS eye rides on
};

// Append a CCW (viewed from `normal`) quad to the world geometry pool and
// return the BspFace describing it. The quad is in the XY plane local to
// the four corners passed in; UVs are derived from world coords for a
// rough "stretch this material across the surface" effect.
void emit_quad(World& w,
               math::Vec3 a,
               math::Vec3 b,
               math::Vec3 c,
               math::Vec3 d,
               math::Vec3 normal,
               u32 color,
               u32 material_id) {
    const u32 base = static_cast<u32>(w.verts.size());
    auto push = [&](math::Vec3 p, math::Vec2 uv) {
        render::raster::Vertex v{};
        v.position = p;
        v.normal = normal;
        v.uv = uv;
        v.lightmap_uv = {0, 0};
        v.color = color;
        w.verts.push_back(v);
    };
    push(a, {0, 0});
    push(b, {1, 0});
    push(c, {1, 1});
    push(d, {0, 1});

    // Fan-triangulate the quad: tris (0,1,2) and (0,2,3). Indices are
    // appended into our per-face index slab; each face stores its base
    // offset + count so the renderer can slice them out.
    const u32 idx_base = static_cast<u32>(w.indices.size());
    w.indices.push_back(base + 0);
    w.indices.push_back(base + 1);
    w.indices.push_back(base + 2);
    w.indices.push_back(base + 0);
    w.indices.push_back(base + 2);
    w.indices.push_back(base + 3);

    world::bsp::BspFace face{};
    face.first_vertex = base;
    face.vertex_count = 4;
    face.material = material_id;
    face.lightmap = 0xFFFFFFFFu;
    w.map.faces.push_back(face);
    w.face_indices_offset.push_back(idx_base);
    w.face_indices_count.push_back(6);
}

// Build the two-room map. We don't build a meaningful node tree (the demo
// can route everything through the PVS path), so the BSP node array stays
// empty and `locate()` falls back to "single leaf == leaves.front()". To
// keep `locate` honest for a walking eye we instead provide a 3-node tree
// that splits on Z and then on X for the doorway, returning leaf 0 / 1 / 2
// depending on where the eye actually is.
void build_world(World& w) {
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

    // Material ids are just opaque slots; the rasterizer doesn't resolve
    // them today (lane 05 will). We use them as logical groupings for
    // human-readable logs.
    constexpr u32 kMatFloor = 1;
    constexpr u32 kMatCeil = 2;
    constexpr u32 kMatWallA = 3;
    constexpr u32 kMatWallB = 4;
    constexpr u32 kMatDoor = 5;

    // Colours per cluster make the BSP structure visible at a glance.
    const u32 kColRoomAFloor = pack_rgba(110, 140, 180);  // cool blue
    const u32 kColRoomACeil = pack_rgba(70, 90, 130);
    const u32 kColRoomAWall = pack_rgba(150, 170, 200);
    const u32 kColRoomBFloor = pack_rgba(180, 140, 110);  // warm orange
    const u32 kColRoomBCeil = pack_rgba(130, 90, 70);
    const u32 kColRoomBWall = pack_rgba(200, 170, 150);
    const u32 kColDoorFloor = pack_rgba(160, 200, 130);  // green tint
    const u32 kColDoorCeil = pack_rgba(110, 150, 90);
    const u32 kColDoorWall = pack_rgba(180, 210, 150);

    // We emit faces grouped by leaf so the runtime can address them with a
    // single (first_face, face_count) pair per leaf.
    const u32 leaf0_first = static_cast<u32>(w.map.faces.size());

    // ── Leaf 0 — Room A (cluster 0) ───────────────────────────────────────
    // Floor (looking down → +Y normal flipped to up = {0,1,0} so the
    // visible side is the room interior).
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ1},
              {kRoomX0, kFloorY, kRoomAZ1},
              {0, 1, 0},
              kColRoomAFloor,
              kMatFloor);
    // Ceiling
    emit_quad(w,
              {kRoomX0, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ0},
              {0, -1, 0},
              kColRoomACeil,
              kMatCeil);
    // -X wall
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX0, kFloorY, kRoomAZ0},
              {1, 0, 0},
              kColRoomAWall,
              kMatWallA);
    // +X wall
    emit_quad(w,
              {kRoomX1, kFloorY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kFloorY, kRoomAZ1},
              {-1, 0, 0},
              kColRoomAWall,
              kMatWallA);
    // -Z back wall
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomAZ0},
              {kRoomX0, kCeilY, kRoomAZ0},
              {kRoomX1, kCeilY, kRoomAZ0},
              {kRoomX1, kFloorY, kRoomAZ0},
              {0, 0, 1},
              kColRoomAWall,
              kMatWallA);
    // +Z front wall (doorway side) — split into two strips around the door
    // so the doorway is a real opening.
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomAZ1},
              {kRoomX0, kCeilY, kRoomAZ1},
              {kDoorX0, kCeilY, kRoomAZ1},
              {kDoorX0, kFloorY, kRoomAZ1},
              {0, 0, -1},
              kColRoomAWall,
              kMatWallA);
    emit_quad(w,
              {kDoorX1, kFloorY, kRoomAZ1},
              {kDoorX1, kCeilY, kRoomAZ1},
              {kRoomX1, kCeilY, kRoomAZ1},
              {kRoomX1, kFloorY, kRoomAZ1},
              {0, 0, -1},
              kColRoomAWall,
              kMatWallA);

    const u32 leaf1_first = static_cast<u32>(w.map.faces.size());

    // ── Leaf 1 — Doorway corridor (cluster 1) ─────────────────────────────
    // Floor / ceiling of the corridor between the two rooms.
    emit_quad(w,
              {kDoorX0, kFloorY, kDoorZ0},
              {kDoorX1, kFloorY, kDoorZ0},
              {kDoorX1, kFloorY, kDoorZ1},
              {kDoorX0, kFloorY, kDoorZ1},
              {0, 1, 0},
              kColDoorFloor,
              kMatDoor);
    emit_quad(w,
              {kDoorX0, kCeilY, kDoorZ1},
              {kDoorX1, kCeilY, kDoorZ1},
              {kDoorX1, kCeilY, kDoorZ0},
              {kDoorX0, kCeilY, kDoorZ0},
              {0, -1, 0},
              kColDoorCeil,
              kMatDoor);
    // Corridor walls (-X and +X).
    emit_quad(w,
              {kDoorX0, kFloorY, kDoorZ1},
              {kDoorX0, kCeilY, kDoorZ1},
              {kDoorX0, kCeilY, kDoorZ0},
              {kDoorX0, kFloorY, kDoorZ0},
              {1, 0, 0},
              kColDoorWall,
              kMatDoor);
    emit_quad(w,
              {kDoorX1, kFloorY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ0},
              {kDoorX1, kCeilY, kDoorZ1},
              {kDoorX1, kFloorY, kDoorZ1},
              {-1, 0, 0},
              kColDoorWall,
              kMatDoor);

    const u32 leaf2_first = static_cast<u32>(w.map.faces.size());

    // ── Leaf 2 — Room B (cluster 2) ───────────────────────────────────────
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ1},
              {kRoomX0, kFloorY, kRoomBZ1},
              {0, 1, 0},
              kColRoomBFloor,
              kMatFloor);
    emit_quad(w,
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ0},
              {0, -1, 0},
              kColRoomBCeil,
              kMatCeil);
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ0},
              {kRoomX0, kFloorY, kRoomBZ0},
              {1, 0, 0},
              kColRoomBWall,
              kMatWallB);
    emit_quad(w,
              {kRoomX1, kFloorY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX1, kFloorY, kRoomBZ1},
              {-1, 0, 0},
              kColRoomBWall,
              kMatWallB);
    // Front wall (+Z far end)
    emit_quad(w,
              {kRoomX1, kFloorY, kRoomBZ1},
              {kRoomX1, kCeilY, kRoomBZ1},
              {kRoomX0, kCeilY, kRoomBZ1},
              {kRoomX0, kFloorY, kRoomBZ1},
              {0, 0, -1},
              kColRoomBWall,
              kMatWallB);
    // Back wall (-Z, doorway side) — split around the doorway.
    emit_quad(w,
              {kRoomX0, kFloorY, kRoomBZ0},
              {kRoomX0, kCeilY, kRoomBZ0},
              {kDoorX0, kCeilY, kRoomBZ0},
              {kDoorX0, kFloorY, kRoomBZ0},
              {0, 0, 1},
              kColRoomBWall,
              kMatWallB);
    emit_quad(w,
              {kDoorX1, kFloorY, kRoomBZ0},
              {kDoorX1, kCeilY, kRoomBZ0},
              {kRoomX1, kCeilY, kRoomBZ0},
              {kRoomX1, kFloorY, kRoomBZ0},
              {0, 0, 1},
              kColRoomBWall,
              kMatWallB);

    const u32 leaf_face_end = static_cast<u32>(w.map.faces.size());

    // ── Leaves ──────────────────────────────────────────────────────────
    w.map.leaves.resize(4);
    w.map.leaves[0].cluster = 0;
    w.map.leaves[0].first_face = leaf0_first;
    w.map.leaves[0].face_count = leaf1_first - leaf0_first;
    w.map.leaves[0].bounds.min = {kRoomX0, kFloorY, kRoomAZ0};
    w.map.leaves[0].bounds.max = {kRoomX1, kCeilY, kRoomAZ1};

    w.map.leaves[1].cluster = 1;
    w.map.leaves[1].first_face = leaf1_first;
    w.map.leaves[1].face_count = leaf2_first - leaf1_first;
    w.map.leaves[1].bounds.min = {kDoorX0, kFloorY, kDoorZ0};
    w.map.leaves[1].bounds.max = {kDoorX1, kCeilY, kDoorZ1};

    w.map.leaves[2].cluster = 2;
    w.map.leaves[2].first_face = leaf2_first;
    w.map.leaves[2].face_count = leaf_face_end - leaf2_first;
    w.map.leaves[2].bounds.min = {kRoomX0, kFloorY, kRoomBZ0};
    w.map.leaves[2].bounds.max = {kRoomX1, kCeilY, kRoomBZ1};

    // Solid-outside leaf (no faces). `locate()` returns this when the eye
    // wanders outside the level — the PVS walker then emits nothing, which
    // is fine: the camera shouldn't escape the rooms in normal play.
    w.map.leaves[3].cluster = world::bsp::kBspSolidCluster;
    w.map.leaves[3].first_face = 0;
    w.map.leaves[3].face_count = 0;
    w.map.leaves[3].bounds.min = {0, 0, 0};
    w.map.leaves[3].bounds.max = {0, 0, 0};

    // ── BSP node tree ───────────────────────────────────────────────────
    // node 0: split on Z == kRoomAZ1 (= kDoorZ0).
    //   back (z < -2)  → leaf 0 (Room A)
    //   front (z >= -2) → node 1
    // node 1: split on Z == kDoorZ1 (= kRoomBZ0).
    //   back (z < 0)  → node 2 (corridor X-test)
    //   front (z >= 0) → leaf 2 (Room B)
    // node 2: split on X == kDoorX0.
    //   back (x < -1)  → solid
    //   front (x >= -1) → node 3
    // node 3: split on X == kDoorX1.
    //   back (x < 1)  → leaf 1 (corridor)
    //   front (x >= 1) → solid
    //
    // Normal-form convention: dot(normal, point) >= d ⇒ front_child.
    w.map.nodes.resize(4);
    // node 0: normal (0,0,1), d = -2  -> z >= -2 is front
    w.map.nodes[0].plane_normal = {0, 0, 1};
    w.map.nodes[0].plane_d = -2.0f;
    w.map.nodes[0].front_child = 1;
    w.map.nodes[0].back_child = world::bsp::bsp_encode_leaf(0);  // Room A
    // node 1: normal (0,0,1), d = 0
    w.map.nodes[1].plane_normal = {0, 0, 1};
    w.map.nodes[1].plane_d = 0.0f;
    w.map.nodes[1].front_child = world::bsp::bsp_encode_leaf(2);  // Room B
    w.map.nodes[1].back_child = 2;                                // corridor X-test
    // node 2: normal (1,0,0), d = -1  -> x >= -1 is front
    w.map.nodes[2].plane_normal = {1, 0, 0};
    w.map.nodes[2].plane_d = -1.0f;
    w.map.nodes[2].front_child = 3;
    w.map.nodes[2].back_child = world::bsp::bsp_encode_leaf(3);  // solid
    // node 3: normal (1,0,0), d = 1
    w.map.nodes[3].plane_normal = {1, 0, 0};
    w.map.nodes[3].plane_d = 1.0f;
    w.map.nodes[3].front_child = world::bsp::bsp_encode_leaf(3);  // solid
    w.map.nodes[3].back_child = world::bsp::bsp_encode_leaf(1);   // corridor

    // ── PVS ─────────────────────────────────────────────────────────────
    // 3 clusters (A=0, corridor=1, B=2). One byte per row (3 bits used).
    // The two rooms are joined by a straight doorway corridor, so a viewer
    // standing in Room A can see through the doorway into Room B (and vice
    // versa). PVS is "potentially visible" — conservative — so each room's
    // row MUST include the far room, otherwise the far room's faces get
    // culled and you see an empty hole through the doorway. Earlier this
    // row only listed {self, corridor}, which is why the other room was
    // invisible from a side room. All three clusters are mutually visible
    // here.
    //   row 0 (Room A)   : sees 0,1,2
    //   row 1 (corridor) : sees 0,1,2
    //   row 2 (Room B)   : sees 0,1,2
    constexpr u32 kClusters = 3;
    constexpr u32 kRowBytes = 1;
    w.map.pvs.assign(kClusters * kRowBytes, 0);
    w.map.pvs[0] = 0b0000'0111;  // 0,1,2
    w.map.pvs[1] = 0b0000'0111;  // 0,1,2
    w.map.pvs[2] = 0b0000'0111;  // 0,1,2

    // ── World bounds ──────────────────────────────────────────────────────
    // Axis-aligned union of room A, the corridor, and room B. The character
    // controller clamps the eye inside this so the player can't walk through
    // the outer walls; `noclip 1` lifts the clamp so you can fly out.
    w.floor_y = kFloorY;
    w.bounds.min = {kRoomX0, kFloorY, kRoomAZ0};
    w.bounds.max = {kRoomX1, kCeilY, kRoomBZ1};
}

// ─── Visibility callback context ─────────────────────────────────────────
struct DrawCtx {
    const World* world = nullptr;
    render::raster::Rasterizer* rasterizer = nullptr;
    u32 draw_count = 0;
};

void emit_leaf_faces(const world::bsp::BspLeaf& leaf, void* user) {
    auto* ctx = static_cast<DrawCtx*>(user);
    const World& w = *ctx->world;
    if (leaf.face_count == 0)
        return;
    const usize face_lo = leaf.first_face;
    const usize face_hi = std::min<usize>(face_lo + leaf.face_count, w.map.faces.size());
    for (usize fi = face_lo; fi < face_hi; ++fi) {
        const world::bsp::BspFace& face = w.map.faces[fi];
        const u32 idx_off = w.face_indices_offset[fi];
        const u32 idx_cnt = w.face_indices_count[fi];
        if (idx_cnt == 0)
            continue;

        render::raster::DrawItem item{};
        item.vertices = w.verts.data();
        item.vertex_count = static_cast<u32>(w.verts.size());
        item.indices = w.indices.data() + idx_off;
        item.index_count = idx_cnt;
        item.model = math::identity4();
        item.material = render::raster::MaterialId{face.material};
        ctx->rasterizer->submit(item);
        ++ctx->draw_count;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 03 (Quake room)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_03: failed to create window");
        return EXIT_FAILURE;
    }

    auto* input = platform::input();

    // Build the synthetic BSP world once.
    World w;
    build_world(w);
    // Back-face culling is on by default now. Room faces carry inward-pointing
    // normals, so rewind every triangle to agree with them — that keeps the
    // interior visible and culls the back of each wall. Then bake a static
    // directional light into the vertex colours so same-coloured adjacent
    // walls read as distinct surfaces (form/depth) instead of a flat slab.
    samples::fix_winding(w.verts.data(),
                         static_cast<u32>(w.verts.size()),
                         w.indices.data(),
                         static_cast<u32>(w.indices.size()));
    samples::apply_gouraud(w.verts.data(), static_cast<u32>(w.verts.size()), samples::DirLight{});
    PSY_LOG_INFO("sample_03: world built — {} faces, {} leaves, {} verts",
                 w.map.faces.size(),
                 w.map.leaves.size(),
                 w.verts.size());

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

    // Shared first-person / free-cam controller (samples/common). FPS mode
    // by default; press V to fly; `noclip 1` lifts collision + gravity.
    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.floor_y = w.floor_y;
    cc_cfg.eye_height = 1.6f;
    // Wall standoff (collision skin). Must comfortably exceed the camera
    // near-plane distance (0.05, see perspective_rh below) so the near clip
    // can't poke through a wall and reveal the void/next room when you stand
    // close. ~0.3 m also reads like a player's body radius. The old 0.05
    // default equalled the near plane, hence the see-through.
    constexpr f32 kWallStandoff = 0.3f;
    cc_cfg.bounds_skin = kWallStandoff;
    samples::CharacterController controller{cc_cfg};
    // Generic collision: keep the eye inside the UNION of the room / corridor
    // volumes (the BSP leaf bounds), sliding along their boundary. A single
    // union AABB (the old set_bounds(w.bounds)) spanned the full room width at
    // the doorway, so you could walk straight through the wall strips beside
    // the corridor. Per-leaf volumes block those. The corridor (leaf 1) is
    // stretched a little into both rooms so its volume overlaps theirs at the
    // doorways (no dead gap to snag on) — the stretch only re-covers floor
    // already inside the adjacent room.
    math::Aabb corridor = w.map.leaves[1].bounds;
    // Overlap each room by > 2*standoff so the skinned volumes still meet at the
    // doorways (no dead gap to get stuck in once the skin grew); the stretch
    // only re-covers floor already inside the adjacent room.
    const f32 portal_overlap = 2.5f * kWallStandoff;  // 0.75 > 2 * 0.3
    corridor.min.z -= portal_overlap;
    corridor.max.z += portal_overlap;
    const std::array<math::Aabb, 3> walk_volumes = {{
        w.map.leaves[0].bounds,  // Room A
        corridor,                // doorway corridor (overlaps both rooms)
        w.map.leaves[2].bounds,  // Room B
    }};
    controller.set_volumes(walk_volumes.data(), static_cast<u32>(walk_volumes.size()));
    controller.set_mode(samples::ControllerMode::Fps);
    controller.set_position({0.0f, w.floor_y + cc_cfg.eye_height, -5.0f});  // in Room A
    controller.set_look(0.0f, 0.0f);

    auto& rasterizer = render::raster::Rasterizer::Get();

    PSY_LOG_INFO("Psynder sample 03 running{}",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();

        // ── Input integration ────────────────────────────────────────────
        // Escape quits — unless the console is open, where Esc closes the
        // console instead (handled inside sample_step's console::update).
        if (!ui::console::is_open() && input->key_down(platform::KeyCode::Escape)) {
            PSY_LOG_INFO("sample_03: escape pressed, exiting");
            break;
        }

        const u64 now = platform::Clock::ticks_now();
        // Smoke runs use a fixed dt so motion is deterministic.
        const f32 dt =
            (args.smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now - last_ticks)));
        last_ticks = now;

        if (args.smoke_frames > 0) {
            // Smoke mode pins the camera to a deterministic path so the
            // capture is identical across hosts. We sweep through Room A →
            // corridor → Room B over the first 60 frames so every PVS branch
            // gets exercised. Drive the controller's pose directly rather
            // than through input so the path stays reproducible.
            const f32 phase = static_cast<f32>(frame) / 60.0f;
            const f32 t01 = std::clamp(phase, 0.0f, 1.0f);
            // Linearly walk z from -5 (deep in A) to +3 (deep in B).
            controller.set_position({0.0f, w.floor_y + cc_cfg.eye_height, -5.0f + 8.0f * t01});
            controller.set_look(0.0f, 0.0f);
        } else if (!ui::console::is_open()) {
            // Live play: WASD + mouse-look via the shared controller. FPS
            // mode walks on the floor and clamps to the room bounds; V flies;
            // `noclip 1` lifts the clamp + gravity. Frozen while the console
            // owns input so typing a command doesn't also walk the player.
            controller.update(*input, dt);
        }

        const math::Vec3 eye = controller.eye();

        // ── Clear + view setup ───────────────────────────────────────────
        render::raster::clear_framebuffer(fb, 0xFF101418u);  // near-black
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

        // ── PVS walk → DrawItem per visible face ────────────────────────
        DrawCtx ctx{};
        ctx.world = &w;
        ctx.rasterizer = &rasterizer;
        world::bsp::walk_visible_leaves(w.map, eye, &emit_leaf_faces, &ctx);

        rasterizer.end_frame();

        // Editor F2/~ toggle + PLAY/EDIT badge bottom-right (lane 18). Drawn
        // after the rasterizer so the badge composites on top of the scene.
        // Mode toggle is independent of the controller's V fly-toggle.
        (void)editor::sample_step(*input, fb);

        // Drop-down developer console (`~`). Drawn last so the panel +
        // scrollback composite over the scene, badge, and HUD.
        ui::console::draw(fb);

        window->present(fb);

        if (args.smoke_frames > 0) {
            PSY_LOG_INFO(
                "sample_03: frame {} — eye ({:.2f},{:.2f},{:.2f}) "
                "submitted {} draws",
                frame,
                eye.x,
                eye.y,
                eye.z,
                ctx.draw_count);
        }

        ++frame;
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_03: smoke target reached ({}); exiting", args.smoke_frames);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_03: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_03: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
