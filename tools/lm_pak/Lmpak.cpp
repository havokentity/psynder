// SPDX-License-Identifier: MIT
// Psynder — .lmpak producer / reader. Lane 24 / tools.

#include "Lmpak.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace psynder::tools::lmpak {

namespace fs = std::filesystem;

namespace {

constexpr usize kHeaderBytes = 56;

template <class T>
void append_le(std::vector<u8>& out, T value) {
    static_assert(std::is_integral_v<T>, "append_le expects an integral");
    using U = std::make_unsigned_t<T>;
    auto u = static_cast<U>(value);
    for (usize i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<u8>(u >> (8 * i)));
    }
}

template <class T>
bool read_le(std::span<const u8> bytes, usize offset, T& out) {
    if (offset + sizeof(T) > bytes.size()) return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        u |= static_cast<U>(bytes[offset + i]) << (8 * i);
    }
    out = static_cast<T>(u);
    return true;
}

// CRC32 (IEEE 802.3 polynomial, reflected). Used for integrity only; the
// archive is still readable if every entry has crc32 == 0 (the "unused"
// sentinel).
u32 crc32_compute(std::span<const u8> data) noexcept {
    static u32 table[256];
    static bool inited = false;
    if (!inited) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        inited = true;
    }
    u32 crc = 0xFFFFFFFFu;
    for (u8 b : data) {
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string normalize_path(std::string_view input) {
    std::string p;
    p.reserve(input.size());
    for (char c : input) {
        if (c == '\\') {
            p.push_back('/');
        } else {
            p.push_back(c);
        }
    }
    // Collapse leading "./" segments and any leading slash.
    while (p.size() >= 2 && p[0] == '.' && p[1] == '/') {
        p.erase(0, 2);
    }
    while (!p.empty() && p.front() == '/') {
        p.erase(0, 1);
    }
    return p;
}

std::string lowercase_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

bool read_file(const fs::path& p, std::vector<u8>& out, std::string& err) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        err = "cannot open " + p.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz < 0) {
        err = "cannot stat " + p.string();
        return false;
    }
    out.resize(static_cast<usize>(sz));
    in.seekg(0, std::ios::beg);
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!in) {
            err = "short read on " + p.string();
            return false;
        }
    }
    return true;
}

bool write_file(const fs::path& p, std::span<const u8> data, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + p.string();
        return false;
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

// ─── Compression — defer to lane 05 helpers when zstd::libzstd_static is
// available; otherwise the archive falls back to raw storage. We avoid
// hard-linking against zstd here because tools should still build on dev
// boxes without the system zstd installed; the "no compression" path is
// always correct.
//
// We use a weak-symbol convention: a future lane-05-internal header could
// drop into our build and expose psynder::asset::zstd_compress(...). For
// Wave A we keep it self-contained — the bit flag is wired in the format
// but pack_options.compress is treated as a hint, and decoders that can't
// inflate gracefully degrade.
bool maybe_compress(std::span<const u8> raw,
                    std::vector<u8>& out,
                    const PackOptions& opt) {
    (void)raw;
    (void)out;
    (void)opt;
    return false;  // raw passthrough; zstd wiring lives in lane 05
}

bool maybe_decompress(std::span<const u8> compressed,
                      u64 expected_raw,
                      std::vector<u8>& out,
                      std::string& err) {
    (void)expected_raw;
    err = "compressed entry encountered but zstd backend not linked in this build";
    out.clear();
    out.reserve(compressed.size());
    return false;
}

// Build the index blob into `out` and return where it starts in `archive`.
void write_index(std::vector<u8>& archive,
                 const std::vector<EntryRecord>& entries,
                 u64& index_offset,
                 u64& index_bytes) {
    index_offset = archive.size();

    // The index has two parts: fixed-size records + path string blob.
    // We emit a 40-byte record per entry (see Lmpak.h header for layout)
    // followed by a packed UTF-8 string blob.
    std::vector<u8> blob;
    std::vector<u32> path_offsets;
    path_offsets.reserve(entries.size());
    for (const auto& e : entries) {
        path_offsets.push_back(static_cast<u32>(blob.size()));
        blob.insert(blob.end(),
                    reinterpret_cast<const u8*>(e.path.data()),
                    reinterpret_cast<const u8*>(e.path.data() + e.path.size()));
    }

    usize start = archive.size();
    for (usize i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        append_le<u64>(archive, e.hash);
        append_le<u64>(archive, e.offset);
        append_le<u64>(archive, e.stored);
        append_le<u64>(archive, e.raw);
        append_le<u32>(archive, path_offsets[i]);
        append_le<u32>(archive, static_cast<u32>(e.path.size()));
        append_le<u32>(archive, e.flags);
        append_le<u32>(archive, e.crc32);
    }
    archive.insert(archive.end(), blob.begin(), blob.end());

    index_bytes = archive.size() - start;
}

void write_header(std::vector<u8>& archive,
                  u32 archive_flags,
                  u32 entry_count,
                  u64 index_offset,
                  u64 index_bytes) {
    // Patch the leading kHeaderBytes that we reserved at the start.
    auto write_at = [&](usize off, std::span<const u8> bytes) {
        std::memcpy(archive.data() + off, bytes.data(), bytes.size());
    };

    u8 hdr[kHeaderBytes];
    std::memset(hdr, 0, sizeof(hdr));
    std::memcpy(hdr + 0,  &kMagic, 4);
    u32 v = kVersion;
    std::memcpy(hdr + 4,  &v, 4);
    std::memcpy(hdr + 8,  &archive_flags, 4);
    std::memcpy(hdr + 12, &entry_count, 4);
    std::memcpy(hdr + 16, &index_offset, 8);
    std::memcpy(hdr + 24, &index_bytes, 8);
    // Reserved bytes are already zeroed.

    write_at(0, std::span<const u8>(hdr, kHeaderBytes));
}

}  // namespace

