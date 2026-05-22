// SPDX-License-Identifier: MIT
// Psynder — scene graph hierarchy / dirty transform tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scene/SceneGraph.h"

#include <cstdint>

using namespace psynder;
using namespace psynder::scene;

namespace {

LocalTransform trs(math::Vec3 t, math::Vec3 s = {1.0f, 1.0f, 1.0f}) {
    LocalTransform out{};
    out.translation = t;
    out.scale = s;
    return out;
}

}  // namespace

TEST_CASE("scene graph: child world transform is parent times local", "[scene][scene_graph]") {
    SceneGraph graph;
    SceneNode root = graph.create_node(kInvalidSceneNode, trs({1.0f, 2.0f, 3.0f}));
    SceneNode child = graph.create_node(root, trs({4.0f, 5.0f, 6.0f}));

    SceneGraphUpdateStats stats = graph.update_world_transforms();
    REQUIRE(stats.transforms_updated == 2u);

    const math::Mat4& world = graph.world_matrix(child);
    REQUIRE_THAT(static_cast<double>(world.m[12]), Catch::Matchers::WithinAbs(5.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(world.m[13]), Catch::Matchers::WithinAbs(7.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(world.m[14]), Catch::Matchers::WithinAbs(9.0, 1e-5));
}

TEST_CASE("scene graph: one parent edit updates the whole dirty subtree in one pass",
          "[scene][scene_graph]") {
    SceneGraph graph;
    SceneNode root = graph.create_node(kInvalidSceneNode, trs({1.0f, 0.0f, 0.0f}));
    SceneNode child = graph.create_node(root, trs({0.0f, 2.0f, 0.0f}));
    SceneNode grandchild = graph.create_node(child, trs({0.0f, 0.0f, 3.0f}));
    graph.update_world_transforms();

    graph.set_local_transform(root, trs({10.0f, 0.0f, 0.0f}));
    SceneGraphUpdateStats stats = graph.update_world_transforms();
    REQUIRE(stats.transforms_updated == 3u);

    const math::Mat4& world = graph.world_matrix(grandchild);
    REQUIRE_THAT(static_cast<double>(world.m[12]), Catch::Matchers::WithinAbs(10.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(world.m[13]), Catch::Matchers::WithinAbs(2.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(world.m[14]), Catch::Matchers::WithinAbs(3.0, 1e-5));
}

TEST_CASE("scene graph: one leaf edit skips clean static siblings", "[scene][scene_graph][dirty]") {
    SceneGraph graph;
    SceneNode root = graph.create_node(kInvalidSceneNode, trs({1.0f, 0.0f, 0.0f}));
    SceneNode dynamic_leaf = graph.create_node(root, trs({0.0f, 2.0f, 0.0f}));
    [[maybe_unused]] SceneNode static_leaf = graph.create_node(root, trs({0.0f, 0.0f, 3.0f}));
    graph.update_world_transforms();

    graph.set_local_transform(dynamic_leaf, trs({0.0f, 20.0f, 0.0f}));
    SceneGraphUpdateStats stats = graph.update_world_transforms();
    REQUIRE(stats.nodes_visited == 1u);
    REQUIRE(stats.transforms_updated == 1u);

    const math::Mat4& world = graph.world_matrix(dynamic_leaf);
    REQUIRE_THAT(static_cast<double>(world.m[12]), Catch::Matchers::WithinAbs(1.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(world.m[13]), Catch::Matchers::WithinAbs(20.0, 1e-5));
}

TEST_CASE("scene graph: transform columns are cache-line aligned SoA",
          "[scene][scene_graph][cache]") {
    SceneGraph graph;
    graph.reserve_nodes(4);
    SceneNode a = graph.create_node(kInvalidSceneNode, trs({1.0f, 0.0f, 0.0f}));
    SceneNode b = graph.create_node(a, trs({0.0f, 1.0f, 0.0f}));
    graph.update_world_transforms();

    const auto local_a = reinterpret_cast<std::uintptr_t>(&graph.local_matrix(a));
    const auto local_b = reinterpret_cast<std::uintptr_t>(&graph.local_matrix(b));
    const auto world_a = reinterpret_cast<std::uintptr_t>(&graph.world_matrix(a));
    const auto world_b = reinterpret_cast<std::uintptr_t>(&graph.world_matrix(b));

    REQUIRE(local_a % kCacheLine == 0u);
    REQUIRE(world_a % kCacheLine == 0u);
    REQUIRE(local_b - local_a == sizeof(math::Mat4));
    REQUIRE(world_b - world_a == sizeof(math::Mat4));
}

TEST_CASE("scene graph: analytic spheres gather from cached world transforms",
          "[scene][scene_graph]") {
    SceneGraph graph;
    SceneNode node = graph.create_node(kInvalidSceneNode, trs({2.0f, 3.0f, 4.0f}, {2.0f, 3.0f, 4.0f}));
    REQUIRE(graph.add_analytic_sphere(
                AnalyticSphereDesc{node, math::Sphere{{1.0f, 0.0f, 0.0f}, 0.5f}, 0xFF00FF00u}) == 0u);
    graph.update_world_transforms();

    std::vector<AnalyticSphereInstance> spheres;
    graph.gather_analytic_spheres(spheres);
    REQUIRE(spheres.size() == 1u);
    REQUIRE_THAT(static_cast<double>(spheres[0].world_sphere.center.x),
                 Catch::Matchers::WithinAbs(4.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(spheres[0].world_sphere.center.y),
                 Catch::Matchers::WithinAbs(3.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(spheres[0].world_sphere.center.z),
                 Catch::Matchers::WithinAbs(4.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(spheres[0].world_sphere.radius),
                 Catch::Matchers::WithinAbs(2.0, 1e-5));
    REQUIRE(spheres[0].material_rgba8 == 0xFF00FF00u);
}
