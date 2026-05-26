// SPDX-License-Identifier: MIT
// Psynder - focused editor/FPS authoring smoke coverage.

#include <catch2/catch_test_macros.hpp>

#include "editor/core/CommandHistory.h"
#include "scene/SceneFile.h"

#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

constexpr f32 kEps = 0.0001f;
constexpr u32 kMeshCrate = 501u;
constexpr u32 kLoadedMeshCrate = 1501u;
constexpr u64 kAuthorGameplayEntityCommand = 0x465053415554484Fu;  // "FPSAUTHO"

bool near(f32 a, f32 b) noexcept {
    return std::fabs(a - b) <= kEps;
}

struct RegistryScope {
    RegistryScope() {
        auto& registry = scene::EcsRegistry::Get();
        registry.clear();
        registry.set_structural_deferred(false);
    }

    ~RegistryScope() { scene::EcsRegistry::Get().clear(); }
};

struct SaveHookState {
    Entity crate{};
};

struct AuthoredEntityPayload {
    u32 entity_raw = 0u;
    scene::GameplayRole role = scene::GameplayRole::None;
    u8 _pad[3] = {};
    u32 flags = 0u;
};

scene::LocalTransform local_at(math::Vec3 translation) {
    scene::LocalTransform local{};
    local.translation = translation;
    return local;
}

std::string_view smoke_mesh_name(void*, render::MeshId mesh) {
    return mesh.raw == kMeshCrate ? "fps.mesh.supply_crate" : std::string_view{};
}

std::string_view smoke_material_name(void*, render::MaterialId material) {
    return material.valid() ? "fps.material.crate" : std::string_view{};
}

std::string_view smoke_material_texture_name(void*,
                                             render::MaterialId material,
                                             const render::MaterialDesc&) {
    return material.valid() ? "textures/fps/crate_albedo.ktx2" : std::string_view{};
}

std::string_view smoke_mesh_group_name(void* user, Entity entity, scene::SceneNode) {
    const auto* state = static_cast<const SaveHookState*>(user);
    return state && state->crate == entity ? "FPS Props" : std::string_view{};
}

Entity spawn_test_mesh(void*,
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
        const auto& node = view.authoring_nodes[i];
        if (std::string_view{scene::scene_file_string(view, node.name_offset)} == name)
            return static_cast<u32>(i);
    }
    return scene::kSceneFileAuthoringRoot;
}

const scene::SceneFileGameplayEntity* gameplay_for_authoring_index(const scene::SceneFileView& view,
                                                                   u32 authoring_index) {
    for (const auto& gameplay : view.gameplay_entities) {
        if (gameplay.authoring_node_index == authoring_index)
            return &gameplay;
    }
    return nullptr;
}

render::MaterialDesc material_desc_from_file(const scene::SceneFileMaterial& material) {
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = material.albedo_rgba8;
    desc.flags = material.flags;
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
    return desc;
}

}  // namespace

