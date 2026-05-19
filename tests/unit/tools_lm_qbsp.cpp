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

// ─── Wave-B: portal generation ───────────────────────────────────────────

namespace {
// Walk every BSP node and count internal nodes that bridge at least one
// empty leaf on each side. The qbsp compiler emits exactly one portal per
// such adjacency (Wave-B scope; full BSP-style clipping lives in Wave-C).
i32 first_empty_leaf_in_subtree(const CompiledBsp& bsp, i32 child) {
    if (child < 0) {
        i32 leaf_idx = ~child;
        if (leaf_idx < 0 || static_cast<usize>(leaf_idx) >= bsp.leaves.size()) return -1;
        return (bsp.leaves[static_cast<usize>(leaf_idx)].flags & kLeafFlagEmpty) ? leaf_idx : -1;
    }
    if (static_cast<usize>(child) >= bsp.nodes.size()) return -1;
    const auto& n = bsp.nodes[static_cast<usize>(child)];
    i32 a = first_empty_leaf_in_subtree(bsp, n.front);
    if (a >= 0) return a;
    return first_empty_leaf_in_subtree(bsp, n.back);
}
u32 expected_portal_count(const CompiledBsp& bsp) {
    u32 n = 0;
    for (const auto& node : bsp.nodes) {
        i32 fl = first_empty_leaf_in_subtree(bsp, node.front);
        i32 bl = first_empty_leaf_in_subtree(bsp, node.back);
        if (fl >= 0 && bl >= 0 && fl != bl) ++n;
    }
    return n;
}
}  // anon

TEST_CASE("lm_qbsp portal output matches Wave-A leaf adjacency", "[tools][lm_qbsp][wave-b]") {
    // A two-brush .map: a small floor + a ceiling chunk. The BSP build will
    // split on each brush's first plane, yielding internal nodes that bridge
    // empty regions and so producing portals.
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        // brush 1 — small box
        "{\n"
        "( -4 -4 -4 ) ( -4 -3 -4 ) ( -4 -4 -3 ) WALL 0 0 0 1 1\n"
        "(  4 -4 -4 ) (  4 -4 -3 ) (  4 -3 -4 ) WALL 0 0 0 1 1\n"
        "( -4 -4 -4 ) ( -4 -4 -3 ) ( -3 -4 -4 ) WALL 0 0 0 1 1\n"
        "( -4  4 -4 ) ( -3  4 -4 ) ( -4  4 -3 ) WALL 0 0 0 1 1\n"
        "( -4 -4 -4 ) ( -3 -4 -4 ) ( -4 -3 -4 ) FLOOR 0 0 0 1 1\n"
        "( -4 -4  0 ) ( -4 -3  0 ) ( -3 -4  0 ) CEIL  0 0 0 1 1\n"
        "}\n"
        // brush 2 — slab above
        "{\n"
        "( -4 -4  4 ) ( -4 -3  4 ) ( -4 -4  5 ) WALL 0 0 0 1 1\n"
        "(  4 -4  4 ) (  4 -4  5 ) (  4 -3  4 ) WALL 0 0 0 1 1\n"
        "( -4 -4  4 ) ( -4 -4  5 ) ( -3 -4  4 ) WALL 0 0 0 1 1\n"
        "( -4  4  4 ) ( -3  4  4 ) ( -4  4  5 ) WALL 0 0 0 1 1\n"
        "( -4 -4  4 ) ( -3 -4  4 ) ( -4 -3  4 ) FLOOR 0 0 0 1 1\n"
        "( -4 -4  5 ) ( -4 -3  5 ) ( -3 -4  5 ) CEIL  0 0 0 1 1\n"
        "}\n"
        "}\n";
    MapFile map;
    std::string err;
    REQUIRE(parse_map(kMap, map, &err));
    CompiledBsp bsp;
    REQUIRE(compile_bsp(map, bsp, &err));
    REQUIRE_FALSE(bsp.nodes.empty());

    // Each emitted portal must point at two distinct empty leaves, carry a
    // plane that matches one of the BSP planes, and have a 4-vertex winding
    // (the Wave-B portal winding is a square on the splitter plane).
    u32 want = expected_portal_count(bsp);
    REQUIRE(bsp.portals.size() == want);
    for (const auto& p : bsp.portals) {
        REQUIRE(p.front_leaf != p.back_leaf);
        REQUIRE(p.vertex_count == 4);
        REQUIRE(static_cast<usize>(p.first_vertex) + p.vertex_count <= bsp.portal_vertices.size());
        bool plane_found = false;
        for (const auto& pl : bsp.planes) {
            if (std::fabs(pl.normal.x - p.plane_normal.x) < 1e-6f &&
                std::fabs(pl.normal.y - p.plane_normal.y) < 1e-6f &&
                std::fabs(pl.normal.z - p.plane_normal.z) < 1e-6f &&
                std::fabs(pl.d - p.plane_d) < 1e-4f) {
                plane_found = true;
                break;
            }
        }
        REQUIRE(plane_found);
    }
}

