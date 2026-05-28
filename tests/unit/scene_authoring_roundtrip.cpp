// SPDX-License-Identifier: MIT
// Psynder - Arcade-style scene authoring save/load regression tests.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "scene/EcsRegistry_Internal.h"
#include "scene/SceneFile.h"

#include <array>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

constexpr u32 kCubeMeshRaw = 101u;
constexpr u32 kSphereMeshRaw = 102u;
constexpr u32 kLoadedCubeMeshRaw = 301u;
constexpr u32 kLoadedSphereMeshRaw = 302u;
constexpr u32 kMissingAuthoringIndex = std::numeric_limits<u32>::max();

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

struct ArcadeHooks {
    Entity cube{};
    Entity sphere{};
};

scene::LocalTransform translate(math::Vec3 t) {
    scene::LocalTransform out{};
    out.translation = t;
    return out;
}

Entity test_spawn_mesh_instance(void*,
                                scene::Scene& scene_ref,
                                render::MeshId mesh,
                                render::MaterialId material,
                                const scene::LocalTransform& local,
                                scene::SceneNode parent,
                                scene::RenderableFlags flags,
                                scene::ObjectMobility mobility) {
    const scene::RenderableComponent renderable =
        scene::make_renderable(scene::GeometryKind::Mesh,
                               mesh.raw,
                               material,
                               math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                               mobility,
                               flags);
    return scene_ref.create_renderable(renderable, local, parent);
}

std::string_view arcade_mesh_name(void*, render::MeshId mesh) {
    switch (mesh.raw) {
        case kCubeMeshRaw:
            return "primitive.cube";
        case kSphereMeshRaw:
            return "primitive.sphere";
        default:
            return {};
    }
}

std::string_view arcade_material_name(void*, render::MaterialId material) {
    return material.valid() ? "arcade.material.glow" : std::string_view{};
}

std::string_view arcade_material_texture_name(void*,
                                              render::MaterialId material,
                                              const render::MaterialDesc&) {
    return material.valid() ? "arcade://generated/checker" : std::string_view{};
}

std::string_view arcade_group_name(void* user, Entity entity, scene::SceneNode) {
    const auto* hooks = static_cast<const ArcadeHooks*>(user);
    if (!hooks)
        return {};
    return entity == hooks->cube || entity == hooks->sphere ? "Arcade Primitives"
                                                            : std::string_view{};
}

Entity find_entity_named(scene::Scene& scene_ref, std::string_view name) {
    auto& registry = scene_ref.registry();
    const u32 count = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(count);
    registry.snapshot_live_entities(entities);
    for (Entity entity : entities) {
        if (scene_ref.entity_name(entity) == name)
            return entity;
    }
    return {};
}

u32 authoring_index_named(const scene::SceneFileView& view, std::string_view name) {
    for (usize i = 0u; i < view.authoring_nodes.size(); ++i) {
        const scene::SceneFileAuthoringNode& node = view.authoring_nodes[i];
        if (std::string_view{scene::scene_file_string(view, node.name_offset)} == name)
            return static_cast<u32>(i);
    }
    return kMissingAuthoringIndex;
}

const scene::SceneFileMeshInstance* mesh_instance_named(const scene::SceneFileView& view,
                                                       std::string_view mesh_name) {
    for (const scene::SceneFileMeshInstance& instance : view.mesh_instances) {
        if (std::string_view{scene::scene_file_string(view, instance.mesh_name_offset)} ==
            mesh_name) {
            return &instance;
        }
    }
    return nullptr;
}

