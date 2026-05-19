// SPDX-License-Identifier: MIT
// Lane 24 tests — lm_mapimport .map → .psylevel round-trip.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_mapimport/MapImport.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::tools::mapimport;

namespace {
constexpr const char* kSampleMap =
    "{\n"
    "\"classname\" \"worldspawn\"\n"
    "\"wad\" \"textures/base.wad\"\n"
    "{\n"
    "( -8 -8 -8 ) ( -8 -7 -8 ) ( -8 -8 -7 ) WALL 0 0 0 1 1\n"
    "(  8 -8 -8 ) (  8 -8 -7 ) (  8 -7 -8 ) WALL 0 0 0 1 1\n"
    "( -8 -8 -8 ) ( -8 -8 -7 ) ( -7 -8 -8 ) WALL 0 0 0 1 1\n"
    "( -8  8 -8 ) ( -7  8 -8 ) ( -8  8 -7 ) WALL 0 0 0 1 1\n"
    "( -8 -8 -8 ) ( -7 -8 -8 ) ( -8 -7 -8 ) FLOOR 0 0 0 1 1\n"
    "( -8 -8  8 ) ( -8 -7  8 ) ( -7 -8  8 ) CEIL  0 0 0 1 1\n"
    "}\n"
    "}\n"
    "{\n"
    "\"classname\" \"info_player_start\"\n"
    "\"origin\" \"0 0 16\"\n"
    "}\n";
}  // anon

TEST_CASE("lm_mapimport imports a brush + player start", "[tools][lm_mapimport]") {
    LevelFile level;
    auto r = import_map(kSampleMap, level);
    REQUIRE(r.ok);
    REQUIRE(r.entity_count == 2);
    REQUIRE(r.brush_count == 1);
    REQUIRE(level.chunks.size() == 2);     // entities + brushes (no BSP yet)
}

TEST_CASE("lm_mapimport optionally embeds the compiled BSP", "[tools][lm_mapimport]") {
    LevelFile level;
    ImportOptions opt;
    opt.compile_bsp = true;
    auto r = import_map(kSampleMap, level, opt);
    REQUIRE(r.ok);
    bool has_bsp = std::any_of(level.chunks.begin(), level.chunks.end(),
        [](const LevelChunk& c) { return c.kind == ChunkKind::kPsyBsp; });
    REQUIRE(has_bsp);
}

TEST_CASE("lm_mapimport .psylevel round-trips byte-for-byte", "[tools][lm_mapimport]") {
    LevelFile level;
    ImportOptions opt;
    opt.compile_bsp = true;
    REQUIRE(import_map(kSampleMap, level, opt).ok);

    std::vector<u8> bytes;
    write_level(level, bytes);
    REQUIRE_FALSE(bytes.empty());

    LevelFile back;
    std::string err;
    REQUIRE(read_level(bytes, back, &err));
    REQUIRE(back.chunks.size() == level.chunks.size());
    for (usize i = 0; i < level.chunks.size(); ++i) {
        REQUIRE(back.chunks[i].kind == level.chunks[i].kind);
        REQUIRE(back.chunks[i].bytes == level.chunks[i].bytes);
    }
}

TEST_CASE("lm_mapimport CLI --help returns 0", "[tools][lm_mapimport]") {
    const char* argv[] = {"lm_mapimport", "--help"};
    int rc = cli_main(2, const_cast<char**>(argv));
    REQUIRE(rc == 0);
}
