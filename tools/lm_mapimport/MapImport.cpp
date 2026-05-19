// SPDX-License-Identifier: MIT
// Psynder — lm_mapimport implementation. Lane 24 / tools.

#include "MapImport.h"
#include "../lm_qbsp/Qbsp.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace psynder::tools::mapimport {

namespace fs = std::filesystem;

namespace {

template <class T>
void append_le(std::vector<u8>& out, T value) {
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}
void append_f32(std::vector<u8>& out, f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_le<u32>(out, bits);
}
template <class T>
bool read_le(std::span<const u8> bytes, usize off, T& out) {
    if (off + sizeof(T) > bytes.size()) return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i) u |= static_cast<U>(bytes[off + i]) << (8 * i);
    out = static_cast<T>(u);
    return true;
}
// read_f32 currently unused — kept around as a sibling of append_f32 for the
// upcoming chunk-reading helpers (Wave-B will pull entity transforms out of
// the brushes chunk via the same helper).


// ─── Encode entities chunk ───────────────────────────────────────────────
//
// Layout: u32 entity_count
//         per entity:
//           u32 kv_count
//           per kv: u16 klen, u16 vlen, klen bytes, vlen bytes
std::vector<u8> encode_entities(const qbsp::MapFile& map) {
    std::vector<u8> out;
    append_le<u32>(out, static_cast<u32>(map.entities.size()));
    for (const auto& ent : map.entities) {
        append_le<u32>(out, static_cast<u32>(ent.kv.size()));
        for (const auto& [k, v] : ent.kv) {
            append_le<u16>(out, static_cast<u16>(k.size()));
            append_le<u16>(out, static_cast<u16>(v.size()));
            out.insert(out.end(), k.begin(), k.end());
            out.insert(out.end(), v.begin(), v.end());
        }
    }
    return out;
}

std::vector<u8> encode_brushes(const qbsp::MapFile& map) {
    std::vector<u8> out;
    if (map.entities.empty()) {
        append_le<u32>(out, 0u);
        return out;
    }
    const auto& world = map.entities.front();
    append_le<u32>(out, static_cast<u32>(world.brushes.size()));
    for (const auto& brush : world.brushes) {
        append_le<u32>(out, static_cast<u32>(brush.planes.size()));
        for (const auto& pl : brush.planes) {
            append_f32(out, pl.normal.x);
            append_f32(out, pl.normal.y);
            append_f32(out, pl.normal.z);
            append_f32(out, pl.d);
            append_le<u16>(out, static_cast<u16>(pl.material.size()));
            out.insert(out.end(), pl.material.begin(), pl.material.end());
        }
    }
    return out;
}

}  // anon namespace

ImportResult import_map(std::string_view map_text,
                        LevelFile& out,
                        const ImportOptions& opt) {
    ImportResult res;
    out = {};

    // Wave-B: run the curve-brush preprocessor on the .map text before
    // handing it to qbsp::parse_map. The preprocessor is idempotent on
    // maps that contain no directives, so this is safe to always invoke.
    std::string expanded;
    std::string_view feed = map_text;
    if (opt.expand_curve_brushes) {
        expanded = expand_curve_brushes(map_text);
        feed = expanded;
    }
    qbsp::MapFile map;
    std::string err;
    if (!qbsp::parse_map(feed, map, &err)) {
        res.error = err;
        return res;
    }
    LevelChunk ents;
    ents.kind = ChunkKind::kEntities;
    ents.bytes = encode_entities(map);
    out.chunks.push_back(std::move(ents));

    LevelChunk brushes;
    brushes.kind = ChunkKind::kBrushes;
    brushes.bytes = encode_brushes(map);
    out.chunks.push_back(std::move(brushes));

    if (opt.compile_bsp) {
        qbsp::CompiledBsp bsp;
        if (!qbsp::compile_bsp(map, bsp, &err)) {
            res.error = err;
            return res;
        }
        LevelChunk bsp_chunk;
        bsp_chunk.kind = ChunkKind::kPsyBsp;
        qbsp::write_psybsp(bsp, bsp_chunk.bytes);
        out.chunks.push_back(std::move(bsp_chunk));
    }

    res.ok = true;
    res.entity_count = static_cast<u32>(map.entities.size());
    res.brush_count  = map.entities.empty() ? 0u
                       : static_cast<u32>(map.entities.front().brushes.size());
    return res;
}