bool has_object_label(const scene::SceneFileView& view,
                      scene::SceneFileObjectKind kind,
                      std::string_view label) {
    for (const scene::SceneFileObjectName& object_name : view.object_names) {
        if (object_name.kind == kind &&
            std::string_view{scene::scene_file_string(view, object_name.name_offset)} == label) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("scene file roundtrips Arcade authoring labels and hierarchy",
          "[scene][scene_file][authoring][arcade]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    render::MaterialDesc material_desc{};

    {
        scene::Scene authored{registry};
        authored.environment().set_clear_color(0xFF183858u);
        authored.environment().set_clear_enabled(false, true);

        const Entity root = authored.create_entity(translate({2.0f, 0.0f, 0.0f}));
        REQUIRE(root.valid());
        REQUIRE(authored.set_entity_name(root, "Arcade Root"));

        const Entity group = authored.create_entity(translate({4.0f, 0.0f, 0.0f}),
                                                    authored.node(root));
        REQUIRE(group.valid());
        REQUIRE(authored.set_entity_name(group, "Arcade Primitives"));
        const scene::SceneNode group_node = authored.node(group);

        scene::CameraDesc camera_desc{};
        camera_desc.position = {0.0f, 5.0f, 12.0f};
        camera_desc.look_at = {0.0f, 1.0f, 0.0f};
        camera_desc.fov_y_rad = 50.0f * math::kDegToRad;
        camera_desc.near_z = 0.2f;
        camera_desc.far_z = 400.0f;
        camera_desc.tile_w = 96u;
        camera_desc.tile_h = 64u;
        const Entity camera = authored.spawn_camera(camera_desc, group_node);
        REQUIRE(camera.valid());
        REQUIRE(authored.set_entity_name(camera, "Camera_Main"));

        const Entity light_entity = authored.create_entity(translate({1.0f, 3.0f, -2.0f}),
                                                           group_node);
        REQUIRE(light_entity.valid());
        REQUIRE(authored.set_entity_name(light_entity, "Key_Light"));
        scene::LightComponent light{};
        light.kind = scene::LightKind::Spot;
        light.color_rgba8 = 0xFFFFD080u;
        light.intensity = 6.75f;
        light.range = 28.0f;
        light.inner_cone_deg = 17.0f;
        light.outer_cone_deg = 42.0f;
        light.casts_shadow = 1u;
        REQUIRE(authored.attach_light(light_entity, light));

        material_desc.albedo_rgba8 = 0xFF44DDEE;
        material_desc.flags = render::MaterialFlags::RasterVisible |
                              render::MaterialFlags::RtVisible |
                              render::MaterialFlags::Editable;
        material_desc.alpha_cutoff = 0.37f;
        material_desc.reflectivity = 0.24f;
        material_desc.roughness = 0.58f;
        material_desc.emissive = 1.25f;
        material_desc.winding = render::MaterialWinding::DoubleSided;
        material_desc.blend = render::MaterialBlendMode::AlphaTest;
        material_desc.raster_shadow_mode = render::MaterialRasterShadowMode::ProjectedDecal;
        material_desc.shadow_alpha = render::MaterialShadowAlphaMode::AlphaBlend;
        material_desc.shadow_opacity = 0.71f;
        material_desc.shadow_softness = 0.19f;
        const render::MaterialId material = authored.materials().create(material_desc);
        REQUIRE(material.valid());

        const scene::RenderableComponent cube_renderable =
            scene::make_renderable(scene::GeometryKind::Mesh,
                                   kCubeMeshRaw,
                                   material,
                                   math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                                   scene::ObjectMobility::Static,
                                   scene::RenderableFlags::Visible |
                                       scene::RenderableFlags::CastsShadowOverride);
        const Entity cube = authored.create_renderable(cube_renderable,
                                                       translate({-1.5f, 0.0f, 0.0f}),
                                                       group_node);
        REQUIRE(cube.valid());
        REQUIRE(authored.set_entity_name(cube, "Cube"));

        const scene::RenderableComponent sphere_renderable =
            scene::make_renderable(scene::GeometryKind::Mesh,
                                   kSphereMeshRaw,
                                   material,
                                   math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                                   scene::ObjectMobility::Dynamic,
                                   scene::RenderableFlags::Visible);
        const Entity sphere = authored.create_renderable(sphere_renderable,
                                                         translate({1.5f, 0.0f, 0.0f}),
                                                         group_node);
        REQUIRE(sphere.valid());
        REQUIRE(authored.set_entity_name(sphere, "Sphere"));

        ArcadeHooks hook_user{.cube = cube, .sphere = sphere};
        const scene::SceneFileSaveHooks hooks{
            .user = &hook_user,
            .mesh_name = &arcade_mesh_name,
            .material_name = &arcade_material_name,
            .material_base_color_texture_name = &arcade_material_texture_name,
            .material_preset_name = nullptr,
            .mesh_instance_group_name = &arcade_group_name,
        };

        std::string error;
        REQUIRE(scene::save_scene_file(authored, hooks, bytes, &stats, &error));
        REQUIRE(error.empty());
    }

    REQUIRE(stats.cameras == 1u);
    REQUIRE(stats.lights == 1u);
    REQUIRE(stats.mesh_instances == 2u);
    REQUIRE(stats.materials == 1u);
    REQUIRE(stats.mesh_instance_group_names == 2u);
    REQUIRE(stats.authoring_nodes == 6u);

    scene::SceneFileView view{};
    std::string error;
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.environments.size() == 1u);
    REQUIRE(view.environments[0].clear_color_rgba8 == 0xFF183858u);
    REQUIRE(view.environments[0].clear_color == 0u);
    REQUIRE(view.environments[0].clear_depth == 1u);
    REQUIRE(view.cameras.size() == 1u);
    REQUIRE(view.lights.size() == 1u);
    REQUIRE(view.mesh_instances.size() == 2u);
    REQUIRE(view.object_names.size() == 4u);
    REQUIRE(view.authoring_nodes.size() == 6u);
    REQUIRE(view.materials.size() == 1u);

    const u32 root_index = authoring_index_named(view, "Arcade Root");
    const u32 group_index = authoring_index_named(view, "Arcade Primitives");
    const u32 camera_index = authoring_index_named(view, "Camera_Main");
    const u32 light_index = authoring_index_named(view, "Key_Light");
    const u32 cube_index = authoring_index_named(view, "Cube");
    const u32 sphere_index = authoring_index_named(view, "Sphere");
    REQUIRE(root_index != kMissingAuthoringIndex);
    REQUIRE(group_index != kMissingAuthoringIndex);
    REQUIRE(camera_index != kMissingAuthoringIndex);
    REQUIRE(light_index != kMissingAuthoringIndex);
    REQUIRE(cube_index != kMissingAuthoringIndex);
    REQUIRE(sphere_index != kMissingAuthoringIndex);
    REQUIRE(view.authoring_nodes[root_index].parent_index == scene::kSceneFileAuthoringRoot);
    REQUIRE(view.authoring_nodes[group_index].parent_index == root_index);
    REQUIRE(view.authoring_nodes[camera_index].parent_index == group_index);
    REQUIRE(view.authoring_nodes[light_index].parent_index == group_index);
    REQUIRE(view.authoring_nodes[cube_index].parent_index == group_index);
    REQUIRE(view.authoring_nodes[sphere_index].parent_index == group_index);

    REQUIRE(has_object_label(view, scene::SceneFileObjectKind::Camera, "Camera_Main"));
    REQUIRE(has_object_label(view, scene::SceneFileObjectKind::Light, "Key_Light"));
    REQUIRE(has_object_label(view, scene::SceneFileObjectKind::MeshInstance, "Cube"));
    REQUIRE(has_object_label(view, scene::SceneFileObjectKind::MeshInstance, "Sphere"));

    const scene::SceneFileMeshInstance* cube_file = mesh_instance_named(view, "primitive.cube");
    const scene::SceneFileMeshInstance* sphere_file = mesh_instance_named(view, "primitive.sphere");
    REQUIRE(cube_file != nullptr);
    REQUIRE(sphere_file != nullptr);
    REQUIRE(std::string_view{scene::scene_file_string(view, cube_file->material_name_offset)} ==
            "arcade.material.glow");
    REQUIRE(std::string_view{scene::scene_file_string(view, sphere_file->material_name_offset)} ==
            "arcade.material.glow");
    REQUIRE(std::string_view{scene::scene_file_string(view, cube_file->group_name_offset)} ==
            "Arcade Primitives");
    REQUIRE(std::string_view{scene::scene_file_string(view, sphere_file->group_name_offset)} ==
            "Arcade Primitives");

    const scene::SceneFileMaterial& material_file = view.materials[0];
    REQUIRE(std::string_view{scene::scene_file_string(view, material_file.name_offset)} ==
            "arcade.material.glow");
    REQUIRE(std::string_view{
                scene::scene_file_string(view, material_file.base_color_texture_name_offset)} ==
            "arcade://generated/checker");
    REQUIRE(material_file.albedo_rgba8 == material_desc.albedo_rgba8);
    REQUIRE(material_file.flags == material_desc.flags);
    REQUIRE_THAT(static_cast<double>(material_file.alpha_cutoff),
                 Catch::Matchers::WithinAbs(static_cast<double>(material_desc.alpha_cutoff),
                                            1e-5));
    REQUIRE_THAT(static_cast<double>(material_file.reflectivity),
                 Catch::Matchers::WithinAbs(static_cast<double>(material_desc.reflectivity),
                                            1e-5));
    REQUIRE_THAT(static_cast<double>(material_file.roughness),
                 Catch::Matchers::WithinAbs(static_cast<double>(material_desc.roughness),
                                            1e-5));
    REQUIRE_THAT(static_cast<double>(material_file.emissive),
                 Catch::Matchers::WithinAbs(static_cast<double>(material_desc.emissive),
                                            1e-5));
    REQUIRE(material_file.winding == material_desc.winding);
    REQUIRE(material_file.blend == material_desc.blend);
    REQUIRE(material_file.raster_shadow_mode == material_desc.raster_shadow_mode);
    REQUIRE(material_file.shadow_alpha == material_desc.shadow_alpha);
    REQUIRE_THAT(static_cast<double>(material_file.shadow_opacity),
                 Catch::Matchers::WithinAbs(static_cast<double>(material_desc.shadow_opacity),
                                            1e-5));
    REQUIRE_THAT(static_cast<double>(material_file.shadow_softness),
                 Catch::Matchers::WithinAbs(static_cast<double>(material_desc.shadow_softness),
                                            1e-5));

    RegistryReset reload_reset;
    auto& reload_registry = scene::EcsRegistry::Get();
    reload_registry.set_structural_deferred(false);
    scene::Scene loaded{reload_registry};
    loaded.bind_mesh_spawner(nullptr, nullptr, test_spawn_mesh_instance);

    const scene::SceneMeshBinding mesh_bindings[] = {
        {.mesh_name = "primitive.cube", .mesh = render::MeshId{kLoadedCubeMeshRaw}, .material = {}},
        {.mesh_name = "primitive.sphere",
         .mesh = render::MeshId{kLoadedSphereMeshRaw},
         .material = {}},
    };
    const scene::SceneMaterialBinding material_bindings[] = {
        {.material_name = "arcade.material.glow", .material = render::MaterialId{9u}},
    };
    Entity loaded_meshes[2]{};
    const scene::SceneFileInstantiateResult result = scene::instantiate_scene_file(
        loaded,
        view,
        std::span<const scene::SceneMeshBinding>{mesh_bindings, 2u},
        std::span<const scene::SceneMaterialBinding>{material_bindings, 1u},
        std::span<Entity>{loaded_meshes, 2u});

    REQUIRE(result.cameras == 1u);
    REQUIRE(result.lights == 1u);
    REQUIRE(result.mesh_instances == 2u);
    REQUIRE(result.missing_mesh_bindings == 0u);
    REQUIRE(result.missing_material_bindings == 0u);

    const Entity loaded_root = find_entity_named(loaded, "Arcade Root");
    const Entity loaded_group = find_entity_named(loaded, "Arcade Primitives");
    const Entity loaded_camera = find_entity_named(loaded, "Camera_Main");
    const Entity loaded_light = find_entity_named(loaded, "Key_Light");
    const Entity loaded_cube = find_entity_named(loaded, "Cube");
    const Entity loaded_sphere = find_entity_named(loaded, "Sphere");
    REQUIRE(loaded_root.valid());
    REQUIRE(loaded_group.valid());
    REQUIRE(loaded_camera.valid());
    REQUIRE(loaded_light.valid());
    REQUIRE(loaded_cube.valid());
    REQUIRE(loaded_sphere.valid());

    REQUIRE_FALSE(loaded.graph().parent(loaded.node(loaded_root)).valid());
    REQUIRE(loaded.graph().parent(loaded.node(loaded_group)) == loaded.node(loaded_root));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_camera)) == loaded.node(loaded_group));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_light)) == loaded.node(loaded_group));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_cube)) == loaded.node(loaded_group));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_sphere)) == loaded.node(loaded_group));

    REQUIRE(loaded.environment().settings().clear_color_rgba8 == 0xFF183858u);
    REQUIRE_FALSE(loaded.environment().settings().clear_color);
    REQUIRE(loaded.environment().settings().clear_depth);
    REQUIRE(loaded.active_camera_entity() == loaded_camera);

    const auto* camera = loaded.registry().get<scene::CameraComponent>(loaded_camera);
    REQUIRE(camera != nullptr);
    REQUIRE(camera->tile_w == 96u);
    REQUIRE(camera->tile_h == 64u);
    REQUIRE_THAT(static_cast<double>(camera->near_z), Catch::Matchers::WithinAbs(0.2, 1e-5));
    REQUIRE_THAT(static_cast<double>(camera->far_z), Catch::Matchers::WithinAbs(400.0, 1e-5));

    const auto* light = loaded.registry().get<scene::LightComponent>(loaded_light);
    REQUIRE(light != nullptr);
    REQUIRE(light->kind == scene::LightKind::Spot);
    REQUIRE(light->color_rgba8 == 0xFFFFD080u);
    REQUIRE(light->casts_shadow == 1u);
    REQUIRE_THAT(static_cast<double>(light->intensity), Catch::Matchers::WithinAbs(6.75, 1e-5));
    REQUIRE_THAT(static_cast<double>(light->range), Catch::Matchers::WithinAbs(28.0, 1e-5));

    const auto* cube_renderable = loaded.registry().get<scene::RenderableComponent>(loaded_cube);
    const auto* sphere_renderable = loaded.registry().get<scene::RenderableComponent>(loaded_sphere);
    REQUIRE(cube_renderable != nullptr);
    REQUIRE(sphere_renderable != nullptr);
    REQUIRE(cube_renderable->geometry_id == kLoadedCubeMeshRaw);
    REQUIRE(sphere_renderable->geometry_id == kLoadedSphereMeshRaw);
    REQUIRE(cube_renderable->material == material_bindings[0].material);
    REQUIRE(sphere_renderable->material == material_bindings[0].material);
    REQUIRE(cube_renderable->mobility == scene::ObjectMobility::Static);
    REQUIRE(sphere_renderable->mobility == scene::ObjectMobility::Dynamic);

    const scene::CachedSceneGroup primitive_group = loaded.cache_group("Arcade Primitives");
    REQUIRE(primitive_group.size() == 2u);
}

