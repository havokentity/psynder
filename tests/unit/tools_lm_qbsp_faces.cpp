// SPDX-License-Identifier: MIT
// Lane W10-2 - lm_qbsp rooms-path REAL face geometry emission + round-trip.
//
// Wave 9 made the rooms compiler emit a loader-consumable engine PBSP v1 blob
// but with EMPTY faces/vertices/indices (the runtime rendered its own scene-mesh
// boxes; the BSP was used only for leaf/cluster/PVS culling). W10-2 makes
// compile_rooms tessellate each room box into 6 inward-facing PBSP v1 faces
// (4 walls + floor + ceiling) so the level can be rendered FROM the loaded BSP
// via world::bsp::BspDraw. This suite asserts:
//   * compile_rooms emits non-zero, correctly-shaped face/vertex/index data;
//   * faces are grouped per leaf (the owning room's leaf carries the range), so
//     the right cluster owns the right faces and PVS culling skips them wholesale;
//   * the winding is INWARD (each face's CCW front normal points into the room);
//   * a round-trip through write_psybsp_engine + the runtime loader exposes the
//     faces (Bsp::load) AND the vertex/index geometry (load_geometry), and the
//     BspDraw converter turns a visible leaf's faces into one DrawItem per face.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_qbsp/Qbsp.h"

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspDraw.h"
#include "world/bsp/BspFormat.h"
#include "render/raster/Raster.h"

#include "asset/Vfs.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace psynder;
using namespace psynder::tools::qbsp;