TEST_CASE("editor FPS authoring smoke preserves gameplay hierarchy and scene metadata",
          "[editor][fps][scene_file][authoring]") {
    RegistryScope registry_scope;
    auto& registry = scene::EcsRegistry::Get();

    render::MaterialDesc crate_material{};
    crate_material.albedo_rgba8 = 0xFFB06A2Cu;
    crate_material.flags = render::MaterialFlags::RasterVisible | render::MaterialFlags::RtVisible |
                           render::MaterialFlags::Editable |
                           render::MaterialFlags::ReceivesRasterShadow;
    crate_material.roughness = 0.44f;
    crate_material.reflectivity = 0.08f;
    crate_material.blend = render::MaterialBlendMode::Opaque;
    crate_material.shadow_alpha = render::MaterialShadowAlphaMode::Opaque;

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;

    {
        scene::Scene authored{registry};
        authored.environment().set_clear_color(0xFF101820u);
        authored.environment().set_clear_enabled(true, false);

        const Entity root = authored.create_entity(local_at({0.0f, 0.0f, 0.0f}));
        REQUIRE(root.valid());
        REQUIRE(authored.set_entity_name(root, "FPS Smoke Level"));

        const Entity gameplay_group =
            authored.create_entity(local_at({0.0f, 0.0f, 0.0f}), authored.node(root));
        REQUIRE(gameplay_group.valid());
        REQUIRE(authored.set_entity_name(gameplay_group, "Gameplay"));
        const scene::SceneNode gameplay_node = authored.node(gameplay_group);

        const Entity props_group =
            authored.create_entity(local_at({4.0f, 0.0f, 0.0f}), authored.node(root));
        REQUIRE(props_group.valid());
        REQUIRE(authored.set_entity_name(props_group, "Props"));
        const scene::SceneNode props_node = authored.node(props_group);

        const Entity player_start =
            authored.create_entity(local_at({0.0f, 0.0f, -2.0f}), gameplay_node);
        REQUIRE(player_start.valid());
        REQUIRE(authored.set_entity_name(player_start, "Player_Start"));
        scene::GameplayTagComponent start_tag{};
        start_tag.role = scene::GameplayRole::PlayerStart;
        start_tag.flags = 0x01u;
        registry.add<scene::GameplayTagComponent>(player_start, start_tag);

        const Entity player = authored.create_entity(local_at({1.0f, 0.0f, -2.0f}), gameplay_node);
        REQUIRE(player.valid());
        REQUIRE(authored.set_entity_name(player, "Player_Controller"));
        scene::GameplayTagComponent player_tag{};
        player_tag.role = scene::GameplayRole::PlayerController;
        player_tag.flags = 0x07u;
        registry.add<scene::GameplayTagComponent>(player, player_tag);
        scene::PlayerControllerComponent controller{};
        controller.walk_speed = 4.2f;
        controller.run_speed = 8.4f;
        controller.jump_speed = 5.6f;
        controller.mouse_sensitivity = 0.16f;
        controller.height = 1.8f;
        controller.radius = 0.36f;
        registry.add<scene::PlayerControllerComponent>(player, controller);
        scene::HealthComponent player_health{};
        player_health.max_health = 125.0f;
        player_health.current_health = 98.0f;
        player_health.faction = 1u;
        registry.add<scene::HealthComponent>(player, player_health);
        scene::WeaponComponent weapon{};
        weapon.damage = 34.0f;
        weapon.range = 80.0f;
        weapon.fire_rate = 9.5f;
        weapon.ammo = 48u;
        weapon.automatic = 1u;
        registry.add<scene::WeaponComponent>(player, weapon);

        const Entity enemy = authored.create_entity(local_at({6.0f, 0.0f, -6.0f}), gameplay_node);
        REQUIRE(enemy.valid());
        REQUIRE(authored.set_entity_name(enemy, "Enemy_Rifleman"));
        scene::GameplayTagComponent enemy_tag{};
        enemy_tag.role = scene::GameplayRole::Enemy;
        enemy_tag.flags = 0x20u;
        registry.add<scene::GameplayTagComponent>(enemy, enemy_tag);
        scene::HealthComponent enemy_health{};
        enemy_health.max_health = 60.0f;
        enemy_health.current_health = 60.0f;
        enemy_health.faction = 2u;
        registry.add<scene::HealthComponent>(enemy, enemy_health);

        scene::LightComponent light{};
        light.kind = scene::LightKind::Spot;
        light.color_rgba8 = 0xFFFFD6A0u;
        light.intensity = 12.5f;
        light.range = 42.0f;
        light.inner_cone_deg = 18.0f;
        light.outer_cone_deg = 48.0f;
        light.casts_shadow = 1u;
        const Entity key_light =
            authored.create_entity(local_at({2.0f, 5.0f, -3.0f}), authored.node(root));
        REQUIRE(key_light.valid());
        REQUIRE(authored.set_entity_name(key_light, "Key_Light"));
        REQUIRE(authored.attach_light(key_light, light));

        const render::MaterialId material = authored.materials().create(crate_material);
        REQUIRE(material.valid());
        const scene::RenderableComponent crate_renderable =
            scene::make_renderable(scene::GeometryKind::Mesh,
                                   kMeshCrate,
                                   material,
                                   math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                                   scene::ObjectMobility::Static,
                                   scene::RenderableFlags::Visible |
                                       scene::RenderableFlags::ReceivesShadowOverride);
        const Entity crate =
            authored.create_renderable(crate_renderable, local_at({0.5f, 0.0f, 1.25f}), props_node);
        REQUIRE(crate.valid());
        REQUIRE(authored.set_entity_name(crate, "Ammo_Crate"));

        SaveHookState hook_state{.crate = crate};
        const scene::SceneFileSaveHooks hooks{
            .user = &hook_state,
            .mesh_name = &smoke_mesh_name,
            .material_name = &smoke_material_name,
            .material_base_color_texture_name = &smoke_material_texture_name,
            .material_preset_name = nullptr,
            .mesh_instance_group_name = &smoke_mesh_group_name,
        };

        REQUIRE(scene::save_scene_file(authored, hooks, bytes, &stats, &error));
        REQUIRE(error.empty());
    }

    REQUIRE(stats.authoring_nodes == 8u);
    REQUIRE(stats.gameplay_entities == 3u);
    REQUIRE(stats.mesh_instances == 1u);
    REQUIRE(stats.lights == 1u);
    REQUIRE(stats.materials == 1u);

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(error.empty());
    REQUIRE(view.environments.size() == 1u);
    REQUIRE(view.authoring_nodes.size() == 8u);
    REQUIRE(view.gameplay_entities.size() == 3u);
    REQUIRE(view.mesh_instances.size() == 1u);
    REQUIRE(view.lights.size() == 1u);
    REQUIRE(view.materials.size() == 1u);

    REQUIRE(view.environments[0].clear_color_rgba8 == 0xFF101820u);
    REQUIRE(view.environments[0].clear_color == 1u);
    REQUIRE(view.environments[0].clear_depth == 0u);

    const u32 root_index = authoring_index_named(view, "FPS Smoke Level");
    const u32 gameplay_index = authoring_index_named(view, "Gameplay");
    const u32 props_index = authoring_index_named(view, "Props");
    const u32 player_start_index = authoring_index_named(view, "Player_Start");
    const u32 player_index = authoring_index_named(view, "Player_Controller");
    const u32 enemy_index = authoring_index_named(view, "Enemy_Rifleman");
    const u32 key_light_index = authoring_index_named(view, "Key_Light");
    const u32 crate_index = authoring_index_named(view, "Ammo_Crate");
    REQUIRE(root_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(gameplay_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(props_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(player_start_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(player_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(enemy_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(key_light_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(crate_index != scene::kSceneFileAuthoringRoot);
    REQUIRE(view.authoring_nodes[root_index].parent_index == scene::kSceneFileAuthoringRoot);
    REQUIRE(view.authoring_nodes[gameplay_index].parent_index == root_index);
    REQUIRE(view.authoring_nodes[props_index].parent_index == root_index);
    REQUIRE(view.authoring_nodes[player_start_index].parent_index == gameplay_index);
    REQUIRE(view.authoring_nodes[player_index].parent_index == gameplay_index);
    REQUIRE(view.authoring_nodes[enemy_index].parent_index == gameplay_index);
    REQUIRE(view.authoring_nodes[key_light_index].parent_index == root_index);
    REQUIRE(view.authoring_nodes[crate_index].parent_index == props_index);

    const auto* player_gameplay = gameplay_for_authoring_index(view, player_index);
    REQUIRE(player_gameplay != nullptr);
    REQUIRE(player_gameplay->role == scene::GameplayRole::PlayerController);
    REQUIRE(player_gameplay->flags == 0x07u);
    REQUIRE(near(player_gameplay->player_controller.run_speed, 8.4f));
    REQUIRE(near(player_gameplay->health.current_health, 98.0f));
    REQUIRE(player_gameplay->weapon.ammo == 48u);

    const auto* enemy_gameplay = gameplay_for_authoring_index(view, enemy_index);
    REQUIRE(enemy_gameplay != nullptr);
    REQUIRE(enemy_gameplay->role == scene::GameplayRole::Enemy);
    REQUIRE(enemy_gameplay->flags == 0x20u);
    REQUIRE(near(enemy_gameplay->health.max_health, 60.0f));

    const scene::SceneFileMaterial& saved_material = view.materials[0];
    REQUIRE(std::string_view{scene::scene_file_string(view, saved_material.name_offset)} ==
            "fps.material.crate");
    REQUIRE(std::string_view{
                scene::scene_file_string(view, saved_material.base_color_texture_name_offset)} ==
            "textures/fps/crate_albedo.ktx2");
    REQUIRE(saved_material.albedo_rgba8 == crate_material.albedo_rgba8);
    REQUIRE(saved_material.flags == crate_material.flags);
    REQUIRE(near(saved_material.roughness, crate_material.roughness));
    REQUIRE(near(saved_material.reflectivity, crate_material.reflectivity));

    const scene::SceneFileLight& saved_light = view.lights[0];
    REQUIRE(saved_light.kind == scene::LightKind::Spot);
    REQUIRE(saved_light.color_rgba8 == 0xFFFFD6A0u);
    REQUIRE(near(saved_light.intensity, 12.5f));
    REQUIRE(near(saved_light.range, 42.0f));
    REQUIRE(saved_light.casts_shadow == 1u);

    registry.clear();
    registry.set_structural_deferred(false);
    scene::Scene loaded{registry};
    loaded.bind_mesh_spawner(nullptr, nullptr, &spawn_test_mesh);
    const render::MaterialId loaded_material =
        loaded.materials().create(material_desc_from_file(saved_material));
    REQUIRE(loaded_material.valid());

    const scene::SceneMeshBinding mesh_binding{
        .mesh_name = "fps.mesh.supply_crate",
        .mesh = render::MeshId{kLoadedMeshCrate},
        .material = {},
    };
    const scene::SceneMaterialBinding material_binding{
        .material_name = "fps.material.crate",
        .material = loaded_material,
    };
    Entity loaded_meshes[1]{};
    const scene::SceneFileInstantiateResult result = scene::instantiate_scene_file(
        loaded,
        view,
        std::span<const scene::SceneMeshBinding>{&mesh_binding, 1u},
        std::span<const scene::SceneMaterialBinding>{&material_binding, 1u},
        std::span<Entity>{loaded_meshes, 1u});
    REQUIRE(result.mesh_instances == 1u);
    REQUIRE(result.lights == 1u);
    REQUIRE(result.missing_mesh_bindings == 0u);
    REQUIRE(result.missing_material_bindings == 0u);

    const Entity loaded_root = find_entity_named(loaded, "FPS Smoke Level");
    const Entity loaded_gameplay = find_entity_named(loaded, "Gameplay");
    const Entity loaded_props = find_entity_named(loaded, "Props");
    const Entity loaded_player_start = find_entity_named(loaded, "Player_Start");
    const Entity loaded_player = find_entity_named(loaded, "Player_Controller");
    const Entity loaded_enemy = find_entity_named(loaded, "Enemy_Rifleman");
    const Entity loaded_crate = find_entity_named(loaded, "Ammo_Crate");
    const Entity loaded_key_light = find_entity_named(loaded, "Key_Light");
    REQUIRE(loaded_root.valid());
    REQUIRE(loaded_gameplay.valid());
    REQUIRE(loaded_props.valid());
    REQUIRE(loaded_player_start.valid());
    REQUIRE(loaded_player.valid());
    REQUIRE(loaded_enemy.valid());
    REQUIRE(loaded_crate.valid());
    REQUIRE(loaded_key_light.valid());
    REQUIRE(loaded.graph().parent(loaded.node(loaded_gameplay)) == loaded.node(loaded_root));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_props)) == loaded.node(loaded_root));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_player_start)) == loaded.node(loaded_gameplay));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_player)) == loaded.node(loaded_gameplay));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_enemy)) == loaded.node(loaded_gameplay));
    REQUIRE(loaded.graph().parent(loaded.node(loaded_crate)) == loaded.node(loaded_props));

    REQUIRE(loaded.environment().settings().clear_color_rgba8 == 0xFF101820u);
    REQUIRE(loaded.environment().settings().clear_color);
    REQUIRE_FALSE(loaded.environment().settings().clear_depth);

    const auto* loaded_player_tag = registry.get<scene::GameplayTagComponent>(loaded_player);
    const auto* loaded_controller = registry.get<scene::PlayerControllerComponent>(loaded_player);
    const auto* loaded_player_health = registry.get<scene::HealthComponent>(loaded_player);
    const auto* loaded_weapon = registry.get<scene::WeaponComponent>(loaded_player);
    REQUIRE(loaded_player_tag != nullptr);
    REQUIRE(loaded_controller != nullptr);
    REQUIRE(loaded_player_health != nullptr);
    REQUIRE(loaded_weapon != nullptr);
    REQUIRE(loaded_player_tag->role == scene::GameplayRole::PlayerController);
    REQUIRE(loaded_player_tag->flags == 0x07u);
    REQUIRE(near(loaded_controller->walk_speed, 4.2f));
    REQUIRE(near(loaded_controller->run_speed, 8.4f));
    REQUIRE(near(loaded_player_health->current_health, 98.0f));
    REQUIRE(loaded_weapon->ammo == 48u);

    const auto* loaded_enemy_tag = registry.get<scene::GameplayTagComponent>(loaded_enemy);
    const auto* loaded_enemy_health = registry.get<scene::HealthComponent>(loaded_enemy);
    REQUIRE(loaded_enemy_tag != nullptr);
    REQUIRE(loaded_enemy_health != nullptr);
    REQUIRE(loaded_enemy_tag->role == scene::GameplayRole::Enemy);
    REQUIRE(loaded_enemy_tag->flags == 0x20u);
    REQUIRE(near(loaded_enemy_health->max_health, 60.0f));

    const auto* loaded_renderable = registry.get<scene::RenderableComponent>(loaded_crate);
    REQUIRE(loaded_renderable != nullptr);
    REQUIRE(loaded_renderable->geometry_id == kLoadedMeshCrate);
    REQUIRE(loaded_renderable->material == loaded_material);

    const auto* loaded_light = registry.get<scene::LightComponent>(loaded_key_light);
    REQUIRE(loaded_light != nullptr);
    REQUIRE(loaded_light->kind == scene::LightKind::Spot);
    REQUIRE(loaded_light->color_rgba8 == 0xFFFFD6A0u);
    REQUIRE(near(loaded_light->intensity, 12.5f));
    REQUIRE(near(loaded_light->range, 42.0f));
    REQUIRE(loaded_light->casts_shadow == 1u);
}

