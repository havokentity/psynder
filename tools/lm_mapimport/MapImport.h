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
// Real .psylevel will grow more sections in Wave-B (lightmaps, entity
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
};

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
