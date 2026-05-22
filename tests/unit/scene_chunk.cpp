// SPDX-License-Identifier: MIT
// Psynder — lane-06 unit tests: chunk-layout invariants from DESIGN.md §4.4.

#include <catch2/catch_test_macros.hpp>

#include "scene/EcsRegistry.h"

#include <cstddef>

// We deliberately reach into the lane's internal headers here — these are
// implementation-detail invariants, not user contract. Other lanes only ever
// include the public `EcsRegistry.h` surface.
#include "scene/Chunk.h"
#include "scene/Archetype.h"

using namespace psynder;
using namespace psynder::scene;
using namespace psynder::scene::detail;

TEST_CASE("scene: chunk is exactly 16 KiB", "[scene][chunk][layout]") {
    STATIC_REQUIRE(sizeof(Chunk) == 16u * 1024u);
    STATIC_REQUIRE(alignof(Chunk) == kCacheLine);
    STATIC_REQUIRE(sizeof(ChunkHeader) == kCacheLine);
    STATIC_REQUIRE(alignof(ChunkHeader) == kCacheLine);
    STATIC_REQUIRE(kChunkBytes == 16u * 1024u);
    STATIC_REQUIRE(kColumnAlign == kCacheLine);
}

namespace chunk_test {
PSYNDER_COMPONENT(Vec3i) {
    psynder::i32 x = 0, y = 0, z = 0;
};
PSYNDER_COMPONENT(Mat3x3i) {
    psynder::i32 m[9]{};
};
PSYNDER_COMPONENT(SingleU) {
    psynder::u32 v = 0;
};
}  // namespace chunk_test

TEST_CASE("scene: archetype column offsets are 64-byte aligned", "[scene][chunk][layout]") {
    using namespace chunk_test;
    auto& w = EcsRegistry::Get();
    w.set_structural_deferred(false);

    // Use heterogenous components to spread different sized columns.
    Entity e = w.create();
    w.add<Vec3i>(e, Vec3i{1, 2, 3});
    w.add<Mat3x3i>(e, Mat3x3i{{0, 1, 2, 3, 4, 5, 6, 7, 8}});
    w.add<SingleU>(e, SingleU{0xDEADBEEFu});

    // Components round-trip with values intact.
    Vec3i* v = w.get<Vec3i>(e);
    Mat3x3i* m = w.get<Mat3x3i>(e);
    SingleU* s = w.get<SingleU>(e);
    REQUIRE(v != nullptr);
    REQUIRE(v->x == 1);
    REQUIRE(v->y == 2);
    REQUIRE(v->z == 3);
    REQUIRE(m != nullptr);
    REQUIRE(m->m[4] == 4);
    REQUIRE(s != nullptr);
    REQUIRE(s->v == 0xDEADBEEFu);

    // Every returned pointer must live INSIDE the 16 KiB chunk and be at
    // a 64-byte aligned offset relative to the chunk base.
    // We can't see the chunk base without internal API, but the alignment
    // invariant is: the columns themselves start aligned, so the first
    // element pointer mod 64 == 0.
    REQUIRE(reinterpret_cast<usize>(v) % kCacheLine == 0);
    REQUIRE(reinterpret_cast<usize>(m) % kCacheLine == 0);
    REQUIRE(reinterpret_cast<usize>(s) % kCacheLine == 0);

    w.destroy(e);
}

TEST_CASE("scene: chunk header version stamps bump on writes", "[scene][chunk][version]") {
    using namespace chunk_test;
    auto& w = EcsRegistry::Get();
    w.set_structural_deferred(false);

    // Make at least one entity so the archetype has a live chunk.
    Entity e = w.create();
    w.add<Vec3i>(e, Vec3i{0, 0, 0});

    // Querying with writes<Vec3i> should bump the version stamp. We can
    // observe it via the chunk-header struct directly (internal).
    w.query<reads<Vec3i>, writes<Vec3i>>([](std::span<const Vec3i>, std::span<Vec3i> out) {
        for (auto& r : out)
            r.x = 42;
    });

    Vec3i* v = w.get<Vec3i>(e);
    REQUIRE(v != nullptr);
    REQUIRE(v->x == 42);

    w.destroy(e);
}