u64 fnv1a64(std::string_view s) noexcept {
    // Lowercase ASCII fold during hashing so windows / unix path-case
    // differences don't change the hash. (Non-ASCII chars are passed
    // through unmodified; mixed-case unicode paths would still collide
    // identically across platforms.)
    constexpr u64 kOffset = 0xcbf29ce484222325ULL;
    constexpr u64 kPrime  = 0x100000001b3ULL;
    u64 h = kOffset;
    for (char raw : s) {
        u8 c = static_cast<u8>(raw);
        if (c >= 'A' && c <= 'Z') c = static_cast<u8>(c - 'A' + 'a');
        h ^= c;
        h *= kPrime;
    }
    return h;
}

PackResult pack_blobs(std::span<const MemEntry> entries,
                      std::vector<u8>& out_bytes,
                      const PackOptions& opt) {
    PackResult result;
    out_bytes.clear();
    out_bytes.reserve(64 * 1024);
    out_bytes.resize(kHeaderBytes);  // reserve header slot

    std::vector<EntryRecord> records;
    records.reserve(entries.size());

    u32 archive_flags = 0;

    for (const auto& e : entries) {
        std::string vp = normalize_path(e.path);
        if (vp.empty()) {
            continue;  // skip directory-only entries
        }
        EntryRecord rec;
        rec.path = vp;
        rec.hash = fnv1a64(lowercase_ascii(vp));
        rec.offset = out_bytes.size();
        rec.raw = e.data.size();

        // Best-effort compression.
        std::vector<u8> compressed;
        bool wrote_compressed = false;
        if (opt.compress && e.data.size() >= opt.min_compress) {
            wrote_compressed = maybe_compress(e.data, compressed, opt);
        }
        if (wrote_compressed) {
            rec.stored = compressed.size();
            rec.flags |= kEntryFlagZstd;
            archive_flags |= kFlagZstd;
            out_bytes.insert(out_bytes.end(), compressed.begin(), compressed.end());
        } else {
            rec.stored = e.data.size();
            if (!e.data.empty()) {
                out_bytes.insert(out_bytes.end(), e.data.begin(), e.data.end());
            }
        }
        rec.crc32 = crc32_compute(e.data);

        records.push_back(std::move(rec));
    }

    // Sort by hash so lookups can binary-search on read.
    std::sort(records.begin(), records.end(),
              [](const EntryRecord& a, const EntryRecord& b) { return a.hash < b.hash; });

    u64 index_offset = 0;
    u64 index_bytes  = 0;
    write_index(out_bytes, records, index_offset, index_bytes);
    write_header(out_bytes, archive_flags,
                 static_cast<u32>(records.size()),
                 index_offset, index_bytes);

    result.ok = true;
    result.entries = static_cast<u32>(records.size());
    result.total_stored = 0;
    result.total_raw = 0;
    for (const auto& r : records) {
        result.total_stored += r.stored;
        result.total_raw    += r.raw;
    }
    return result;
}

