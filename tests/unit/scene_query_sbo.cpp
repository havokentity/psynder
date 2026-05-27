// SPDX-License-Identifier: MIT
// Psynder — lane-06 unit tests: query work-list small-buffer-optimization.
//
// QueryBuilder::run no longer allocates a std::vector for its flattened
// (archetype, chunk) work-list on every multi-chunk query. It builds that list
// in an on-stack SBO array (kWorkSbo == 64 chunks) with a C-heap fallback only
// when the chunk count overflows. The `required` component-id set likewise
// moved to a compile-time-bounded std::array. These tests prove:
//   1. The HEAP-FALLBACK work-list path (> 64 non-empty chunks) is correct.
//   2. A query issued from INSIDE another query's body (re-entrant) does not
//      corrupt either call's work-list — each call owns a private stack frame.
// Run under ASan/UBSan via the mac-debug preset; a heap overflow / clobber /
// use-after-free in the SBO logic would trip the sanitizer.

#include <catch2/catch_test_macros.hpp>

#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"

#include <atomic>
#include <span>
#include <vector>

namespace lane06_query_sbo {

PSYNDER_COMPONENT(QPos) {
    psynder::f32 x = 0, y = 0, z = 0;
};
PSYNDER_COMPONENT(QVel) {
    psynder::f32 dx = 0, dy = 0, dz = 0;
};

// Mirror scene_query.cpp: the EcsRegistry is a process-global singleton and
// Catch2 randomizes case order, so fully clear it around every case for
// isolation.
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

}  // namespace lane06_query_sbo

using namespace psynder;
using namespace psynder::scene;
using lane06_query_sbo::QPos;
using lane06_query_sbo::QVel;

TEST_CASE("scene: query work-list spills to heap past the SBO threshold",
          "[scene][query][sbo]") {
    lane06_query_sbo::RegistryReset reset;
    auto& w = EcsRegistry::Get();
    w.set_structural_deferred(false);

    // Each (QPos, QVel) row is 24 bytes, so a chunk holds ~256 rows. To force
    // the work-list well past the 64-chunk SBO inline capacity we need a great
    // many chunks; 40,000 entities yields > 150 chunks, exercising the
    // std::malloc fallback + its capacity doubling.
    constexpr int kCount = 40'000;
    w.reserve_archetype<QPos, QVel>(kCount);

    std::vector<Entity> es;
    es.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        Entity e = w.create();
        w.add<QPos>(e, QPos{static_cast<f32>(i % 1000), 0.0f, 0.0f});
        w.add<QVel>(e, QVel{1.0f, 0.0f, 0.0f});
        es.push_back(e);
    }

    std::atomic<u32> rows_seen{0};
    std::atomic<bool> sizes_match{true};
    w.query<reads<QPos>, writes<QVel>>(
        [&](std::span<const QPos> pos, std::span<QVel> vel) {
            if (pos.size() != vel.size())
                sizes_match.store(false, std::memory_order_relaxed);
            for (usize i = 0; i < pos.size(); ++i)
                vel[i].dz = pos[i].x;
            rows_seen.fetch_add(static_cast<u32>(pos.size()), std::memory_order_relaxed);
        });

    REQUIRE(sizes_match.load());
    REQUIRE(rows_seen.load() == static_cast<u32>(kCount));

    for (auto e : es)
        w.destroy(e);
}

TEST_CASE("scene: query issued from inside a query body is re-entrancy-safe",
          "[scene][query][sbo][reentrant]") {
    lane06_query_sbo::RegistryReset reset;
    auto& w = EcsRegistry::Get();
    w.set_structural_deferred(false);

    // Enough entities to span multiple chunks both for the outer and inner
    // query so the work-lists are non-trivial. The inner query reads the same
    // archetype; if the inner call clobbered the outer call's SBO work-list,
    // the outer row count would be wrong or ASan would fire.
    constexpr int kCount = 1200;
    w.reserve_archetype<QPos, QVel>(kCount);

    std::vector<Entity> es;
    es.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        Entity e = w.create();
        w.add<QPos>(e, QPos{static_cast<f32>(i), 0.0f, 0.0f});
        w.add<QVel>(e, QVel{0.0f, 0.0f, 0.0f});
        es.push_back(e);
    }

    std::atomic<u32> outer_rows{0};
    // Query bodies run on worker threads and Catch2's REQUIRE is not
    // thread-safe, so the nested-query result is tallied into atomics and
    // asserted on the main thread after the outer query returns. `outer_bodies`
    // counts how many times the outer body ran; `inner_correct` counts how many
    // of those nested queries saw the full entity set.
    std::atomic<u32> outer_bodies{0};
    std::atomic<u32> inner_correct{0};

    w.query<reads<QPos>, writes<QVel>>([&](std::span<const QPos> opos, std::span<QVel>) {
        outer_rows.fetch_add(static_cast<u32>(opos.size()), std::memory_order_relaxed);
        outer_bodies.fetch_add(1, std::memory_order_relaxed);
        // Nested query from inside the outer query's body.
        std::atomic<u32> inner_rows{0};
        w.query<reads<QPos>, writes<QVel>>([&](std::span<const QPos> ip, std::span<QVel>) {
            inner_rows.fetch_add(static_cast<u32>(ip.size()), std::memory_order_relaxed);
        });
        // Every nested query must see the full entity set.
        if (inner_rows.load() == static_cast<u32>(kCount))
            inner_correct.fetch_add(1, std::memory_order_relaxed);
    });

    REQUIRE(outer_rows.load() == static_cast<u32>(kCount));
    // Every outer body invocation must have run a correct nested query.
    REQUIRE(inner_correct.load() == outer_bodies.load());
    REQUIRE(outer_bodies.load() > 0);

    for (auto e : es)
        w.destroy(e);
}
