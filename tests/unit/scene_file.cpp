// SPDX-License-Identifier: MIT
// Psynder — cooked scene file validation.

#include <catch2/catch_test_macros.hpp>

#include "scene/SceneFile.h"
#include "scene/EcsRegistry_Internal.h"
#include "../../tools/scene_cook/SceneCook.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace psynder;

namespace {

void append_bytes(std::vector<u8>& out, const void* data, usize bytes) {
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + bytes);
}

void pad_to_scene_alignment(std::vector<u8>& out) {
    const usize aligned = ((out.size() + scene::kPsySceneAlignment - 1u) / scene::kPsySceneAlignment) *
                          scene::kPsySceneAlignment;
    out.resize(aligned, 0u);
}

template <class T>
void append_chunk(std::vector<u8>& bytes,
                  std::vector<scene::SceneFileChunk>& chunks,
                  scene::SceneFileChunkType type,
                  std::span<const T> data,
                  u32 stride) {
    pad_to_scene_alignment(bytes);
    scene::SceneFileChunk chunk{};
    chunk.type = type;
    chunk.offset = static_cast<u32>(bytes.size());
    chunk.bytes = static_cast<u32>(data.size_bytes());
    chunk.stride = stride;
    if (!data.empty())
        append_bytes(bytes, data.data(), data.size_bytes());
    chunks.push_back(chunk);
}

std::vector<u8> make_scene_blob() {
    constexpr char strings[] =
        "\0crateCube\0crate.wood\0textures.procedural.wooden_crate\0crates\0crate_spin\0";
    const scene::SceneFileEnvironment environment{0xFF182030u, 1u, 1u, {}};
    const math::Vec3 translations[] = {{0.0f, 1.5f, 1.5f}, {-1.3f, 0.0f, -3.0f}};
    const math::Quat rotations[] = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    const math::Vec3 scales[] = {{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
    scene::SceneFileCamera camera{};
    camera.transform_index = 0u;
    camera.look_at = {0.0f, 0.0f, -3.0f};
    scene::SceneFileMeshInstance mesh{};
    mesh.transform_index = 1u;
    mesh.mesh_name_offset = 1u;
    mesh.material_name_offset = 11u;
    mesh.group_name_offset = 55u;
    scene::SceneFileMaterial material_file{};
    material_file.name_offset = 11u;
    material_file.base_color_texture_name_offset = 22u;
    material_file.flags = render::MaterialFlags::RasterVisible;
    scene::SceneFileBehaviorSpinOp spin{};
    spin.name_offset = 62u;
    spin.target_group_name_offset = 55u;
    spin.axis = {0.0f, 1.0f, 0.0f};
    spin.speed_base = 0.35f;
    spin.speed_step = 0.12f;
    scene::SceneFileBehaviorTranslateOp translate{};
    translate.name_offset = 62u;
    translate.target_group_name_offset = 55u;
    translate.axis = {0.0f, 1.0f, 0.0f};
    translate.amount_base = 0.5f;
    translate.amount_step = 0.25f;

    std::vector<u8> bytes(sizeof(scene::SceneFileHeader) + 10u * sizeof(scene::SceneFileChunk));
    std::vector<scene::SceneFileChunk> chunks;
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Strings,
                 std::span<const char>{strings, sizeof(strings)},
                 1u);
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Environment,
                 std::span<const scene::SceneFileEnvironment>{&environment, 1u},
                 sizeof(environment));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::TransformTranslation,
                 std::span<const math::Vec3>{translations, 2u},
                 sizeof(math::Vec3));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::TransformRotation,
                 std::span<const math::Quat>{rotations, 2u},
                 sizeof(math::Quat));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::TransformScale,
                 std::span<const math::Vec3>{scales, 2u},
                 sizeof(math::Vec3));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Cameras,
                 std::span<const scene::SceneFileCamera>{&camera, 1u},
                 sizeof(camera));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::MeshInstances,
                 std::span<const scene::SceneFileMeshInstance>{&mesh, 1u},
                 sizeof(mesh));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::Materials,
                 std::span<const scene::SceneFileMaterial>{&material_file, 1u},
                 sizeof(material_file));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::BehaviorSpinOps,
                 std::span<const scene::SceneFileBehaviorSpinOp>{&spin, 1u},
                 sizeof(spin));
    append_chunk(bytes,
                 chunks,
                 scene::SceneFileChunkType::BehaviorTranslateOps,
                 std::span<const scene::SceneFileBehaviorTranslateOp>{&translate, 1u},
                 sizeof(translate));

    scene::SceneFileHeader header{};
    header.file_bytes = static_cast<u32>(bytes.size());
    header.chunk_count = static_cast<u32>(chunks.size());
    header.transform_count = 2u;
    header.camera_count = 1u;
    header.mesh_instance_count = 1u;
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), chunks.data(), chunks.size() * sizeof(chunks[0]));
    return bytes;
}

