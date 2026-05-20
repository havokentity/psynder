// SPDX-License-Identifier: MIT
// Psynder physics unit tests — island detection (union-find).
// Header-only: pulls kernel_detect_islands from internal/Kernels.h.

#include "physics/internal/Kernels.h"

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>
#include <vector>

using namespace psynder;
using namespace psynder::physics::detail;

namespace {
Body make_dynamic_body(math::Vec3 pos) {
    Body b{};
    b.position = pos;
    b.inv_mass = 1.0f;
    b.mass = 1.0f;
    b.shape = 0;
    b.half_extent = {0.5f, 0, 0};
    b.inertia.local = {1, 1, 1};
    b.inertia.inv_local = {1, 1, 1};
    b.rotation = {0, 0, 0, 1};
    return b;
}
Body make_static_body(math::Vec3 pos) {
    Body b = make_dynamic_body(pos);
    b.inv_mass = 0.0f;
    return b;
}
Contact make_contact(u32 a, u32 b) {
    Contact c{};
    c.body_a = a;
    c.body_b = b;
    c.normal_world = {0, 1, 0};
    c.depth = 0.0f;
    return c;
}
}  // namespace

TEST_CASE("detect_islands isolates disconnected contact subgraphs", "[physics][solver][islands]") {
    std::vector<Body> bodies{
        make_dynamic_body({0, 0, 0}),
        make_dynamic_body({1, 0, 0}),
        make_dynamic_body({2, 0, 0}),
        make_dynamic_body({100, 0, 0}),
        make_dynamic_body({101, 0, 0}),
    };
    std::vector<Contact> contacts{
        make_contact(0, 1),
        make_contact(1, 2),
        make_contact(3, 4),
    };
    std::vector<u32> body_idx;
    std::vector<Island> islands;
    kernels::kernel_detect_islands(contacts, {bodies.data(), bodies.size()}, body_idx, islands);

    REQUIRE(islands.size() == 2);
    REQUIRE(islands[0].contact_count + islands[1].contact_count == 3);
    REQUIRE(islands[0].body_count + islands[1].body_count == 5);

    std::unordered_set<u32> set0, set1;
    for (u32 i = 0; i < islands[0].body_count; ++i)
        set0.insert(body_idx[islands[0].first_body + i]);
    for (u32 i = 0; i < islands[1].body_count; ++i)
        set1.insert(body_idx[islands[1].first_body + i]);
    for (u32 b : set0)
        REQUIRE(set1.find(b) == set1.end());
}

TEST_CASE("static bodies do not bridge islands", "[physics][solver][islands]") {
    std::vector<Body> bodies{
        make_static_body({0, 0, 0}),
        make_dynamic_body({1, 1, 0}),
        make_dynamic_body({-1, 1, 0}),
    };
    std::vector<Contact> contacts{
        make_contact(0, 1),
        make_contact(0, 2),
    };
    std::vector<u32> body_idx;
    std::vector<Island> islands;
    kernels::kernel_detect_islands(contacts, {bodies.data(), bodies.size()}, body_idx, islands);
    REQUIRE(islands.size() == 2);
}

TEST_CASE("dense contact chain becomes a single island", "[physics][solver][islands]") {
    std::vector<Body> bodies;
    bodies.reserve(8);
    for (u32 i = 0; i < 8; ++i)
        bodies.push_back(make_dynamic_body({static_cast<f32>(i), 0, 0}));
    std::vector<Contact> contacts;
    for (u32 i = 0; i < 7; ++i)
        contacts.push_back(make_contact(i, i + 1));
    std::vector<u32> body_idx;
    std::vector<Island> islands;
    kernels::kernel_detect_islands(contacts, {bodies.data(), bodies.size()}, body_idx, islands);
    REQUIRE(islands.size() == 1);
    REQUIRE(islands[0].body_count == 8);
    REQUIRE(islands[0].contact_count == 7);
}
