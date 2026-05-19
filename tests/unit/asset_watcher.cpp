// SPDX-License-Identifier: MIT
// Psynder — Lane 05 hot-reload watcher hardening: rename + delete + recreate.
//
// Wave A covered the happy path (mtime advance). Wave B adds three edge
// transitions the runtime needs for `.psyc` reloads:
//
//   1. File renamed away → watcher should fire so consumers drop their
//      cached parse and we don't keep serving stale bytes from a vpath
//      whose source moved.
//   2. File deleted outright → same as above; the watcher can't tell the
//      two apart without inotify, so they share a single edge.
//   3. File recreated after a delete → fire again so the consumer can
//      re-mount the new bytes.
//
// The poll-and-stat watcher can't observe events strictly atomically, so
// these tests use the in-process `poll_watchers_now()` hook to drive the
// state machine deterministically.

#include "asset/Vfs.h"
#include "asset/VfsInternal.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
using namespace psynder;
using psynder::asset::Vfs;

namespace {

fs::path make_scratch_dir(const char* tag) {
    static int counter = 0;
    fs::path base = fs::temp_directory_path() / "psynder_asset_test";
    fs::create_directories(base);
    fs::path d = base / (std::string(tag) + "_w_" + std::to_string(++counter));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_file(const fs::path& p, std::string_view bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct ResetGuard {
    ResetGuard()  { psynder::asset::internal::reset_for_tests(); }
    ~ResetGuard() { psynder::asset::internal::reset_for_tests(); }
};

struct WatchState {
    int         fires = 0;
    std::string last_path;
};

auto watch_cb = +[](std::string_view path, void* user) noexcept {
    auto* s = static_cast<WatchState*>(user);
    ++s->fires;
    s->last_path.assign(path);
};

}  // namespace

TEST_CASE("asset/watch: watcher fires on file rename away from vpath",
          "[asset][vfs][watch]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("rename_away");
    auto src = dir / "doc.txt";
    write_file(src, "original");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    WatchState st;
    Vfs::Get().watch("doc.txt", watch_cb, &st);
    REQUIRE(asset::internal::watcher_thread_running());

    // First poll: record baseline (file exists, no fire).
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires == 0);

    // Rename the source file out from under the vpath. The new name does
    // NOT match `doc.txt`, so the vpath now resolves to nothing.
    fs::rename(src, dir / "doc.txt.bak");
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires >= 1);
    REQUIRE(st.last_path == "doc.txt");

    // Subsequent polls without a change must NOT fire again — exactly
    // one edge per transition.
    int after_first = st.fires;
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires == after_first);
}

TEST_CASE("asset/watch: watcher fires on file delete",
          "[asset][vfs][watch]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("delete");
    auto file = dir / "config.json";
    write_file(file, R"({"a":1})");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    WatchState st;
    Vfs::Get().watch("config.json", watch_cb, &st);
    asset::internal::poll_watchers_now();  // baseline
    REQUIRE(st.fires == 0);

    fs::remove(file);
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires >= 1);
    REQUIRE(st.last_path == "config.json");

    int after_delete = st.fires;
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires == after_delete);  // idempotent
}

TEST_CASE("asset/watch: watcher fires when a deleted file is recreated",
          "[asset][vfs][watch]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("recreate");
    auto file = dir / "shader.txt";
    write_file(file, "v1");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    WatchState st;
    Vfs::Get().watch("shader.txt", watch_cb, &st);
    asset::internal::poll_watchers_now();  // baseline
    REQUIRE(st.fires == 0);

    fs::remove(file);
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires >= 1);
    int after_delete = st.fires;

    // Recreate at the same vpath. Watcher should re-baseline and fire.
    // Sleep 50 ms so mtime moves on filesystems with second-granularity
    // timestamps.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_file(file, "v2-after-recreation");
    asset::internal::poll_watchers_now();
    REQUIRE(st.fires > after_delete);
    REQUIRE(st.last_path == "shader.txt");
}

TEST_CASE("asset/watch: rename-into-place fires when a new file lands at the vpath",
          "[asset][vfs][watch]") {
    ResetGuard rg;
    auto dir = make_scratch_dir("rename_into");
    auto vpath_file = dir / "target.txt";
    auto staging   = dir / "target.txt.new";
    write_file(vpath_file, "old-bytes");
    REQUIRE(Vfs::Get().mount_directory(dir.string()));

    WatchState st;
    Vfs::Get().watch("target.txt", watch_cb, &st);
    asset::internal::poll_watchers_now();  // baseline (exists)
    REQUIRE(st.fires == 0);

    // Atomic-ish update pattern: write to staging, then rename over.
    // Many cookers use this so partial writes never expose a half file.
    write_file(staging, "fresh-bytes");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fs::rename(staging, vpath_file);

    asset::internal::poll_watchers_now();
    REQUIRE(st.fires >= 1);
    REQUIRE(st.last_path == "target.txt");
}
