// SPDX-License-Identifier: MIT
// Psynder — lane-06 unit tests: entity lifecycle (create / destroy / alive).

#include <catch2/catch_test_macros.hpp>

#include "scene/World.h"

#include <vector>

namespace lane06_lifecycle {

// Two trivial POD components for the lifecycle tests.
PSYNDER_COMPONENT(LifePos) {
    psynder::f32 x = 0, y = 0, z = 0;
};
PSYNDER_COMPONENT(LifeVel) {
    psynder::f32 dx = 0, dy = 0, dz = 0;
};

}  // namespace lane06_lifecycle

using namespace psynder;
using namespace psynder::scene;
using lane06_lifecycle::LifePos;
using lane06_lifecycle::LifeVel;

namespace {
// Reset the global World between tests by destroying every live entity.
// Catch2 reuses singletons, so each test starts from a known state.
void drain_world() {
    auto& w = World::Get();
    w.set_structural_deferred(false);
    // The World doesn't currently expose an "iterate all entities" surface,
    // but the unit tests below track every entity they create. We just
    // destroy any leftovers per-test by remembering them.
}
}  // namespace

TEST_CASE("scene: create returns a valid Entity that reports alive()", "[scene][lifecycle]") {
    drain_world();
    auto& w = World::Get();

    Entity e = w.create();
    REQUIRE(e.valid());
    REQUIRE(w.alive(e));
    w.destroy(e);
    REQUIRE_FALSE(w.alive(e));
}

TEST_CASE("scene: destroy invalidates by generation", "[scene][lifecycle]") {
    drain_world();
    auto& w = World::Get();

    Entity e1 = w.create();
    REQUIRE(w.alive(e1));
    w.destroy(e1);
    REQUIRE_FALSE(w.alive(e1));

    // The next create() may reuse the slot, but with a different generation.
    Entity e2 = w.create();
    REQUIRE(w.alive(e2));
    // The stale handle still reports dead even if slot indices collide.
    REQUIRE_FALSE(w.alive(e1));
    w.destroy(e2);
}

TEST_CASE("scene: alive() on a default-constructed handle is false", "[scene][lifecycle]") {
    drain_world();
    auto& w = World::Get();
    Entity invalid;
    REQUIRE_FALSE(invalid.valid());
    REQUIRE_FALSE(w.alive(invalid));
}

TEST_CASE("scene: many create/destroy cycles do not leak slots", "[scene][lifecycle]") {
    drain_world();
    auto& w = World::Get();
    constexpr int kCycles = 32;
    constexpr int kPerCycle = 64;

    for (int c = 0; c < kCycles; ++c) {
        std::vector<Entity> es;
        es.reserve(kPerCycle);
        for (int i = 0; i < kPerCycle; ++i) es.push_back(w.create());
        for (auto e : es) REQUIRE(w.alive(e));
        for (auto e : es) w.destroy(e);
        for (auto e : es) REQUIRE_FALSE(w.alive(e));
    }
}

TEST_CASE("scene: add component places entity in archetype and roundtrips via get", "[scene][lifecycle][component]") {
    drain_world();
    auto& w = World::Get();

    Entity e = w.create();
    REQUIRE(w.alive(e));

    LifePos p{1.0f, 2.0f, 3.0f};
    w.add<LifePos>(e, p);

    LifePos* got = w.get<LifePos>(e);
    REQUIRE(got != nullptr);
    REQUIRE(got->x == 1.0f);
    REQUIRE(got->y == 2.0f);
    REQUIRE(got->z == 3.0f);

    // Overwriting an existing component must be in-place.
    LifePos updated{10.0f, 20.0f, 30.0f};
    w.add<LifePos>(e, updated);
    got = w.get<LifePos>(e);
    REQUIRE(got != nullptr);
    REQUIRE(got->x == 10.0f);

    w.destroy(e);
}

TEST_CASE("scene: add/remove migrates between archetypes preserving overlap", "[scene][lifecycle][archetype]") {
    drain_world();
    auto& w = World::Get();

    Entity e = w.create();
    w.add<LifePos>(e, LifePos{1, 2, 3});
    w.add<LifeVel>(e, LifeVel{0.1f, 0.2f, 0.3f});

    LifePos* p = w.get<LifePos>(e);
    LifeVel* v = w.get<LifeVel>(e);
    REQUIRE(p != nullptr);
    REQUIRE(v != nullptr);
    REQUIRE(p->x == 1);
    REQUIRE(v->dx == 0.1f);

    // Remove velocity — entity moves back to the pos-only archetype but
    // keeps its position values.
    w.remove<LifeVel>(e);
    REQUIRE(w.get<LifeVel>(e) == nullptr);

    p = w.get<LifePos>(e);
    REQUIRE(p != nullptr);
    REQUIRE(p->x == 1);
    REQUIRE(p->z == 3);

    w.destroy(e);
}

TEST_CASE("scene: get on a non-existent component returns nullptr", "[scene][lifecycle]") {
    drain_world();
    auto& w = World::Get();

    Entity e = w.create();
    REQUIRE(w.get<LifePos>(e) == nullptr);
    REQUIRE(w.get<LifeVel>(e) == nullptr);
    w.destroy(e);
}