Entity test_spawn_mesh_instance(void*,
                                scene::Scene& scene_ref,
                                render::MeshId mesh,
                                render::MaterialId material,
                                const scene::LocalTransform& local,
                                scene::SceneNode parent,
                                scene::RenderableFlags flags,
                                scene::ObjectMobility mobility) {
    const scene::RenderableComponent renderable = scene::make_renderable(scene::GeometryKind::Mesh,
                                                                         mesh.raw,
                                                                         material,
                                                                         math::aabb_empty(),
                                                                         mobility,
                                                                         flags);
    return scene_ref.create_renderable(renderable, local, parent);
}

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

struct SaveRoundtripHooks {
    Entity mesh_entity{};
};

std::string_view save_roundtrip_mesh_name(void*, render::MeshId mesh) {
    return mesh.raw == 77u ? "mesh.box" : std::string_view{};
}

std::string_view save_roundtrip_material_name(void*, render::MaterialId material) {
    return material.valid() ? "material.generated" : std::string_view{};
}

std::string_view save_roundtrip_material_preset_name(void*,
                                                     render::MaterialId material,
                                                     const render::MaterialDesc&) {
    return material.valid() ? "preset.clay" : std::string_view{};
}

std::string_view save_roundtrip_group_name(void* user, Entity entity, scene::SceneNode) {
    const auto* hooks = static_cast<const SaveRoundtripHooks*>(user);
    return hooks && entity == hooks->mesh_entity ? "Hero Box" : std::string_view{};
}

}  // namespace

TEST_CASE("cooked scene file exposes SoA chunks and instantiates entities", "[scene][scene_file]") {
    const std::vector<u8> bytes = make_scene_blob();
    scene::SceneFileView view{};
    std::string error;
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(view.translations.size() == 2u);
    REQUIRE(view.cameras.size() == 1u);
    REQUIRE(view.mesh_instances.size() == 1u);
    REQUIRE(view.lights.empty());
    REQUIRE(view.materials.size() == 1u);
    REQUIRE(view.behavior_spin_ops.size() == 1u);
    REQUIRE(view.behavior_translate_ops.size() == 1u);
    REQUIRE(std::string_view{scene::scene_file_string(view, 1u)} == "crateCube");
    REQUIRE(std::string_view{scene::scene_file_string(view, 11u)} == "crate.wood");
    REQUIRE(std::string_view{scene::scene_file_string(view, 22u)} ==
            "textures.procedural.wooden_crate");
    REQUIRE(std::string_view{scene::scene_file_string(view, 55u)} == "crates");

    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene_ref{registry};
    scene_ref.bind_mesh_spawner(nullptr, nullptr, test_spawn_mesh_instance);
    const render::MaterialId material{1u};
    const scene::SceneMeshBinding binding{.mesh_name = "crateCube",
                                          .mesh = render::MeshId{1u},
                                          .material = {}};
    const scene::SceneMaterialBinding material_binding{.material_name = "crate.wood",
                                                       .material = material};
    Entity mesh_entity{};
    const scene::SceneFileInstantiateResult result = scene::instantiate_scene_file(
        scene_ref,
        view,
        std::span<const scene::SceneMeshBinding>{&binding, 1u},
        std::span<const scene::SceneMaterialBinding>{&material_binding, 1u},
        std::span<Entity>{&mesh_entity, 1u});

    REQUIRE(result.cameras == 1u);
    REQUIRE(result.mesh_instances == 1u);
    REQUIRE(result.lights == 0u);
    REQUIRE(result.missing_mesh_bindings == 0u);
    REQUIRE(result.missing_material_bindings == 0u);
    REQUIRE(mesh_entity.valid());
    REQUIRE(scene_ref.spin_behavior_count() == 1u);
    REQUIRE(scene_ref.translate_behavior_count() == 1u);
    const auto* renderable = scene_ref.registry().get<scene::RenderableComponent>(mesh_entity);
    REQUIRE(renderable != nullptr);
    REQUIRE(renderable->material.raw == material.raw);
    const scene::SceneGroupId crates_group = scene_ref.group_id("crates");
    const scene::CachedSceneGroup group = scene_ref.cache_group(crates_group);
    REQUIRE(group.size() == 1u);
    REQUIRE(group.entities()[0].raw == mesh_entity.raw);
    for (auto [entity, transform, authored] : group.transforms()) {
        REQUIRE(entity.raw == mesh_entity.raw);
        transform.translation.x = authored.local.translation.x + 2.0f;
    }
    const auto* transform = scene_ref.registry().get<scene::TransformComponent>(mesh_entity);
    REQUIRE(transform != nullptr);
    REQUIRE(transform->local.translation.x > 0.69f);
    REQUIRE(transform->local.translation.x < 0.71f);
    scene_ref.update_entity_behaviors(1.0f);
    REQUIRE(std::abs(transform->local.rotation.y) > 0.01f);
    REQUIRE(transform->local.translation.y > 0.49f);
    REQUIRE(transform->local.translation.y < 0.51f);
    REQUIRE(scene_ref.active_camera_entity().valid());
}

