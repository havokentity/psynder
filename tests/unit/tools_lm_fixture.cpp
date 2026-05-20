// SPDX-License-Identifier: MIT
// Lane 24 — Wave-D end-to-end fixture pipeline test.
//
// Build glue in tools/lm_pak/CMakeLists.txt cooks assets/fixtures/crate.png
// and tone_440hz.wav through lm_cook, then packs the cooked .lmt + .lma
// into ${CMAKE_BINARY_DIR}/assets/demo.lmpak via lm_pak. This test:
//
//   1. Opens the built archive, verifies its header is sane, and that both
//      cooked entries are present.
//   2. Asserts the FNV-1a-64 index works: each known virtual path hashes to
//      a record whose `hash` field matches.
//   3. Extracts both entries and verifies the cooked file magics ('LMT1',
//      'LMA1') survived the round-trip.
//   4. Mounts the same archive through lane 05's Vfs::mount_pak. Wave-E
//      unified the on-disk dialects: Vfs now detects the tools-dialect
//      header layout and materializes a canonical entry table in memory,
//      so the lookup + read paths see the same shape they did for the
//      canonical LmpakWriter output. The mount + per-entry round-trip is
//      a hard REQUIRE.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_pak/Lmpak.h"
#include "LmpakFixturePaths.h"  // generated absolute path to demo.lmpak

#include "asset/LmpakWriter.h"
#include "asset/Vfs.h"
#include "asset/VfsInternal.h"
#include "core/Types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace psynder;
using namespace psynder::tools::lmpak;

namespace {

constexpr u32 kLmtMagicLE = 0x31544D4Cu;  // 'LMT1'
constexpr u32 kLmaMagicLE = 0x31414D4Cu;  // 'LMA1'

bool slurp(const std::filesystem::path& p, std::vector<u8>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz < 0)
        return false;
    out.resize(static_cast<usize>(sz));
    in.seekg(0, std::ios::beg);
    if (!out.empty())
        in.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(in);
}

u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

