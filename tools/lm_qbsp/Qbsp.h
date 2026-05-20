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

struct CompiledBsp {
    std::vector<BspPlane> planes;
    std::vector<BspNode> nodes;
    std::vector<BspLeaf> leaves;
    std::vector<BspBrush> brushes;
    std::vector<u32> brush_planes;            // flattened plane indices per brush
    std::vector<BspPortal> portals;           // Wave-B
    std::vector<math::Vec3> portal_vertices;  // Wave-B (windings)
};

// Compile the worldspawn (entity 0) brushes into a leafy BSP. Other
// entities are passed through unused in Wave-A (the runtime loads them
// from the level file).
bool compile_bsp(const MapFile& map, CompiledBsp& out, std::string* err = nullptr);

void write_psybsp(const CompiledBsp& bsp, std::vector<u8>& out);
bool read_psybsp(std::span<const u8> bytes, CompiledBsp& out, std::string* err = nullptr);

int cli_main(int argc, char** argv);
void print_help();

}  // namespace psynder::tools::qbsp
