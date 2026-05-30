// SPDX-License-Identifier: MIT
// Lane W12-2 - lm_qbsp rooms-path baked LIGHTMAPS + runtime lit sampling.
//
// Wave 10 made the rooms compiler emit real BSP faces but FULL-BRIGHT
// (kBspNoLightmap). W12-2 BAKES a per-face lightmap (ambient + per-room point
// lights with coarse occlusion + edge AO), packs it into the engine PBSP v1
// blob (the new lightmap directory + RGB16F lumel chunks in BspFormat.h), and
// has BspDraw modulate each lit face by the atlas lumel. This suite asserts:
//   * bake_room_lightmaps lights every face and repoints QbFace::lightmap;
//   * the compiled .psybsp advertises a non-empty lightmap chunk with the
//     expected per-face directory rows (face id + NxN dims);
//   * the bake is DETERMINISTIC: same input -> same bytes across runs;
//   * a lit face samples a NON-UNIFORM lightmap (shade varies lumel-to-lumel);
//   * a NO-lightmap face (un-baked blob) is unaffected: BspDraw leaves its
//     DrawItem full-bright (lightmap_texels == nullptr), nothing regresses.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_qbsp/Qbsp.h"

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspDraw.h"
#include "world/bsp/BspFormat.h"
#include "render/raster/Raster.h"

#include "asset/Vfs.h"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace psynder;
using namespace psynder::tools::qbsp;

namespace {

// Two rooms in a row, linked: enough to exercise the per-face bake, the
// occlusion test between neighbouring boxes, and the round-trip.
constexpr const char* kRoomsSrc =
    "rooms 2\n"
    "room 0  -10 0 -2   -6 3 2   START\n"
    "room 1   -6 0 -2   -2 3 2   HUB\n"
    "portals 1\n"
    "portal 0 1\n";

constexpr u32 kFacesPerRoom = 6u;

bool write_blob(const std::filesystem::path& p, const std::vector<u8>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

CompiledBsp compile_and_bake() {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));
    const u32 lit = bake_room_lightmaps(rooms, bsp);
    REQUIRE(lit == bsp.faces.size());
    return bsp;
}

}  // namespace

TEST_CASE("lm_qbsp bake lights every room face", "[tools][lm_qbsp][lightmap]") {
    CompiledBsp bsp = compile_and_bake();

    REQUIRE(bsp.faces.size() == 2u * kFacesPerRoom);
    REQUIRE(bsp.lightmaps.size() == bsp.faces.size());
    REQUIRE_FALSE(bsp.lightmap_pixels.empty());

    // Every face now points at its own directory row (no longer the unlit
    // sentinel), and each row is an NxN RGB16F block addressed by face id.
    const u32 N = LightmapBakeParams{}.lumels_per_axis;
    for (usize fi = 0; fi < bsp.faces.size(); ++fi) {
        REQUIRE(bsp.faces[fi].lightmap != kBspNoLightmap);
        const QbLightmap& lm = bsp.lightmaps[bsp.faces[fi].lightmap];
        REQUIRE(lm.face == static_cast<u32>(fi));
        REQUIRE(lm.width == N);
        REQUIRE(lm.height == N);
        // The lumel block fits inside the packed pixel blob.
        const u64 block = static_cast<u64>(N) * N * kQbLightmapTexelBytes;
        REQUIRE(lm.pixel_offset + block <= bsp.lightmap_pixels.size());
    }
}

TEST_CASE("lm_qbsp bake is deterministic (same input -> same bytes)",
          "[tools][lm_qbsp][lightmap]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));

    CompiledBsp a;
    REQUIRE(compile_rooms(rooms, a, &err));
    bake_room_lightmaps(rooms, a);
    std::vector<u8> bytes_a;
    write_psybsp_engine(a, bytes_a, nullptr, nullptr);

    CompiledBsp b;
    REQUIRE(compile_rooms(rooms, b, &err));
    bake_room_lightmaps(rooms, b);
    std::vector<u8> bytes_b;
    write_psybsp_engine(b, bytes_b, nullptr, nullptr);

    REQUIRE(bytes_a.size() == bytes_b.size());
    REQUIRE(bytes_a == bytes_b);
    // The lumel pixels themselves are identical too (no RNG / time creeping in).
    REQUIRE(a.lightmap_pixels == b.lightmap_pixels);
}

TEST_CASE("compiled .psybsp advertises a non-empty lightmap chunk",
          "[tools][lm_qbsp][lightmap]") {
    CompiledBsp bsp = compile_and_bake();
    std::vector<u8> bytes;
    write_psybsp_engine(bsp, bytes, nullptr, nullptr);

    world::bsp::BspFileHeader header{};
    REQUIRE(bytes.size() >= sizeof(header));
    std::memcpy(&header, bytes.data(), sizeof(header));

    // The directory has one row per lit face; the pixel chunk is the packed
    // RGB16F lumels those rows index into. Both must be present (non-empty).
    REQUIRE(header.lightmaps.count == bsp.faces.size());
    REQUIRE(header.lightmaps.count > 0u);
    REQUIRE(header.lightmap_pixels.count > 0u);
    REQUIRE(header.lightmap_pixels.count ==
            bsp.faces.size() * LightmapBakeParams{}.lumels_per_axis *
                LightmapBakeParams{}.lumels_per_axis * world::bsp::kBspLightmapTexelBytes);
    // The whole blob (header + chunks) is internally consistent.
    REQUIRE(header.total_bytes == bytes.size());
    // The lightmap chunks live AFTER the pvs chunk (additive layout).
    REQUIRE(header.lightmaps.offset >= header.pvs.offset + header.pvs.count);
}

