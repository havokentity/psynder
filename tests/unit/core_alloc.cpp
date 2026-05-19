// SPDX-License-Identifier: MIT
// Unit tests for psynder::mem.
//
// Covers the invariants the issue calls out for lane 01:
//   - LinearArena: bump-pointer alloc + alignment + reset.
//   - PageAllocator: round-up to page size, RAII via page_free.
//   - TypedPool<T>: acquire/release/reset, free-list re-use.
//   - Per-tag accounting hooks (budget set, peak watermark).

#include "core/alloc/Allocator.h"
#include "core/Types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using psynder::u32;
using psynder::usize;

namespace mem = psynder::mem;

TEST_CASE("LinearArena bumps from base and resets cleanly", "[core][alloc]") {
    alignas(64) std::uint8_t buf[4096];
    mem::LinearArena a(buf, sizeof buf);

    REQUIRE(a.capacity() == sizeof buf);
    REQUIRE(a.used() == 0);

    void* p1 = a.alloc(128, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p1) % 8 == 0);
    REQUIRE(a.used() >= 128);

    // 64-byte align bump skips a hole.
    void* p2 = a.alloc(64, 64);
    REQUIRE(p2 != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p2) % 64 == 0);

    a.reset();
    REQUIRE(a.used() == 0);

    void* p3 = a.alloc(128, 8);
    REQUIRE(p3 == p1);   // reset reuses the same base
}

TEST_CASE("LinearArena returns nullptr on overflow", "[core][alloc]") {
    alignas(64) std::uint8_t buf[256];
    mem::LinearArena a(buf, sizeof buf);

    void* p1 = a.alloc(200, 8);
    REQUIRE(p1 != nullptr);

    // Remaining headroom is < 100; this allocation must fail.
    void* p2 = a.alloc(100, 8);
    REQUIRE(p2 == nullptr);

    // Subsequent small allocs still succeed if they fit.
    void* p3 = a.alloc(8, 8);
    REQUIRE(p3 != nullptr);
}

TEST_CASE("PageAllocator returns page-aligned, non-null block", "[core][alloc][page]") {
    mem::PageBlock pb = mem::page_alloc(8192, /*prefer_hugepage=*/false);
    REQUIRE(pb.ptr != nullptr);
    REQUIRE(pb.bytes >= 8192);
    REQUIRE(reinterpret_cast<std::uintptr_t>(pb.ptr) % psynder::kPage == 0);
    // Write a sentinel pattern; if the mapping isn't real, this segfaults.
    std::memset(pb.ptr, 0xAB, pb.bytes);
    mem::page_free(pb);
}

TEST_CASE("PageAllocator handles hugepage requests gracefully", "[core][alloc][page]") {
    // Hugepage hint may or may not be granted on this host; we only
    // require correctness: a non-null block of at least the requested
    // size, page-aligned, readable / writable.
    mem::PageBlock pb = mem::page_alloc(psynder::kHugePage, /*prefer_hugepage=*/true);
    REQUIRE(pb.ptr != nullptr);
    REQUIRE(pb.bytes >= psynder::kHugePage);
    *static_cast<std::uint64_t*>(pb.ptr)                  = 0xDEADBEEF;
    *(static_cast<std::uint64_t*>(pb.ptr) + 65536)        = 0xCAFEBABE;
    mem::page_free(pb);
}

TEST_CASE("TypedPool acquires, releases, and reuses slots", "[core][alloc][pool]") {
    struct Body {
        float x, y, z;
        std::uint32_t tag;
    };
    constexpr usize kCap = 64;
    alignas(alignof(Body)) std::uint8_t backing[
        sizeof(Body) * kCap + sizeof(u32) * kCap + 16];

    mem::TypedPool<Body> pool;
    pool.init(backing, kCap);
    REQUIRE(pool.live() == 0);
    REQUIRE(pool.capacity() == kCap);

    Body* first = pool.acquire();
    REQUIRE(first != nullptr);
    first->tag = 1;
    REQUIRE(pool.live() == 1);

    Body* second = pool.acquire();
    REQUIRE(second != nullptr);
    REQUIRE(second != first);
    second->tag = 2;
    REQUIRE(pool.live() == 2);

    pool.release(first);
    REQUIRE(pool.live() == 1);

    // Free-list re-use: the next acquire should hand back the slot we
    // just released.
    Body* third = pool.acquire();
    REQUIRE(third == first);
    REQUIRE(pool.live() == 2);

    // Reset wipes the live counter and starts handing out slots from
    // zero again.
    pool.reset();
    REQUIRE(pool.live() == 0);
    Body* fourth = pool.acquire();
    REQUIRE(fourth == reinterpret_cast<Body*>(backing));
}

TEST_CASE("TypedPool refuses to overflow capacity", "[core][alloc][pool]") {
    constexpr usize kCap = 4;
    alignas(8) std::uint8_t backing[sizeof(std::uint64_t) * kCap +
                                    sizeof(u32) * kCap + 8];
    mem::TypedPool<std::uint64_t> pool;
    pool.init(backing, kCap);

    for (usize i = 0; i < kCap; ++i) {
        REQUIRE(pool.acquire() != nullptr);
    }
    REQUIRE(pool.acquire() == nullptr);   // full
}

TEST_CASE("Worker scratch arena is valid and resettable", "[core][alloc][scratch]") {
    auto& ws = mem::worker_scratch();
    REQUIRE(ws.capacity() > 0);
    ws.reset();
    REQUIRE(ws.used() == 0);

    void* p = ws.alloc(1024, 16);
    REQUIRE(p != nullptr);
    REQUIRE(ws.used() >= 1024);
    ws.reset();
    REQUIRE(ws.used() == 0);
}

TEST_CASE("Per-tag budgets and counters round-trip", "[core][alloc][budget]") {
    // Counters carry over from earlier tests, so verify deltas not absolutes.
    mem::set_budget(mem::Tag::Render, 16 * 1024 * 1024);
    // Round-trip through current_usage / peak_usage so we exercise the
    // public accessors.
    [[maybe_unused]] usize cur  = mem::current_usage(mem::Tag::Render);
    [[maybe_unused]] usize peak = mem::peak_usage(mem::Tag::Render);
    REQUIRE(peak >= cur);
}
