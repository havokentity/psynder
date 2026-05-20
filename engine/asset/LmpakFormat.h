// SPDX-License-Identifier: MIT
// Psynder — .lmpak archive on-disk format v1.
//
// Layout (little-endian, packed, no padding beyond the explicit alignment
// fields below):
//
//     +---------------------+  offset 0
//     |  LmpakHeader        |    magic 'L''M''P''K', version=1, flags,
//     |                     |    entry_count, name_table_size,
//     |                     |    entry_table_offset, blob_section_offset
//     +---------------------+
//     |  Blob bytes         |    raw entry payloads, each at entry.offset.
//     |  (entry payloads)   |    Compressed entries use zstd raw framed
//     |                     |    bytes; uncompressed are stored verbatim.
//     +---------------------+
//     |  LmpakEntry[N]      |    entry table, indexed by hash via the
//     |                     |    cooker (entries are sorted by name_hash
//     |                     |    so a runtime binary search finds them).
//     +---------------------+
//     |  Name table         |    NUL-terminated UTF-8 paths, referenced
//     |                     |    by LmpakEntry::name_offset.
//     +---------------------+
//
// Hashing: paths are normalized to forward slashes, lowercase ASCII, and
// hashed with FNV-1a-64 (seed = kFnvOffsetBasis). The cooker stores
// entries sorted ascending by name_hash so the reader binary-searches.
//
// All offsets are byte offsets from the start of the archive file. The
// runtime mmaps the entire file; Blob views are pointers into that
// mapping when an entry is uncompressed, and pointers into a streaming
// arena when compressed (lane 05 keeps a small decompress cache).

#pragma once

#include "core/Types.h"

namespace psynder::asset::lmpak {

inline constexpr u32 kMagic = 0x4B504D4Cu;  // 'L','M','P','K' little-endian
inline constexpr u32 kVersion = 1u;

// Archive-wide flag bits in LmpakHeader::flags.
inline constexpr u32 kFlagSorted = 1u << 0;    // entries sorted by name_hash
inline constexpr u32 kFlagHasCompr = 1u << 1;  // at least one zstd entry

// Per-entry flag bits in LmpakEntry::flags.
inline constexpr u16 kEntryZstd = 1u << 0;       // payload is zstd-framed
inline constexpr u16 kEntryDirectory = 1u << 1;  // reserved (not used yet)

// FNV-1a-64 constants (path hash). Centralized so the cooker and reader
// agree byte-for-byte.
inline constexpr u64 kFnvOffsetBasis = 0xcbf29ce484222325ull;
inline constexpr u64 kFnvPrime = 0x00000100000001b3ull;

// Normalize a path: forward slashes, lowercase ASCII. The hash is
// computed over the normalized form.
inline constexpr u64 fnv1a_path(const char* s, usize n) noexcept {
    u64 h = kFnvOffsetBasis;
    for (usize i = 0; i < n; ++i) {
        u8 c = static_cast<u8>(s[i]);
        if (c == '\\')
            c = '/';
        if (c >= 'A' && c <= 'Z')
            c = static_cast<u8>(c + ('a' - 'A'));
        h ^= c;
        h *= kFnvPrime;
    }
    return h;
}

// Header is fixed-size 64 bytes; aligned so the mmap base can be cast to
// LmpakHeader* without alignment fault.
struct alignas(8) LmpakHeader {
    u32 magic;                // = kMagic
    u32 version;              // = kVersion
    u32 flags;                // archive-wide flags (kFlag*)
    u32 entry_count;          // number of LmpakEntry records
    u64 entry_table_offset;   // byte offset of the LmpakEntry[] array
    u64 name_table_offset;    // byte offset of the name pool (UTF-8)
    u64 name_table_size;      // size of the name pool in bytes
    u64 blob_section_offset;  // byte offset of the first payload
    u64 blob_section_size;    // size of the payload section
    u64 build_unix_time;      // optional, 0 if unset
};
static_assert(sizeof(LmpakHeader) == 64, "LmpakHeader is the v1 wire size");

// One per archived file. Sorted by name_hash if kFlagSorted is set.
struct alignas(8) LmpakEntry {
    u64 name_hash;     // FNV-1a-64 of the normalized path
    u64 offset;        // payload byte offset (from file start)
    u64 size;          // payload size on disk (compressed if kEntryZstd)
    u64 uncompressed;  // size after decompression (== size when raw)
    u32 name_offset;   // offset within the name pool (relative)
    u16 name_len;      // length (no NUL)
    u16 flags;         // per-entry flags (kEntry*)
};
static_assert(sizeof(LmpakEntry) == 40, "LmpakEntry is the v1 wire size");

}  // namespace psynder::asset::lmpak
