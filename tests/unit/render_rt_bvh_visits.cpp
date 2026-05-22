// SPDX-License-Identifier: MIT
// Psynder — Lane 08 unit test: BVH topology for a known scene.
//
// We build a Bvh8 from a small grid of triangles and assert that the wide
// node count is in the expected range (≥1, ≤ scene-prim-count: each leaf
// holds up to kMaxLeafPrims=4 triangles; with ≤ 8 children per wide node
// the tree is shallow and small). The Tlas test then verifies that a TLAS
// built over 4 instances of the same BLAS produces nodes too.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/rt/Bvh.h"

#include <array>
#include <vector>

using namespace psynder;
using namespace psynder::render::rt;

namespace {

// Make a 4×4 grid of small triangles on the XY plane at z=5.
std::vector<Triangle> make_grid_scene(u32 dim) {
    std::vector<Triangle> tris;
    tris.reserve(static_cast<size_t>(dim) * dim);
    for (u32 j = 0; j < dim; ++j) {
        for (u32 i = 0; i < dim; ++i) {
            const f32 x = static_cast<f32>(i) - static_cast<f32>(dim) * 0.5f;
            const f32 y = static_cast<f32>(j) - static_cast<f32>(dim) * 0.5f;
            tris.push_back(Triangle{
                math::Vec3{x, y, 5.0f},
                math::Vec3{x + 0.8f, y, 5.0f},
                math::Vec3{x, y + 0.8f, 5.0f},
            });
        }
    }
    return tris;
}

}  // namespace

TEST_CASE("Bvh8 visits: non-trivial scene builds non-empty BVH", "[render_rt][bvh_visits]") {
    auto tris = make_grid_scene(4);  // 16 triangles
    Bvh8 bvh;
    bvh.build(tris.data(), static_cast<u32>(tris.size()));

    // 16 prims, max-leaf=4 → at most 4 leaves → root + up-to-4 children.
    // The collapse-to-8 step shoves this into ≥1 wide node. The upper
    // bound is liberal — we just need the tree to be finite + non-empty.
    const u32 nc = bvh.node_count();
    REQUIRE(nc >= 1u);
    REQUIRE(nc <= tris.size());
}

TEST_CASE("Bvh8 visits: rays through the grid hit some triangle", "[render_rt][bvh_visits]") {
    auto tris = make_grid_scene(4);  // grid corners at (i-2, j-2), i,j ∈ [0,3]
    Bvh8 bvh;
    bvh.build(tris.data(), static_cast<u32>(tris.size()));

    // The (-1, -1) cell has corners (-1, -1), (-0.2, -1), (-1, -0.2);
    // interior point (-0.7, -0.7) lies inside it.
    Ray r;
    r.origin = {-0.7f, -0.7f, 0.0f};
    r.direction = {0.0f, 0.0f, 1.0f};
    r.t_min = 1e-4f;
    r.t_max = 1e30f;

    Hit h = bvh.intersect(r);
    REQUIRE(h.hit);
    REQUIRE_THAT(static_cast<double>(h.t), Catch::Matchers::WithinAbs(5.0, 1e-3));
    REQUIRE(h.primitive < tris.size());
}

TEST_CASE("Bvh8 refit preserves intersect correctness", "[render_rt][bvh_visits][refit]") {
    auto tris = make_grid_scene(4);  // dim=4 → grid spans x,y ∈ [-2, 2)
    Bvh8 bvh;
    bvh.build(tris.data(), static_cast<u32>(tris.size()));
    const u32 nodes_before = bvh.node_count();
    bvh.refit();
    const u32 nodes_after = bvh.node_count();
    REQUIRE(nodes_after == nodes_before);

    // Same interior point as above: the (-1, -1) cell contains (-0.7, -0.7).
    Ray r;
    r.origin = {-0.7f, -0.7f, 0.0f};
    r.direction = {0.0f, 0.0f, 1.0f};
    r.t_min = 1e-4f;
    r.t_max = 1e30f;
    Hit h = bvh.intersect(r);
    REQUIRE(h.hit);
    REQUIRE_THAT(static_cast<double>(h.t), Catch::Matchers::WithinAbs(5.0, 1e-3));
}

TEST_CASE("Tlas over 4 transformed instances finds all corners", "[render_rt][bvh_visits][tlas]") {
    Triangle one_tri{
        math::Vec3{-0.5f, -0.5f, 0.0f},
        math::Vec3{0.5f, -0.5f, 0.0f},
        math::Vec3{0.0f, 0.5f, 0.0f},
    };
    Bvh8 blas;
    blas.build(&one_tri, 1);

    // Four instances at z=5, offset in X and Y.
    std::array<Tlas::InstanceDesc, 4> inst;
    for (u32 i = 0; i < 4; ++i) {
        const f32 dx = (i & 1) ? 3.0f : -3.0f;
        const f32 dy = (i & 2) ? 3.0f : -3.0f;
        inst[i].blas = &blas;
        inst[i].transform = math::translate(math::Vec3{dx, dy, 5.0f});
    }
    Tlas tlas;
    tlas.build(inst.data(), 4);

    // A ray pointing at the +X+Y instance hits it.
    Ray r;
    r.origin = {3.0f, 3.0f, 0.0f};
    r.direction = {0.0f, 0.0f, 1.0f};
    r.t_min = 1e-4f;
    r.t_max = 1e30f;
    Hit h = tlas.intersect(r);
    REQUIRE(h.hit);
    REQUIRE_THAT(static_cast<double>(h.t), Catch::Matchers::WithinAbs(5.0, 1e-3));
    REQUIRE(h.instance == 3u);  // mask 11 → +X +Y instance is index 3

    // Aim at the −X −Y corner.
    r.origin = {-3.0f, -3.0f, 0.0f};
    h = tlas.intersect(r);
    REQUIRE(h.hit);
    REQUIRE(h.instance == 0u);

    // Aim at empty space.
    r.origin = {0.0f, 0.0f, 0.0f};
    h = tlas.intersect(r);
    REQUIRE_FALSE(h.hit);
}

TEST_CASE("Tlas refit updates world bounds when transforms move",
          "[render_rt][bvh_visits][tlas][refit]") {
    Triangle one_tri{
        math::Vec3{-0.5f, -0.5f, 0.0f},
        math::Vec3{0.5f, -0.5f, 0.0f},
        math::Vec3{0.0f, 0.5f, 0.0f},
    };
    Bvh8 blas;
    blas.build(&one_tri, 1);

    Tlas::InstanceDesc desc{};
    desc.blas = &blas;
    desc.transform = math::translate(math::Vec3{0.0f, 0.0f, 5.0f});

    Tlas tlas;
    tlas.build(&desc, 1);

    Ray r;
    r.origin = {0.0f, 0.0f, 0.0f};
    r.direction = {0.0f, 0.0f, 1.0f};
    r.t_min = 1e-4f;
    r.t_max = 1e30f;
    REQUIRE(tlas.intersect(r).hit);

    // Move the instance far off-axis through the dynamic path: update the
    // stored transform, then refit without rebuilding the TLAS topology.
    REQUIRE(tlas.update_instance_transform(0u, math::translate(math::Vec3{100.0f, 100.0f, 5.0f})));
    tlas.refit();
    REQUIRE_FALSE(tlas.intersect(r).hit);
}
