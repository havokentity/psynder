// SPDX-License-Identifier: MIT
// Psynder — .lmpak archive library entrypoint (Lane 24 / tools).
//
// .lmpak format (v1):
//
//   ┌──────────────────────────────────────────────────────────┐
//   │ Header                                                   │
//   │   char     magic[4]   = "LMPK"                           │
//   │   u32      version    = 1                                │
//   │   u32      flags      (bit0 = zstd-per-entry)            │
//   │   u32      entry_count                                   │
//   │   u64      index_offset                                  │
//   │   u64      index_bytes                                   │
//   │   u8       reserved[24]                                  │
//   ├──────────────────────────────────────────────────────────┤
//   │ Payload region: tightly packed entry blobs (raw or zstd) │
//   ├──────────────────────────────────────────────────────────┤
//   │ Index table @ index_offset, sorted by FNV-1a64(path).    │
//   │   For each entry:                                        │
//   │     u64 path_hash    (FNV-1a 64-bit, lowercased path)    │
//   │     u64 data_offset                                      │
//   │     u64 stored_bytes (on-disk, possibly compressed)      │
//   │     u64 raw_bytes    (decompressed size)                 │
//   │     u32 path_offset  (offset into path string blob)      │
//   │     u32 path_length                                      │
//   │     u32 flags        (bit0 = compressed, bit1 = reserved)│
//   │     u32 crc32        (optional integrity; 0 = unused)    │
//   │   Followed by the concatenated path-string blob.         │
//   └──────────────────────────────────────────────────────────┘
//
// Lane 24 ships the producer here; lane 05's VFS will grow a consumer that
// mounts these archives (currently a stub).
//
// All multi-byte integers are little-endian. The reference machines (x64
// desktop, Apple Silicon, modern Linux) are all LE; we'd byte-swap on the
// read side if a big-endian target ever appears.

#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::tools::lmpak {

inline constexpr u32 kMagic = 0x4B504D4Cu;  // 'LMPK' LE
inline constexpr u32 kVersion = 1u;
inline constexpr u32 kFlagZstd = 1u << 0;       // archive contains compressed entries
inline constexpr u32 kEntryFlagZstd = 1u << 0;  // per-entry compressed marker

// FNV-1a 64-bit over the case-folded UTF-8 byte sequence. Stable; this is
// the same hash lane 05's VFS will use to look entries up.
u64 fnv1a64(std::string_view s) noexcept;

struct PackOptions {
    bool compress = false;  // zstd per-entry when available
    u32 zstd_level = 3;     // lane 05 helpers default
    u64 min_compress = 64;  // skip compression for tiny entries
};

struct EntryRecord {
    std::string path;  // virtual path (forward-slashes, lowercased on hash)
    u64 hash = 0;
    u64 offset = 0;
    u64 stored = 0;
    u64 raw = 0;
    u32 flags = 0;
    u32 crc32 = 0;
};

struct PackResult {
    bool ok = false;
    u32 entries = 0;
    u64 total_raw = 0;
    u64 total_stored = 0;
    std::string error;
};

// Pack every regular file under `source_dir` into the .lmpak written at
// `archive_path`. Paths inside the archive are made relative to `source_dir`
// and normalized to forward slashes; comparisons are case-insensitive.
PackResult pack_directory(std::string_view source_dir,
                          std::string_view archive_path,
                          const PackOptions& opt = {});

// In-memory pack: caller provides path → bytes pairs. Useful for tests and
// for tools that synthesize archives without ever touching the filesystem.
struct MemEntry {
    std::string path;
    std::vector<u8> data;
};
PackResult pack_blobs(std::span<const MemEntry> entries,
                      std::vector<u8>& out_bytes,
                      const PackOptions& opt = {});

struct UnpackResult {
    bool ok = false;
    u32 entries = 0;
    std::string error;
};

// Unpack the archive into `dest_dir`, recreating the directory tree.
UnpackResult unpack_archive(std::string_view archive_path, std::string_view dest_dir);

// Read the directory of an archive without unpacking the payload — useful
// for diagnostics, listing tools, and unit tests.
struct ArchiveInfo {
    u32 version = 0;
    u32 flags = 0;
    u32 entry_count = 0;
    std::vector<EntryRecord> entries;
};
bool read_index(std::span<const u8> bytes, ArchiveInfo& out, std::string* err = nullptr);
bool read_index_file(std::string_view archive_path, ArchiveInfo& out, std::string* err = nullptr);

// Extract one entry from an in-memory archive. `out` is resized to the raw
// (decompressed) size on success.
bool extract_entry(std::span<const u8> bytes,
                   const EntryRecord& rec,
                   std::vector<u8>& out,
                   std::string* err = nullptr);

// CLI entry point — exposed for tests so we can drive argv without forking.
int cli_main(int argc, char** argv);

void print_help();

}  // namespace psynder::tools::lmpak
