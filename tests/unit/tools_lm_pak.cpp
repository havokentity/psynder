// SPDX-License-Identifier: MIT
// Lane 24 tests — lm_pak archive round-trip.

#include <catch2/catch_test_macros.hpp>

#include "../../tools/lm_pak/Lmpak.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace psynder;
using namespace psynder::tools::lmpak;

namespace {

MemEntry make_entry(std::string path, std::string body) {
    MemEntry e;
    e.path = std::move(path);
    e.data.assign(body.begin(), body.end());
    return e;
}

}  // anon

TEST_CASE("lm_pak FNV-1a64 is case-folded", "[tools][lm_pak]") {
    REQUIRE(fnv1a64("foo/Bar.PNG") == fnv1a64("FOO/bar.png"));
    REQUIRE(fnv1a64("path/a") != fnv1a64("path/b"));
}

TEST_CASE("lm_pak round-trips a small set of blobs", "[tools][lm_pak]") {
    std::vector<MemEntry> entries;
    entries.push_back(make_entry("levels/test.psybsp",
                                 "PSBP\x01\x00\x00\x00..."));
    entries.push_back(make_entry("textures/wall.lmt",
                                 std::string(1024, '\xAB')));
    entries.push_back(make_entry("audio/clip.lma",
                                 std::string("LMA1 sample audio data")));
    entries.push_back(make_entry("meshes/cube.lmm",
                                 std::string("LMM1 placeholder mesh body")));

    std::vector<u8> bytes;
    PackResult pr = pack_blobs(entries, bytes);
    REQUIRE(pr.ok);
    REQUIRE(pr.entries == entries.size());
    REQUIRE_FALSE(bytes.empty());

    ArchiveInfo info;
    std::string err;
    REQUIRE(read_index(bytes, info, &err));
    REQUIRE(info.version == kVersion);
    REQUIRE(info.entry_count == entries.size());
    REQUIRE(info.entries.size() == entries.size());

    // Entries sorted by hash; we can still locate each.
    for (const auto& in : entries) {
        u64 h = fnv1a64(in.path);
        auto it = std::find_if(info.entries.begin(), info.entries.end(),
                               [&](const EntryRecord& r) { return r.hash == h; });
        REQUIRE(it != info.entries.end());
        REQUIRE(it->path == in.path);
        REQUIRE(it->raw == in.data.size());

        std::vector<u8> raw;
        std::string sub_err;
        REQUIRE(extract_entry(bytes, *it, raw, &sub_err));
        REQUIRE(raw.size() == in.data.size());
        REQUIRE(std::memcmp(raw.data(), in.data.data(), raw.size()) == 0);
    }
}

TEST_CASE("lm_pak rejects garbage input", "[tools][lm_pak]") {
    std::vector<u8> garbage{0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0};
    ArchiveInfo info;
    std::string err;
    REQUIRE_FALSE(read_index(garbage, info, &err));
    REQUIRE_FALSE(err.empty());
}

TEST_CASE("lm_pak CLI cli_main prints help cleanly", "[tools][lm_pak]") {
    const char* argv[] = {"lm_pak", "--help"};
    int rc = cli_main(2, const_cast<char**>(argv));
    REQUIRE(rc == 0);
}