TEST_CASE("scene cooker lowers PsyScript and PsyGraph sources to behavior ops",
          "[scene][scene_file][scene_cook]") {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "psynder_scene_cook_behavior_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "behaviors");

    {
        std::ofstream script{root / "behaviors/spin.psyscript"};
        script << "behavior SpinCrates {\n"
               << "  target_group \"crates\"\n"
               << "  update { transform.spin(axis = [0, 1, 0], speed = linear_index(0.35, 0.12), "
                  "phase = 0.0) }\n"
               << "  update { transform.translate(axis = [0, 1, 0], amount = linear_index(0.1, "
                  "0.25)) }\n"
               << "}\n";
    }
    {
        std::ofstream graph{root / "behaviors/spin.psygraph.json"};
        graph << "{"
              << "\"name\":\"SpinCratesGraph\","
              << "\"targetGroup\":\"crates\","
              << "\"nodes\":[{\"op\":\"spin\",\"axis\":[0,1,0],"
              << "\"speed\":{\"type\":\"linearIndex\",\"base\":0.5,\"step\":0.25}},"
              << "{\"op\":\"translate\",\"axis\":[0,1,0],"
              << "\"amount\":{\"type\":\"linearIndex\",\"base\":0.1,\"step\":0.25}}],"
              << "\"links\":[]"
              << "}";
    }
    const auto cook_with_source = [&](const char* source, const char* out_name) {
        const std::filesystem::path scene_json = root / out_name;
        const std::filesystem::path output = root / (std::string{out_name} + ".bin");
        {
            std::ofstream scene{scene_json};
            scene << "{"
                  << "\"version\":1,"
                  << "\"entityBehaviorSources\":[\"" << source << "\"]"
                  << "}";
        }
        tools::SceneCookStats stats{};
        std::string error;
        REQUIRE(tools::cook_psyscene_json_file(scene_json, output, &stats, &error));
        REQUIRE(stats.behavior_spin_ops == 1u);
        REQUIRE(stats.behavior_translate_ops == 1u);
    };

    cook_with_source("behaviors/spin.psyscript", "script.psyscene.json");
    cook_with_source("behaviors/spin.psygraph.json", "graph.psyscene.json");

    std::filesystem::remove_all(root);
}