void write_level(const LevelFile& level, std::vector<u8>& out) {
    out.clear();
    append_le<u32>(out, kPsyLevelMagic);
    append_le<u32>(out, kPsyLevelVersion);
    append_le<u32>(out, static_cast<u32>(level.chunks.size()));
    append_le<u32>(out, 0);   // reserved

    // Two-pass: emit chunk descriptors (kind, offset, length) first, then
    // the payload region.
    usize desc_off = out.size();
    for (usize i = 0; i < level.chunks.size(); ++i) {
        append_le<u32>(out, static_cast<u32>(level.chunks[i].kind));
        append_le<u64>(out, 0u);   // patched below
        append_le<u64>(out, static_cast<u64>(level.chunks[i].bytes.size()));
    }
    for (usize i = 0; i < level.chunks.size(); ++i) {
        u64 off = static_cast<u64>(out.size());
        std::memcpy(out.data() + desc_off + i * 20u + 4u, &off, sizeof(off));
        out.insert(out.end(), level.chunks[i].bytes.begin(), level.chunks[i].bytes.end());
    }
}

bool read_level(std::span<const u8> bytes, LevelFile& out, std::string* err) {
    auto fail = [&](const char* msg) { if (err) *err = msg; return false; };
    if (bytes.size() < 16) return fail("psylevel header truncated");
    u32 magic = 0, version = 0, chunk_count = 0;
    read_le<u32>(bytes, 0, magic);
    if (magic != kPsyLevelMagic) return fail("psylevel bad magic");
    read_le<u32>(bytes, 4, version);
    if (version != kPsyLevelVersion) return fail("psylevel unsupported version");
    read_le<u32>(bytes, 8, chunk_count);

    out.chunks.clear();
    out.chunks.reserve(chunk_count);
    usize cursor = 16;
    for (u32 i = 0; i < chunk_count; ++i) {
        u32 kind = 0;
        u64 off = 0, len = 0;
        if (!read_le<u32>(bytes, cursor, kind)) return fail("chunk desc truncated");
        if (!read_le<u64>(bytes, cursor + 4, off)) return fail("chunk desc truncated");
        if (!read_le<u64>(bytes, cursor + 12, len)) return fail("chunk desc truncated");
        cursor += 20;
        if (off + len > bytes.size()) return fail("chunk payload out of range");
        LevelChunk c;
        c.kind = static_cast<ChunkKind>(kind);
        c.bytes.assign(bytes.data() + off, bytes.data() + off + len);
        out.chunks.push_back(std::move(c));
    }
    return true;
}

void print_help() {
    std::fprintf(stdout,
        "lm_mapimport — Psynder TrenchBroom .map → .psylevel one-way bridge\n"
        "\n"
        "Usage:\n"
        "  lm_mapimport <in.map> <out.psylevel> [--compile-bsp]\n"
        "  lm_mapimport --help\n"
        "\n"
        "Per DESIGN.md §10.8 this is the courtesy importer for legacy\n"
        "Quake-style maps. The native authoring tool is the in-engine\n"
        "editor (lane 18); use this only when porting existing assets.\n");
}

namespace {

bool read_file(const fs::path& p, std::string& out, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { err = "cannot open " + p.string(); return false; }
    in.seekg(0, std::ios::end);
    out.resize(static_cast<usize>(in.tellg()));
    in.seekg(0, std::ios::beg);
    if (!out.empty()) in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(in);
}
bool write_file(const fs::path& p, std::span<const u8> data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write " + p.string(); return false; }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

}  // anon

int cli_main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }
    std::string_view a = argv[1];
    if (a == "--help" || a == "-h" || a == "help") { print_help(); return 0; }
    if (argc < 3) { print_help(); return 1; }

    ImportOptions opt;
    for (int i = 3; i < argc; ++i) {
        std::string_view k = argv[i];
        if (k == "--compile-bsp") opt.compile_bsp = true;
    }
    std::string text, err;
    if (!read_file(fs::path(argv[1]), text, err)) {
        std::fprintf(stderr, "lm_mapimport: %s\n", err.c_str());
        return 1;
    }
    LevelFile level;
    auto r = import_map(text, level, opt);
    if (!r.ok) {
        std::fprintf(stderr, "lm_mapimport: %s\n", r.error.c_str());
        return 1;
    }
    std::vector<u8> bytes;
    write_level(level, bytes);
    if (!write_file(fs::path(argv[2]), bytes, err)) {
        std::fprintf(stderr, "lm_mapimport: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout,
                 "lm_mapimport: %u entities (%u worldspawn brushes) -> %s%s\n",
                 r.entity_count, r.brush_count, argv[2],
                 opt.compile_bsp ? " [bsp embedded]" : "");
    return 0;
}

}  // namespace psynder::tools::mapimport
