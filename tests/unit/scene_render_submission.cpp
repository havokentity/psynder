// SPDX-License-Identifier: MIT
// Psynder — unified scene/material render submission tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "render/Material.h"
#include "scene/SceneEcs.h"

#include <vector>

using namespace psynder;

namespace {

scene::LocalTransform translate(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

}  // namespace

TEST_CASE("render material library stores editable raster and RT state in SoA columns",
          "[render][material]") {
    render::MaterialLibrary library;
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = 0xFF3366AAu;
    desc.base_color_texture = 42u;
    desc.reflectivity = 0.75f;
    desc.roughness = 0.25f;
    desc.winding = render::MaterialWinding::DoubleSided;
    desc.blend = render::MaterialBlendMode::AlphaBlend;
    desc.raster_shadow_mode = render::MaterialRasterShadowMode::ProjectedDecal;
    desc.shadow_alpha = render::MaterialShadowAlphaMode::AlphaTest;
    desc.shadow_opacity = 0.75f;
    desc.shadow_softness = 0.25f;
    desc.flags = render::MaterialFlags::RasterVisible | render::MaterialFlags::RtVisible |
                 render::MaterialFlags::CastsRtShadow | render::MaterialFlags::Editable |
                 render::MaterialFlags::BakeVisible | render::MaterialFlags::CastsBakedShadow;

    const render::MaterialId id = library.create(desc);
    REQUIRE(library.valid(id));
    REQUIRE(library.live_count() == 1u);

    const render::MaterialView view = library.view();
    REQUIRE(view.albedo_rgba8.size() == 1u);
    REQUIRE(view.albedo_rgba8[0] == 0xFF3366AAu);
    REQUIRE(view.base_color_texture[0] == 42u);
    REQUIRE_THAT(static_cast<double>(view.reflectivity[0]), Catch::Matchers::WithinAbs(0.75, 1e-6));
    REQUIRE(view.winding[0] == render::MaterialWinding::DoubleSided);
    REQUIRE(view.blend[0] == render::MaterialBlendMode::AlphaBlend);
    REQUIRE(view.raster_shadow_mode[0] == render::MaterialRasterShadowMode::ProjectedDecal);
    REQUIRE(view.shadow_alpha[0] == render::MaterialShadowAlphaMode::AlphaTest);
    REQUIRE_THAT(static_cast<double>(view.shadow_opacity[0]), Catch::Matchers::WithinAbs(0.75, 1e-6));
    REQUIRE_THAT(static_cast<double>(view.shadow_softness[0]), Catch::Matchers::WithinAbs(0.25, 1e-6));
    REQUIRE((view.flags[0] & render::MaterialFlags::CastsRtShadow) != 0u);
    REQUIRE((view.flags[0] & render::MaterialFlags::CastsBakedShadow) != 0u);
}

TEST_CASE("scene creates transform-backed renderable entities for shared renderers",
          "[scene][render_submission]") {
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene scene{registry};
    render::MaterialDesc material_desc{};
    material_desc.albedo_rgba8 = 0xFF2040C0u;
    material_desc.reflectivity = 1.0f;
    material_desc.winding = render::MaterialWinding::Cw;
    const render::MaterialId material = scene.materials().create(material_desc);

    scene::RenderableComponent renderable{};
    renderable.geometry = scene::GeometryKind::AnalyticSphere;
    renderable.geometry_id = 7u;
    renderable.material = material;
    renderable.mobility = scene::ObjectMobility::Static;
    renderable.local_bounds = math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};

    const Entity entity = scene.create_renderable(renderable, translate({3.0f, 4.0f, 5.0f}));
    REQUIRE(registry.get<scene::TransformComponent>(entity) != nullptr);
    REQUIRE(registry.get<scene::SceneNodeComponent>(entity) != nullptr);
    REQUIRE(registry.get<scene::RenderableComponent>(entity) != nullptr);

    const scene::SceneGraphUpdateStats stats = scene.update_transforms();
    REQUIRE(stats.transforms_updated >= 1u);

    std::vector<scene::SceneRenderItem> items;
    scene.gather_render_items(items);
    REQUIRE(items.size() == 1u);
    REQUIRE(items[0].entity == entity);
    REQUIRE(items[0].node.valid());
    REQUIRE(items[0].geometry == scene::GeometryKind::AnalyticSphere);
    REQUIRE(items[0].geometry_id == 7u);
    REQUIRE(items[0].material == material);
    REQUIRE(items[0].mobility == scene::ObjectMobility::Static);
    REQUIRE_THAT(static_cast<double>(items[0].world_bounds.min.x),
                 Catch::Matchers::WithinAbs(2.0, 1e-5));
    REQUIRE_THAT(static_cast<double>(items[0].world_bounds.max.z),
                 Catch::Matchers::WithinAbs(6.0, 1e-5));

    REQUIRE(scene.destroy_entity(entity));
}

