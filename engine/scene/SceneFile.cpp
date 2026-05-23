// SPDX-License-Identifier: MIT
// Psynder — cooked scene file parser/instantiator.

#include "scene/SceneFile.h"

#include "core/Log.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace psynder::scene {
namespace scene_file_detail {

struct LoadState {
    mutable std::mutex mutex;
    SceneFileRequest::State state = SceneFileRequest::State::Idle;
    SceneFileLoaded loaded{};
    std::string error{};
};

struct LoadPayload {
    std::shared_ptr<LoadState> state;
};

}  // namespace scene_file_detail

namespace {

template <class T>
[[nodiscard]] const T* as_array(std::span<const u8> bytes,
                                const SceneFileChunk& chunk,
                                u32 expected_stride,
                                std::string* error) {
    if (chunk.stride != expected_stride) {
        if (error)
            *error = "psyscene: chunk stride mismatch";
        return nullptr;
    }
    if ((chunk.offset % kPsySceneAlignment) != 0u) {
        if (error)
            *error = "psyscene: chunk offset is not cache-line aligned";
        return nullptr;
    }
    if (chunk.offset > bytes.size() || chunk.bytes > bytes.size() - chunk.offset) {
        if (error)
            *error = "psyscene: chunk range outside file";
        return nullptr;
    }
    if ((chunk.bytes % sizeof(T)) != 0u) {
        if (error)
            *error = "psyscene: chunk byte count is not a whole array";
        return nullptr;
    }
    const auto* ptr = bytes.data() + chunk.offset;
    if ((reinterpret_cast<usize>(ptr) % alignof(T)) != 0u) {
        if (error)
            *error = "psyscene: chunk pointer is not type aligned";
        return nullptr;
    }
    return reinterpret_cast<const T*>(ptr);
}

[[nodiscard]] const SceneFileChunk* find_chunk(std::span<const SceneFileChunk> chunks,
                                               SceneFileChunkType type) noexcept {
    for (const SceneFileChunk& chunk : chunks) {
        if (chunk.type == type)
            return &chunk;
    }
    return nullptr;
}

[[nodiscard]] bool copy_blob(asset::Blob blob, detail::AlignedVector<u8>& out) {
    out.clear();
    if (!blob.data || blob.bytes == 0u)
        return false;
    out.resize(blob.bytes);
    std::memcpy(out.data(), blob.data, blob.bytes);
    return true;
}

[[nodiscard]] ::psynder::render::MaterialId binding_material(
    const SceneMeshBinding& binding) noexcept {
    return binding.material;
}

}  // namespace

