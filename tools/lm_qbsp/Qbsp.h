// SPDX-License-Identifier: MIT
// Psynder — lm_qbsp library entrypoint (Lane 24 / tools).
//
// Accepts a brush-list .map (Quake / TrenchBroom format) and emits a
// .psybsp file consumable by lane 10's BSP loader once that grows a
// reader. We compile a leafy BSP: each leaf marked solid / empty, each
// internal node owns a splitting plane.
//
// Wave-A scope:
//   - .map parser: classic single-line `( x y z ) ( x y z ) ( x y z ) tex u_off v_off rot u_scale v_scale` brush faces.
//   - Brush -> plane-list -> AABB.
//   - BSP build: choose a plane per node (largest brush in span), split
//     the brush set, recurse. No CSG vertex construction; we record face
//     planes and per-leaf brush counts, which is enough to validate the
//     pipeline end-to-end. (Full vertex tessellation lives in Wave-B.)
//
// Wave-B additions:
//   - Portal generation: emit a `BspPortal` per internal node that
//     bridges leaves, mirroring lane 10's `BspPortal` layout. The portal
//     winding is a square clipped on the splitting plane and bounded by
//     the world AABB, which is enough for adjacency-based culling tests.
//   - Format bumped to version 2; the v1 header layout is retired.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::qbsp {

struct MapPlane {
    math::Vec3 normal{0, 0, 0};
    f32 d = 0.0f;  // plane: dot(normal, p) = d
    std::string material;
};

struct MapBrush {
    std::vector<MapPlane> planes;  // half-spaces; brush = intersection
    math::Aabb bounds{};
};

struct MapEntity {
    std::vector<std::pair<std::string, std::string>> kv;
    std::vector<MapBrush> brushes;
};

struct MapFile {
    std::vector<MapEntity> entities;
};

bool parse_map(std::string_view text, MapFile& out, std::string* err = nullptr);

// ─── .psybsp output ──────────────────────────────────────────────────────
//
//   char  magic[4]      = "PSBP"
//   u32   version       = 2
//   u32   node_count
//   u32   leaf_count
//   u32   plane_count
//   u32   brush_count
//   u32   brush_planes_count
//   u32   portal_count            (v2)
//   u32   portal_vertices_count   (v2)
//   u32   reserved
//   ── plane table     (plane_count × Plane { Vec3 normal; f32 d }) ──
//   ── node table      (node_count  × Node  { i32 plane; i32 front, back }) ──
//   ── leaf table      (leaf_count  × Leaf  { i32 cluster; u32 flags; Aabb }) ──
//   ── brush table     (brush_count × BrushRef { u32 first_plane; u32 plane_count; Aabb }) ──
//   ── brush planes    (brush_planes_count × u32) ──
//   ── portal table    (portal_count × Portal { i32 front_leaf; i32 back_leaf; u32 first_vertex;
//                                               u32 vertex_count; Vec3 plane_normal; f32 plane_d }) ──
//   ── portal vertices (portal_vertices_count × Vec3) ──
//
// Negative `front` or `back` in a node points at a leaf; positive points at
// another node. Leaf index `~child`.

inline constexpr u32 kPsyBspMagic = 0x50425350u;  // 'PSBP'
inline constexpr u32 kPsyBspVersion = 2u;

inline constexpr u32 kLeafFlagSolid = 1u << 0;
inline constexpr u32 kLeafFlagEmpty = 1u << 1;

struct BspPlane {
    math::Vec3 normal;
    f32 d;
};
struct BspNode {
    i32 plane;
    i32 front;
    i32 back;
};
struct BspLeaf {
    i32 cluster;
    u32 flags;
    math::Aabb bounds;
};
struct BspBrush {
    u32 first_plane;
    u32 plane_count;
    math::Aabb bounds;
};

// Wave-B portal record. Mirrors lane 10's `BspPortal` (engine/world/bsp/Portal.h)
// so the loader can copy bytes 1:1. The winding lives in `portal_vertices`
// shared across all portals (CCW when viewed from `front_leaf` toward
// `back_leaf`).
struct BspPortal {
    i32 front_leaf;
    i32 back_leaf;
    u32 first_vertex;
    u32 vertex_count;
    math::Vec3 plane_normal;
    f32 plane_d;
};

