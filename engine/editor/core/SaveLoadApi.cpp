// SPDX-License-Identifier: MIT
// Psynder — public save/load API for .psylevel + .psyc. The format itself
// is defined in Serialization.h; this TU wires it into std::fstream and
// to lane 05's VFS for reads (writes always go to the host filesystem
// because the VFS is read-only).
//
// zstd is optional: if the asset lane wires it in (find_package(zstd))
// the encoder uses it; otherwise the payload is written uncompressed
// and the on-disk flags bit signals "raw".

#include "Editor.h"
#include "EditorState.h"
#include "Serialization.h"

#include "asset/Vfs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if __has_include(<zstd.h>)
#   include <zstd.h>
#   define PSYNDER_EDITOR_HAS_ZSTD 1
#else
#   define PSYNDER_EDITOR_HAS_ZSTD 0
#endif

namespace psynder::editor {

namespace {

struct DiskHeader {
    u32 magic;
    u16 version;
    u16 flags;
    u32 payload_size;     // uncompressed body size
    u32 body_size;        // on-disk body size
};
static_assert(sizeof(DiskHeader) == 16, "psylevel header must stay 16 bytes");

PSY_FORCEINLINE std::string to_string(std::string_view v) { return std::string(v); }

bool write_file_bytes(std::string_view path, const std::vector<u8>& bytes) {
    std::ofstream f(to_string(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

bool read_file_bytes(std::string_view path, std::vector<u8>& out) {
    // First try the VFS (lane 05). If that returns nothing, fall back to
    // a host-filesystem read so editor sessions can load loose files
    // (which is the common authoring workflow).
    auto& vfs = asset::Vfs::Get();
    asset::Blob blob = vfs.read(path);
    if (blob.data && blob.bytes > 0) {
        out.assign(blob.data, blob.data + blob.bytes);
        return true;
    }
    std::ifstream f(to_string(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0);
    out.resize(static_cast<usize>(n));
    f.read(reinterpret_cast<char*>(out.data()), n);
    return f.good();
}

#if PSYNDER_EDITOR_HAS_ZSTD
bool compress_body(const std::vector<u8>& in, std::vector<u8>& out, u16& flags_out) {
    const usize bound = ZSTD_compressBound(in.size());
    out.resize(bound);
    const usize n = ZSTD_compress(out.data(), out.size(),
                                  in.data(),  in.size(),
                                  ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    flags_out |= serial::kFlagZstd;
    return true;
}

bool decompress_body(const u8* src, usize src_size, usize payload_size, std::vector<u8>& out) {
    out.resize(payload_size);
    const usize n = ZSTD_decompress(out.data(), out.size(), src, src_size);
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    return true;
}
#endif

bool save_blob(std::string_view path, u32 magic, bool with_terrain) {
    auto& s = detail::get_state();

    std::vector<u8> body;
    body.reserve(4096);
    serial::encode_state(body, s, with_terrain);

    std::vector<u8> on_disk;
    u16 flags = 0;

#if PSYNDER_EDITOR_HAS_ZSTD
    if (!compress_body(body, on_disk, flags)) {
        on_disk = body;
        flags   = 0;
    }
#else
    on_disk = body;
#endif

    DiskHeader hdr{};
    hdr.magic        = magic;
    hdr.version      = serial::kFormatVersion;
    hdr.flags        = flags;
    hdr.payload_size = static_cast<u32>(body.size());
    hdr.body_size    = static_cast<u32>(on_disk.size());

    std::vector<u8> file;
    file.reserve(sizeof(hdr) + on_disk.size());
    file.resize(sizeof(hdr));
    std::memcpy(file.data(), &hdr, sizeof(hdr));
    file.insert(file.end(), on_disk.begin(), on_disk.end());

    return write_file_bytes(path, file);
}

bool load_blob(std::string_view path, u32 expected_magic, bool with_terrain) {
    std::vector<u8> file;
    if (!read_file_bytes(path, file)) return false;
    if (file.size() < sizeof(DiskHeader)) return false;

    DiskHeader hdr{};
    std::memcpy(&hdr, file.data(), sizeof(hdr));
    if (hdr.magic != expected_magic)             return false;
    if (hdr.version > serial::kFormatVersion)    return false;
    if (file.size() < sizeof(hdr) + hdr.body_size) return false;

    const u8* body_src = file.data() + sizeof(hdr);
    std::vector<u8> body;

    if (hdr.flags & serial::kFlagZstd) {
#if PSYNDER_EDITOR_HAS_ZSTD
        if (!decompress_body(body_src, hdr.body_size, hdr.payload_size, body)) return false;
#else
        // File claims zstd but we weren't built with it; refuse so the
        // user gets a clear error rather than garbage data.
        return false;
#endif
    } else {
        body.assign(body_src, body_src + hdr.body_size);
    }

    auto& s = detail::get_state();
    if (with_terrain) {
        // Full reset of the editor state on level load.
        s.brushes.clear();
        s.entities.clear();
        s.bodies.clear();
        s.constraint_graph.clear();
        s.selection.clear();
        s.undo.clear();
    } else {
        // Contraption merges into the running scene (sandbox spawning).
        // We still clear constraints so the graph reflects the loaded set
        // — full graph merging is M6 polish.
        s.constraint_graph.clear();
    }
    return serial::decode_state(body.data(), body.size(), s, with_terrain);
}

}  // namespace

bool save_level(std::string_view path) {
    return save_blob(path, serial::kPsyLevelMagic, /*with_terrain=*/true);
}

bool load_level(std::string_view path) {
    return load_blob(path, serial::kPsyLevelMagic, /*with_terrain=*/true);
}

bool save_contraption(std::string_view path) {
    return save_blob(path, serial::kPsyConMagic, /*with_terrain=*/false);
}

}  // namespace psynder::editor