bool parse_scene_file(std::span<const u8> bytes, SceneFileView& out, std::string* error) {
    out = {};
    if (bytes.size() < sizeof(SceneFileHeader)) {
        if (error)
            *error = "psyscene: file is smaller than header";
        return false;
    }

    const auto* header = reinterpret_cast<const SceneFileHeader*>(bytes.data());
    if (header->magic != kPsySceneMagic) {
        if (error)
            *error = "psyscene: bad magic";
        return false;
    }
    if (header->version != kPsySceneVersion) {
        if (error)
            *error = "psyscene: unsupported version";
        return false;
    }
    if (header->header_bytes != sizeof(SceneFileHeader)) {
        if (error)
            *error = "psyscene: header size mismatch";
        return false;
    }
    if (static_cast<usize>(header->file_bytes) != bytes.size()) {
        if (error)
            *error = "psyscene: file size mismatch";
        return false;
    }

    const usize chunk_table_bytes =
        static_cast<usize>(header->chunk_count) * sizeof(SceneFileChunk);
    if (sizeof(SceneFileHeader) + chunk_table_bytes > bytes.size()) {
        if (error)
            *error = "psyscene: truncated chunk table";
        return false;
    }

    const auto* chunk_ptr =
        reinterpret_cast<const SceneFileChunk*>(bytes.data() + sizeof(SceneFileHeader));
    const std::span<const SceneFileChunk> chunks{chunk_ptr,
                                                 static_cast<usize>(header->chunk_count)};

    const auto load_span = [&](SceneFileChunkType type,
                               u32 stride,
                               auto& span,
                               std::string_view name) -> bool {
        using SpanT = std::remove_reference_t<decltype(span)>;
        using ValueT = typename SpanT::value_type;
        const SceneFileChunk* chunk = find_chunk(chunks, type);
        if (!chunk) {
            span = {};
            return true;
        }
        const ValueT* data = as_array<ValueT>(bytes, *chunk, stride, error);
        if (!data) {
            if (error && !error->empty()) {
                *error += " (";
                *error += name;
                *error += ")";
            }
            return false;
        }
        span = std::span<const ValueT>{data, static_cast<usize>(chunk->bytes) / sizeof(ValueT)};
        return true;
    };

    if (!load_span(SceneFileChunkType::Strings, 1u, out.strings, "strings") ||
        !load_span(SceneFileChunkType::Environment,
                   sizeof(SceneFileEnvironment),
                   out.environments,
                   "environment") ||
        !load_span(SceneFileChunkType::TransformTranslation,
                   sizeof(math::Vec3),
                   out.translations,
                   "translations") ||
        !load_span(SceneFileChunkType::TransformRotation,
                   sizeof(math::Quat),
                   out.rotations,
                   "rotations") ||
        !load_span(SceneFileChunkType::TransformScale, sizeof(math::Vec3), out.scales, "scales") ||
        !load_span(SceneFileChunkType::Cameras, sizeof(SceneFileCamera), out.cameras, "cameras") ||
        !load_span(SceneFileChunkType::MeshInstances,
                   sizeof(SceneFileMeshInstance),
                   out.mesh_instances,
                   "mesh_instances")) {
        return false;
    }

    if (out.translations.size() != header->transform_count ||
        out.rotations.size() != header->transform_count ||
        out.scales.size() != header->transform_count ||
        out.cameras.size() != header->camera_count ||
        out.mesh_instances.size() != header->mesh_instance_count) {
        if (error)
            *error = "psyscene: header counts do not match chunk sizes";
        return false;
    }

    out.header = header;
    return true;
}

const char* scene_file_string(const SceneFileView& scene_file, u32 offset) noexcept {
    if (static_cast<usize>(offset) >= scene_file.strings.size())
        return "";
    const char* base = scene_file.strings.data() + offset;
    const usize remaining = scene_file.strings.size() - static_cast<usize>(offset);
    for (usize i = 0; i < remaining; ++i) {
        if (base[i] == '\0')
            return base;
    }
    return "";
}

LocalTransform scene_file_transform(const SceneFileView& scene_file, u32 index) noexcept {
    LocalTransform out{};
    const usize slot = static_cast<usize>(index);
    if (slot >= scene_file.translations.size() || slot >= scene_file.rotations.size() ||
        slot >= scene_file.scales.size())
        return out;
    out.translation = scene_file.translations[slot];
    out.rotation = scene_file.rotations[slot];
    out.scale = scene_file.scales[slot];
    return out;
}

ScenePrewarmConfig scene_file_prewarm_config(const SceneFileView& scene_file) noexcept {
    const u32 mesh_instances = static_cast<u32>(scene_file.mesh_instances.size());
    const u32 cameras = static_cast<u32>(scene_file.cameras.size());
    return {.scene_entities = mesh_instances + cameras,
            .renderables = mesh_instances,
            .cameras = cameras,
            .analytic_spheres = 0u,
            .render_items = mesh_instances,
            .deferred_structural_changes = (mesh_instances + cameras) * 3u};
}