TEST_CASE("scene file roundtrips FPS gameplay authoring components",
          "[scene][scene_file][authoring][gameplay]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene authored{registry};
    scene::LocalTransform player_transform{};
    player_transform.translation = {2.0f, 0.0f, -4.0f};
    const Entity player = authored.create_entity(player_transform);
    REQUIRE(player.valid());
    REQUIRE(authored.set_entity_name(player, "FPS Player"));

    scene::GameplayTagComponent tag{};
    tag.role = scene::GameplayRole::PlayerController;
    tag.flags = 7u;
    registry.add<scene::GameplayTagComponent>(player, tag);

    scene::PlayerControllerComponent controller{};
    controller.walk_speed = 4.25f;
    controller.run_speed = 8.5f;
    controller.jump_speed = 5.75f;
    controller.mouse_sensitivity = 0.18f;
    controller.height = 1.82f;
    controller.radius = 0.38f;
    registry.add<scene::PlayerControllerComponent>(player, controller);

    scene::HealthComponent health{};
    health.max_health = 125.0f;
    health.current_health = 80.0f;
    health.faction = 3u;
    registry.add<scene::HealthComponent>(player, health);

    scene::WeaponComponent weapon{};
    weapon.damage = 33.0f;
    weapon.range = 72.0f;
    weapon.fire_rate = 9.0f;
    weapon.ammo = 42u;
    weapon.automatic = 0u;
    registry.add<scene::WeaponComponent>(player, weapon);

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;
    REQUIRE(scene::save_scene_file(authored, {}, bytes, &stats, &error));
    REQUIRE(error.empty());
    REQUIRE(stats.authoring_nodes == 1u);
    REQUIRE(stats.gameplay_entities == 1u);

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()},
                                    view,
                                    &error));
    REQUIRE(error.empty());
    REQUIRE(view.authoring_nodes.size() == 1u);
    REQUIRE(view.gameplay_entities.size() == 1u);
    REQUIRE(view.gameplay_entities[0].authoring_node_index == 0u);
    REQUIRE(view.gameplay_entities[0].role == scene::GameplayRole::PlayerController);
    REQUIRE(view.gameplay_entities[0].flags == 7u);
    REQUIRE(view.gameplay_entities[0].weapon.ammo == 42u);

    registry.clear();
    registry.set_structural_deferred(false);
    scene::Scene loaded{registry};
    std::array<scene::SceneMeshBinding, 0> mesh_bindings{};
    std::array<Entity, 0> out_mesh_entities{};
    const scene::SceneFileInstantiateResult result =
        scene::instantiate_scene_file(loaded, view, mesh_bindings, out_mesh_entities);
    REQUIRE(result.mesh_instances == 0u);

    const Entity loaded_player = find_entity_named(loaded, "FPS Player");
    REQUIRE(loaded_player.valid());
    const auto* loaded_tag = registry.get<scene::GameplayTagComponent>(loaded_player);
    const auto* loaded_controller =
        registry.get<scene::PlayerControllerComponent>(loaded_player);
    const auto* loaded_health = registry.get<scene::HealthComponent>(loaded_player);
    const auto* loaded_weapon = registry.get<scene::WeaponComponent>(loaded_player);
    REQUIRE(loaded_tag != nullptr);
    REQUIRE(loaded_controller != nullptr);
    REQUIRE(loaded_health != nullptr);
    REQUIRE(loaded_weapon != nullptr);
    REQUIRE(loaded_tag->role == scene::GameplayRole::PlayerController);
    REQUIRE(loaded_tag->flags == 7u);
    REQUIRE_THAT(static_cast<double>(loaded_controller->walk_speed),
                 Catch::Matchers::WithinAbs(static_cast<double>(controller.walk_speed), 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_controller->run_speed),
                 Catch::Matchers::WithinAbs(static_cast<double>(controller.run_speed), 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_health->current_health),
                 Catch::Matchers::WithinAbs(static_cast<double>(health.current_health), 0.0001));
    REQUIRE(loaded_health->faction == 3u);
    REQUIRE_THAT(static_cast<double>(loaded_weapon->damage),
                 Catch::Matchers::WithinAbs(static_cast<double>(weapon.damage), 0.0001));
    REQUIRE(loaded_weapon->ammo == 42u);
    REQUIRE(loaded_weapon->automatic == 0u);
}

