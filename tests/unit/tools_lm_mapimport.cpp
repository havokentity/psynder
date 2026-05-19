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

// ─── Wave-B: curve-brush tessellation ────────────────────────────────────

TEST_CASE("lm_mapimport tessellates a cylinder into an N+2-face convex brush", "[tools][lm_mapimport][wave-b]") {
    CurveCylinder c;
    c.origin   = { 0, 0, 0 };
    c.axis     = { 0, 0, 1 };
    c.radius   = 4.0f;
    c.height   = 8.0f;
    c.segments = 8;
    c.material = "WALL";
    std::string faces = tessellate_cylinder(c);
    REQUIRE_FALSE(faces.empty());
    // Count the `\n`-terminated face lines — each face is emitted on one
    // line by tessellate_cylinder.
    usize face_count = 0;
    for (char ch : faces) {
        if (ch == '\n') ++face_count;
    }
    REQUIRE(face_count == c.segments + 2);   // N sides + 2 caps
}

TEST_CASE("lm_mapimport curve-brush directive feeds qbsp::parse_map", "[tools][lm_mapimport][wave-b]") {
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "{\n"
        "@cylinder 0 0 0 0 0 1 4 8 6 WALL\n"
        "}\n"
        "}\n";
    LevelFile level;
    auto r = import_map(kMap, level);
    REQUIRE(r.ok);
    REQUIRE(r.entity_count == 1);
    REQUIRE(r.brush_count  == 1);

    // Decode the brushes chunk to confirm the cylinder expansion produced
    // segments + 2 face planes.
    auto it = std::find_if(level.chunks.begin(), level.chunks.end(),
        [](const LevelChunk& c) { return c.kind == ChunkKind::kBrushes; });
    REQUIRE(it != level.chunks.end());
    const auto& bytes = it->bytes;
    REQUIRE(bytes.size() >= 8);
    u32 nb = static_cast<u32>(bytes[0])
           | (static_cast<u32>(bytes[1]) << 8)
           | (static_cast<u32>(bytes[2]) << 16)
           | (static_cast<u32>(bytes[3]) << 24);
    REQUIRE(nb == 1);
    u32 nplanes = static_cast<u32>(bytes[4])
                | (static_cast<u32>(bytes[5]) << 8)
                | (static_cast<u32>(bytes[6]) << 16)
                | (static_cast<u32>(bytes[7]) << 24);
    REQUIRE(nplanes == 6u + 2u);   // segs + 2 caps
}

TEST_CASE("lm_mapimport sphere tessellation produces a convex polyhedron", "[tools][lm_mapimport][wave-b]") {
    CurveSphere s;
    s.origin = { 0, 0, 0 };
    s.radius = 4.0f;
    s.subdivisions = 0;   // icosahedron — 20 faces
    s.material = "WALL";
    std::string faces = tessellate_sphere(s);
    REQUIRE_FALSE(faces.empty());
    usize face_count = 0;
    for (usize i = 0; i < faces.size(); ++i) {
        if (faces[i] == '\n') ++face_count;
    }
    REQUIRE(face_count == 20);

    s.subdivisions = 1;   // each tri → 4 → 80 faces
    std::string faces1 = tessellate_sphere(s);
    usize fc1 = 0;
    for (usize i = 0; i < faces1.size(); ++i) if (faces1[i] == '\n') ++fc1;
    REQUIRE(fc1 == 80);
}

TEST_CASE("lm_mapimport curve-brush directive is idempotent on non-curve maps", "[tools][lm_mapimport][wave-b]") {
    constexpr const char* kMap =
        "{\n"
        "\"classname\" \"worldspawn\"\n"
        "{\n"
        "( -8 -8 -8 ) ( -8 -7 -8 ) ( -8 -8 -7 ) WALL 0 0 0 1 1\n"
        "(  8 -8 -8 ) (  8 -8 -7 ) (  8 -7 -8 ) WALL 0 0 0 1 1\n"
        "( -8 -8 -8 ) ( -8 -8 -7 ) ( -7 -8 -8 ) WALL 0 0 0 1 1\n"
        "( -8  8 -8 ) ( -7  8 -8 ) ( -8  8 -7 ) WALL 0 0 0 1 1\n"
        "( -8 -8 -8 ) ( -7 -8 -8 ) ( -8 -7 -8 ) FLOOR 0 0 0 1 1\n"
        "( -8 -8  8 ) ( -8 -7  8 ) ( -7 -8  8 ) CEIL  0 0 0 1 1\n"
        "}\n"
        "}\n";
    // The Wave-A path (curve-brush expansion enabled) and the directly-
    // parsed path must produce identical brush data.
    LevelFile a, b;
    REQUIRE(import_map(kMap, a, ImportOptions{}).ok);
    ImportOptions opt; opt.expand_curve_brushes = false;
    REQUIRE(import_map(kMap, b, opt).ok);
    REQUIRE(a.chunks.size() == b.chunks.size());
    for (usize i = 0; i < a.chunks.size(); ++i) {
        REQUIRE(a.chunks[i].kind  == b.chunks[i].kind);
        REQUIRE(a.chunks[i].bytes == b.chunks[i].bytes);
    }
}
