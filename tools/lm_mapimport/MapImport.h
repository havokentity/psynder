// SPDX-License-Identifier: MIT
// Psynder — lm_mapimport: one-way TrenchBroom .map → .psylevel converter.
// Lane 24 / tools. Per DESIGN.md §10.8: "a one-way courtesy bridge for
// existing Quake-style maps. That's not a dependency, just a one-way bridge."
//
// Wave-A scope:
//   - Parse the same .map subset lm_qbsp accepts (worldspawn brushes +
//     entity key/values).
//   - Emit a `.psylevel` text-prefixed binary holding:
//       (a) every entity's classname + key/values,
//       (b) per worldspawn brush, its plane list (normal + d + material).
//   - Optionally invoke the lm_qbsp compile pass and embed the resulting
//     .psybsp inside the .psylevel as a chunk. (Useful for round-trip
//     tests: import-then-load reproduces the same BSP without re-running
//     the compiler.) Gated behind --compile-bsp.
//
// Wave-B additions:
//   - Curve-brush support: TrenchBroom-style cylinder / sphere primitives
//     get tessellated into BSP-compatible convex brushes (each shape is a
//     single convex polyhedron — N-gonal prism for cylinder, icosahedron-
//     subdivided polyhedron for sphere). The .map preprocessor recognises
//     `@cylinder` / `@sphere` directives inside brace blocks and synthesises
//     the equivalent face list before handing the text to qbsp::parse_map.
//
// Real .psylevel will grow more sections in later waves (lightmaps, entity
// graph, prefabs); this format reserves a chunk-index header so we can
// add chunk types later without invalidating Wave-A files.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::mapimport {

inline constexpr u32 kPsyLevelMagic   = 0x4C56534Du;  // 'MSVL'
inline constexpr u32 kPsyLevelVersion = 1u;

enum class ChunkKind : u32 {
    kEntities  = 0x01,    // text payload: classname + kv per entity (NUL-separated records)
    kBrushes   = 0x02,    // binary: world brushes (plane lists)
    kPsyBsp    = 0x03,    // binary: embedded compiled .psybsp bytes
};

struct LevelChunk {
    ChunkKind        kind = ChunkKind::kEntities;
    std::vector<u8>  bytes;
};

struct LevelFile {
    std::vector<LevelChunk> chunks;
};

struct ImportOptions {
    bool compile_bsp = false;        // emit kPsyBsp chunk too
    bool expand_curve_brushes = true;   // Wave-B: tessellate @cylinder / @sphere
};

// ─── Curve-brush API (Wave-B) ────────────────────────────────────────────
// The .map text preprocessor recognises two directives, one per line, inside
// a brace block (where face lists usually live):
//
//   @cylinder  cx cy cz  ax ay az  radius height segments material
//   @sphere    cx cy cz  radius subdivisions material
//
// Each directive is replaced by the equivalent convex brush face list. The
// surrounding `{` / `}` is preserved so qbsp::parse_map sees a normal brush.
//
// Standalone helpers are exposed for tests:
struct CurveCylinder {
    math::Vec3 origin{0,0,0};
    math::Vec3 axis{0,0,1};
    f32        radius   = 1.0f;
    f32        height   = 1.0f;
    u32        segments = 8;
    std::string material = "WALL";
};
struct CurveSphere {
    math::Vec3 origin{0,0,0};
    f32        radius = 1.0f;
    u32        subdivisions = 1;  // 0 = icosahedron, +1 each subdivision
    std::string material = "WALL";
};

// Emit a .map-formatted face block (the run between `{` and `}`, exclusive)
// representing the convex polyhedron approximation. The result can be spliced
// straight into a brace block fed to qbsp::parse_map.
std::string tessellate_cylinder(const CurveCylinder& c);
std::string tessellate_sphere(const CurveSphere& s);

// Walk `text`, replacing every `@cylinder ...` / `@sphere ...` directive
// inside a brace block with the equivalent face block. Returns the rewritten
// text. Idempotent if no directives are present.
std::string expand_curve_brushes(std::string_view text);

struct ImportResult {
    bool        ok = false;
    u32         entity_count = 0;
    u32         brush_count = 0;
    std::string error;
};

// Convert .map text to a LevelFile in memory; tests use this entrypoint.
ImportResult import_map(std::string_view map_text,
                        LevelFile& out,
                        const ImportOptions& opt = {});

// Read / write .psylevel from / to bytes.
void write_level(const LevelFile& level, std::vector<u8>& out);
bool read_level(std::span<const u8> bytes, LevelFile& out, std::string* err = nullptr);

int  cli_main(int argc, char** argv);
void print_help();

}  // namespace psynder::tools::mapimport
