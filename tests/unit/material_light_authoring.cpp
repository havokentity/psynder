// SPDX-License-Identifier: MIT
// Psynder - material/light/environment authoring scene-file roundtrip.

#include <catch2/catch_test_macros.hpp>

#include "scene/SceneFile.h"

#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

constexpr f32 kEps = 0.0001f;

[[nodiscard]] bool near(f32 a, f32 b) noexcept {
    return std::fabs(a - b) < kEps;
}

struct RegistryScope {
    RegistryScope() {
        auto& registry = scene::EcsRegistry::Get();
        registry.clear();
        registry.set_structural_deferred(false);
    }

    ~RegistryScope() { scene::EcsRegistry::Get().clear(); }
};

struct SaveHooks {
    Entity mesh_entity{};
};

std::string_view mesh_name(void*, render::MeshId mesh) {
    return mesh.raw == 901u ? "fixtures.authoring.mesh" : std::string_view{};
}

std::string_view material_name(void*, render::MaterialId material) {
    return material.valid() ? "fixtures.authoring.material" : std::string_view{};
}

std::string_view material_texture_name(void*,
                                       render::MaterialId material,
                                       const render::MaterialDesc&) {
    return material.valid() ? "fixtures.authoring.albedo" : std::string_view{};
}

std::string_view mesh_group_name(void* user, Entity entity, scene::SceneNode) {
    const auto* hooks = static_cast<const SaveHooks*>(user);
    return hooks && hooks->mesh_entity == entity ? "authoring.meshes" : std::string_view{};
}

Entity spawn_mesh_instance(void*,
                           scene::Scene& scene_ref,
                           render::MeshId mesh,
                           render::MaterialId material,
                           const scene::LocalTransform& local,
                           scene::SceneNode parent,
                           scene::RenderableFlags flags,
                           scene::ObjectMobility mobility) {
    const scene::RenderableComponent renderable = scene::make_renderable(
        scene::GeometryKind::Mesh,
        mesh.raw,
        material,
        math::Aabb{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}},
        mobility,
        flags);
    return scene_ref.create_renderable(renderable, local, parent);
}

render::MaterialDesc desc_from_scene_file(const scene::SceneFileMaterial& material) {
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = material.albedo_rgba8;
    desc.alpha_cutoff = material.alpha_cutoff;
    desc.reflectivity = material.reflectivity;
    desc.roughness = material.roughness;
    desc.emissive = material.emissive;
    desc.winding = material.winding;
    desc.blend = material.blend;
    desc.raster_shadow_mode = material.raster_shadow_mode;
    desc.shadow_alpha = material.shadow_alpha;
    desc.shadow_opacity = material.shadow_opacity;
    desc.shadow_softness = material.shadow_softness;
    desc.flags = material.flags;
    return desc;
}

}  // namespace