PackResult pack_directory(std::string_view source_dir,
                          std::string_view archive_path,
                          const PackOptions& opt) {
    PackResult result;
    std::error_code ec;
    fs::path root(source_dir);
    if (!fs::is_directory(root, ec)) {
        result.error = "not a directory: " + std::string(source_dir);
        return result;
    }

    std::vector<MemEntry> entries;
    for (auto it = fs::recursive_directory_iterator(root, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        fs::path rel = fs::relative(it->path(), root, ec);
        if (ec) continue;
        MemEntry me;
        me.path = rel.generic_string();
        std::string err;
        if (!read_file(it->path(), me.data, err)) {
            result.error = err;
            return result;
        }
        entries.push_back(std::move(me));
    }

    std::vector<u8> bytes;
    PackResult sub = pack_blobs(entries, bytes, opt);
    if (!sub.ok) {
        return sub;
    }

    std::string err;
    if (!write_file(fs::path(archive_path), bytes, err)) {
        result.error = err;
        return result;
    }
    sub.ok = true;
    return sub;
}

bool read_index(std::span<const u8> bytes, ArchiveInfo& out, std::string* err) {
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return false;
    };
    if (bytes.size() < kHeaderBytes) return fail("archive too small for header");
    u32 magic = 0;
    if (!read_le<u32>(bytes, 0, magic) || magic != kMagic)
        return fail("bad magic");
    u32 version = 0;
    if (!read_le<u32>(bytes, 4, version)) return fail("truncated header");
    out.version = version;
    if (version != kVersion) return fail("unsupported version");
    u32 archive_flags = 0;
    if (!read_le<u32>(bytes, 8, archive_flags)) return fail("truncated header");
    out.flags = archive_flags;
    u32 entry_count = 0;
    if (!read_le<u32>(bytes, 12, entry_count)) return fail("truncated header");
    u64 index_offset = 0;
    u64 index_bytes = 0;
    if (!read_le<u64>(bytes, 16, index_offset)) return fail("truncated header");
    if (!read_le<u64>(bytes, 24, index_bytes)) return fail("truncated header");
    if (index_offset + index_bytes > bytes.size()) return fail("index out of range");

    constexpr usize kRecordBytes = 48;
    if (entry_count * kRecordBytes > index_bytes) return fail("index too short for entry table");
    out.entry_count = entry_count;
    out.entries.clear();
    out.entries.reserve(entry_count);

    usize cursor = static_cast<usize>(index_offset);
    std::vector<std::pair<u32, u32>> path_slots;
    path_slots.reserve(entry_count);
    for (u32 i = 0; i < entry_count; ++i) {
        EntryRecord r;
        if (!read_le<u64>(bytes, cursor +  0, r.hash))   return fail("truncated record");
        if (!read_le<u64>(bytes, cursor +  8, r.offset)) return fail("truncated record");
        if (!read_le<u64>(bytes, cursor + 16, r.stored)) return fail("truncated record");
        if (!read_le<u64>(bytes, cursor + 24, r.raw))    return fail("truncated record");
        u32 path_off = 0, path_len = 0;
        if (!read_le<u32>(bytes, cursor + 32, path_off)) return fail("truncated record");
        if (!read_le<u32>(bytes, cursor + 36, path_len)) return fail("truncated record");
        if (!read_le<u32>(bytes, cursor + 40, r.flags))  return fail("truncated record flags");
        if (!read_le<u32>(bytes, cursor + 44, r.crc32))  return fail("truncated record crc");
        cursor += kRecordBytes;
        path_slots.emplace_back(path_off, path_len);
        out.entries.push_back(std::move(r));
    }
    usize string_blob = static_cast<usize>(index_offset) + entry_count * kRecordBytes;
    if (string_blob > index_offset + index_bytes) return fail("string blob runs past index");
    for (u32 i = 0; i < entry_count; ++i) {
        auto [off, len] = path_slots[i];
        usize start = string_blob + off;
        if (start + len > index_offset + index_bytes) return fail("path slice out of range");
        out.entries[i].path.assign(reinterpret_cast<const char*>(bytes.data() + start), len);
    }
    return true;
}

bool read_index_file(std::string_view archive_path, ArchiveInfo& out, std::string* err) {
    std::string e;
    std::vector<u8> bytes;
    if (!read_file(fs::path(archive_path), bytes, e)) {
        if (err) *err = e;
        return false;
    }
    return read_index(bytes, out, err);
}

