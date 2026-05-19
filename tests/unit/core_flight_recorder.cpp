// SPDX-License-Identifier: MIT
// Unit tests for the allocation flight recorder (Wave B).
//
// The recorder is a circular ring of the last N alloc/free entries. We
// cover:
//   - init+record+count: sane bookkeeping for the recorder counters.
//   - dump writes a file with the expected number of records.
//   - call_site_hash is stable across calls and varies with file/line.
//   - When the ring wraps, dump still emits the most-recent `capacity`
//     records.
//   - LinearArena::alloc participates in the recorder.

#include "core/alloc/Allocator.h"
#include "core/alloc/FlightRecorder.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace mem = psynder::mem;
using psynder::u32;
using psynder::usize;

namespace {

// Build a unique tmp path so parallel ctest runs don't collide.
std::string make_tmp_path(const char* tag) {
    const char* env = std::getenv("TMPDIR");
    std::string base = (env && *env) ? env : "/tmp";
    if (!base.empty() && base.back() != '/') base.push_back('/');
    base += "psynder_flight_";
    base += tag;
    base += "_";
    base += std::to_string(static_cast<long long>(std::rand()));
    base += ".csv";
    return base;
}

usize count_data_lines(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    usize n = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') continue;
        ++n;
    }
    return n;
}

}  // namespace

TEST_CASE("call_site_hash is deterministic and varies with line",
          "[core][alloc][flightrec]") {
    constexpr u32 a = mem::call_site_hash("/foo/bar.cpp", 42);
    constexpr u32 b = mem::call_site_hash("/foo/bar.cpp", 42);
    constexpr u32 c = mem::call_site_hash("/foo/bar.cpp", 43);
    constexpr u32 d = mem::call_site_hash("/baz/qux.cpp", 42);
    REQUIRE(a == b);
    REQUIRE(a != c);
    REQUIRE(a != d);
}

TEST_CASE("flight recorder records and dumps", "[core][alloc][flightrec]") {
    mem::flight_recorder_init(256);
    REQUIRE(mem::flight_recorder_capacity() == 256);

    mem::flight_recorder_clear();
    REQUIRE(mem::flight_recorder_count() == 0);

    constexpr usize kRecords = 32;
    for (usize i = 0; i < kRecords; ++i) {
        mem::flight_recorder_record(
            /*site=*/static_cast<u32>(0x10000 + i),
            /*op=*/(i % 2 == 0) ? +1 : -1,
            /*size=*/static_cast<u32>(64 + i),
            /*tag=*/mem::Tag::Render);
    }
    REQUIRE(mem::flight_recorder_count() == kRecords);

    const std::string path = make_tmp_path("records");
    const usize written = mem::flight_recorder_dump(path.c_str());
    REQUIRE(written == kRecords);
    REQUIRE(count_data_lines(path) == kRecords);
    std::remove(path.c_str());
}

TEST_CASE("flight recorder ring wraps and dumps only the last `capacity`",
          "[core][alloc][flightrec]") {
    constexpr usize kCap = 16;
    mem::flight_recorder_init(kCap);
    mem::flight_recorder_clear();

    // Push more than capacity so the ring wraps several times.
    constexpr usize kPush = kCap * 3 + 5;
    for (usize i = 0; i < kPush; ++i) {
        mem::flight_recorder_record(
            /*site=*/static_cast<u32>(0xBEEF0000 + i),
            /*op=*/+1,
            /*size=*/static_cast<u32>(i),
            /*tag=*/mem::Tag::Physics);
    }
    REQUIRE(mem::flight_recorder_count() == kPush);

    const std::string path = make_tmp_path("wrap");
    const usize written = mem::flight_recorder_dump(path.c_str());
    REQUIRE(written == kCap);   // exactly capacity records survive
    REQUIRE(count_data_lines(path) == kCap);
    std::remove(path.c_str());
}

TEST_CASE("flight recorder dump returns 0 for a bad path",
          "[core][alloc][flightrec]") {
    mem::flight_recorder_init(64);
    mem::flight_recorder_clear();
    mem::flight_recorder_record(1, +1, 8, mem::Tag::Misc);
    // A directory that does not exist must fail open() cleanly.
    REQUIRE(mem::flight_recorder_dump(
        "/this/path/does/not/exist/psynder_flight.csv") == 0);
    REQUIRE(mem::flight_recorder_dump(nullptr) == 0);
}

TEST_CASE("LinearArena::alloc registers into the flight recorder",
          "[core][alloc][flightrec]") {
    mem::flight_recorder_init(128);
    mem::flight_recorder_clear();

    alignas(64) std::uint8_t buf[1024];
    mem::LinearArena a(buf, sizeof buf, mem::Tag::Audio);
    REQUIRE(a.alloc(64, 16) != nullptr);
    REQUIRE(a.alloc(128, 16) != nullptr);

    const auto count = mem::flight_recorder_count();
    REQUIRE(count >= 2);   // each successful alloc adds one entry

    const std::string path = make_tmp_path("arena");
    const usize written = mem::flight_recorder_dump(path.c_str());
    REQUIRE(written >= 2);
    REQUIRE(count_data_lines(path) >= 2);
    std::remove(path.c_str());
}