TEST_CASE("scene file roundtrips authoring physics components",
          "[scene][scene_file][authoring][physics]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene authored{registry};

    // A rigid body (non-Box shape + non-default mass/half-extent to prove the
    // shape enum + all authoring fields survive).
    scene::LocalTransform box_transform{};
    box_transform.translation = {3.0f, 1.0f, -2.0f};
    const Entity box = authored.create_entity(box_transform);
    REQUIRE(box.valid());
    REQUIRE(authored.set_entity_name(box, "Crate"));
    scene::RigidBodyComponent rb{};
    rb.shape = scene::ColliderShape::Sphere;
    rb.mass = 7.5f;
    rb.half_extent = {1.25f, 0.5f, 2.0f};
    rb.friction = 0.42f;
    rb.restitution = 0.31f;
    rb.runtime_body = 12345u;  // RUNTIME junk — must NOT be persisted.
    registry.add<scene::RigidBodyComponent>(box, rb);

    // A player vehicle with non-default authoring params.
    const Entity car = authored.create_entity(translate({-4.0f, 0.0f, 0.0f}));
    REQUIRE(car.valid());
    REQUIRE(authored.set_entity_name(car, "Car"));
    scene::VehicleComponent vc{};
    vc.half_extent = {1.1f, 0.45f, 2.2f};
    vc.mass = 1450.0f;
    vc.engine_max_torque = 520.0f;
    vc.drag = 0.27f;
    vc.wheel_radius = 0.36f;
    vc.suspension = 0.33f;
    vc.stiffness = 36000.0f;
    vc.damping = 4700.0f;
    vc.is_player = 1u;
    vc.runtime_vehicle = 999u;  // RUNTIME junk — must NOT be persisted.
    vc.runtime_chassis = 888u;  // RUNTIME junk — must NOT be persisted.
    registry.add<scene::VehicleComponent>(car, vc);

    // A player helicopter with non-default authoring params.
    const Entity heli = authored.create_entity(translate({0.0f, 6.0f, 0.0f}));
    REQUIRE(heli.valid());
    REQUIRE(authored.set_entity_name(heli, "Heli"));
    scene::HelicopterComponent hc{};
    hc.half_extent = {1.3f, 0.65f, 2.1f};
    hc.mass = 950.0f;
    hc.max_thrust_n = 15000.0f;
    hc.pitch_torque = 8200.0f;
    hc.roll_torque = 8100.0f;
    hc.yaw_torque = 4100.0f;
    hc.angular_damping = 2.5f;
    hc.hover_assist = 1u;
    hc.is_player = 1u;
    hc.runtime_body = 777u;  // RUNTIME junk — must NOT be persisted.
    registry.add<scene::HelicopterComponent>(heli, hc);

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;
    REQUIRE(scene::save_scene_file(authored, {}, bytes, &stats, &error));
    REQUIRE(error.empty());
    REQUIRE(stats.authoring_nodes == 3u);
    REQUIRE(stats.physics_bodies == 3u);

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.header->version == scene::kPsySceneVersion);
    REQUIRE(view.authoring_nodes.size() == 3u);
    REQUIRE(view.physics_bodies.size() == 3u);

    registry.clear();
    registry.set_structural_deferred(false);
    scene::Scene loaded{registry};
    std::array<scene::SceneMeshBinding, 0> mesh_bindings{};
    std::array<Entity, 0> out_mesh_entities{};
    const scene::SceneFileInstantiateResult result =
        scene::instantiate_scene_file(loaded, view, mesh_bindings, out_mesh_entities);
    REQUIRE(result.mesh_instances == 0u);

    const Entity loaded_box = find_entity_named(loaded, "Crate");
    const Entity loaded_car = find_entity_named(loaded, "Car");
    const Entity loaded_heli = find_entity_named(loaded, "Heli");
    REQUIRE(loaded_box.valid());
    REQUIRE(loaded_car.valid());
    REQUIRE(loaded_heli.valid());

    const auto* loaded_rb = registry.get<scene::RigidBodyComponent>(loaded_box);
    REQUIRE(loaded_rb != nullptr);
    REQUIRE(loaded_rb->shape == scene::ColliderShape::Sphere);
    REQUIRE_THAT(static_cast<double>(loaded_rb->mass),
                 Catch::Matchers::WithinAbs(7.5, 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_rb->half_extent.x),
                 Catch::Matchers::WithinAbs(1.25, 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_rb->half_extent.y),
                 Catch::Matchers::WithinAbs(0.5, 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_rb->half_extent.z),
                 Catch::Matchers::WithinAbs(2.0, 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_rb->friction),
                 Catch::Matchers::WithinAbs(0.42, 0.0001));
    REQUIRE_THAT(static_cast<double>(loaded_rb->restitution),
                 Catch::Matchers::WithinAbs(0.31, 0.0001));
    // Runtime handle is NOT serialized: it must be 0 on load.
    REQUIRE(loaded_rb->runtime_body == 0u);

    const auto* loaded_vc = registry.get<scene::VehicleComponent>(loaded_car);
    REQUIRE(loaded_vc != nullptr);
    REQUIRE_THAT(static_cast<double>(loaded_vc->mass),
                 Catch::Matchers::WithinAbs(1450.0, 0.01));
    REQUIRE_THAT(static_cast<double>(loaded_vc->engine_max_torque),
                 Catch::Matchers::WithinAbs(520.0, 0.01));
    REQUIRE_THAT(static_cast<double>(loaded_vc->wheel_radius),
                 Catch::Matchers::WithinAbs(0.36, 0.0001));
    REQUIRE(loaded_vc->is_player == 1u);
    REQUIRE(loaded_vc->runtime_vehicle == 0u);
    REQUIRE(loaded_vc->runtime_chassis == 0u);

    const auto* loaded_hc = registry.get<scene::HelicopterComponent>(loaded_heli);
    REQUIRE(loaded_hc != nullptr);
    REQUIRE_THAT(static_cast<double>(loaded_hc->mass),
                 Catch::Matchers::WithinAbs(950.0, 0.01));
    REQUIRE_THAT(static_cast<double>(loaded_hc->max_thrust_n),
                 Catch::Matchers::WithinAbs(15000.0, 0.01));
    REQUIRE_THAT(static_cast<double>(loaded_hc->yaw_torque),
                 Catch::Matchers::WithinAbs(4100.0, 0.01));
    REQUIRE(loaded_hc->hover_assist == 1u);
    REQUIRE(loaded_hc->is_player == 1u);
    REQUIRE(loaded_hc->runtime_body == 0u);
}
