// SPDX-License-Identifier: MIT
// Psynder — lane-06 unit tests: query iteration over many entities.
// The brief calls for "query iteration over a few hundred entities"; we
// exercise 600 entities split across two archetypes.

#include <catch2/catch_test_macros.hpp>

#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"

#include <atomic>
#include <span>
#include <vector>

namespace lane06_query {

PSYNDER_COMPONENT(Pos) {
    psynder::f32 x = 0, y = 0, z = 0;
};
PSYNDER_COMPONENT(Vel) {
    psynder::f32 dx = 0, dy = 0, dz = 0;
};
PSYNDER_COMPONENT(Tag) {
    psynder::u32 group = 0;
};

// EcsRegistry::Get() is a process-global singleton; Catch2 runs test cases in a
// randomized order, so cases that create Pos/Vel entities here would otherwise
// leak rows into a later case's counts (e.g. the deferred case's pre_count==0).
// Fully clear the registry at the start of each case so every case is isolated
// regardless of order (mirrors the RegistryReset guard in editor_play_runtime).
struct RegistryReset {
    RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { psynder::scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

}  // namespace lane06_query

using namespace psynder;
using namespace psynder::scene;
using lane06_query::Pos;
using lane06_query::Tag;
using lane06_query::Vel;

TEST_CASE("scene: query iterates every entity matching the components", "[scene][query]") {
    lane06_query::RegistryReset reset;
    auto& w = EcsRegistry::Get();
    w.set_structural_deferred(false);

    // Build two archetypes: (Pos, Vel) and (Pos, Vel, Tag). Both match a
    // query asking for `reads<Pos>, writes<Vel>`. The total of 600
    // entities exercises the chunk-spanning iteration path: with a row
    // footprint of 24 bytes per archetype-alpha entity, each chunk holds
    // roughly 256 rows, so 300/400 entities span 2 chunks per archetype.
    constexpr int kAlpha = 300;  // Pos + Vel
    constexpr int kBeta = 400;   // Pos + Vel + Tag

    std::vector<Entity> alpha;
    alpha.reserve(kAlpha);
    for (int i = 0; i < kAlpha; ++i) {
        Entity e = w.create();
        w.add<Pos>(e, Pos{static_cast<f32>(i), 0.0f, 0.0f});
        w.add<Vel>(e, Vel{1.0f, 0.0f, 0.0f});
        alpha.push_back(e);
    }

    std::vector<Entity> beta;
    beta.reserve(kBeta);
    for (int i = 0; i < kBeta; ++i) {
        Entity e = w.create();
        w.add<Pos>(e, Pos{0.0f, static_cast<f32>(i), 0.0f});
        w.add<Vel>(e, Vel{0.0f, 2.0f, 0.0f});
        w.add<Tag>(e, Tag{static_cast<u32>(i)});
        beta.push_back(e);
    }

    // Integrate motion over one step. The lambda receives one span<const Pos>
    // and one span<Vel> per chunk — i.e. column-at-a-time iteration.
    // query() fires the body once per chunk ACROSS WORKER THREADS, so any
    // accumulation the body does must be synchronized (atomics here).
    std::atomic<u32> rows_seen{0};
    std::atomic<bool> sizes_match{true};
    w.query<reads<Pos>, writes<Vel>>(
        [&rows_seen, &sizes_match](std::span<const Pos> pos, std::span<Vel> vel) {
            if (pos.size() != vel.size())
                sizes_match.store(false, std::memory_order_relaxed);
            for (usize i = 0; i < pos.size(); ++i) {
                vel[i].dz = pos[i].x + pos[i].y;
            }
            rows_seen.fetch_add(static_cast<u32>(pos.size()), std::memory_order_relaxed);
        });
    REQUIRE(sizes_match.load());

    REQUIRE(rows_seen.load() == kAlpha + kBeta);

    // Verify the write landed for both archetypes. We aggregate the
    // pass-counts into a single REQUIRE rather than asserting per-entity —
    // Catch2 3.7.1's default reporter is fragile when fed thousands of
    // captures within one TEST_CASE on macOS arm64 (observed SIGBUS in
    // teardown when stdout is piped).
    u32 alpha_correct = 0;
    for (usize i = 0; i < static_cast<usize>(kAlpha); ++i) {
        const Vel* v = w.get<Vel>(alpha[i]);
        if (v && v->dz == static_cast<f32>(i))
            ++alpha_correct;
    }
    u32 beta_correct = 0;
    for (usize i = 0; i < static_cast<usize>(kBeta); ++i) {
        const Vel* v = w.get<Vel>(beta[i]);
        if (v && v->dz == static_cast<f32>(i))
            ++beta_correct;
    }
    REQUIRE(alpha_correct == static_cast<u32>(kAlpha));
    REQUIRE(beta_correct == static_cast<u32>(kBeta));

    // Clean up.
    for (auto e : alpha)
        w.destroy(e);
    for (auto e : beta)
        w.destroy(e);
}

TEST_CASE("scene: query with extra required components filters out other archetypes",
          "[scene][query]") {
    lane06_query::RegistryReset reset;
    auto& w = EcsRegistry::Get();
    w.set_structural_deferred(false);

    constexpr int kAlpha = 64;  // Pos only — no Vel, won't match
    constexpr int kBeta = 96;   // Pos + Vel — matches

    std::vector<Entity> alpha, beta;
    for (int i = 0; i < kAlpha; ++i) {
        Entity e = w.create();
        w.add<Pos>(e, Pos{1, 1, 1});
        alpha.push_back(e);
    }
    for (int i = 0; i < kBeta; ++i) {
        Entity e = w.create();
        w.add<Pos>(e, Pos{2, 2, 2});
        w.add<Vel>(e, Vel{3, 3, 3});
        beta.push_back(e);
    }

    std::atomic<u32> rows{0};
    std::atomic<bool> all_beta{true};
    w.query<reads<Pos>, writes<Vel>>([&rows, &all_beta](std::span<const Pos> p, std::span<Vel> v) {
        for (usize i = 0; i < p.size(); ++i) {
            if (p[i].x != 2.0f)
                all_beta.store(false, std::memory_order_relaxed);  // only beta archetype
            v[i].dx = -1.0f;
        }
        rows.fetch_add(static_cast<u32>(p.size()), std::memory_order_relaxed);
    });
    REQUIRE(all_beta.load());
    REQUIRE(rows.load() == kBeta);

    for (auto e : alpha)
        w.destroy(e);
    for (auto e : beta)
        w.destroy(e);
}

TEST_CASE("scene: structural changes batched via deferred mode apply at boundary",
          "[scene][query][deferred]") {
    lane06_query::RegistryReset reset;
    auto& w = EcsRegistry::Get();

    // Pre-create one entity in non-deferred mode so it exists when we flip
    // the toggle.
    Entity stable = w.create();
    w.add<Pos>(stable, Pos{7, 8, 9});

    w.set_structural_deferred(true);

    // Defer 50 new entities — none should exist until apply.
    std::vector<Entity> deferred_es;
    for (int i = 0; i < 50; ++i) {
        Entity e = w.create();  // create() is immediate (entity-slot only)
        deferred_es.push_back(e);
        w.add<Pos>(e, Pos{static_cast<f32>(i), 0, 0});  // deferred
    }

    // Before apply, the query sees only the stable entity.
    std::atomic<u32> pre_count{0};
    w.query<reads<Pos>, writes<Vel>>([&pre_count](std::span<const Pos> p, std::span<Vel>) {
        pre_count.fetch_add(static_cast<u32>(p.size()), std::memory_order_relaxed);
    });
    // The stable entity has no Vel so the query sees zero rows.
    REQUIRE(pre_count.load() == 0);

    // Apply structural changes — the 50 entities now show up with Pos.
    w.apply_structural_changes();
    w.set_structural_deferred(false);

    // Add Vel non-deferred so they match the query.
    for (auto e : deferred_es)
        w.add<Vel>(e, Vel{0, 0, 0});

    std::atomic<u32> post_count{0};
    w.query<reads<Pos>, writes<Vel>>([&post_count](std::span<const Pos> p, std::span<Vel> v) {
        for (usize i = 0; i < p.size(); ++i)
            v[i].dx = p[i].x;
        post_count.fetch_add(static_cast<u32>(p.size()), std::memory_order_relaxed);
    });
    REQUIRE(post_count.load() == 50);

    for (auto e : deferred_es)
        w.destroy(e);
    w.destroy(stable);
}
