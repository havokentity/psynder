// SPDX-License-Identifier: MIT
// Psynder — internal .lmpak v1 builder.
//
// The canonical cooker lives in `tools/lm_pak/` (lane 24). This header
// is the in-lane builder used by Wave-A unit tests to synthesize
// archives that exercise the reader without depending on the tools
// lane being built. Header-only + inline so it links cleanly into both
// `psynder_asset` and `psynder_unit`.
//
// Output layout matches LmpakFormat.h byte-for-byte. Entries are
// emitted in insertion order, then re-sorted by FNV-1a-64 path hash so
// the runtime binary search is valid.

#pragma once

#include "LmpakFormat.h"
#include "core/Types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace psynder::asset::lmpak {

// Compression helpers — defined in LmpakWriter.cpp so the optional zstd
// dependency stays a single-TU concern. Tests + tools call them through
// this header without touching the zstd headers directly.
bool zstd_available() noexcept;
bool zstd_compress(const u8* src, usize src_len, int level, std::vector<u8>& out);

struct WriterEntry {
    std::string path;          // virtual path (normalized inside)
    std::vector<u8> payload;   // raw bytes; if compressed, already zstd-framed
    bool        compressed = false;
    u64         uncompressed_size = 0;  // valid when compressed
};

class Writer {
public:
    // Stage an uncompressed entry.
    void add_raw(std::string path, std::vector<u8> bytes) {
        WriterEntry e;
        e.path              = std::move(path);
        e.payload           = std::move(bytes);
        e.compressed        = false;
        e.uncompressed_size = e.payload.size();
        entries_.push_back(std::move(e));
    }

    // Convenience: stage an entry, optionally compressing it with zstd.
    // Returns true unless `compress` was requested and zstd is unavailable.
    bool add_file(std::string path, const u8* bytes, usize size, bool compress,
                  int level = 9) {
        if (!compress) {
            add_raw(std::move(path), std::vector<u8>(bytes, bytes + size));
            return true;
        }
        std::vector<u8> framed;
        if (!zstd_compress(bytes, size, level, framed)) return false;
        add_zstd(std::move(path), std::move(framed), size);
        return true;
    }

    // Stage a pre-compressed entry. The caller is responsible for the
    // zstd framing; we just record `uncompressed_size` for the reader.
    void add_zstd(std::string path, std::vector<u8> framed_bytes, u64 uncompressed_size) {
        WriterEntry e;
        e.path              = std::move(path);
        e.payload           = std::move(framed_bytes);
        e.compressed        = true;
        e.uncompressed_size = uncompressed_size;
        entries_.push_back(std::move(e));
    }

    // Serialize the staged archive to disk at `out_path`. Returns true
    // on success.
    bool write(const char* out_path) const {
        std::vector<u8> file = build_bytes();
        std::FILE* fp = std::fopen(out_path, "wb");
        if (!fp) return false;
        usize w = std::fwrite(file.data(), 1, file.size(), fp);
        std::fclose(fp);
        return w == file.size();
    }

    // Build the in-memory archive bytes.
    std::vector<u8> build_bytes() const {
        // Step 1: normalize paths + compute hashes, build name pool.
        struct Built {
            u64 hash;
            u32 name_off;
            u16 name_len;
            const WriterEntry* src;
        };
        std::string  name_pool;
        std::vector<Built> built;
        built.reserve(entries_.size());
        for (const auto& e : entries_) {
            std::string n;
            n.reserve(e.path.size());
            for (char c : e.path) {
                if (c == '\\') c = '/';
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
                n.push_back(c);
            }
            u32 off = static_cast<u32>(name_pool.size());
            name_pool.append(n);
            name_pool.push_back('\0');  // optional NUL
            Built b{};
            b.hash     = fnv1a_path(n.data(), n.size());
            b.name_off = off;
            b.name_len = static_cast<u16>(n.size());
            b.src      = &e;
            built.push_back(b);
        }

        // Step 2: sort by hash so the reader can binary search.
        std::sort(built.begin(), built.end(),
                  [](const Built& a, const Built& c) { return a.hash < c.hash; });

        // Step 3: layout — header, blob section, entry table, name table.
        const u64 header_size = sizeof(LmpakHeader);
        u64 cursor = header_size;
        const u64 blob_off = cursor;

        // Reserve payload offsets first.
        std::vector<u64> blob_offsets(built.size(), 0);
        for (usize i = 0; i < built.size(); ++i) {
            // Align payload to 8 bytes for predictable mmap views.
            cursor = (cursor + 7) & ~u64{7};
            blob_offsets[i] = cursor;
            cursor += built[i].src->payload.size();
        }
        const u64 blob_end = cursor;

        cursor = (cursor + 7) & ~u64{7};
        const u64 entry_off = cursor;
        cursor += built.size() * sizeof(LmpakEntry);

        const u64 name_off = cursor;
        cursor += name_pool.size();

        const u64 total_size = cursor;
        std::vector<u8> file(total_size, 0);

        // Step 4: write header.
        LmpakHeader hdr{};
        hdr.magic               = kMagic;
        hdr.version             = kVersion;
        hdr.flags               = kFlagSorted;
        hdr.entry_count         = static_cast<u32>(built.size());
        hdr.entry_table_offset  = entry_off;
        hdr.name_table_offset   = name_off;
        hdr.name_table_size     = name_pool.size();
        hdr.blob_section_offset = blob_off;
        hdr.blob_section_size   = blob_end - blob_off;
        hdr.build_unix_time     = 0;
        bool any_compressed = false;
        for (const auto& e : entries_) {
            if (e.compressed) { any_compressed = true; break; }
        }
        if (any_compressed) hdr.flags |= kFlagHasCompr;
        std::memcpy(file.data(), &hdr, sizeof(hdr));

        // Step 5: write payloads.
        for (usize i = 0; i < built.size(); ++i) {
            const auto& p = built[i].src->payload;
            std::memcpy(file.data() + blob_offsets[i], p.data(), p.size());
        }

        // Step 6: write entry table.
        u8* entry_ptr = file.data() + entry_off;
        for (usize i = 0; i < built.size(); ++i) {
            LmpakEntry e{};
            e.name_hash    = built[i].hash;
            e.offset       = blob_offsets[i];
            e.size         = built[i].src->payload.size();
            e.uncompressed = built[i].src->uncompressed_size;
            e.name_offset  = built[i].name_off;
            e.name_len     = built[i].name_len;
            e.flags        = built[i].src->compressed ? kEntryZstd : u16{0};
            std::memcpy(entry_ptr + i * sizeof(LmpakEntry), &e, sizeof(e));
        }

        // Step 7: write name pool.
        if (!name_pool.empty()) {
            std::memcpy(file.data() + name_off, name_pool.data(), name_pool.size());
        }
        return file;
    }

private:
    std::vector<WriterEntry> entries_;
};

}  // namespace psynder::asset::lmpak
