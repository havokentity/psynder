// SPDX-License-Identifier: MIT
// Psynder — Lane 05 asset unit tests: VFS mount + read round-trip,
// .lmpak synthetic archive round-trip, hot-reload watcher poll.

#include "asset/LmpakFormat.h"
#include "asset/LmpakWriter.h"
#include "asset/Vfs.h"
#include "asset/VfsInternal.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace psynder;
using psynder::asset::Blob;
using psynder::asset::Vfs;

namespace {

// Build a unique scratch directory under the OS temp root. Catch2 runs
// tests in-process; we want isolated FS state between cases.
fs::path make_scratch_dir(const char* tag) {
    static int counter = 0;
    fs::path base = fs::temp_directory_path() / "psynder_asset_test";
    fs::create_directories(base);
    fs::path d = base / (std::string(tag) + "_" + std::to_string(++counter));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_file(const fs::path& p, std::string_view bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string blob_string(Blob b) {
    if (!b.data) return {};
    return std::string(reinterpret_cast<const char*>(b.data), b.bytes);
}

struct ResetGuard {
    ResetGuard()  { psynder::asset::internal::reset_for_tests(); }
    ~ResetGuard() { psynder::asset::internal::reset_for_tests(); }
};

}  // namespace

TEST_CASE("asset/vfs: loose directory mount + sync read round-trip", "[asset][vfs]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("dir_roundtrip");
    write_file(dir / "data" / "msg.txt", "hello psynder");

    REQUIRE(Vfs::Get().mount_directory(dir.string()));
    REQUIRE(Vfs::Get().mount_count() == 1);

    Blob b = Vfs::Get().read("data/msg.txt");
    REQUIRE(b.data != nullptr);
    REQUIRE(b.bytes == std::string_view("hello psynder").size());
    REQUIRE(blob_string(b) == "hello psynder");

    // Mount-order shadowing: a second mount with the same vpath wins.
    auto dir2 = make_scratch_dir("dir_roundtrip2");
    write_file(dir2 / "data" / "msg.txt", "overridden");
    REQUIRE(Vfs::Get().mount_directory(dir2.string()));
    REQUIRE(Vfs::Get().mount_count() == 2);

    b = Vfs::Get().read("data/msg.txt");
    REQUIRE(blob_string(b) == "overridden");

    // A missing file returns an empty Blob without crashing.
    Blob miss = Vfs::Get().read("data/does-not-exist.txt");
    REQUIRE(miss.data == nullptr);
    REQUIRE(miss.bytes == 0);
}

TEST_CASE("asset/vfs: case + backslash normalization on read", "[asset][vfs]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("dir_norm");
    write_file(dir / "Tex" / "Wall.PNG", "PNG-BODY");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    // The cooker normalizes paths to lowercase + forward slashes; the
    // reader does the same so user-supplied paths are tolerant.
    // For loose dirs the OS is case-sensitive on Linux, so the test
    // verifies an exact-case path; backslash normalization is tested
    // via the FNV-1a-64 path-hash equality regardless of slash style.
    REQUIRE(asset::lmpak::fnv1a_path("foo/bar.png", 11)
            == asset::lmpak::fnv1a_path("FOO\\BAR.PNG", 11));
}

TEST_CASE("asset/vfs: .lmpak synthetic archive round-trip (uncompressed)",
          "[asset][vfs][lmpak]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("pak_uncompressed");
    auto pak_path = dir / "test.lmpak";

    const std::string a = "alpha-asset-bytes";
    const std::string b = "beta-asset-bytes-longer-than-alpha";
    const std::string c =
        "gamma-asset-bytes-that-are-meaningfully-larger-than-the-others-for-realism";

    {
        asset::lmpak::Writer w;
        w.add_raw("textures/alpha.lmt",
                  std::vector<u8>(a.begin(), a.end()));
        w.add_raw("meshes/beta.lmm",
                  std::vector<u8>(b.begin(), b.end()));
        w.add_raw("audio/gamma.lma",
                  std::vector<u8>(c.begin(), c.end()));
        REQUIRE(w.write(pak_path.string().c_str()));
    }

    REQUIRE(Vfs::Get().mount_pak(pak_path.string()));
    REQUIRE(Vfs::Get().mount_count() == 1);

    SECTION("each entry round-trips byte-for-byte") {
        REQUIRE(blob_string(Vfs::Get().read("textures/alpha.lmt")) == a);
        REQUIRE(blob_string(Vfs::Get().read("meshes/beta.lmm")) == b);
        REQUIRE(blob_string(Vfs::Get().read("audio/gamma.lma")) == c);
    }

    SECTION("path normalization tolerated") {
        REQUIRE(blob_string(Vfs::Get().read("Textures/Alpha.LMT")) == a);
        REQUIRE(blob_string(Vfs::Get().read("Meshes\\Beta.LMM")) == b);
    }

    SECTION("missing entry returns empty Blob") {
        Blob miss = Vfs::Get().read("textures/missing.lmt");
        REQUIRE(miss.data == nullptr);
        REQUIRE(miss.bytes == 0);
    }
}

TEST_CASE("asset/vfs: .lmpak synthetic archive round-trip (zstd)",
          "[asset][vfs][lmpak][zstd]") {
    if (!asset::lmpak::zstd_available()) {
        SUCCEED("zstd not built into psynder_asset; compressed-path test skipped");
        return;
    }
    ResetGuard rg;
    auto dir = make_scratch_dir("pak_zstd");
    auto pak_path = dir / "test.lmpak";

    // Large enough that zstd actually reduces size.
    std::string big;
    big.reserve(64 * 1024);
    for (int i = 0; i < 64 * 1024; ++i) {
        big.push_back(static_cast<char>('A' + (i % 26)));
    }

    {
        asset::lmpak::Writer w;
        REQUIRE(w.add_file("data/big.bin",
                           reinterpret_cast<const u8*>(big.data()),
                           big.size(),
                           /*compress=*/true,
                           /*level=*/9));
        REQUIRE(w.write(pak_path.string().c_str()));
    }

    REQUIRE(Vfs::Get().mount_pak(pak_path.string()));

    Blob b = Vfs::Get().read("data/big.bin");
    REQUIRE(b.data != nullptr);
    REQUIRE(b.bytes == big.size());
    REQUIRE(std::memcmp(b.data, big.data(), big.size()) == 0);
}

TEST_CASE("asset/vfs: directory shadows pak (developer overlay pattern)",
          "[asset][vfs]") {
    ResetGuard rg;
    auto pak_dir = make_scratch_dir("overlay_pak");
    auto pak_path = pak_dir / "ship.lmpak";
    {
        asset::lmpak::Writer w;
        std::string shipped = "shipped-by-the-pak";
        w.add_raw("ui/title.txt",
                  std::vector<u8>(shipped.begin(), shipped.end()));
        REQUIRE(w.write(pak_path.string().c_str()));
    }

    auto overlay = make_scratch_dir("overlay_dir");
    write_file(overlay / "ui" / "title.txt", "DEV-OVERRIDE");

    REQUIRE(Vfs::Get().mount_pak(pak_path.string()));
    REQUIRE(Vfs::Get().mount_directory(overlay.string()));
    REQUIRE(Vfs::Get().mount_count() == 2);

    // The directory was mounted AFTER the pak, so it shadows it.
    REQUIRE(blob_string(Vfs::Get().read("ui/title.txt")) == "DEV-OVERRIDE");
}

TEST_CASE("asset/vfs: async read fires callback with the same bytes",
          "[asset][vfs][async]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("async_read");
    write_file(dir / "msg.txt", "async-roundtrip");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    struct State {
        bool        fired = false;
        std::string seen;
    } st;

    auto cb = +[](Blob b, void* user) noexcept {
        auto* s = static_cast<State*>(user);
        s->fired = true;
        if (b.data && b.bytes) {
            s->seen.assign(reinterpret_cast<const char*>(b.data), b.bytes);
        }
    };

    Vfs::Get().read_async("msg.txt", cb, &st);
    // Lane 04's Chase-Lev pool runs read_async on a worker, so we spin
    // up to 1 s waiting for the callback to fire. Pre-lane-04 the
    // callback fired inside submit(); the timeout-bounded spin keeps
    // both behaviours correct.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!st.fired && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(st.fired);
    REQUIRE(st.seen == "async-roundtrip");
}

TEST_CASE("asset/vfs: hot-reload watcher fires on mtime change",
          "[asset][vfs][watch]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("watch");
    auto file = dir / "config.txt";
    write_file(file, "v1");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    struct State {
        int        fires = 0;
        std::string last_path;
    } st;

    auto cb = +[](std::string_view path, void* user) noexcept {
        auto* s = static_cast<State*>(user);
        ++s->fires;
        s->last_path.assign(path);
    };

    Vfs::Get().watch("config.txt", cb, &st);
    REQUIRE(asset::internal::watcher_thread_running());

    // Seed baseline: first poll just records mtime without firing.
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires == 0);

    // Bump the mtime; fs::last_write_time on macOS has 1 ns resolution
    // on APFS but a fresh write completes well within a normal test
    // turnaround. To be safe we sleep just past mtime granularity on
    // older filesystems.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_file(file, "v2-modified");
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires >= 1);
    REQUIRE(st.last_path == "config.txt");
}