// W10-2 room geometry records. `QbVertex` mirrors the rasterizer
// `render::raster::Vertex` packed layout (position / normal / uv / lightmap_uv /
// packed RGBA8) so the runtime bulk-memcpys the bytes straight into a
// `render::raster::Vertex` array; the tool serialises field-by-field little-
// endian (write_psybsp_engine) and the loader reads with the runtime struct
// stride - both agree on kQbVertexBytes == 44. `QbFace` is a fan-triangulated
// convex n-gon: `first_vertex` indexes BOTH the vertex AND the (parallel) index
// slab, since the runtime BspDraw converter aliases `geom.indices[first_vertex]`
// as the face's index block (BspDraw.h). Indices are FACE-LOCAL (0-based within
// the face's vertex block). `lightmap` == kBspNoLightmap when unlit.
struct QbVertex {
    math::Vec3 position;
    math::Vec3 normal;
    math::Vec2 uv;
    math::Vec2 lightmap_uv;
    u32 color = 0xFFFFFFFFu;
};
inline constexpr u32 kQbVertexBytes = 44u;  // 3+3+2+2 f32 + 1 u32, packed LE.

struct QbFace {
    u32 first_vertex;
    u32 vertex_count;
    u32 material;
    u32 lightmap;
};
inline constexpr u32 kBspNoLightmap = 0xFFFFFFFFu;  // "no baked lightmap" sentinel.

// W12-2 per-face baked lightmap. One block of `width * height` RGB16F lumels
// (3 half-floats each, row-major) sampled by the face's base UV (0..1). The
// directory record `face` ties it to a `CompiledBsp::faces` index; `pixel_offset`
// is the BYTE offset into the shared `lightmap_pixels` blob. Mirrors the engine
// `world::bsp::BspFileLightmap` on-disk record (BspFormat.h) so write_psybsp_engine
// emits it 1:1. Lumels are stored half-float so the bake can carry HDR irradiance.
struct QbLightmap {
    u32 face = 0u;
    u32 width = 0u;
    u32 height = 0u;
    u32 pixel_offset = 0u;  // byte offset into CompiledBsp::lightmap_pixels.
};
inline constexpr u32 kQbLightmapTexelBytes = 6u;  // RGB16F.

struct CompiledBsp {
    std::vector<BspPlane> planes;
    std::vector<BspNode> nodes;
    std::vector<BspLeaf> leaves;
    std::vector<BspBrush> brushes;
    std::vector<u32> brush_planes;            // flattened plane indices per brush
    std::vector<BspPortal> portals;           // Wave-B
    std::vector<math::Vec3> portal_vertices;  // Wave-B (windings)

    // W10-2: emitted room geometry (rooms path only). Faces are ordered by leaf
    // (leaf i owns faces [leaf_first_face[i], leaf_first_face[i]+leaf_face_count[i]))
    // so write_psybsp_engine can stamp the per-leaf face range into the PBSP v1
    // leaf records and PVS culling skips a culled leaf's faces wholesale.
    std::vector<QbFace> faces;
    std::vector<QbVertex> vertices;
    std::vector<u32> indices;                 // face-local, parallel to vertices
    std::vector<u32> leaf_first_face;         // per leaf: index into `faces`
    std::vector<u32> leaf_face_count;         // per leaf: face count

    // W12-2: baked lightmaps (filled by bake_room_lightmaps; empty otherwise so
    // the brush path / an un-baked rooms blob stays unlit). `lightmaps` is the
    // per-lit-face directory; `lightmap_pixels` is the packed RGB16F lumel blob
    // the directory's `pixel_offset` rows index into.
    std::vector<QbLightmap> lightmaps;
    std::vector<u8> lightmap_pixels;
};

// Compile the worldspawn (entity 0) brushes into a leafy BSP. Other
// entities are passed through unused in Wave-A (the runtime loads them
// from the level file).
bool compile_bsp(const MapFile& map, CompiledBsp& out, std::string* err = nullptr);

void write_psybsp(const CompiledBsp& bsp, std::vector<u8>& out);
bool read_psybsp(std::span<const u8> bytes, CompiledBsp& out, std::string* err = nullptr);

// ─── Additive: `.rooms` source + engine-format (PBSP v1) emitter ─────────────
//
// The Wave-A/B path above compiles a Quake `.map` brush list into the tool's own
// `PSBP` v2 blob (planes/nodes/leaves/brushes/portals, NO faces and NO baked
// PVS). The engine runtime loader `world::bsp::Bsp::load` reads a *different*
// on-disk format — `PBSP` v1 (BspFormat.h): nodes / leaves / faces / vertices /
// indices + a **baked PVS bit-vector table**. So the brush path could not feed
// the runtime end-to-end (it emits neither the loader's magic/layout nor a PVS).
//
// This block closes that gap additively, without touching the existing brush
// pipeline or its format:
//   * a small, deterministic `.rooms` source format (axis-aligned room volumes
//     + explicit portals) that authors a clean multi-room indoor level — the
//     on-disk authoring of what games/duke_demo assembled in code;
//   * `compile_rooms`, which builds a leafy BSP (one leaf/cluster per room, a
//     median-split kd-tree of nodes so `Bsp::locate` descends correctly) and a
//     portal table from the explicit portal list;
//   * `write_psybsp_engine`, which BAKES the PVS (Quake-style leaf-portal flood,
//     reusing engine `world::bsp::build_pvs`) and serialises the engine `PBSP`
//     v1 layout that `Bsp::load` validates and consumes.
//
// W10-2: the rooms compiler ALSO emits REAL room geometry (see compile_rooms +
// write_psybsp_engine). For each room box it tessellates the 6 axis-aligned
// faces (4 walls + floor + ceiling) as PBSP v1 faces + vertices + indices,
// INWARD-facing so the room interior is visible from a camera standing inside
// it. Faces are stored per-leaf so PVS culling skips a culled leaf's faces.
// Lightmaps are out of scope (flat/unlit). The QbVertex / QbFace records used
// by CompiledBsp are declared above (next to the other compiled tables).