TEST_CASE("lm_qbsp .psybsp v2 round-trips portals", "[tools][lm_qbsp][wave-b]") {
    // Same map as above so we exercise non-empty portal output through the
    // write_psybsp / read_psybsp serialiser.
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "{\n"
        "( -4 -4 -4 ) ( -4 -3 -4 ) ( -4 -4 -3 ) WALL 0 0 0 1 1\n"
        "(  4 -4 -4 ) (  4 -4 -3 ) (  4 -3 -4 ) WALL 0 0 0 1 1\n"
        "( -4 -4 -4 ) ( -4 -4 -3 ) ( -3 -4 -4 ) WALL 0 0 0 1 1\n"
        "( -4  4 -4 ) ( -3  4 -4 ) ( -4  4 -3 ) WALL 0 0 0 1 1\n"
        "( -4 -4 -4 ) ( -3 -4 -4 ) ( -4 -3 -4 ) FLOOR 0 0 0 1 1\n"
        "( -4 -4  0 ) ( -4 -3  0 ) ( -3 -4  0 ) CEIL  0 0 0 1 1\n"
        "}\n"
        "{\n"
        "( -4 -4  4 ) ( -4 -3  4 ) ( -4 -4  5 ) WALL 0 0 0 1 1\n"
        "(  4 -4  4 ) (  4 -4  5 ) (  4 -3  4 ) WALL 0 0 0 1 1\n"
        "( -4 -4  4 ) ( -4 -4  5 ) ( -3 -4  4 ) WALL 0 0 0 1 1\n"
        "( -4  4  4 ) ( -3  4  4 ) ( -4  4  5 ) WALL 0 0 0 1 1\n"
        "( -4 -4  4 ) ( -3 -4  4 ) ( -4 -3  4 ) FLOOR 0 0 0 1 1\n"
        "( -4 -4  5 ) ( -4 -3  5 ) ( -3 -4  5 ) CEIL  0 0 0 1 1\n"
        "}\n"
        "}\n";
    MapFile map;
    std::string err;
    REQUIRE(parse_map(kMap, map, &err));
    CompiledBsp bsp;
    REQUIRE(compile_bsp(map, bsp, &err));
    std::vector<u8> bytes;
    write_psybsp(bsp, bytes);

    CompiledBsp back;
    REQUIRE(read_psybsp(bytes, back, &err));
    REQUIRE(back.portals.size() == bsp.portals.size());
    REQUIRE(back.portal_vertices.size() == bsp.portal_vertices.size());
    for (usize i = 0; i < bsp.portals.size(); ++i) {
        REQUIRE(back.portals[i].front_leaf   == bsp.portals[i].front_leaf);
        REQUIRE(back.portals[i].back_leaf    == bsp.portals[i].back_leaf);
        REQUIRE(back.portals[i].vertex_count == bsp.portals[i].vertex_count);
        REQUIRE(back.portals[i].first_vertex == bsp.portals[i].first_vertex);
    }
}
