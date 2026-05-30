// SPDX-License-Identifier: MIT
// Lane W9-1 — lm_qbsp `.rooms` source -> engine PBSP v1 + baked PVS pipeline.
//
// The brush `.map` path (tools_lm_qbsp.cpp) emits the tool's own PSBP v2 blob,
// which the runtime loader does NOT read. This suite covers the additive
// `--rooms` path that authors a multi-room indoor level and emits the engine
// PBSP v1 format (world::bsp::BspFormat.h) WITH a baked leaf-portal-flood PVS,
// then loads it back through the real runtime loader `world::bsp::load` and
// asserts the PVS culls the sealed rooms.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_qbsp/Qbsp.h"

#include <vector>  // Bsp.h uses std::vector without including <vector>.
#include "world/bsp/Bsp.h"
#include "world/bsp/BspFormat.h"

#include "asset/Vfs.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace psynder;
using namespace psynder::tools::qbsp;

namespace {

// A compact two-spine level: rooms 0->1->2 are linked; room 3 is a sealed
// island (no portal). PVS must therefore cull cluster 3 from clusters 0/1/2.
constexpr const char* kRoomsSrc =
    "rooms 4\n"
    "room 0  -10 0 -2   -6 3 2   START\n"
    "room 1   -6 0 -1   -2 3 1   CORRIDOR\n"
    "room 2   -2 0 -2    2 3 2   HUB\n"
    "room 3   -2 0  6    2 3 10  SEALED\n"
    "portals 2\n"
    "portal 0 1\n"
    "portal 1 2\n";

bool write_blob(const std::filesystem::path& p, const std::vector<u8>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

struct LeafCollector {
    std::set<i32> clusters;
};
void collect(const world::bsp::BspLeaf& leaf, void* user) {
    static_cast<LeafCollector*>(user)->clusters.insert(leaf.cluster);
}

}  // namespace

TEST_CASE("lm_qbsp parses a .rooms source", "[tools][lm_qbsp][rooms]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    REQUIRE(rooms.rooms.size() == 4);
    REQUIRE(rooms.rooms[0].cluster == 0);
    REQUIRE(rooms.rooms[0].name == "START");
    REQUIRE(rooms.rooms[3].name == "SEALED");
    REQUIRE(rooms.portals.size() == 2);
    REQUIRE(rooms.portals[0].cluster_a == 0);
    REQUIRE(rooms.portals[1].cluster_b == 2);
}

TEST_CASE("lm_qbsp rejects duplicate room clusters", "[tools][lm_qbsp][rooms]") {
    constexpr const char* kDup =
        "rooms 2\n"
        "room 0 0 0 0 1 1 1\n"
        "room 0 2 0 0 3 1 1\n";
    RoomsFile rooms;
    std::string err;
    REQUIRE_FALSE(parse_rooms(kDup, rooms, &err));
}

TEST_CASE("lm_qbsp compiles rooms into a leafy BSP", "[tools][lm_qbsp][rooms]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));

    REQUIRE(bsp.leaves.size() == 4);
    REQUIRE_FALSE(bsp.nodes.empty());
    REQUIRE(bsp.portals.size() == 2);
    // One leaf per room, in room order, carrying the room bounds + cluster.
    REQUIRE(bsp.leaves[0].cluster == 0);
    REQUIRE(bsp.leaves[3].cluster == 3);
    REQUIRE(bsp.leaves[0].bounds.min.x == -10.0f);
    // Every node child stays in range (front/back point at a node or ~leaf).
    const i32 node_count = static_cast<i32>(bsp.nodes.size());
    const i32 leaf_count = static_cast<i32>(bsp.leaves.size());
    for (const BspNode& n : bsp.nodes) {
        for (i32 child : {n.front, n.back}) {
            if (child < 0)
                REQUIRE((~child) < leaf_count);
            else
                REQUIRE(child < node_count);
        }
    }
}

TEST_CASE("lm_qbsp emits an engine PBSP v1 blob with a baked PVS", "[tools][lm_qbsp][rooms]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));

    std::vector<u8> bytes;
    u32 clusters = 0u, row_bytes = 0u;
    write_psybsp_engine(bsp, bytes, &clusters, &row_bytes);

    REQUIRE(bytes.size() >= sizeof(world::bsp::BspFileHeader));
    world::bsp::BspFileHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    REQUIRE(header.magic == world::bsp::kBspFileMagic);
    REQUIRE(header.version == world::bsp::kBspFileVersion);
    REQUIRE(header.total_bytes == bytes.size());
    REQUIRE(header.cluster_count == 4u);
    REQUIRE(clusters == 4u);
    REQUIRE(row_bytes == 1u);  // ceil(4/8) == 1
    REQUIRE(header.leaves.count == 4u);
    REQUIRE(header.faces.count == 0u);  // runtime renders its own meshes
    REQUIRE(header.pvs.count == clusters * row_bytes);
}

TEST_CASE("lm_qbsp rooms pipeline loads through the runtime and culls sealed rooms",
          "[tools][lm_qbsp][rooms]") {
    RoomsFile rooms;
    std::string err;
    REQUIRE(parse_rooms(kRoomsSrc, rooms, &err));
    CompiledBsp bsp;
    REQUIRE(compile_rooms(rooms, bsp, &err));
    std::vector<u8> bytes;
    write_psybsp_engine(bsp, bytes, nullptr, nullptr);

    // Write to a temp dir and mount it so the real loader reads it via the Vfs.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "psynder_lm_qbsp_rooms_test";
    fs::create_directories(dir / "maps");
    const fs::path blob = dir / "maps" / "rooms_pipeline.psybsp";
    REQUIRE(write_blob(blob, bytes));
    REQUIRE(asset::Vfs::Get().mount_directory(dir.string()));

    world::bsp::BspMap map;
    REQUIRE(world::bsp::load("maps/rooms_pipeline.psybsp", map));
    REQUIRE(map.leaves.size() == 4);
    REQUIRE_FALSE(map.pvs.empty());

    // locate() must resolve a point inside HUB (cluster 2) to a leaf with that
    // cluster (the kd-tree descent works on the compiled nodes).
    const world::bsp::BspLeaf hub = world::bsp::locate(map, math::Vec3{0.0f, 1.5f, 0.0f});
    REQUIRE(hub.cluster == 2);

    // From START (cluster 0): PVS sees {0,1,2} via the portal chain but NOT the
    // sealed cluster 3. walk_visible_leaves consumes the baked PVS row.
    LeafCollector vis;
    world::bsp::walk_visible_leaves(map, math::Vec3{-8.0f, 1.5f, 0.0f}, &collect, &vis);
    REQUIRE(vis.clusters.count(0) == 1);
    REQUIRE(vis.clusters.count(1) == 1);
    REQUIRE(vis.clusters.count(2) == 1);
    REQUIRE(vis.clusters.count(3) == 0);          // sealed island -> culled
    REQUIRE(vis.clusters.size() < map.leaves.size());  // genuine PVS cull

    fs::remove_all(dir);
}