struct RoomVolume {
    i32 cluster = 0;
    math::Aabb bounds{};
    std::string name;
};

struct RoomPortal {
    i32 cluster_a = 0;
    i32 cluster_b = 0;
};

struct RoomsFile {
    std::vector<RoomVolume> rooms;
    std::vector<RoomPortal> portals;
};

// Parse a `.rooms` source (see assets/maps/duke_e1m1.rooms for the grammar).
bool parse_rooms(std::string_view text, RoomsFile& out, std::string* err = nullptr);

// Compile rooms -> a leafy CompiledBsp: leaves carry the room bounds + cluster,
// nodes form a median-split kd-tree over the leaf boxes (so `Bsp::locate`
// resolves an arbitrary point to its room leaf), portals mirror the explicit
// open connections (front_leaf/back_leaf are LEAF indices == room order).
bool compile_rooms(const RoomsFile& rooms, CompiledBsp& out, std::string* err = nullptr);

// ─── W12-2: per-face lightmap bake ────────────────────────────────────────
//
// Bake a per-lumel lightmap for every room face emitted by compile_rooms and
// stash it on `bsp` (bsp.lightmaps + bsp.lightmap_pixels; each face's
// QbFace::lightmap is repointed at its directory row). The bake is OFFLINE,
// pure-CPU, and DETERMINISTIC (no RNG, no time, integer lumel centres) so the
// emitted .psybsp bytes are stable across runs.
//
// Lighting model (DESIGN.md §8.1, "believable per-lumel shade", not full
// radiosity): ambient + N point lights, each lumel's irradiance =
//   ambient + sum_l ( color_l * intensity_l * max(0, dot(N, L)) * atten(dist)
//                      * visibility(lumel -> light) )
// `visibility` is a coarse ray-vs-room-box occlusion test against the OTHER
// room boxes (so a wall shadows the lumels a neighbouring room would block) plus
// a cheap corner-darkening AO term (lumels near a wall edge gather less of the
// hemisphere), which gives the brighter-near-the-light / darker-in-corners look.
// Lumels are written as RGB16F half-floats.
struct LightmapBakeLight {
    math::Vec3 position{0, 0, 0};
    math::Vec3 color{1, 1, 1};
    f32 intensity = 1.0f;
    f32 range = 12.0f;  // metres; quadratic falloff reaches ~0 at `range`.
};

struct LightmapBakeParams {
    u32 lumels_per_axis = 12u;        // NxN lumel grid per face.
    math::Vec3 ambient{0.12f, 0.13f, 0.16f};
    std::vector<LightmapBakeLight> lights;  // empty -> a default per-room light.
};

// Bake lightmaps for the rooms-path faces of `bsp` (in place). `rooms` supplies
// the room volumes used for the occlusion/AO geometry. Returns the number of
// lit faces. A face with no resolvable lighting still gets an ambient-only
// lightmap (so the whole level is consistently lit, no full-bright patches).
u32 bake_room_lightmaps(const RoomsFile& rooms,
                        CompiledBsp& bsp,
                        const LightmapBakeParams& params = {});

// Bake the PVS from the compiled portal graph and write the engine `PBSP` v1
// blob (world::bsp::BspFormat.h) that `Bsp::load` consumes. `out_clusters` and
// `out_pvs_row_bytes` (when non-null) receive the baked PVS dimensions. W12-2:
// when `bsp.lightmaps` is non-empty (bake_room_lightmaps ran) the lightmap
// directory + packed RGB16F lumels are serialised into the new header chunks.
void write_psybsp_engine(const CompiledBsp& bsp,
                         std::vector<u8>& out,
                         u32* out_clusters = nullptr,
                         u32* out_pvs_row_bytes = nullptr);

int cli_main(int argc, char** argv);
void print_help();

}  // namespace psynder::tools::qbsp