const EntryRecord* find_by_path(const ArchiveInfo& info, std::string_view path) {
    const u64 h = fnv1a64(path);
    for (const auto& r : info.entries) {
        if (r.hash == h)
            return &r;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("lm_fixture: demo.lmpak built from real .png/.wav fixtures",
          "[tools][lm_pak][lm_cook][fixture]") {
    namespace fs = std::filesystem;
    const fs::path archive = PSYNDER_FIXTURE_LMPAK_PATH;

    INFO("expecting fixture at: " << archive.string());
    REQUIRE(fs::exists(archive));
    REQUIRE(fs::file_size(archive) > 0);

    std::vector<u8> bytes;
    REQUIRE(slurp(archive, bytes));

    ArchiveInfo info;
    std::string err;
    REQUIRE(read_index(bytes, info, &err));
    REQUIRE(info.version == kVersion);
    REQUIRE(info.entry_count == 2);
    REQUIRE(info.entries.size() == 2);

    // ─── FNV-1a-64 indexing ─────────────────────────────────────────────
    // Cooked entries land in the pak under their cooked filenames; the
    // packer normalizes to forward slashes and indexes by FNV-1a-64.
    const EntryRecord* lmt = find_by_path(info, "crate.lmt");
    const EntryRecord* lma = find_by_path(info, "tone_440hz.lma");
    REQUIRE(lmt != nullptr);
    REQUIRE(lma != nullptr);
    REQUIRE(lmt->path == "crate.lmt");
    REQUIRE(lma->path == "tone_440hz.lma");

    // Hash is recomputable & case-insensitive.
    REQUIRE(lmt->hash == fnv1a64("CRATE.LMT"));
    REQUIRE(lma->hash == fnv1a64("Tone_440Hz.LMA"));

    // Index ordered ascending by hash for binary search.
    for (usize i = 1; i < info.entries.size(); ++i) {
        REQUIRE(info.entries[i - 1].hash < info.entries[i].hash);
    }

    // ─── Payload round-trip ─────────────────────────────────────────────
    std::vector<u8> lmt_blob, lma_blob;
    REQUIRE(extract_entry(bytes, *lmt, lmt_blob, &err));
    REQUIRE(extract_entry(bytes, *lma, lma_blob, &err));

    REQUIRE(lmt_blob.size() >= 4);
    REQUIRE(read_u32_le(lmt_blob.data()) == kLmtMagicLE);

    REQUIRE(lma_blob.size() >= 4);
    REQUIRE(read_u32_le(lma_blob.data()) == kLmaMagicLE);

    // ─── Vfs::mount_pak round-trip (Wave-E unified format) ──────────────
    // The reader now accepts both the canonical LmpakWriter layout and the
    // tools/lm_pak dialect. Mounting the cooker's archive must succeed,
    // and both cooked entries must round-trip byte-for-byte through
    // Vfs::read.
    psynder::asset::internal::reset_for_tests();
    REQUIRE(asset::Vfs::Get().mount_pak(archive.string()));

    asset::Blob vfs_lmt = asset::Vfs::Get().read("crate.lmt");
    REQUIRE(vfs_lmt.data != nullptr);
    REQUIRE(vfs_lmt.bytes == lmt_blob.size());
    REQUIRE(std::memcmp(vfs_lmt.data, lmt_blob.data(), vfs_lmt.bytes) == 0);

    asset::Blob vfs_lma = asset::Vfs::Get().read("tone_440hz.lma");
    REQUIRE(vfs_lma.data != nullptr);
    REQUIRE(vfs_lma.bytes == lma_blob.size());
    REQUIRE(std::memcmp(vfs_lma.data, lma_blob.data(), vfs_lma.bytes) == 0);

    psynder::asset::internal::reset_for_tests();
}

// Wave-E closes the loop: emit an archive via the canonical LmpakWriter
// (lane 05) and read it back through Vfs::mount_pak + Vfs::read. The
// canonical dialect was the reader's native format pre-Wave-E; this case
// guards against regressions in the layout-detection branch when the new
// tools-dialect path is added alongside.
TEST_CASE("lm_fixture: LmpakWriter -> Vfs::mount_pak round-trip", "[tools][lm_pak][lmpak][vfs]") {
    namespace fs = std::filesystem;
    psynder::asset::internal::reset_for_tests();

    psynder::asset::lmpak::Writer w;
    const std::string greeting = "hello, wave-e\n";
    const std::vector<u8> note_bytes(greeting.begin(), greeting.end());
    w.add_raw("docs/readme.txt", std::vector<u8>(note_bytes.begin(), note_bytes.end()));
    const std::vector<u8> bin_bytes =
        {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
    w.add_raw("bin/blob.bin", std::vector<u8>(bin_bytes.begin(), bin_bytes.end()));

    fs::path tmp = fs::temp_directory_path() / "psynder_wave_e_canonical.lmpak";
    fs::remove(tmp);
    REQUIRE(w.write(tmp.string().c_str()));

    REQUIRE(asset::Vfs::Get().mount_pak(tmp.string()));
    asset::Blob a = asset::Vfs::Get().read("docs/readme.txt");
    REQUIRE(a.data != nullptr);
    REQUIRE(a.bytes == note_bytes.size());
    REQUIRE(std::memcmp(a.data, note_bytes.data(), a.bytes) == 0);

    asset::Blob b = asset::Vfs::Get().read("bin/blob.bin");
    REQUIRE(b.data != nullptr);
    REQUIRE(b.bytes == bin_bytes.size());
    REQUIRE(std::memcmp(b.data, bin_bytes.data(), b.bytes) == 0);

    psynder::asset::internal::reset_for_tests();
    fs::remove(tmp);
}

// Wave-E closes the loop on the OTHER side too: emit an archive via the
// tools/lm_pak producer (lane 24), then read it back through Vfs::mount_pak
// + Vfs::read. This is the actual bug Wave-E fixes — the two on-disk
// dialects (header bytes + index placement) used to be incompatible.
TEST_CASE("lm_fixture: tools::pack_blobs -> Vfs::mount_pak round-trip", "[tools][lm_pak][vfs]") {
    namespace fs = std::filesystem;
    psynder::asset::internal::reset_for_tests();

    std::vector<MemEntry> entries;
    {
        MemEntry e;
        e.path = "docs/notes.txt";
        const std::string body = "wave-e tools->vfs round-trip\n";
        e.data.assign(body.begin(), body.end());
        entries.push_back(std::move(e));
    }
    {
        MemEntry e;
        e.path = "art/blob.bin";
        e.data = {0xCA, 0xFE, 0xBA, 0xBE, 0x10, 0x20, 0x30, 0x40};
        entries.push_back(std::move(e));
    }

    std::vector<u8> bytes;
    auto packed = pack_blobs(entries, bytes);
    REQUIRE(packed.ok);
    REQUIRE(packed.entries == 2);

    fs::path tmp = fs::temp_directory_path() / "psynder_wave_e_tools.lmpak";
    fs::remove(tmp);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        REQUIRE(out);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out);
    }

    REQUIRE(asset::Vfs::Get().mount_pak(tmp.string()));

    asset::Blob a = asset::Vfs::Get().read("docs/notes.txt");
    REQUIRE(a.data != nullptr);
    REQUIRE(a.bytes == entries[0].data.size());
    REQUIRE(std::memcmp(a.data, entries[0].data.data(), a.bytes) == 0);

    asset::Blob b = asset::Vfs::Get().read("art/blob.bin");
    REQUIRE(b.data != nullptr);
    REQUIRE(b.bytes == entries[1].data.size());
    REQUIRE(std::memcmp(b.data, entries[1].data.data(), b.bytes) == 0);

    psynder::asset::internal::reset_for_tests();
    fs::remove(tmp);
}
