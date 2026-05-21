// SPDX-License-Identifier: MIT
// Psynder — analytic sphere rendering through the scene graph.

#include <catch2/catch_test_macros.hpp>

#include "render/rt/FrameRenderer.h"
#include "scene/SceneGraph.h"

#include <array>

using namespace psynder;

namespace {

scene::LocalTransform translate(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

}  // namespace

TEST_CASE("render rt: frame renderer hits scene graph analytic sphere without TLAS",
          "[render_rt][analytic_sphere]") {
    scene::SceneGraph graph;
    scene::SceneNode sphere_node =
        graph.create_node(scene::kInvalidSceneNode, translate({0.0f, 0.0f, 5.0f}));
    graph.add_analytic_sphere(
        scene::AnalyticSphereDesc{sphere_node, math::Sphere{{0.0f, 0.0f, 0.0f}, 2.0f}, 0xFF20C040u});
    graph.update_world_transforms();

    render::rt::FrameRenderInput input{};
    input.scene_graph = &graph;
    input.camera.origin = {0.0f, 0.0f, 0.0f};
    input.camera.forward = {0.0f, 0.0f, 1.0f};
    input.camera.right = {1.0f, 0.0f, 0.0f};
    input.camera.up = {0.0f, 1.0f, 0.0f};
    input.camera.fov_tan = 0.25f;
    input.camera.aspect = 1.0f;

    render::rt::FrameRenderConfig config{};
    config.output_width = 16u;
    config.output_height = 16u;
    config.trace_width = 16u;
    config.trace_height = 16u;
    config.parallel = false;
    config.ambient_scale = 255.0f;
    config.direct_scale = 0.0f;

    std::array<u32, 16u * 16u> pixels{};
    render::rt::FrameRenderStats stats{};
    render::rt::FrameRenderer renderer;
    renderer.render(input, config, pixels.data(), &stats);

    const u32 center = pixels[8u * 16u + 8u];
    REQUIRE((center & 0xFFu) == 0x40u);
    REQUIRE(((center >> 8) & 0xFFu) == 0xC0u);
    REQUIRE(((center >> 16) & 0xFFu) == 0x20u);
    REQUIRE(stats.hit_pixels > 0u);
}