namespace {

// Three rooms in a row: 0 <-> 1 <-> 2 linked; that is enough to exercise the
// per-leaf face ranges and a non-trivial kd-tree. Each room is an axis-aligned
// box, so each emits exactly 6 faces / 24 real vertices / 36 fan indices.
constexpr const char* kRoomsSrc =
    "rooms 3\n"
    "room 0  -10 0 -2   -6 3 2   START\n"
    "room 1   -6 0 -1   -2 3 1   CORRIDOR\n"
    "room 2   -2 0 -2    2 3 2   HUB\n"
    "portals 2\n"
    "portal 0 1\n"
    "portal 1 2\n";

constexpr u32 kFacesPerRoom = 6u;
constexpr u32 kVertsPerFaceStride = 6u;  // 4 real corners + 2 padding (BspDraw addressing)

bool write_blob(const std::filesystem::path& p, const std::vector<u8>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace

TEST_CASE("lm_qbsp rooms path emits real face geometry", "[tools][lm_qbsp][faces]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));

    // 3 rooms x 6 faces = 18 faces; each face has 4 real vertices but the slab
    // advances by the stride (6) so the parallel index blocks never overlap.
    REQUIRE(bsp.faces.size() == 3u * kFacesPerRoom);
    REQUIRE(bsp.vertices.size() == bsp.faces.size() * kVertsPerFaceStride);
    REQUIRE(bsp.indices.size() == bsp.faces.size() * kVertsPerFaceStride);
    REQUIRE_FALSE(bsp.faces.empty());

    // Every face is a quad with the unlit lightmap sentinel and a material id
    // equal to one of the room clusters (0,1,2).
    for (const QbFace& f : bsp.faces) {
        REQUIRE(f.vertex_count == 4u);
        REQUIRE(f.lightmap == kBspNoLightmap);
        REQUIRE(f.material <= 2u);
        REQUIRE(f.first_vertex + 4u <= bsp.vertices.size());
    }
}

TEST_CASE("lm_qbsp faces are grouped per leaf / cluster", "[tools][lm_qbsp][faces]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));

    REQUIRE(bsp.leaf_first_face.size() == bsp.leaves.size());
    REQUIRE(bsp.leaf_face_count.size() == bsp.leaves.size());

    // Leaf i (== room i) owns a contiguous run of 6 faces, and every face in
    // that run carries that leaf's cluster as its material id.
    for (usize li = 0; li < bsp.leaves.size(); ++li) {
        const u32 first = bsp.leaf_first_face[li];
        const u32 count = bsp.leaf_face_count[li];
        REQUIRE(count == kFacesPerRoom);
        REQUIRE(first == static_cast<u32>(li) * kFacesPerRoom);
        const u32 cluster = static_cast<u32>(bsp.leaves[li].cluster);
        for (u32 fi = first; fi < first + count; ++fi) {
            REQUIRE(bsp.faces[fi].material == cluster);
        }
    }
}

TEST_CASE("lm_qbsp face winding is inward-facing", "[tools][lm_qbsp][faces]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));

    // For each face the CCW front normal (computed from the first triangle of
    // the {0,1,2,...} fan) must point in the SAME hemisphere as the stored
    // per-vertex normal (which is the room's INWARD normal). dot > 0 proves the
    // interior side is the front face.
    for (const QbFace& f : bsp.faces) {
        const QbVertex& v0 = bsp.vertices[f.first_vertex + 0u];
        const QbVertex& v1 = bsp.vertices[f.first_vertex + 1u];
        const QbVertex& v2 = bsp.vertices[f.first_vertex + 2u];
        const math::Vec3 e1 = math::sub(v1.position, v0.position);
        const math::Vec3 e2 = math::sub(v2.position, v0.position);
        const math::Vec3 gn = math::cross(e1, e2);
        const f32 d = math::dot(gn, v0.normal);
        REQUIRE(d > 0.0f);
        // The stored normal is one of the 6 axis directions (unit length).
        const f32 nlen2 = math::dot(v0.normal, v0.normal);
        REQUIRE(std::fabs(nlen2 - 1.0f) < 1e-4f);
    }
}

TEST_CASE("lm_qbsp faces round-trip through the runtime loader + BspDraw",
          "[tools][lm_qbsp][faces]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));

    std::vector<u8> bytes;
    write_psybsp_engine(bsp, bytes, nullptr, nullptr);

    // The PBSP v1 header must now advertise the geometry chunks.
    world::bsp::BspFileHeader header{};
    REQUIRE(bytes.size() >= sizeof(header));
    std::memcpy(&header, bytes.data(), sizeof(header));
    REQUIRE(header.faces.count == bsp.faces.size());
    REQUIRE(header.vertices.count == bsp.vertices.size());
    REQUIRE(header.indices.count == bsp.indices.size());
    REQUIRE(header.faces.count > 0u);

    // Write to a temp dir + mount it so the real loader reads it via the Vfs.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "psynder_lm_qbsp_faces_test";
    fs::create_directories(dir / "maps");
    const fs::path blob = dir / "maps" / "faces_pipeline.psybsp";
    REQUIRE(write_blob(blob, bytes));
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    // Loader: faces land in the BspMap; leaves carry the per-leaf face range.
    world::bsp::BspMap map;
    REQUIRE(world::bsp::load("maps/faces_pipeline.psybsp", map));
    REQUIRE(map.faces.size() == bsp.faces.size());
    REQUIRE(map.leaves.size() == 3u);
    for (usize li = 0; li < map.leaves.size(); ++li) {
        REQUIRE(map.leaves[li].face_count == kFacesPerRoom);
        REQUIRE(map.leaves[li].first_face == static_cast<u32>(li) * kFacesPerRoom);
    }

    // Geometry loader: vertices + indices come back from the same blob.
    world::bsp::BspGeometry geom;
    REQUIRE(world::bsp::load_geometry("maps/faces_pipeline.psybsp", geom));
    REQUIRE(geom.vertices.size() == bsp.vertices.size());
    REQUIRE(geom.indices.size() == bsp.indices.size());
    // A loaded vertex matches the compiled one bit-for-bit (packed-layout copy).
    REQUIRE(geom.vertices[0].position.x == bsp.vertices[0].position.x);
    REQUIRE(geom.vertices[0].position.y == bsp.vertices[0].position.y);
    REQUIRE(geom.vertices[0].position.z == bsp.vertices[0].position.z);
    REQUIRE(geom.vertices[0].color == bsp.vertices[0].color);

    // BspDraw: build_leaf_draws turns one leaf's faces into one DrawItem each,
    // with face-local index ranges and the material id passed through.
    world::bsp::BspMaterialResolve resolve{};
    std::vector<render::raster::DrawItem> draws;
    world::bsp::build_leaf_draws(map, geom, map.leaves[2], resolve, draws);
    REQUIRE(draws.size() == kFacesPerRoom);
    for (const render::raster::DrawItem& di : draws) {
        REQUIRE(di.vertices != nullptr);
        REQUIRE(di.indices != nullptr);
        REQUIRE(di.vertex_count == 4u);
        REQUIRE(di.index_count == 6u);  // (4-2)*3 fan triangles
        REQUIRE(di.material.raw == 2u);  // leaf 2 -> cluster 2
    }
    // The DrawItem index range stays in [0, vertex_count) (face-local indices).
    for (u32 k = 0; k < draws[0].index_count; ++k)
        REQUIRE(draws[0].indices[k] < draws[0].vertex_count);

    fs::remove_all(dir);
}