TEST_CASE("scene file roundtrips material light and environment authoring",
          "[scene][scene_file][authoring]") {
    RegistryScope registry_scope;
    auto& registry = scene::EcsRegistry::Get();

    render::MaterialDesc authored_material{};
    authored_material.albedo_rgba8 = 0xB84080FFu;
    authored_material.alpha_cutoff = 0.37f;
    authored_material.reflectivity = 0.62f;
    authored_material.roughness = 0.18f;
    authored_material.emissive = 2.25f;
    authored_material.winding = render::MaterialWinding::DoubleSided;
    authored_material.blend = render::MaterialBlendMode::AlphaBlend;
    authored_material.raster_shadow_mode = render::MaterialRasterShadowMode::ProjectedDecal;
    authored_material.shadow_alpha = render::MaterialShadowAlphaMode::AlphaTest;
    authored_material.shadow_opacity = 0.73f;
    authored_material.shadow_softness = 0.41f;
    authored_material.flags = render::MaterialFlags::RasterVisible |
                              render::MaterialFlags::RtVisible |
                              render::MaterialFlags::CastsRtShadow |
                              render::MaterialFlags::ReceivesRtShadow |
                              render::MaterialFlags::CastsRasterShadow |
                              render::MaterialFlags::ReceivesRasterShadow |
                              render::MaterialFlags::Editable |
                              render::MaterialFlags::CastsBakedShadow |
                              render::MaterialFlags::EmissiveBakes;

    scene::LightComponent authored_light{};
    authored_light.kind = scene::LightKind::Spot;
    authored_light.color_rgba8 = 0xCCFFE0A0u;
    authored_light.intensity = 17.5f;
    authored_light.range = 36.0f;
    authored_light.inner_cone_deg = 13.5f;
    authored_light.outer_cone_deg = 57.25f;
    authored_light.casts_shadow = 1u;

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;

    {
        scene::Scene authored{registry};
        authored.environment().set_clear_color(0xFF112233u);
        authored.environment().set_clear_enabled(true, false);

        const render::MaterialId material = authored.materials().create(authored_material);
        REQUIRE(material.valid());

        const render::MeshId mesh{901u};
        scene::LocalTransform mesh_transform{};
        mesh_transform.translation = {1.0f, 2.0f, 3.0f};
        const scene::RenderableComponent renderable = scene::make_renderable(
            scene::GeometryKind::Mesh,
            mesh.raw,
            material,
            math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
            scene::ObjectMobility::Static,
            scene::RenderableFlags::Visible | scene::RenderableFlags::CastsShadowOverride |
                scene::RenderableFlags::ReceivesShadowOverride);
        const Entity mesh_entity = authored.create_renderable(renderable, mesh_transform);
        REQUIRE(mesh_entity.valid());
        REQUIRE(authored.set_entity_name(mesh_entity, "Glow Light"));
        REQUIRE(authored.attach_light(mesh_entity, authored_light));

        SaveHooks hook_user{.mesh_entity = mesh_entity};
        const scene::SceneFileSaveHooks hooks{
            .user = &hook_user,
            .mesh_name = &mesh_name,
            .material_name = &material_name,
            .material_base_color_texture_name = &material_texture_name,
            .material_preset_name = nullptr,
            .mesh_instance_group_name = &mesh_group_name,
        };

        REQUIRE(scene::save_scene_file(authored, hooks, bytes, &stats, &error));
        REQUIRE(error.empty());
    }

    REQUIRE(stats.mesh_instances == 1u);
    REQUIRE(stats.lights == 1u);
    REQUIRE(stats.materials == 1u);
    REQUIRE(stats.mesh_instance_group_names == 1u);

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.environments.size() == 1u);
    REQUIRE(view.materials.size() == 1u);
    REQUIRE(view.lights.size() == 1u);
    REQUIRE(view.mesh_instances.size() == 1u);

    const scene::SceneFileEnvironment& saved_env = view.environments[0];
    REQUIRE(saved_env.clear_color_rgba8 == 0xFF112233u);
    REQUIRE(saved_env.clear_color == 1u);
    REQUIRE(saved_env.clear_depth == 0u);

    const scene::SceneFileMaterial& saved_material = view.materials[0];
    REQUIRE(std::string_view{scene::scene_file_string(view, saved_material.name_offset)} ==
            "fixtures.authoring.material");
    REQUIRE(std::string_view{
                scene::scene_file_string(view, saved_material.base_color_texture_name_offset)} ==
            "fixtures.authoring.albedo");
    REQUIRE(saved_material.albedo_rgba8 == authored_material.albedo_rgba8);
    REQUIRE(saved_material.flags == authored_material.flags);
    REQUIRE(near(saved_material.alpha_cutoff, authored_material.alpha_cutoff));
    REQUIRE(near(saved_material.reflectivity, authored_material.reflectivity));
    REQUIRE(near(saved_material.roughness, authored_material.roughness));
    REQUIRE(near(saved_material.emissive, authored_material.emissive));
    REQUIRE(saved_material.winding == authored_material.winding);
    REQUIRE(saved_material.blend == authored_material.blend);
    REQUIRE(saved_material.raster_shadow_mode == authored_material.raster_shadow_mode);
    REQUIRE(saved_material.shadow_alpha == authored_material.shadow_alpha);
    REQUIRE(near(saved_material.shadow_opacity, authored_material.shadow_opacity));
    REQUIRE(near(saved_material.shadow_softness, authored_material.shadow_softness));

    const scene::SceneFileMeshInstance& saved_mesh = view.mesh_instances[0];
    REQUIRE(std::string_view{scene::scene_file_string(view, saved_mesh.mesh_name_offset)} ==
            "fixtures.authoring.mesh");
    REQUIRE(std::string_view{scene::scene_file_string(view, saved_mesh.material_name_offset)} ==
            "fixtures.authoring.material");
    REQUIRE(std::string_view{scene::scene_file_string(view, saved_mesh.group_name_offset)} ==
            "authoring.meshes");
    REQUIRE(saved_mesh.mobility == scene::ObjectMobility::Static);
    REQUIRE(saved_mesh.flags == (scene::RenderableFlags::Visible |
                                 scene::RenderableFlags::CastsShadowOverride |
                                 scene::RenderableFlags::ReceivesShadowOverride));

    const scene::SceneFileLight& saved_light = view.lights[0];
    REQUIRE(saved_light.kind == authored_light.kind);
    REQUIRE(saved_light.color_rgba8 == authored_light.color_rgba8);
    REQUIRE(near(saved_light.intensity, authored_light.intensity));
    REQUIRE(near(saved_light.range, authored_light.range));
    REQUIRE(near(saved_light.inner_cone_deg, authored_light.inner_cone_deg));
    REQUIRE(near(saved_light.outer_cone_deg, authored_light.outer_cone_deg));
    REQUIRE(saved_light.casts_shadow == 1u);

    registry.clear();
    registry.set_structural_deferred(false);

    scene::Scene loaded{registry};
    loaded.bind_mesh_spawner(nullptr, nullptr, &spawn_mesh_instance);
    const render::MaterialId loaded_material =
        loaded.materials().create(desc_from_scene_file(saved_material));
    REQUIRE(loaded_material.valid());

    const scene::SceneMeshBinding mesh_binding{
        .mesh_name = "fixtures.authoring.mesh",
        .mesh = render::MeshId{901u},
        .material = {},
    };
    const scene::SceneMaterialBinding material_binding{
        .material_name = "fixtures.authoring.material",
        .material = loaded_material,
    };

    Entity loaded_mesh{};
    const scene::SceneFileInstantiateResult result = scene::instantiate_scene_file(
        loaded,
        view,
        std::span<const scene::SceneMeshBinding>{&mesh_binding, 1u},
        std::span<const scene::SceneMaterialBinding>{&material_binding, 1u},
        std::span<Entity>{&loaded_mesh, 1u});
    REQUIRE(result.mesh_instances == 1u);
    REQUIRE(result.lights == 1u);
    REQUIRE(result.missing_mesh_bindings == 0u);
    REQUIRE(result.missing_material_bindings == 0u);
    REQUIRE(loaded_mesh.valid());

    REQUIRE(loaded.environment().settings().clear_color_rgba8 == 0xFF112233u);
    REQUIRE(loaded.environment().settings().clear_color);
    REQUIRE_FALSE(loaded.environment().settings().clear_depth);

    const auto* loaded_renderable = loaded.registry().get<scene::RenderableComponent>(loaded_mesh);
    REQUIRE(loaded_renderable != nullptr);
    REQUIRE(loaded_renderable->material == loaded_material);
    REQUIRE(loaded_renderable->mobility == scene::ObjectMobility::Static);
    REQUIRE(loaded_renderable->flags == saved_mesh.flags);

    const render::MaterialDesc loaded_material_desc = loaded.materials().get(loaded_material);
    REQUIRE(loaded_material_desc.albedo_rgba8 == authored_material.albedo_rgba8);
    REQUIRE(loaded_material_desc.flags == authored_material.flags);
    REQUIRE(near(loaded_material_desc.reflectivity, authored_material.reflectivity));
    REQUIRE(near(loaded_material_desc.roughness, authored_material.roughness));
    REQUIRE(near(loaded_material_desc.emissive, authored_material.emissive));
    REQUIRE(loaded_material_desc.blend == authored_material.blend);
    REQUIRE(loaded_material_desc.raster_shadow_mode == authored_material.raster_shadow_mode);
    REQUIRE(loaded_material_desc.shadow_alpha == authored_material.shadow_alpha);
    REQUIRE(near(loaded_material_desc.shadow_opacity, authored_material.shadow_opacity));
    REQUIRE(near(loaded_material_desc.shadow_softness, authored_material.shadow_softness));

    const auto* loaded_light = loaded.registry().get<scene::LightComponent>(loaded_mesh);
    REQUIRE(loaded_light != nullptr);
    REQUIRE(loaded_light->kind == authored_light.kind);
    REQUIRE(loaded_light->color_rgba8 == authored_light.color_rgba8);
    REQUIRE(near(loaded_light->intensity, authored_light.intensity));
    REQUIRE(near(loaded_light->range, authored_light.range));

    std::vector<scene::SceneLightItem> loaded_lights;
    loaded.update_transforms();
    loaded.gather_lights(loaded_lights);
    REQUIRE(loaded_lights.size() == 1u);
    REQUIRE(loaded_lights[0].kind == authored_light.kind);
    REQUIRE(loaded_lights[0].color_rgba8 == authored_light.color_rgba8);
    REQUIRE(near(loaded_lights[0].intensity, authored_light.intensity));
    REQUIRE(near(loaded_lights[0].range, authored_light.range));
    REQUIRE(near(loaded_lights[0].inner_cone_deg, authored_light.inner_cone_deg));
    REQUIRE(near(loaded_lights[0].outer_cone_deg, authored_light.outer_cone_deg));
    REQUIRE(loaded_lights[0].casts_shadow);
}