TEST_CASE("scene save roundtrips authoring metadata through cooked v1",
          "[scene][scene_file][editor]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    scene::Scene authored{registry};
    authored.environment().set_clear_color(0xFF203040u);
    authored.environment().set_clear_enabled(true, false);

    scene::LocalTransform parent_transform{};
    parent_transform.translation = {10.0f, 0.0f, 0.0f};
    const Entity parent = authored.create_entity(parent_transform);
    REQUIRE(parent.valid());
    REQUIRE(authored.set_entity_name(parent, "Parent Group"));
    const scene::SceneNode parent_node = authored.node(parent);
    REQUIRE(parent_node.valid());

    scene::CameraDesc camera_desc{};
    camera_desc.position = {0.0f, 2.0f, 6.0f};
    camera_desc.look_at = {0.0f, 1.0f, 0.0f};
    camera_desc.near_z = 0.25f;
    camera_desc.far_z = 250.0f;
    camera_desc.tile_w = 160u;
    camera_desc.tile_h = 120u;
    const Entity camera = authored.spawn_camera(camera_desc, parent_node);
    REQUIRE(camera.valid());
    REQUIRE(authored.set_entity_name(camera, "Editor Camera"));

    scene::LocalTransform light_transform{};
    light_transform.translation = {0.0f, 4.0f, -2.0f};
    const Entity light_entity = authored.create_entity(light_transform, parent_node);
    REQUIRE(light_entity.valid());
    REQUIRE(authored.set_entity_name(light_entity, "Key Light"));
    scene::LightComponent light{};
    light.kind = scene::LightKind::Spot;
    light.color_rgba8 = 0xFFFFCC88u;
    light.intensity = 9.5f;
    light.range = 24.0f;
    light.inner_cone_deg = 18.0f;
    light.outer_cone_deg = 44.0f;
    light.casts_shadow = 1u;
    REQUIRE(authored.attach_light(light_entity, light));

    render::MaterialDesc material_desc{};
    material_desc.albedo_rgba8 = 0xFFAA7733u;
    material_desc.reflectivity = 0.42f;
    material_desc.roughness = 0.31f;
    material_desc.emissive = 0.125f;
    material_desc.blend = render::MaterialBlendMode::AlphaBlend;
    material_desc.shadow_opacity = 0.66f;
    const render::MaterialId material = authored.materials().create(material_desc);
    REQUIRE(material.valid());

    const render::MeshId mesh{77u};
    scene::LocalTransform mesh_transform{};
    mesh_transform.translation = {1.0f, 2.0f, 3.0f};
    const scene::RenderableComponent renderable =
        scene::make_renderable(scene::GeometryKind::Mesh,
                               mesh.raw,
                               material,
                               math::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
                               scene::ObjectMobility::Static,
                               scene::RenderableFlags::Visible |
                                   scene::RenderableFlags::CastsShadowOverride);
    const Entity mesh_entity = authored.create_renderable(renderable, mesh_transform, parent_node);
    REQUIRE(mesh_entity.valid());
    REQUIRE(authored.set_entity_name(mesh_entity, "Hero Box Entity"));

    SaveRoundtripHooks hook_user{.mesh_entity = mesh_entity};
    const scene::SceneFileSaveHooks hooks{
        .user = &hook_user,
        .mesh_name = &save_roundtrip_mesh_name,
        .material_name = &save_roundtrip_material_name,
        .material_base_color_texture_name = nullptr,
        .material_preset_name = &save_roundtrip_material_preset_name,
        .mesh_instance_group_name = &save_roundtrip_group_name,
    };

    scene::detail::AlignedVector<u8> bytes;
    scene::SceneFileSaveStats stats{};
    std::string error;
    REQUIRE(scene::save_scene_file(authored, hooks, bytes, &stats, &error));
    REQUIRE(error.empty());
    REQUIRE(stats.cameras == 1u);
    REQUIRE(stats.mesh_instances == 1u);
    REQUIRE(stats.lights == 1u);
    REQUIRE(stats.materials == 1u);
    REQUIRE(stats.material_preset_names == 1u);
    REQUIRE(stats.mesh_instance_group_names == 1u);
    REQUIRE(stats.parented_cameras == 1u);
    REQUIRE(stats.parented_mesh_instances == 1u);
    REQUIRE(stats.parented_lights == 1u);
    REQUIRE(stats.baked_world_transforms == 3u);
    REQUIRE(stats.authoring_nodes == 4u);

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(view.environments.size() == 1u);
    REQUIRE(view.environments[0].clear_color_rgba8 == 0xFF203040u);
    REQUIRE(view.environments[0].clear_color == 1u);
    REQUIRE(view.environments[0].clear_depth == 0u);
    REQUIRE(view.cameras.size() == 1u);
    REQUIRE(view.mesh_instances.size() == 1u);
    REQUIRE(view.lights.size() == 1u);
    REQUIRE(view.object_names.size() == 3u);
    REQUIRE(view.authoring_nodes.size() == 4u);
    REQUIRE(view.materials.size() == 1u);
    REQUIRE(view.authoring_nodes[0].kind == scene::SceneFileObjectKind::Empty);
    REQUIRE(view.authoring_nodes[0].parent_index == scene::kSceneFileAuthoringRoot);
    REQUIRE(std::string_view{scene::scene_file_string(view, view.authoring_nodes[0].name_offset)} ==
            "Parent Group");
    REQUIRE(view.authoring_nodes[1].parent_index == 0u);
    REQUIRE(view.authoring_nodes[2].parent_index == 0u);
    REQUIRE(view.authoring_nodes[3].parent_index == 0u);
    REQUIRE(view.object_names[0].kind == scene::SceneFileObjectKind::Camera);
    REQUIRE(view.object_names[0].object_index == 0u);
    REQUIRE(std::string_view{scene::scene_file_string(view, view.object_names[0].name_offset)} ==
            "Editor Camera");
    REQUIRE(view.object_names[1].kind == scene::SceneFileObjectKind::Light);
    REQUIRE(view.object_names[1].object_index == 0u);
    REQUIRE(std::string_view{scene::scene_file_string(view, view.object_names[1].name_offset)} ==
            "Key Light");
    REQUIRE(view.object_names[2].kind == scene::SceneFileObjectKind::MeshInstance);
    REQUIRE(view.object_names[2].object_index == 0u);
    REQUIRE(std::string_view{scene::scene_file_string(view, view.object_names[2].name_offset)} ==
            "Hero Box Entity");
    REQUIRE(std::string_view{scene::scene_file_string(view, view.mesh_instances[0].mesh_name_offset)} ==
            "mesh.box");
    REQUIRE(std::string_view{
                scene::scene_file_string(view, view.mesh_instances[0].material_name_offset)} ==
            "preset.clay");
    REQUIRE(std::string_view{scene::scene_file_string(view, view.mesh_instances[0].group_name_offset)} ==
            "Hero Box");
    REQUIRE(view.materials[0].albedo_rgba8 == material_desc.albedo_rgba8);
    REQUIRE(std::abs(view.materials[0].reflectivity - material_desc.reflectivity) < 0.0001f);
    REQUIRE(std::abs(view.materials[0].roughness - material_desc.roughness) < 0.0001f);
    REQUIRE(view.materials[0].blend == render::MaterialBlendMode::AlphaBlend);

    const scene::LocalTransform saved_mesh_transform =
        scene::scene_file_transform(view, view.mesh_instances[0].transform_index);
    REQUIRE(std::abs(saved_mesh_transform.translation.x - 11.0f) < 0.0001f);
    REQUIRE(std::abs(saved_mesh_transform.translation.y - 2.0f) < 0.0001f);
    REQUIRE(std::abs(saved_mesh_transform.translation.z - 3.0f) < 0.0001f);
    const scene::SceneFileLight& saved_light = view.lights[0];
    REQUIRE(saved_light.kind == scene::LightKind::Spot);
    REQUIRE(saved_light.color_rgba8 == light.color_rgba8);
    REQUIRE(std::abs(saved_light.intensity - light.intensity) < 0.0001f);
    REQUIRE(std::abs(saved_light.range - light.range) < 0.0001f);
    REQUIRE(saved_light.casts_shadow == 1u);
    const scene::LocalTransform saved_light_transform =
        scene::scene_file_transform(view, saved_light.transform_index);
    REQUIRE(std::abs(saved_light_transform.translation.x - 10.0f) < 0.0001f);
    REQUIRE(std::abs(saved_light_transform.translation.y - 4.0f) < 0.0001f);
    REQUIRE(std::abs(saved_light_transform.translation.z + 2.0f) < 0.0001f);

    RegistryReset reload_reset;
    auto& reload_registry = scene::EcsRegistry::Get();
    reload_registry.set_structural_deferred(false);
    scene::Scene loaded{reload_registry};
    loaded.bind_mesh_spawner(nullptr, nullptr, test_spawn_mesh_instance);
    const scene::SceneMeshBinding mesh_binding{
        .mesh_name = "mesh.box",
        .mesh = mesh,
        .material = {},
    };
    const scene::SceneMaterialBinding material_binding{
        .material_name = "preset.clay",
        .material = render::MaterialId{5u},
    };
    Entity loaded_mesh{};
    const scene::SceneFileInstantiateResult instantiate = scene::instantiate_scene_file(
        loaded,
        view,
        std::span<const scene::SceneMeshBinding>{&mesh_binding, 1u},
        std::span<const scene::SceneMaterialBinding>{&material_binding, 1u},
        std::span<Entity>{&loaded_mesh, 1u});
    REQUIRE(instantiate.cameras == 1u);
    REQUIRE(instantiate.mesh_instances == 1u);
    REQUIRE(instantiate.lights == 1u);
    REQUIRE(instantiate.missing_mesh_bindings == 0u);
    REQUIRE(instantiate.missing_material_bindings == 0u);
    REQUIRE(loaded_mesh.valid());
    REQUIRE(loaded.environment().settings().clear_color_rgba8 == 0xFF203040u);
    REQUIRE(!loaded.environment().settings().clear_depth);
    REQUIRE(loaded.entity_name(loaded.active_camera_entity()) == "Editor Camera");
    REQUIRE(loaded.entity_name(loaded_mesh) == "Hero Box Entity");
    const scene::SceneNode loaded_parent = loaded.graph().parent(loaded.node(loaded_mesh));
    REQUIRE(loaded_parent.valid());
    Entity loaded_parent_entity{};
    const u32 live_count = reload_registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> live_entities(live_count);
    reload_registry.snapshot_live_entities(live_entities);
    for (Entity entity : live_entities) {
        if (loaded.node(entity) == loaded_parent)
            loaded_parent_entity = entity;
    }
    REQUIRE(loaded_parent_entity.valid());
    REQUIRE(loaded.entity_name(loaded_parent_entity) == "Parent Group");
    REQUIRE(loaded.graph().parent(loaded.node(loaded.active_camera_entity())) == loaded_parent);

    const auto* loaded_renderable = loaded.registry().get<scene::RenderableComponent>(loaded_mesh);
    REQUIRE(loaded_renderable != nullptr);
    REQUIRE(loaded_renderable->material.raw == material_binding.material.raw);
    const scene::CachedSceneGroup hero_group = loaded.cache_group(loaded.group_id("Hero Box"));
    REQUIRE(hero_group.size() == 1u);
    REQUIRE(hero_group.entities()[0] == loaded_mesh);

    std::vector<scene::SceneLightItem> loaded_lights;
    loaded.update_transforms();
    loaded.gather_lights(loaded_lights);
    REQUIRE(loaded_lights.size() == 1u);
    REQUIRE(loaded_lights[0].kind == scene::LightKind::Spot);
    REQUIRE(loaded.entity_name(loaded_lights[0].entity) == "Key Light");
    REQUIRE(loaded_lights[0].color_rgba8 == light.color_rgba8);
    REQUIRE(std::abs(loaded_lights[0].intensity - light.intensity) < 0.0001f);
    REQUIRE(std::abs(loaded_lights[0].range - light.range) < 0.0001f);
    REQUIRE(loaded_lights[0].casts_shadow);
}

