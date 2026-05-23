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
    const usize aligned =
        ((out.size() + scene::kPsySceneAlignment - 1u) / scene::kPsySceneAlignment) *
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

    std::vector<u8> bytes(sizeof(scene::SceneFileHeader) +
                          9u * sizeof(scene::SceneFileChunk));
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
    const scene::RenderableComponent renderable = scene::make_renderable(
        scene::GeometryKind::Mesh, mesh.raw, material, math::aabb_empty(), mobility, flags);
    return scene_ref.create_renderable(renderable, local, parent);
}

struct RegistryReset {
    RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
    ~RegistryReset() { scene::detail::EcsRegistryImpl::Get().shutdown(); }
};

}  // namespace

TEST_CASE("cooked scene file exposes SoA chunks and instantiates entities", "[scene][scene_file]") {
    const std::vector<u8> bytes = make_scene_blob();
    scene::SceneFileView view{};
    std::string error;
    REQUIRE(scene::parse_scene_file(std::span<const u8>{bytes.data(), bytes.size()}, view, &error));
    REQUIRE(view.translations.size() == 2u);
    REQUIRE(view.cameras.size() == 1u);
    REQUIRE(view.mesh_instances.size() == 1u);
    REQUIRE(view.materials.size() == 1u);
    REQUIRE(view.behavior_spin_ops.size() == 1u);
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
    const scene::SceneFileInstantiateResult result =
        scene::instantiate_scene_file(scene_ref,
                                      view,
                                      std::span<const scene::SceneMeshBinding>{&binding, 1u},
                                      std::span<const scene::SceneMaterialBinding>{&material_binding, 1u},
                                      std::span<Entity>{&mesh_entity, 1u});

    REQUIRE(result.cameras == 1u);
    REQUIRE(result.mesh_instances == 1u);
    REQUIRE(result.missing_mesh_bindings == 0u);
    REQUIRE(result.missing_material_bindings == 0u);
    REQUIRE(mesh_entity.valid());
    REQUIRE(scene_ref.spin_behavior_count() == 1u);
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
               << "  update { transform.spin(axis = [0, 1, 0], speed = linear_index(0.35, 0.12), phase = 0.0) }\n"
               << "}\n";
    }
    {
        std::ofstream graph{root / "behaviors/spin.psygraph.json"};
        graph << "{"
              << "\"name\":\"SpinCratesGraph\","
              << "\"targetGroup\":\"crates\","
              << "\"nodes\":[{\"op\":\"spin\",\"axis\":[0,1,0],"
              << "\"speed\":{\"type\":\"linearIndex\",\"base\":0.5,\"step\":0.25}}],"
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
    };

    cook_with_source("behaviors/spin.psyscript", "script.psyscene.json");
    cook_with_source("behaviors/spin.psygraph.json", "graph.psyscene.json");

    std::filesystem::remove_all(root);
}