SceneFileInstantiateResult instantiate_scene_file(Scene& scene,
                                                  const SceneFileView& scene_file,
                                                  std::span<const SceneMeshBinding> mesh_bindings,
                                                  std::span<Entity> out_mesh_entities) {
    SceneFileInstantiateResult result{};
    if (!scene_file.environments.empty()) {
        const SceneFileEnvironment& e = scene_file.environments[0];
        scene.environment().set_clear_color(e.clear_color_rgba8);
        scene.environment().set_clear_enabled(e.clear_color != 0u, e.clear_depth != 0u);
    }

    for (const SceneFileCamera& camera_file : scene_file.cameras) {
        CameraDesc camera{};
        camera.position = scene_file_transform(scene_file, camera_file.transform_index).translation;
        camera.look_at = camera_file.look_at;
        camera.up = camera_file.up;
        camera.fov_y_rad = camera_file.fov_y_rad;
        camera.near_z = camera_file.near_z;
        camera.far_z = camera_file.far_z;
        camera.tile_w = camera_file.tile_w;
        camera.tile_h = camera_file.tile_h;
        camera.active = camera_file.active != 0u;
        if (scene.spawn_camera(camera).valid())
            ++result.cameras;
    }

    const usize writable = out_mesh_entities.size();
    for (usize i = 0; i < scene_file.mesh_instances.size(); ++i) {
        const SceneFileMeshInstance& mesh_file = scene_file.mesh_instances[i];
        const std::string_view mesh_name{scene_file_string(scene_file, mesh_file.mesh_name_offset)};
        const auto it = std::find_if(mesh_bindings.begin(),
                                     mesh_bindings.end(),
                                     [&](const SceneMeshBinding& binding) {
                                         return binding.mesh_name == mesh_name;
                                     });
        if (it == mesh_bindings.end() || !it->mesh.valid()) {
            ++result.missing_mesh_bindings;
            continue;
        }
        const Entity entity = scene.spawn_mesh_instance(it->mesh,
                                                        binding_material(*it),
                                                        scene_file_transform(scene_file,
                                                                             mesh_file.transform_index),
                                                        kInvalidSceneNode,
                                                        mesh_file.flags,
                                                        mesh_file.mobility);
        if (entity.valid()) {
            if (i < writable)
                out_mesh_entities[i] = entity;
            ++result.mesh_instances;
        }
    }
    return result;
}

void SceneFileRequest::load_async(std::string_view virtual_path) {
    auto state = std::make_shared<scene_file_detail::LoadState>();
    {
        std::scoped_lock lock{state->mutex};
        state->state = State::Loading;
        state->loaded = {};
        state->error.clear();
    }
    auto* payload = new scene_file_detail::LoadPayload{state};
    state_ = std::move(state);
    asset::Vault::Get().read_async(virtual_path, &SceneFileRequest::on_loaded, payload);
}

SceneFileRequest::State SceneFileRequest::state() const {
    if (!state_)
        return State::Idle;
    std::scoped_lock lock{state_->mutex};
    return state_->state;
}

std::string SceneFileRequest::error() const {
    if (!state_)
        return {};
    std::scoped_lock lock{state_->mutex};
    return state_->error;
}

bool SceneFileRequest::consume(SceneFileLoaded& out) {
    if (!state_)
        return false;
    std::scoped_lock lock{state_->mutex};
    if (state_->state != State::Ready)
        return false;
    out = std::move(state_->loaded);
    state_->state = State::Idle;
    return true;
}

void SceneFileRequest::on_loaded(asset::Blob blob, void* user) noexcept {
    std::unique_ptr<scene_file_detail::LoadPayload> payload(
        static_cast<scene_file_detail::LoadPayload*>(user));
    SceneFileLoaded loaded{};
    std::string error;
    if (!copy_blob(blob, loaded.bytes)) {
        error = "psyscene: async read returned no bytes";
    } else {
        const std::span<const u8> bytes{loaded.bytes.data(), loaded.bytes.size()};
        if (!parse_scene_file(bytes, loaded.view, &error))
            loaded = {};
    }

    std::scoped_lock lock{payload->state->mutex};
    if (error.empty()) {
        payload->state->loaded = std::move(loaded);
        payload->state->state = State::Ready;
    } else {
        payload->state->loaded = {};
        payload->state->error = std::move(error);
        payload->state->state = State::Failed;
        PSY_LOG_ERROR("{}", payload->state->error);
    }
}

}  // namespace psynder::scene