TEST_CASE("editor FPS authoring command history keeps undo redo metadata", "[editor][fps][undo]") {
    using editor::command_history::Command;
    using editor::command_history::CommandKind;
    using editor::command_history::History;

    AuthoredEntityPayload payload{};
    payload.entity_raw = 42u;
    payload.role = scene::GameplayRole::PlayerController;
    payload.flags = 0x07u;

    History history{4u};
    history.push_record("author FPS player", kAuthorGameplayEntityCommand, payload);

    REQUIRE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
    REQUIRE(history.undo_label() == "author FPS player");

    Command command{};
    REQUIRE(history.undo(command));
    REQUIRE(command.kind() == CommandKind::Record);
    REQUIRE(command.label() == "author FPS player");
    REQUIRE(command.record_type() == kAuthorGameplayEntityCommand);
    REQUIRE(command.payload().size() == sizeof(AuthoredEntityPayload));

    AuthoredEntityPayload restored{};
    std::memcpy(&restored, command.payload().data(), sizeof(restored));
    REQUIRE(restored.entity_raw == payload.entity_raw);
    REQUIRE(restored.role == scene::GameplayRole::PlayerController);
    REQUIRE(restored.flags == 0x07u);
    REQUIRE_FALSE(history.can_undo());
    REQUIRE(history.can_redo());
    REQUIRE(history.redo_label() == "author FPS player");

    REQUIRE(history.redo(command));
    REQUIRE(command.label() == "author FPS player");
    REQUIRE(history.can_undo());
    REQUIRE_FALSE(history.can_redo());
}
