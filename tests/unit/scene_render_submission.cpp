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
    desc.flags = render::Material_RasterVisible | render::Material_RtVisible |
                 render::Material_CastsRtShadow | render::Material_Editable |
                 render::Material_BakeVisible | render::Material_CastsBakedShadow;

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
    REQUIRE((view.flags[0] & render::Material_CastsRtShadow) != 0u);
    REQUIRE((view.flags[0] & render::Material_CastsBakedShadow) != 0u);
}

TEST_CASE("runtime scene creates transform-backed renderable entities for shared renderers",
          "[scene][render_submission]") {
    auto& world = scene::World::Get();
    world.set_structural_deferred(false);

    scene::RuntimeScene scene{world};
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
    REQUIRE(world.get<scene::TransformComponent>(entity) != nullptr);
    REQUIRE(world.get<scene::SceneNodeComponent>(entity) != nullptr);
    REQUIRE(world.get<scene::RenderableComponent>(entity) != nullptr);

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
