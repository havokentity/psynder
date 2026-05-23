// SPDX-License-Identifier: MIT
// Psynder — cooked scene file validation.

#include <catch2/catch_test_macros.hpp>

#include "scene/SceneFile.h"
#include "scene/EcsRegistry_Internal.h"

#include <cstring>
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
    constexpr char strings[] = "\0crateCube\0";
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

    std::vector<u8> bytes(sizeof(scene::SceneFileHeader) +
                          7u * sizeof(scene::SceneFileChunk));
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
    REQUIRE(std::string_view{scene::scene_file_string(view, 1u)} == "crateCube");

    RegistryReset reset;
    auto& registry = scene::EcsRegistry::Get();
    scene::Scene scene_ref{registry};
    scene_ref.bind_mesh_spawner(nullptr, nullptr, test_spawn_mesh_instance);
    const render::MaterialId material{1u};
    const scene::SceneMeshBinding binding{.mesh_name = "crateCube",
                                          .mesh = render::MeshId{1u},
                                          .material = material};
    Entity mesh_entity{};
    const scene::SceneFileInstantiateResult result =
        scene::instantiate_scene_file(scene_ref,
                                      view,
                                      std::span<const scene::SceneMeshBinding>{&binding, 1u},
                                      std::span<Entity>{&mesh_entity, 1u});

    REQUIRE(result.cameras == 1u);
    REQUIRE(result.mesh_instances == 1u);
    REQUIRE(result.missing_mesh_bindings == 0u);
    REQUIRE(mesh_entity.valid());
    REQUIRE(scene_ref.active_camera_entity().valid());
}