TEST_CASE("scene save roundtrips render settings through the SRND chunk",
          "[scene][scene_file][render_settings]") {
    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    registry.set_structural_deferred(false);

    // A freshly-built scene defaults to Raster + sun disabled so older scenes
    // and goldens are unaffected.
    scene::Scene defaults{registry};
    REQUIRE(defaults.render_settings().render_mode == scene::RenderMode::Raster);
    REQUIRE(defaults.render_settings().sun_enabled == 0u);

    scene::Scene authored{registry};
    scene::RenderSettings rs{};
    rs.render_mode = scene::RenderMode::Raytraced;
    rs.sun_enabled = 1u;
    rs.sun_direction = math::Vec3{0.25f, -0.5f, 0.75f};
    rs.sun_color_rgba8 = 0xFF8040C0u;
    rs.sun_intensity = 2.5f;
    rs.ambient_color_rgba8 = 0xFF203040u;
    rs.ambient_intensity = 0.6f;
    rs.shadows_enabled = 0u;
    rs.shadow_softness = 0.33f;
    rs.shadow_opacity = 0.88f;
    rs.rt_trace_downscale = 3u;
    rs.rt_ao = 0u;
    rs.rt_reflection_bounces = 4u;
    rs.rt_samples = 8u;
    authored.set_render_settings(rs);

    const scene::SceneFileSaveHooks hooks{};
    scene::detail::AlignedVector<u8> bytes;
    std::string error;
    REQUIRE(scene::save_scene_file(authored, hooks, bytes, nullptr, &error));
    REQUIRE(error.empty());

    scene::SceneFileView view{};
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(view.header != nullptr);
    REQUIRE(view.header->version == scene::kPsySceneVersion);
    REQUIRE(view.render_settings.size() == 1u);
    const scene::SceneFileRenderSettings& cooked = view.render_settings[0];
    REQUIRE(cooked.render_mode == static_cast<u8>(scene::RenderMode::Raytraced));
    REQUIRE(cooked.sun_enabled == 1u);
    REQUIRE(cooked.shadows_enabled == 0u);
    REQUIRE(cooked.rt_ao == 0u);
    REQUIRE(std::abs(cooked.sun_direction.x - 0.25f) < 0.0001f);
    REQUIRE(std::abs(cooked.sun_direction.y + 0.5f) < 0.0001f);
    REQUIRE(std::abs(cooked.sun_direction.z - 0.75f) < 0.0001f);
    REQUIRE(cooked.sun_color_rgba8 == 0xFF8040C0u);
    REQUIRE(cooked.ambient_color_rgba8 == 0xFF203040u);
    REQUIRE(cooked.rt_trace_downscale == 3u);
    REQUIRE(cooked.rt_reflection_bounces == 4u);
    REQUIRE(cooked.rt_samples == 8u);

    RegistryReset reload_reset;
    auto& reload_registry = scene::EcsRegistry::Get();
    reload_registry.set_structural_deferred(false);
    scene::Scene loaded{reload_registry};
    const scene::SceneFileInstantiateResult instantiate =
        scene::instantiate_scene_file(loaded, view, {});
    (void)instantiate;
    const scene::RenderSettings& got = loaded.render_settings();
    REQUIRE(got.render_mode == scene::RenderMode::Raytraced);
    REQUIRE(got.sun_enabled == 1u);
    REQUIRE(std::abs(got.sun_direction.x - 0.25f) < 0.0001f);
    REQUIRE(std::abs(got.sun_direction.y + 0.5f) < 0.0001f);
    REQUIRE(std::abs(got.sun_direction.z - 0.75f) < 0.0001f);
    REQUIRE(got.sun_color_rgba8 == 0xFF8040C0u);
    REQUIRE(std::abs(got.sun_intensity - 2.5f) < 0.0001f);
    REQUIRE(got.ambient_color_rgba8 == 0xFF203040u);
    REQUIRE(std::abs(got.ambient_intensity - 0.6f) < 0.0001f);
    REQUIRE(got.shadows_enabled == 0u);
    REQUIRE(std::abs(got.shadow_softness - 0.33f) < 0.0001f);
    REQUIRE(std::abs(got.shadow_opacity - 0.88f) < 0.0001f);
    REQUIRE(got.rt_trace_downscale == 3u);
    REQUIRE(got.rt_ao == 0u);
    REQUIRE(got.rt_reflection_bounces == 4u);
    REQUIRE(got.rt_samples == 8u);
}
