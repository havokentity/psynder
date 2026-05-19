// SPDX-License-Identifier: MIT
// Lane 24 tests — lm_qbsp .map parser + BSP compiler round-trip.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_qbsp/Qbsp.h"

#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::tools::qbsp;

TEST_CASE("lm_qbsp parses a Quake-style worldspawn brush", "[tools][lm_qbsp]") {
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "{\n"
        "( -16 -16 -16 ) ( -16 -15 -16 ) ( -16 -16 -15 ) WALL 0 0 0 1 1\n"
        "(  16 -16 -16 ) (  16 -16 -15 ) (  16 -15 -16 ) WALL 0 0 0 1 1\n"
        "( -16 -16 -16 ) ( -16 -16 -15 ) ( -15 -16 -16 ) WALL 0 0 0 1 1\n"
        "( -16  16 -16 ) ( -15  16 -16 ) ( -16  16 -15 ) WALL 0 0 0 1 1\n"
        "( -16 -16 -16 ) ( -15 -16 -16 ) ( -16 -15 -16 ) FLOOR 0 0 0 1 1\n"
        "( -16 -16  16 ) ( -16 -15  16 ) ( -15 -16  16 ) CEIL 0 0 0 1 1\n"
        "}\n"
        "}\n";

    MapFile map;
    std::string err;
    REQUIRE(parse_map(kMap, map, &err));
    REQUIRE(map.entities.size() == 1);
    REQUIRE(map.entities[0].kv.size() == 1);
    REQUIRE(map.entities[0].kv[0].first == "classname");
    REQUIRE(map.entities[0].kv[0].second == "worldspawn");
    REQUIRE(map.entities[0].brushes.size() == 1);
    REQUIRE(map.entities[0].brushes[0].planes.size() == 6);
}

TEST_CASE("lm_qbsp parses multiple entities", "[tools][lm_qbsp]") {
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "}\n"
        "{\n"
        "\"classname\" \"info_player_start\"\n"
        "\"origin\" \"0 0 32\"\n"
        "}\n";
    MapFile map;
    std::string err;
    REQUIRE(parse_map(kMap, map, &err));
    REQUIRE(map.entities.size() == 2);
    REQUIRE(map.entities[1].kv.size() == 2);
    REQUIRE(map.entities[1].kv[1].first == "origin");
}

TEST_CASE("lm_qbsp compiles and round-trips a .psybsp blob", "[tools][lm_qbsp]") {
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "{\n"
        "( -4 -4 -4 ) ( -4 -3 -4 ) ( -4 -4 -3 ) STONE 0 0 0 1 1\n"
        "(  4 -4 -4 ) (  4 -4 -3 ) (  4 -3 -4 ) STONE 0 0 0 1 1\n"
        "( -4 -4 -4 ) ( -4 -4 -3 ) ( -3 -4 -4 ) STONE 0 0 0 1 1\n"
        "( -4  4 -4 ) ( -3  4 -4 ) ( -4  4 -3 ) STONE 0 0 0 1 1\n"
        "( -4 -4 -4 ) ( -3 -4 -4 ) ( -4 -3 -4 ) FLOOR 0 0 0 1 1\n"
        "( -4 -4  4 ) ( -4 -3  4 ) ( -3 -4  4 ) CEIL  0 0 0 1 1\n"
        "}\n"
        "}\n";
    MapFile map;
    std::string err;
    REQUIRE(parse_map(kMap, map, &err));
    CompiledBsp bsp;
    REQUIRE(compile_bsp(map, bsp, &err));
    REQUIRE_FALSE(bsp.planes.empty());
    REQUIRE_FALSE(bsp.nodes.empty());
    REQUIRE_FALSE(bsp.leaves.empty());
    REQUIRE(bsp.brushes.size() == 1);
    REQUIRE(bsp.brush_planes.size() == 6);

    std::vector<u8> bytes;
    write_psybsp(bsp, bytes);
    REQUIRE(bytes.size() >= 32);
    // Magic check.
    u32 magic = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
                (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
    REQUIRE(magic == kPsyBspMagic);

    CompiledBsp back;
    REQUIRE(read_psybsp(bytes, back, &err));
    REQUIRE(back.planes.size() == bsp.planes.size());
    REQUIRE(back.nodes.size() == bsp.nodes.size());
    REQUIRE(back.leaves.size() == bsp.leaves.size());
    REQUIRE(back.brushes.size() == bsp.brushes.size());
    REQUIRE(back.brush_planes == bsp.brush_planes);
}

TEST_CASE("lm_qbsp CLI cli_main --help returns 0", "[tools][lm_qbsp]") {
    const char* argv[] = {"lm_qbsp", "--help"};
    int rc = cli_main(2, const_cast<char**>(argv));
    REQUIRE(rc == 0);
}
