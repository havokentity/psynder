// SPDX-License-Identifier: MIT
// Psynder — hybrid scene renderer queue tests.

#include <catch2/catch_test_macros.hpp>

#include "render/SceneRenderer.h"

using namespace psynder;

namespace {

scene::RenderableComponent renderable(render::MaterialId material, u32 geometry_id) {
    scene::RenderableComponent out{};
    out.geometry = scene::GeometryKind::Mesh;
    out.geometry_id = geometry_id;
    out.material = material;
    out.local_bounds = math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    return out;
}

}  // namespace

TEST_CASE("scene renderer queues split raster, transparent, RT, and shadow work",
          "[render][scene_renderer]") {
    auto& world = scene::World::Get();
    world.set_structural_deferred(false);
    scene::RuntimeScene scene{world};

    render::MaterialDesc opaque_desc{};
    opaque_desc.flags = render::Material_RasterVisible | render::Material_RtVisible |
                        render::Material_CastsRtShadow;
    const render::MaterialId opaque = scene.materials().create(opaque_desc);

    render::MaterialDesc glass_desc{};
    glass_desc.blend = render::MaterialBlendMode::AlphaBlend;
    glass_desc.flags = render::Material_RasterVisible | render::Material_RtVisible;
    const render::MaterialId glass = scene.materials().create(glass_desc);

    render::MaterialDesc probe_only_desc{};
    probe_only_desc.flags = render::Material_RtVisible;
    const render::MaterialId probe_only = scene.materials().create(probe_only_desc);

    const Entity a = scene.create_renderable(renderable(opaque, 1u));
    const Entity b = scene.create_renderable(renderable(glass, 2u));
    const Entity c = scene.create_renderable(renderable(probe_only, 3u));

    render::SceneRenderQueues queues;
    render::build_scene_render_queues(scene, queues);

    REQUIRE(queues.all.size() == 3u);
    REQUIRE(queues.raster_opaque.size() == 1u);
    REQUIRE(queues.raster_transparent.size() == 1u);
    REQUIRE(queues.rt_visible.size() == 3u);
    REQUIRE(queues.rt_shadow_casters.size() == 1u);
    REQUIRE(queues.raster_opaque[0].entity == a);
    REQUIRE(queues.raster_transparent[0].entity == b);
    REQUIRE(queues.rt_shadow_casters[0].entity == a);

    scene.destroy_entity(a);
    scene.destroy_entity(b);
    scene.destroy_entity(c);
}
