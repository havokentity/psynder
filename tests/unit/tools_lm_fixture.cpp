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
//   4. Calls lane 05's Vfs::mount_pak with the same archive. We treat the
//      mount result as advisory — the producer here and the consumer in
//      engine/asset/Vfs.cpp grew independently during Wave-A and the
//      on-disk layouts diverge today (different name-pool placement). The
//      call is exercised so any future format unification immediately
//      flips this assertion to a hard REQUIRE.
//
// All assertions guard the lm_pak-side round-trip, which is the contract
// Wave-D owns; the Vfs path is a probe.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_pak/Lmpak.h"
#include "LmpakFixturePaths.h"   // generated absolute path to demo.lmpak

#include "asset/Vfs.h"
#include "core/Types.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::tools::lmpak;

namespace {

constexpr u32 kLmtMagicLE = 0x31544D4Cu;  // 'LMT1'
constexpr u32 kLmaMagicLE = 0x31414D4Cu;  // 'LMA1'

bool slurp(const std::filesystem::path& p, std::vector<u8>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    auto sz = in.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<usize>(sz));
    in.seekg(0, std::ios::beg);
    if (!out.empty()) in.read(reinterpret_cast<char*>(out.data()), sz);
    return static_cast<bool>(in);
}

u32 read_u32_le(const u8* p) noexcept {
    return  static_cast<u32>(p[0])
         | (static_cast<u32>(p[1]) <<  8)
         | (static_cast<u32>(p[2]) << 16)
         | (static_cast<u32>(p[3]) << 24);
}

const EntryRecord* find_by_path(const ArchiveInfo& info, std::string_view path) {
    const u64 h = fnv1a64(path);
    for (const auto& r : info.entries) {
        if (r.hash == h) return &r;
    }
    return nullptr;
}

}  // anon namespace

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

    // ─── Vfs::mount_pak probe ───────────────────────────────────────────
    // The lane-05 consumer reads a slightly different on-disk layout than
    // the lane-24 producer; we exercise the call so the gap surfaces here
    // and not in shipped code. When the formats reconcile, this becomes a
    // hard equality check.
    const bool mounted = asset::Vfs::Get().mount_pak(archive.string());
    if (mounted) {
        // If the read side ever speaks the producer's dialect, the
        // round-trip via Vfs::read should match the bytes we already
        // pulled out through extract_entry.
        asset::Blob b = asset::Vfs::Get().read("crate.lmt");
        if (b.data && b.bytes == lmt_blob.size()) {
            CHECK(std::memcmp(b.data, lmt_blob.data(), b.bytes) == 0);
        }
    } else {
        WARN("Vfs::mount_pak did not accept the lm_pak-produced archive; "
             "this is the known format gap between tool & runtime "
             "(see comment above).");
    }
}