TEST_CASE("scene owns environment clear settings", "[scene][render_submission][environment]") {
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene scene{registry};
    REQUIRE(scene.environment().settings().clear_color);
    REQUIRE(scene.environment().settings().clear_depth);
    REQUIRE(scene.environment().clear_enabled());
    REQUIRE(scene.environment().settings().clear_enabled());
    REQUIRE(scene.environment().settings().clear_color_rgba8 == 0xFF000000u);

    scene.environment().set_clear_color(0xFF202028u);
    scene.environment().set_clear_enabled(true, false);
    REQUIRE(scene.environment().settings().clear_color_rgba8 == 0xFF202028u);
    REQUIRE_FALSE(scene.environment().settings().clear_depth);
    REQUIRE(scene.environment().clear_enabled());

    scene.environment().disable_clear();
    REQUIRE_FALSE(scene.environment().clear_enabled());
    REQUIRE_FALSE(scene.environment().settings().clear_enabled());

    REQUIRE_FALSE(scene.environment().clouds().enabled);
    scene.environment().clouds().enabled = true;
    scene.environment().clouds().coverage = 0.65f;
    REQUIRE(scene.environment().settings().clouds.enabled);
    REQUIRE_THAT(static_cast<double>(scene.environment().settings().clouds.coverage),
                 Catch::Matchers::WithinAbs(0.65, 1e-5));
}

TEST_CASE("scene camera is a transform-backed hierarchy entity",
          "[scene][render_submission][camera]") {
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene scene{registry};
    const Entity camera_rig = scene.create_entity(translate({0.0f, 2.0f, 0.0f}));

    scene::CameraComponent camera{};
    camera.aspect = 2.0f;
    camera.tile_w = 32u;
    camera.tile_h = 32u;
    const Entity camera_entity =
        scene.create_camera(camera, translate({0.0f, 0.0f, 5.0f}), scene.node(camera_rig));

    REQUIRE(registry.get<scene::TransformComponent>(camera_entity) != nullptr);
    REQUIRE(registry.get<scene::SceneNodeComponent>(camera_entity) != nullptr);
    REQUIRE(registry.get<scene::CameraComponent>(camera_entity) != nullptr);
    REQUIRE(scene.graph().parent(scene.node(camera_entity)) == scene.node(camera_rig));
    REQUIRE(scene.active_camera_entity() == camera_entity);

    scene::SceneCameraView camera_view{};
    REQUIRE(scene.active_camera_view(1.0f, camera_view));
    REQUIRE(camera_view.entity == camera_entity);
    REQUIRE(camera_view.node == scene.node(camera_entity));
    REQUIRE(camera_view.tile_w == 32u);
    REQUIRE(camera_view.tile_h == 32u);

    REQUIRE(scene.destroy_entity(camera_entity));
    REQUIRE(scene.destroy_entity(camera_rig));

    scene::SceneCameraView empty_view{};
    REQUIRE_FALSE(scene.active_camera_view(1.0f, empty_view));
}

TEST_CASE("scene prewarm preserves capacity through dynamic renderable updates",
          "[scene][render_submission][prewarm]") {
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene scene{registry};
    scene::ScenePrewarmConfig config{};
    config.scene_entities = 8u;
    config.renderables = 6u;
    config.render_items = 6u;
    scene.prewarm_capacity(config);

    REQUIRE(scene.graph().node_capacity() >= config.scene_entities);
    REQUIRE(registry.entity_capacity() >= config.scene_entities);

    render::MaterialDesc material_desc{};
    material_desc.albedo_rgba8 = 0xFF88CC44u;
    const render::MaterialId material = scene.materials().create(material_desc);

    scene::RenderableComponent static_renderable =
        scene::make_static_renderable(scene::GeometryKind::AnalyticSphere,
                                      17u,
                                      material,
                                      math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}});
    scene::RenderableComponent dynamic_renderable =
        scene::make_dynamic_renderable(scene::GeometryKind::AnalyticSphere,
                                       23u,
                                       material,
                                       math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}});

    const u32 chunks_after_prewarm = registry.chunk_live_count();
    const u32 graph_capacity = scene.graph().node_capacity();
    const u32 dirty_capacity = scene.graph().dirty_root_capacity();

    std::vector<Entity> static_entities;
    static_entities.reserve(5u);
    for (u32 i = 0; i < 5u; ++i) {
        static_entities.push_back(
            scene.create_renderable(static_renderable, translate({static_cast<f32>(i), 0.0f, 0.0f})));
    }
    const Entity dynamic_entity =
        scene.create_renderable(dynamic_renderable, translate({0.0f, 2.0f, 0.0f}));

    scene.update_transforms();
    REQUIRE(registry.chunk_live_count() == chunks_after_prewarm);
    REQUIRE(scene.graph().node_capacity() == graph_capacity);
    REQUIRE(scene.graph().dirty_root_capacity() == dirty_capacity);

    std::vector<scene::SceneRenderItem> items;
    scene.gather_render_items(items);
    REQUIRE(items.size() == 6u);
    const usize item_capacity = items.capacity();
    REQUIRE(item_capacity >= config.render_items);

    for (u32 frame = 0; frame < 12u; ++frame) {
        scene.set_transform(dynamic_entity, translate({static_cast<f32>(frame) * 0.25f, 2.0f, 0.0f}));
        const scene::SceneGraphUpdateStats stats = scene.update_transforms();
        REQUIRE(stats.nodes_visited == 1u);
        REQUIRE(stats.transforms_updated == 1u);
        scene.gather_render_items(items);
        REQUIRE(items.size() == 6u);
        REQUIRE(items.capacity() == item_capacity);
        REQUIRE(registry.chunk_live_count() == chunks_after_prewarm);
        REQUIRE(scene.graph().node_capacity() == graph_capacity);
        REQUIRE(scene.graph().dirty_root_capacity() == dirty_capacity);
    }

    for (Entity entity : static_entities)
        REQUIRE(scene.destroy_entity(entity));
    REQUIRE(scene.destroy_entity(dynamic_entity));
}