TEST_CASE("a baked face round-trips and samples a NON-UNIFORM lightmap",
          "[tools][lm_qbsp][lightmap]") {
    CompiledBsp bsp = compile_and_bake();
    std::vector<u8> bytes;
    write_psybsp_engine(bsp, bytes, nullptr, nullptr);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "psynder_bsp_lightmap_test";
    fs::create_directories(dir / "maps");
    const fs::path blob = dir / "maps" / "lit.psybsp";
    REQUIRE(write_blob(blob, bytes));
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    world::bsp::BspMap map;
    REQUIRE(world::bsp::load("maps/lit.psybsp", map));
    world::bsp::BspGeometry geom;
    REQUIRE(world::bsp::load_geometry("maps/lit.psybsp", geom));

    // The lightmap loader decodes the RGB16F lumels into the RGBA8 pool and maps
    // every face to its decoded chunk.
    REQUIRE(world::bsp::load_lightmaps("maps/lit.psybsp",
                                       static_cast<u32>(map.faces.size()), geom));
    REQUIRE(geom.face_lightmap.size() == map.faces.size());
    REQUIRE(geom.lightmaps.size() == map.faces.size());
    REQUIRE_FALSE(geom.lightmap_texels.empty());

    // Every face resolves to a decoded chunk, and at least one face's lumels are
    // NON-UNIFORM (the bake genuinely shades: brighter near the light, darker in
    // the corners, so not every lumel is the same value).
    bool any_nonuniform = false;
    for (u32 fi = 0; fi < map.faces.size(); ++fi) {
        const u32 lm_idx = geom.face_lightmap[fi];
        REQUIRE(lm_idx != world::bsp::BspGeometry::kNoLightmap);
        const world::bsp::BspFaceLightmap& fl = geom.lightmaps[lm_idx];
        const usize count = static_cast<usize>(fl.width) * fl.height;
        REQUIRE(count > 0u);
        REQUIRE(static_cast<usize>(fl.first_texel) + count <= geom.lightmap_texels.size());
        const u32 first = geom.lightmap_texels[fl.first_texel];
        for (usize t = 1; t < count; ++t) {
            if (geom.lightmap_texels[fl.first_texel + t] != first) {
                any_nonuniform = true;
                break;
            }
        }
        if (any_nonuniform)
            break;
    }
    REQUIRE(any_nonuniform);

    // BspDraw wires the decoded chunk into the DrawItem so the rasterizer's
    // per-draw lightmap (SurfaceCached) path modulates the face by the lumels.
    world::bsp::BspMaterialResolve resolve{};
    std::vector<render::raster::DrawItem> draws;
    world::bsp::build_leaf_draws(map, geom, map.leaves[0], resolve, draws);
    REQUIRE(draws.size() == kFacesPerRoom);
    const u32 N = LightmapBakeParams{}.lumels_per_axis;
    for (const render::raster::DrawItem& di : draws) {
        REQUIRE(di.lightmap_texels != nullptr);
        REQUIRE(di.lightmap_w == N);
        REQUIRE(di.lightmap_h == N);
    }

    fs::remove_all(dir);
}

TEST_CASE("a NO-lightmap (un-baked) face stays full-bright in BspDraw",
          "[tools][lm_qbsp][lightmap]") {
    // Compile WITHOUT baking -> faces keep kBspNoLightmap, the blob carries no
    // lightmap chunk, and BspDraw must leave the DrawItem full-bright.
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));
    REQUIRE(bsp.lightmaps.empty());
    for (const QbFace& f : bsp.faces)
        REQUIRE(f.lightmap == kBspNoLightmap);

    std::vector<u8> bytes;
    write_psybsp_engine(bsp, bytes, nullptr, nullptr);

    // Header must report an EMPTY lightmap chunk for an un-baked blob.
    world::bsp::BspFileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    REQUIRE(header.lightmaps.count == 0u);
    REQUIRE(header.lightmap_pixels.count == 0u);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "psynder_bsp_unlit_test";
    fs::create_directories(dir / "maps");
    const fs::path blob = dir / "maps" / "unlit.psybsp";
    REQUIRE(write_blob(blob, bytes));
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    world::bsp::BspMap map;
    REQUIRE(world::bsp::load("maps/unlit.psybsp", map));
    world::bsp::BspGeometry geom;
    REQUIRE(world::bsp::load_geometry("maps/unlit.psybsp", geom));
    // load_lightmaps on an unlit blob succeeds with everything kNoLightmap.
    REQUIRE(world::bsp::load_lightmaps("maps/unlit.psybsp",
                                       static_cast<u32>(map.faces.size()), geom));
    REQUIRE(geom.lightmaps.empty());
    REQUIRE(geom.lightmap_texels.empty());
    for (u32 v : geom.face_lightmap)
        REQUIRE(v == world::bsp::BspGeometry::kNoLightmap);

    world::bsp::BspMaterialResolve resolve{};
    std::vector<render::raster::DrawItem> draws;
    world::bsp::build_leaf_draws(map, geom, map.leaves[0], resolve, draws);
    REQUIRE(draws.size() == kFacesPerRoom);
    for (const render::raster::DrawItem& di : draws) {
        REQUIRE(di.lightmap_texels == nullptr);  // full-bright: no modulation.
        REQUIRE(di.lightmap_w == 0u);
        REQUIRE(di.lightmap_h == 0u);
    }

    fs::remove_all(dir);
}