bool extract_entry(std::span<const u8> bytes,
                   const EntryRecord& rec,
                   std::vector<u8>& out,
                   std::string* err) {
    if (rec.offset + rec.stored > bytes.size()) {
        if (err) *err = "entry payload out of range";
        return false;
    }
    auto payload = bytes.subspan(static_cast<usize>(rec.offset),
                                 static_cast<usize>(rec.stored));
    if (rec.flags & kEntryFlagZstd) {
        std::string e;
        bool ok = maybe_decompress(payload, rec.raw, out, e);
        if (!ok) {
            if (err) *err = e;
            return false;
        }
        return true;
    }
    out.assign(payload.begin(), payload.end());
    return true;
}

UnpackResult unpack_archive(std::string_view archive_path,
                            std::string_view dest_dir) {
    UnpackResult res;
    std::vector<u8> bytes;
    std::string e;
    if (!read_file(fs::path(archive_path), bytes, e)) {
        res.error = e;
        return res;
    }
    ArchiveInfo info;
    if (!read_index(bytes, info, &res.error)) {
        return res;
    }
    fs::path dest(dest_dir);
    for (const auto& rec : info.entries) {
        std::vector<u8> raw;
        if (!extract_entry(bytes, rec, raw, &res.error)) {
            return res;
        }
        fs::path out_path = dest / rec.path;
        std::string werr;
        if (!write_file(out_path, raw, werr)) {
            res.error = werr;
            return res;
        }
        ++res.entries;
    }
    res.ok = true;
    return res;
}

void print_help() {
    std::fprintf(stdout,
        "lm_pak — Psynder .lmpak archive packer / unpacker\n"
        "\n"
        "Usage:\n"
        "  lm_pak pack   <source_dir>   <archive.lmpak> [--zstd] [--level N]\n"
        "  lm_pak unpack <archive.lmpak> <dest_dir>\n"
        "  lm_pak list   <archive.lmpak>\n"
        "  lm_pak --help\n"
        "\n"
        "Notes:\n"
        "  - FNV-1a 64-bit hash indexes the file table (case-insensitive paths).\n"
        "  - --zstd compresses each entry independently when supported by the build.\n"
        "  - Reads can be performed via lane 05's Vfs::mount_pak once that lands.\n");
}

namespace {

int cmd_pack(int argc, char** argv) {
    if (argc < 4) { print_help(); return 1; }
    PackOptions opt;
    for (int i = 4; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--zstd") {
            opt.compress = true;
        } else if (a == "--level" && i + 1 < argc) {
            opt.zstd_level = static_cast<u32>(std::atoi(argv[++i]));
        }
    }
    auto r = pack_directory(argv[2], argv[3], opt);
    if (!r.ok) {
        std::fprintf(stderr, "lm_pak: pack failed: %s\n", r.error.c_str());
        return 1;
    }
    std::fprintf(stdout, "lm_pak: packed %u entries (%llu raw, %llu stored) -> %s\n",
                 r.entries,
                 static_cast<unsigned long long>(r.total_raw),
                 static_cast<unsigned long long>(r.total_stored),
                 argv[3]);
    return 0;
}

int cmd_unpack(int argc, char** argv) {
    if (argc < 4) { print_help(); return 1; }
    auto r = unpack_archive(argv[2], argv[3]);
    if (!r.ok) {
        std::fprintf(stderr, "lm_pak: unpack failed: %s\n", r.error.c_str());
        return 1;
    }
    std::fprintf(stdout, "lm_pak: unpacked %u entries -> %s\n", r.entries, argv[3]);
    return 0;
}

int cmd_list(int argc, char** argv) {
    if (argc < 3) { print_help(); return 1; }
    ArchiveInfo info;
    std::string err;
    if (!read_index_file(argv[2], info, &err)) {
        std::fprintf(stderr, "lm_pak: list failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout, "lm_pak: %u entries (version=%u flags=0x%08x)\n",
                 info.entry_count, info.version, info.flags);
    for (const auto& e : info.entries) {
        std::fprintf(stdout, "  %016llx  raw=%-10llu stored=%-10llu %s\n",
                     static_cast<unsigned long long>(e.hash),
                     static_cast<unsigned long long>(e.raw),
                     static_cast<unsigned long long>(e.stored),
                     e.path.c_str());
    }
    return 0;
}

}  // namespace

int cli_main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 1; }
    std::string_view cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") { print_help(); return 0; }
    if (cmd == "pack")   return cmd_pack(argc, argv);
    if (cmd == "unpack") return cmd_unpack(argc, argv);
    if (cmd == "list")   return cmd_list(argc, argv);
    std::fprintf(stderr, "lm_pak: unknown command '%.*s'\n",
                 static_cast<int>(cmd.size()), cmd.data());
    print_help();
    return 1;
}

}  // namespace psynder::tools::lmpak
