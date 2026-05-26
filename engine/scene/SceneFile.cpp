// SPDX-License-Identifier: MIT
// Psynder — cooked scene file parser/instantiator.

#include "scene/SceneFile.h"

#include "core/Log.h"
#include "math/MathExt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

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

struct ScopedImmediateStructural {
    explicit ScopedImmediateStructural(Scene& scene) noexcept
        : scene(&scene)
        , restore_deferred(scene.structural_deferred()) {
        scene.set_structural_deferred(false);
    }

    ~ScopedImmediateStructural() {
        if (scene)
            scene->set_structural_deferred(restore_deferred);
    }

    Scene* scene = nullptr;
    bool restore_deferred = false;
};

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

[[nodiscard]] ::psynder::render::MaterialId resolve_scene_material(
    std::string_view material_name,
    const SceneMeshBinding& mesh_binding,
    std::span<const SceneMaterialBinding> material_bindings,
    bool& missing_material) noexcept {
    if (material_name.empty())
        return binding_material(mesh_binding);

    const auto it = std::find_if(material_bindings.begin(),
                                 material_bindings.end(),
                                 [&](const SceneMaterialBinding& binding) {
                                     return binding.material_name == material_name;
                                 });
    if (it != material_bindings.end() && it->material.valid())
        return it->material;

    missing_material = true;
    return binding_material(mesh_binding);
}

struct SceneFileSaveScratch {
    SceneFileEnvironment environment{};
    std::vector<math::Vec3> translations;
    std::vector<math::Quat> rotations;
    std::vector<math::Vec3> scales;
    std::vector<SceneFileCamera> cameras;
    std::vector<SceneFileMeshInstance> mesh_instances;
    std::vector<SceneFileLight> lights;
    std::vector<SceneFileObjectName> object_names;
    std::vector<SceneFileMaterial> materials;
    std::vector<SceneFileBehaviorSpinOp> behavior_spin_ops;
    std::vector<SceneFileBehaviorTranslateOp> behavior_translate_ops;
    std::vector<SceneFileAuthoringNode> authoring_nodes;
    std::vector<SceneFileGameplayEntity> gameplay_entities;
    std::vector<char> strings{'\0'};
    std::unordered_map<std::string, u32> string_offsets;
    std::unordered_map<u32, u32> material_slots;
    std::unordered_map<u32, SceneFileObjectName> object_refs;
};

constexpr u32 kSceneFileGameplayTagMask = 1u << 0u;
constexpr u32 kSceneFilePlayerControllerMask = 1u << 1u;
constexpr u32 kSceneFileHealthMask = 1u << 2u;
constexpr u32 kSceneFileWeaponMask = 1u << 3u;

[[nodiscard]] bool fail_save(std::string* error, std::string_view message) {
    if (error)
        error->assign(message.data(), message.size());
    return false;
}

[[nodiscard]] bool fail_save_missing_mesh_name(std::string* error,
                                               ::psynder::render::MeshId mesh,
                                               bool hook_present) {
    if (error) {
        *error = hook_present ? "psyscene save: mesh_name hook returned empty for mesh "
                              : "psyscene save: mesh_name hook is required for mesh ";
        *error += std::to_string(mesh.raw);
    }
    return false;
}

[[nodiscard]] bool fits_u32(usize value) noexcept {
    return value <= static_cast<usize>(std::numeric_limits<u32>::max());
}

[[nodiscard]] bool add_save_string(SceneFileSaveScratch& scene,
                                   std::string_view value,
                                   u32& out_offset,
                                   std::string* error) {
    if (value.empty()) {
        out_offset = 0u;
        return true;
    }

    const std::string key{value};
    if (const auto it = scene.string_offsets.find(key); it != scene.string_offsets.end()) {
        out_offset = it->second;
        return true;
    }

    const usize next_size = scene.strings.size() + key.size() + 1u;
    if (!fits_u32(next_size))
        return fail_save(error, "psyscene save: string table exceeds 4 GiB");

    out_offset = static_cast<u32>(scene.strings.size());
    scene.strings.insert(scene.strings.end(), key.begin(), key.end());
    scene.strings.push_back('\0');
    scene.string_offsets.emplace(key, out_offset);
    return true;
}

[[nodiscard]] bool append_save_transform(SceneFileSaveScratch& scene,
                                         const LocalTransform& local,
                                         u32& out_index,
                                         std::string* error) {
    if (scene.translations.size() == static_cast<usize>(std::numeric_limits<u32>::max()))
        return fail_save(error, "psyscene save: too many transforms");

    out_index = static_cast<u32>(scene.translations.size());
    scene.translations.push_back(local.translation);
    scene.rotations.push_back(local.rotation);
    scene.scales.push_back(local.scale);
    return true;
}

[[nodiscard]] bool append_save_object_name(SceneFileSaveScratch& scene,
                                           SceneFileObjectKind kind,
                                           u32 object_index,
                                           EcsRegistry& registry,
                                           Entity entity,
                                           std::string* error) {
    const auto* name = registry.get<EntityNameComponent>(entity);
    if (!name || entity_name_empty(*name))
        return true;

    u32 name_offset = 0u;
    if (!add_save_string(scene, entity_name_view(*name), name_offset, error))
        return false;

    scene.object_names.push_back(SceneFileObjectName{
        .kind = kind,
        .object_index = object_index,
        .name_offset = name_offset,
        .reserved = 0u,
    });
    return true;
}

[[nodiscard]] std::string_view scene_file_object_name(const SceneFileView& scene_file,
                                                      SceneFileObjectKind kind,
                                                      u32 object_index) noexcept {
    for (const SceneFileObjectName& name : scene_file.object_names) {
        if (name.kind == kind && name.object_index == object_index)
            return scene_file_string(scene_file, name.name_offset);
    }
    return {};
}

[[nodiscard]] bool append_save_authoring_nodes(Scene& scene,
                                               SceneFileSaveScratch& saved,
                                               SceneFileSaveStats& stats,
                                               std::string* error) {
    EcsRegistry& registry = scene.registry();
    const u32 total = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = registry.snapshot_live_entities(entities);
    entities.resize(copied);
    std::sort(entities.begin(), entities.end(), [&](Entity a, Entity b) {
        const SceneNode na = scene.node(a);
        const SceneNode nb = scene.node(b);
        return na.raw < nb.raw;
    });

    std::unordered_map<u32, u32> authoring_index_by_node;
    authoring_index_by_node.reserve(entities.size());
    saved.authoring_nodes.reserve(entities.size());

    for (Entity entity : entities) {
        const auto* node_component = registry.get<SceneNodeComponent>(entity);
        const auto* transform = registry.get<TransformComponent>(entity);
        if (!node_component || !transform || !scene.graph().alive(node_component->node))
            continue;

        u32 transform_index = 0u;
        if (!append_save_transform(saved, transform->local, transform_index, error))
            return false;

        u32 parent_index = kSceneFileAuthoringRoot;
        const SceneNode parent_node = scene.graph().parent(node_component->node);
        if (parent_node.valid()) {
            const auto parent_it = authoring_index_by_node.find(parent_node.raw);
            if (parent_it != authoring_index_by_node.end())
                parent_index = parent_it->second;
        }

        u32 name_offset = 0u;
        if (const auto* name = registry.get<EntityNameComponent>(entity);
            name && !entity_name_empty(*name)) {
            if (!add_save_string(saved, entity_name_view(*name), name_offset, error))
                return false;
        }

        SceneFileObjectKind kind = SceneFileObjectKind::Empty;
        u32 object_index = 0u;
        if (const auto ref_it = saved.object_refs.find(entity.raw);
            ref_it != saved.object_refs.end()) {
            kind = ref_it->second.kind;
            object_index = ref_it->second.object_index;
        }

        const u32 authoring_index = static_cast<u32>(saved.authoring_nodes.size());
        saved.authoring_nodes.push_back(SceneFileAuthoringNode{
            .kind = kind,
            .object_index = object_index,
            .transform_index = transform_index,
            .parent_index = parent_index,
            .name_offset = name_offset,
        });

        SceneFileGameplayEntity gameplay{};
        gameplay.authoring_node_index = authoring_index;
        if (const auto* tag = registry.get<GameplayTagComponent>(entity)) {
            const GameplayTagComponent sanitized = sanitize_gameplay_tag(*tag);
            gameplay.component_mask |= kSceneFileGameplayTagMask;
            gameplay.role = sanitized.role;
            gameplay.flags = sanitized.flags;
        }
        if (const auto* controller = registry.get<PlayerControllerComponent>(entity)) {
            gameplay.component_mask |= kSceneFilePlayerControllerMask;
            gameplay.player_controller = sanitize_player_controller(*controller);
        }
        if (const auto* health = registry.get<HealthComponent>(entity)) {
            gameplay.component_mask |= kSceneFileHealthMask;
            gameplay.health = sanitize_health_component(*health);
        }
        if (const auto* weapon = registry.get<WeaponComponent>(entity)) {
            gameplay.component_mask |= kSceneFileWeaponMask;
            gameplay.weapon = sanitize_weapon_component(*weapon);
        }
        if (gameplay.component_mask != 0u) {
            saved.gameplay_entities.push_back(gameplay);
            ++stats.gameplay_entities;
        }

        authoring_index_by_node[node_component->node.raw] = authoring_index;
        ++stats.authoring_nodes;
    }
    return true;
}

[[nodiscard]] math::Vec3 camera_forward(const LocalTransform& local) noexcept {
    return math::normalize(math::quat_rotate(local.rotation, math::Vec3{0.0f, 0.0f, -1.0f}));
}

[[nodiscard]] math::Vec3 camera_up(const LocalTransform& local) noexcept {
    return math::normalize(math::quat_rotate(local.rotation, math::Vec3{0.0f, 1.0f, 0.0f}));
}

[[nodiscard]] math::Vec3 matrix_column(const math::Mat4& m, u32 column) noexcept {
    const u32 base = column * 4u;
    return {m.m[base + 0u], m.m[base + 1u], m.m[base + 2u]};
}

[[nodiscard]] f32 abs_dot(math::Vec3 a, math::Vec3 b) noexcept {
    return std::fabs(math::dot(a, b));
}

[[nodiscard]] math::Quat quat_from_basis(math::Vec3 x,
                                         math::Vec3 y,
                                         math::Vec3 z) noexcept {
    const f32 m00 = x.x;
    const f32 m01 = y.x;
    const f32 m02 = z.x;
    const f32 m10 = x.y;
    const f32 m11 = y.y;
    const f32 m12 = z.y;
    const f32 m20 = x.z;
    const f32 m21 = y.z;
    const f32 m22 = z.z;
    const f32 trace = m00 + m11 + m22;

    math::Quat q{};
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return math::quat_normalize(q);
}

[[nodiscard]] LocalTransform decompose_world_transform(const math::Mat4& world,
                                                       const LocalTransform& fallback,
                                                       bool& approximate) noexcept {
    constexpr f32 kMinScale = 1.0e-6f;
    constexpr f32 kShearEpsilon = 1.0e-3f;

    LocalTransform out = fallback;
    out.translation = {world.m[12], world.m[13], world.m[14]};

    math::Vec3 x = matrix_column(world, 0u);
    math::Vec3 y = matrix_column(world, 1u);
    math::Vec3 z = matrix_column(world, 2u);
    f32 sx = math::length(x);
    f32 sy = math::length(y);
    f32 sz = math::length(z);

    if (sx <= kMinScale || sy <= kMinScale || sz <= kMinScale) {
        approximate = true;
        out.scale = {sx, sy, sz};
        return out;
    }

    x = math::mul(x, 1.0f / sx);
    y = math::mul(y, 1.0f / sy);
    z = math::mul(z, 1.0f / sz);

    if (math::dot(math::cross(x, y), z) < 0.0f) {
        sz = -sz;
        z = math::mul(z, -1.0f);
    }

    if (abs_dot(x, y) > kShearEpsilon || abs_dot(x, z) > kShearEpsilon ||
        abs_dot(y, z) > kShearEpsilon) {
        approximate = true;
    }

    out.rotation = quat_from_basis(x, y, z);
    out.scale = {sx, sy, sz};
    return out;
}

[[nodiscard]] LocalTransform save_transform_for_node(Scene& scene,
                                                     SceneNode node,
                                                     const LocalTransform& local,
                                                     bool& parented,
                                                     bool& approximate) noexcept {
    parented = scene.graph().parent(node).valid();
    approximate = false;
    if (!parented)
        return local;
    return decompose_world_transform(scene.graph().world_matrix(node), local, approximate);
}

void pad_save_blob(detail::AlignedVector<u8>& out) {
    const usize aligned =
        ((out.size() + kPsySceneAlignment - 1u) / kPsySceneAlignment) * kPsySceneAlignment;
    out.resize(aligned, 0u);
}

[[nodiscard]] bool append_save_bytes(detail::AlignedVector<u8>& out,
                                     const void* data,
                                     usize bytes,
                                     std::string* error) {
    if (bytes > static_cast<usize>(std::numeric_limits<u32>::max()) ||
        out.size() > static_cast<usize>(std::numeric_limits<u32>::max()) - bytes) {
        return fail_save(error, "psyscene save: file exceeds 4 GiB");
    }
    const usize offset = out.size();
    out.resize(offset + bytes);
    if (bytes != 0u)
        std::memcpy(out.data() + offset, data, bytes);
    return true;
}

template <class T>
[[nodiscard]] bool append_save_chunk(detail::AlignedVector<u8>& bytes,
                                     std::vector<SceneFileChunk>& chunks,
                                     SceneFileChunkType type,
                                     std::span<const T> data,
                                     u32 stride,
                                     std::string* error) {
    pad_save_blob(bytes);
    if (!fits_u32(bytes.size()) || !fits_u32(data.size_bytes()))
        return fail_save(error, "psyscene save: chunk exceeds 4 GiB");

    SceneFileChunk chunk{};
    chunk.type = type;
    chunk.offset = static_cast<u32>(bytes.size());
    chunk.bytes = static_cast<u32>(data.size_bytes());
    chunk.stride = stride;
    if (!data.empty() && !append_save_bytes(bytes, data.data(), data.size_bytes(), error))
        return false;
    chunks.push_back(chunk);
    return true;
}

[[nodiscard]] bool write_save_blob(const SceneFileSaveScratch& scene,
                                   detail::AlignedVector<u8>& out,
                                   std::string* error) {
    constexpr usize kSceneFileChunkCount = 14u;
    out.clear();
    out.resize(sizeof(SceneFileHeader) + kSceneFileChunkCount * sizeof(SceneFileChunk));

    std::vector<SceneFileChunk> chunks;
    chunks.reserve(kSceneFileChunkCount);
    if (!append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::Strings,
                           std::span<const char>{scene.strings.data(), scene.strings.size()},
                           1u,
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::Environment,
                           std::span<const SceneFileEnvironment>{&scene.environment, 1u},
                           sizeof(SceneFileEnvironment),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::TransformTranslation,
                           std::span<const math::Vec3>{scene.translations.data(),
                                                       scene.translations.size()},
                           sizeof(math::Vec3),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::TransformRotation,
                           std::span<const math::Quat>{scene.rotations.data(), scene.rotations.size()},
                           sizeof(math::Quat),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::TransformScale,
                           std::span<const math::Vec3>{scene.scales.data(), scene.scales.size()},
                           sizeof(math::Vec3),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::Cameras,
                           std::span<const SceneFileCamera>{scene.cameras.data(), scene.cameras.size()},
                           sizeof(SceneFileCamera),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::MeshInstances,
                           std::span<const SceneFileMeshInstance>{scene.mesh_instances.data(),
                                                                  scene.mesh_instances.size()},
                           sizeof(SceneFileMeshInstance),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::Lights,
                           std::span<const SceneFileLight>{scene.lights.data(), scene.lights.size()},
                           sizeof(SceneFileLight),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::ObjectNames,
                           std::span<const SceneFileObjectName>{scene.object_names.data(),
                                                                scene.object_names.size()},
                           sizeof(SceneFileObjectName),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::Materials,
                           std::span<const SceneFileMaterial>{scene.materials.data(),
                                                              scene.materials.size()},
                           sizeof(SceneFileMaterial),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::BehaviorSpinOps,
                           std::span<const SceneFileBehaviorSpinOp>{scene.behavior_spin_ops.data(),
                                                                    scene.behavior_spin_ops.size()},
                           sizeof(SceneFileBehaviorSpinOp),
                           error) ||
        !append_save_chunk(
            out,
            chunks,
            SceneFileChunkType::BehaviorTranslateOps,
            std::span<const SceneFileBehaviorTranslateOp>{scene.behavior_translate_ops.data(),
                                                          scene.behavior_translate_ops.size()},
            sizeof(SceneFileBehaviorTranslateOp),
            error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::AuthoringNodes,
                           std::span<const SceneFileAuthoringNode>{scene.authoring_nodes.data(),
                                                                   scene.authoring_nodes.size()},
                           sizeof(SceneFileAuthoringNode),
                           error) ||
        !append_save_chunk(out,
                           chunks,
                           SceneFileChunkType::GameplayEntities,
                           std::span<const SceneFileGameplayEntity>{scene.gameplay_entities.data(),
                                                                    scene.gameplay_entities.size()},
                           sizeof(SceneFileGameplayEntity),
                           error)) {
        out.clear();
        return false;
    }

    SceneFileHeader header{};
    header.file_bytes = static_cast<u32>(out.size());
    header.chunk_count = static_cast<u32>(chunks.size());
    header.transform_count = static_cast<u32>(scene.translations.size());
    header.camera_count = static_cast<u32>(scene.cameras.size());
    header.mesh_instance_count = static_cast<u32>(scene.mesh_instances.size());
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), chunks.data(), chunks.size() * sizeof(chunks[0]));
    return true;
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
                   "mesh_instances") ||
        !load_span(SceneFileChunkType::Lights, sizeof(SceneFileLight), out.lights, "lights") ||
        !load_span(SceneFileChunkType::ObjectNames,
                   sizeof(SceneFileObjectName),
                   out.object_names,
                   "object_names") ||
        !load_span(SceneFileChunkType::Materials,
                   sizeof(SceneFileMaterial),
                   out.materials,
                   "materials") ||
        !load_span(SceneFileChunkType::BehaviorSpinOps,
                   sizeof(SceneFileBehaviorSpinOp),
                   out.behavior_spin_ops,
                   "behavior_spin_ops") ||
        !load_span(SceneFileChunkType::BehaviorTranslateOps,
                   sizeof(SceneFileBehaviorTranslateOp),
                   out.behavior_translate_ops,
                   "behavior_translate_ops") ||
        !load_span(SceneFileChunkType::AuthoringNodes,
                   sizeof(SceneFileAuthoringNode),
                   out.authoring_nodes,
                   "authoring_nodes") ||
        !load_span(SceneFileChunkType::GameplayEntities,
                   sizeof(SceneFileGameplayEntity),
                   out.gameplay_entities,
                   "gameplay_entities")) {
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
    const u32 lights = static_cast<u32>(scene_file.lights.size());
    const u32 authoring_nodes = static_cast<u32>(scene_file.authoring_nodes.size());
    const u32 scene_entities = std::max(authoring_nodes, mesh_instances + cameras + lights);
    return {.scene_entities = scene_entities,
            .renderables = mesh_instances,
            .cameras = cameras,
            .lights = lights,
            .analytic_spheres = 0u,
            .render_items = mesh_instances,
            .deferred_structural_changes = scene_entities * 3u};
}

SceneFileInstantiateResult instantiate_scene_file(Scene& scene,
                                                  const SceneFileView& scene_file,
                                                  std::span<const SceneMeshBinding> mesh_bindings,
                                                  std::span<Entity> out_mesh_entities) {
    return instantiate_scene_file(scene, scene_file, mesh_bindings, {}, out_mesh_entities);
}

[[nodiscard]] bool same_saved_transform(const LocalTransform& a,
                                        const LocalTransform& b) noexcept {
    return std::fabs(a.translation.x - b.translation.x) <= 0.0001f &&
           std::fabs(a.translation.y - b.translation.y) <= 0.0001f &&
           std::fabs(a.translation.z - b.translation.z) <= 0.0001f &&
           std::fabs(a.rotation.x - b.rotation.x) <= 0.0001f &&
           std::fabs(a.rotation.y - b.rotation.y) <= 0.0001f &&
           std::fabs(a.rotation.z - b.rotation.z) <= 0.0001f &&
           std::fabs(a.rotation.w - b.rotation.w) <= 0.0001f &&
           std::fabs(a.scale.x - b.scale.x) <= 0.0001f &&
           std::fabs(a.scale.y - b.scale.y) <= 0.0001f &&
           std::fabs(a.scale.z - b.scale.z) <= 0.0001f;
}

[[nodiscard]] bool find_matching_authored_light(const SceneFileView& scene_file,
                                                const SceneFileAuthoringNode& authored,
                                                const LocalTransform& local,
                                                LightComponent& out) {
    std::string_view label = scene_file_string(scene_file, authored.name_offset);
    if (label.empty())
        label = scene_file_object_name(scene_file, authored.kind, authored.object_index);
    if (label.empty())
        return false;

    for (usize i = 0; i < scene_file.lights.size(); ++i) {
        const std::string_view light_label =
            scene_file_object_name(scene_file, SceneFileObjectKind::Light, static_cast<u32>(i));
        if (light_label != label)
            continue;

        const SceneFileLight& light_file = scene_file.lights[i];
        if (!same_saved_transform(local, scene_file_transform(scene_file, light_file.transform_index)))
            continue;

        out = LightComponent{
            .kind = light_file.kind,
            .color_rgba8 = light_file.color_rgba8,
            .intensity = light_file.intensity,
            .range = light_file.range,
            .inner_cone_deg = light_file.inner_cone_deg,
            .outer_cone_deg = light_file.outer_cone_deg,
            .casts_shadow = static_cast<u8>(light_file.casts_shadow != 0u ? 1u : 0u),
        };
        return true;
    }
    return false;
}

void attach_saved_gameplay_components(Scene& scene,
                                       Entity entity,
                                       const SceneFileGameplayEntity& saved) {
    if (!entity.valid())
        return;
    EcsRegistry& registry = scene.registry();
    if ((saved.component_mask & kSceneFileGameplayTagMask) != 0u) {
        GameplayTagComponent tag{};
        tag.role = saved.role;
        tag.flags = saved.flags;
        registry.add<GameplayTagComponent>(entity, sanitize_gameplay_tag(tag));
    }
    if ((saved.component_mask & kSceneFilePlayerControllerMask) != 0u) {
        registry.add<PlayerControllerComponent>(
            entity, sanitize_player_controller(saved.player_controller));
    }
    if ((saved.component_mask & kSceneFileHealthMask) != 0u) {
        registry.add<HealthComponent>(entity, sanitize_health_component(saved.health));
    }
    if ((saved.component_mask & kSceneFileWeaponMask) != 0u) {
        registry.add<WeaponComponent>(entity, sanitize_weapon_component(saved.weapon));
    }
}

[[nodiscard]] SceneFileInstantiateResult instantiate_authoring_scene_file(
    Scene& scene,
    const SceneFileView& scene_file,
    std::span<const SceneMeshBinding> mesh_bindings,
    std::span<const SceneMaterialBinding> material_bindings,
    std::span<Entity> out_mesh_entities) {
    SceneFileInstantiateResult result{};
    std::vector<Entity> entities(scene_file.authoring_nodes.size());

    const usize writable_meshes = out_mesh_entities.size();
    for (usize i = 0; i < scene_file.authoring_nodes.size(); ++i) {
        const SceneFileAuthoringNode& authored = scene_file.authoring_nodes[i];
        const LocalTransform local = scene_file_transform(scene_file, authored.transform_index);
        SceneNode parent_node = kInvalidSceneNode;
        if (authored.parent_index != kSceneFileAuthoringRoot &&
            authored.parent_index < entities.size()) {
            parent_node = scene.node(entities[authored.parent_index]);
        }

        Entity entity{};
        switch (authored.kind) {
            case SceneFileObjectKind::Empty:
                entity = scene.create_entity(local, parent_node);
                break;
            case SceneFileObjectKind::Camera: {
                if (authored.object_index >= scene_file.cameras.size())
                    break;
                const SceneFileCamera& camera_file = scene_file.cameras[authored.object_index];
                CameraComponent camera{};
                camera.fov_y_rad = camera_file.fov_y_rad;
                camera.near_z = camera_file.near_z;
                camera.far_z = camera_file.far_z;
                camera.tile_w = camera_file.tile_w;
                camera.tile_h = camera_file.tile_h;
                camera.active = camera_file.active != 0u ? 1u : 0u;
                entity = scene.create_camera(camera, local, parent_node);
                if (entity.valid())
                    ++result.cameras;
                break;
            }
            case SceneFileObjectKind::Light: {
                if (authored.object_index >= scene_file.lights.size())
                    break;
                const SceneFileLight& light_file = scene_file.lights[authored.object_index];
                entity = scene.create_entity(local, parent_node);
                if (!entity.valid())
                    break;
                LightComponent light{};
                light.kind = light_file.kind;
                light.color_rgba8 = light_file.color_rgba8;
                light.intensity = light_file.intensity;
                light.range = light_file.range;
                light.inner_cone_deg = light_file.inner_cone_deg;
                light.outer_cone_deg = light_file.outer_cone_deg;
                light.casts_shadow = light_file.casts_shadow != 0u ? 1u : 0u;
                if (scene.attach_light(entity, light))
                    ++result.lights;
                break;
            }
            case SceneFileObjectKind::MeshInstance: {
                if (authored.object_index >= scene_file.mesh_instances.size())
                    break;
                const SceneFileMeshInstance& mesh_file =
                    scene_file.mesh_instances[authored.object_index];
                const std::string_view mesh_name{
                    scene_file_string(scene_file, mesh_file.mesh_name_offset)};
                const auto it = std::find_if(mesh_bindings.begin(),
                                             mesh_bindings.end(),
                                             [&](const SceneMeshBinding& binding) {
                                                 return binding.mesh_name == mesh_name;
                                             });
                if (it == mesh_bindings.end() || !it->mesh.valid()) {
                    ++result.missing_mesh_bindings;
                    break;
                }
                bool missing_material = false;
                const std::string_view material_name{
                    scene_file_string(scene_file, mesh_file.material_name_offset)};
                const ::psynder::render::MaterialId material =
                    resolve_scene_material(material_name, *it, material_bindings, missing_material);
                if (missing_material)
                    ++result.missing_material_bindings;
                entity = scene.spawn_mesh_instance(it->mesh,
                                                   material,
                                                   local,
                                                   parent_node,
                                                   mesh_file.flags,
                                                   mesh_file.mobility);
                if (entity.valid()) {
                    LightComponent authored_light{};
                    if (find_matching_authored_light(scene_file, authored, local, authored_light) &&
                        scene.attach_light(entity, authored_light)) {
                        ++result.lights;
                    }
                    const std::string_view group_name{
                        scene_file_string(scene_file, mesh_file.group_name_offset)};
                    scene.add_to_group(group_name, entity, local);
                    if (authored.object_index < writable_meshes)
                        out_mesh_entities[authored.object_index] = entity;
                    ++result.mesh_instances;
                }
                break;
            }
        }

        if (!entity.valid())
            continue;

        entities[i] = entity;
        std::string_view name = scene_file_string(scene_file, authored.name_offset);
        if (name.empty() && authored.kind != SceneFileObjectKind::Empty) {
            name = scene_file_object_name(scene_file, authored.kind, authored.object_index);
        }
        if (!name.empty())
            scene.set_entity_name(entity, name);
    }

    for (const SceneFileGameplayEntity& gameplay : scene_file.gameplay_entities) {
        if (gameplay.authoring_node_index >= entities.size())
            continue;
        attach_saved_gameplay_components(scene, entities[gameplay.authoring_node_index], gameplay);
    }

    return result;
}

SceneFileInstantiateResult instantiate_scene_file(
    Scene& scene,
    const SceneFileView& scene_file,
    std::span<const SceneMeshBinding> mesh_bindings,
    std::span<const SceneMaterialBinding> material_bindings,
    std::span<Entity> out_mesh_entities) {
    SceneFileInstantiateResult result{};
    if (!scene_file.environments.empty()) {
        const SceneFileEnvironment& e = scene_file.environments[0];
        scene.environment().set_clear_color(e.clear_color_rgba8);
        scene.environment().set_clear_enabled(e.clear_color != 0u, e.clear_depth != 0u);
    }

    if (!scene_file.authoring_nodes.empty()) {
        return instantiate_authoring_scene_file(scene,
                                                scene_file,
                                                mesh_bindings,
                                                material_bindings,
                                                out_mesh_entities);
    }

    for (usize camera_index = 0; camera_index < scene_file.cameras.size(); ++camera_index) {
        const SceneFileCamera& camera_file = scene_file.cameras[camera_index];
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
        const Entity entity = scene.spawn_camera(camera);
        if (entity.valid()) {
            const std::string_view name = scene_file_object_name(
                scene_file, SceneFileObjectKind::Camera, static_cast<u32>(camera_index));
            if (!name.empty())
                scene.set_entity_name(entity, name);
            ++result.cameras;
        }
    }

    for (usize light_index = 0; light_index < scene_file.lights.size(); ++light_index) {
        const SceneFileLight& light_file = scene_file.lights[light_index];
        const LocalTransform local = scene_file_transform(scene_file, light_file.transform_index);
        const Entity entity = scene.create_entity(local);
        if (!entity.valid())
            continue;
        LightComponent light{};
        light.kind = light_file.kind;
        light.color_rgba8 = light_file.color_rgba8;
        light.intensity = light_file.intensity;
        light.range = light_file.range;
        light.inner_cone_deg = light_file.inner_cone_deg;
        light.outer_cone_deg = light_file.outer_cone_deg;
        light.casts_shadow = light_file.casts_shadow != 0u ? 1u : 0u;
        if (scene.attach_light(entity, light)) {
            const std::string_view name = scene_file_object_name(
                scene_file, SceneFileObjectKind::Light, static_cast<u32>(light_index));
            if (!name.empty())
                scene.set_entity_name(entity, name);
            ++result.lights;
        }
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
        bool missing_material = false;
        const std::string_view material_name{
            scene_file_string(scene_file, mesh_file.material_name_offset)};
        const ::psynder::render::MaterialId material =
            resolve_scene_material(material_name, *it, material_bindings, missing_material);
        if (missing_material)
            ++result.missing_material_bindings;
        const LocalTransform local = scene_file_transform(scene_file, mesh_file.transform_index);
        const Entity entity = scene.spawn_mesh_instance(it->mesh,
                                                        material,
                                                        local,
                                                        kInvalidSceneNode,
                                                        mesh_file.flags,
                                                        mesh_file.mobility);
        if (entity.valid()) {
            const std::string_view group_name{
                scene_file_string(scene_file, mesh_file.group_name_offset)};
            scene.add_to_group(group_name, entity, local);
            const std::string_view name = scene_file_object_name(
                scene_file, SceneFileObjectKind::MeshInstance, static_cast<u32>(i));
            if (!name.empty())
                scene.set_entity_name(entity, name);
            if (i < writable)
                out_mesh_entities[i] = entity;
            ++result.mesh_instances;
        }
    }

    scene.reserve_spin_behaviors(static_cast<u32>(scene_file.behavior_spin_ops.size()));
    for (const SceneFileBehaviorSpinOp& behavior_file : scene_file.behavior_spin_ops) {
        const std::string_view group_name{
            scene_file_string(scene_file, behavior_file.target_group_name_offset)};
        if (group_name.empty())
            continue;
        scene.add_spin_behavior(SpinEntityBehaviorDesc{
            .target_group = scene.group_id(group_name),
            .axis = behavior_file.axis,
            .speed_base = behavior_file.speed_base,
            .speed_step = behavior_file.speed_step,
            .phase_base = behavior_file.phase_base,
            .phase_step = behavior_file.phase_step,
            .flags = static_cast<EntityBehaviorFlags>(behavior_file.flags),
        });
    }

    scene.reserve_translate_behaviors(static_cast<u32>(scene_file.behavior_translate_ops.size()));
    for (const SceneFileBehaviorTranslateOp& behavior_file : scene_file.behavior_translate_ops) {
        const std::string_view group_name{
            scene_file_string(scene_file, behavior_file.target_group_name_offset)};
        if (group_name.empty())
            continue;
        scene.add_translate_behavior(TranslateEntityBehaviorDesc{
            .target_group = scene.group_id(group_name),
            .axis = behavior_file.axis,
            .amount_base = behavior_file.amount_base,
            .amount_step = behavior_file.amount_step,
            .flags = static_cast<EntityBehaviorFlags>(behavior_file.flags),
        });
    }
    return result;
}

bool save_scene_file(Scene& scene,
                     const SceneFileSaveHooks& hooks,
                     detail::AlignedVector<u8>& out_bytes,
                     SceneFileSaveStats* stats,
                     std::string* error) {
    out_bytes.clear();
    if (error)
        error->clear();

    SceneFileSaveScratch saved{};
    SceneFileSaveStats local_stats{};

    const EnvironmentSettings env = sanitize_environment_settings(scene.environment().settings());
    saved.environment.clear_color_rgba8 = env.clear_color_rgba8;
    saved.environment.clear_color = env.clear_color ? 1u : 0u;
    saved.environment.clear_depth = env.clear_depth ? 1u : 0u;
    scene.graph().update_world_transforms();

    bool ok = true;
    // EcsRegistry::query fires the body once PER CHUNK and dispatches chunks
    // across worker threads (parallel_for, grain=1 — see EcsRegistry_Internal.h).
    // These save bodies accumulate into shared scratch (saved.* vectors/maps,
    // local_stats, ok, error) with read-then-append index math, which races and
    // corrupts the blob once a component spans more than one chunk (large
    // scenes). Save is a cold path, so serialize each body wholesale under this
    // mutex — correctness over the lost parallelism.
    std::mutex save_mu;
    scene.registry().query<reads<SceneNodeComponent, CameraComponent, TransformComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const CameraComponent> cameras,
            std::span<const TransformComponent> transforms) {
            std::lock_guard<std::mutex> save_lk(save_mu);
            if (!ok)
                return;
            const usize n = std::min({nodes.size(), cameras.size(), transforms.size()});
            for (usize i = 0; i < n; ++i) {
                if (!scene.graph().alive(nodes[i].node)) {
                    ++local_stats.skipped_dead_nodes;
                    continue;
                }

                u32 transform_index = 0u;
                bool parented = false;
                bool approximate = false;
                const LocalTransform local = save_transform_for_node(scene,
                                                                     nodes[i].node,
                                                                     transforms[i].local,
                                                                     parented,
                                                                     approximate);
                if (parented) {
                    ++local_stats.parented_cameras;
                    ++local_stats.flattened_parent_relations;
                    ++local_stats.baked_world_transforms;
                }
                if (approximate)
                    ++local_stats.approximate_world_transforms;
                if (!append_save_transform(saved, local, transform_index, error)) {
                    ok = false;
                    return;
                }

                const math::Vec3 forward = camera_forward(local);
                SceneFileCamera camera{};
                camera.transform_index = transform_index;
                camera.look_at = math::add(local.translation, forward);
                camera.up = camera_up(local);
                camera.fov_y_rad = cameras[i].fov_y_rad;
                camera.near_z = cameras[i].near_z;
                camera.far_z = cameras[i].far_z;
                camera.tile_w = cameras[i].tile_w;
                camera.tile_h = cameras[i].tile_h;
                camera.active = cameras[i].active != 0u ? 1u : 0u;
                const u32 camera_index = static_cast<u32>(saved.cameras.size());
                saved.cameras.push_back(camera);
                saved.object_refs[nodes[i].entity.raw] = SceneFileObjectName{
                    .kind = SceneFileObjectKind::Camera,
                    .object_index = camera_index,
                };
                if (!append_save_object_name(saved,
                                             SceneFileObjectKind::Camera,
                                             camera_index,
                                             scene.registry(),
                                             nodes[i].entity,
                                             error)) {
                    ok = false;
                    return;
                }
                ++local_stats.cameras;
            }
        });
    if (!ok) {
        if (stats)
            *stats = local_stats;
        return false;
    }

    scene.registry().query<reads<SceneNodeComponent, LightComponent, TransformComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const LightComponent> lights,
            std::span<const TransformComponent> transforms) {
            std::lock_guard<std::mutex> save_lk(save_mu);
            if (!ok)
                return;
            const usize n = std::min({nodes.size(), lights.size(), transforms.size()});
            for (usize i = 0; i < n; ++i) {
                if (!scene.graph().alive(nodes[i].node)) {
                    ++local_stats.skipped_dead_nodes;
                    continue;
                }

                u32 transform_index = 0u;
                bool parented = false;
                bool approximate = false;
                const LocalTransform local = save_transform_for_node(scene,
                                                                     nodes[i].node,
                                                                     transforms[i].local,
                                                                     parented,
                                                                     approximate);
                if (parented) {
                    ++local_stats.parented_lights;
                    ++local_stats.flattened_parent_relations;
                    ++local_stats.baked_world_transforms;
                }
                if (approximate)
                    ++local_stats.approximate_world_transforms;
                if (!append_save_transform(saved, local, transform_index, error)) {
                    ok = false;
                    return;
                }

                const LightComponent authored_light = sanitize_light_component(lights[i]);
                SceneFileLight light{};
                light.transform_index = transform_index;
                light.kind = authored_light.kind;
                light.color_rgba8 = authored_light.color_rgba8;
                light.intensity = authored_light.intensity;
                light.range = authored_light.range;
                light.inner_cone_deg = authored_light.inner_cone_deg;
                light.outer_cone_deg = authored_light.outer_cone_deg;
                light.casts_shadow = authored_light.casts_shadow != 0u ? 1u : 0u;
                const u32 light_index = static_cast<u32>(saved.lights.size());
                saved.lights.push_back(light);
                saved.object_refs[nodes[i].entity.raw] = SceneFileObjectName{
                    .kind = SceneFileObjectKind::Light,
                    .object_index = light_index,
                };
                if (!append_save_object_name(saved,
                                             SceneFileObjectKind::Light,
                                             light_index,
                                             scene.registry(),
                                             nodes[i].entity,
                                             error)) {
                    ok = false;
                    return;
                }
                ++local_stats.lights;
            }
        });
    if (!ok) {
        if (stats)
            *stats = local_stats;
        return false;
    }

    scene.registry().query<reads<SceneNodeComponent, RenderableComponent, TransformComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const RenderableComponent> renderables,
            std::span<const TransformComponent> transforms) {
            std::lock_guard<std::mutex> save_lk(save_mu);
            if (!ok)
                return;
            const usize n = std::min({nodes.size(), renderables.size(), transforms.size()});
            for (usize i = 0; i < n; ++i) {
                if (!scene.graph().alive(nodes[i].node)) {
                    ++local_stats.skipped_dead_nodes;
                    continue;
                }

                const RenderableComponent& renderable = renderables[i];
                if (renderable.geometry != GeometryKind::Mesh) {
                    ++local_stats.skipped_non_mesh_renderables;
                    continue;
                }

                const ::psynder::render::MeshId mesh =
                    ::psynder::render::mesh_id_from_raw(renderable.geometry_id);
                const std::string_view mesh_name =
                    hooks.mesh_name ? hooks.mesh_name(hooks.user, mesh) : std::string_view{};
                if (mesh_name.empty()) {
                    ++local_stats.missing_mesh_names;
                    ok = fail_save_missing_mesh_name(error, mesh, hooks.mesh_name != nullptr);
                    return;
                }

                u32 mesh_name_offset = 0u;
                if (!add_save_string(saved, mesh_name, mesh_name_offset, error)) {
                    ok = false;
                    return;
                }

                u32 material_name_offset = 0u;
                const ::psynder::render::MaterialId material = renderable.material;
                if (material.valid() && scene.materials().valid(material)) {
                    const ::psynder::render::MaterialDesc desc = scene.materials().get(material);
                    std::string_view material_name =
                        hooks.material_name ? hooks.material_name(hooks.user, material)
                                            : std::string_view{};
                    if (hooks.material_preset_name) {
                        const std::string_view preset_name =
                            hooks.material_preset_name(hooks.user, material, desc);
                        if (!preset_name.empty()) {
                            material_name = preset_name;
                            ++local_stats.material_preset_names;
                        }
                    }
                    if (material_name.empty()) {
                        ++local_stats.missing_material_names;
                    } else {
                        if (!add_save_string(saved, material_name, material_name_offset, error)) {
                            ok = false;
                            return;
                        }

                        const auto [slot_it, inserted] =
                            saved.material_slots.emplace(material.raw,
                                                         static_cast<u32>(saved.materials.size()));
                        (void)slot_it;
                        if (inserted) {
                            SceneFileMaterial material_file{};
                            material_file.name_offset = material_name_offset;
                            if (hooks.material_base_color_texture_name) {
                                const std::string_view texture_name =
                                    hooks.material_base_color_texture_name(hooks.user, material, desc);
                                if (!add_save_string(saved,
                                                     texture_name,
                                                     material_file.base_color_texture_name_offset,
                                                     error)) {
                                    ok = false;
                                    return;
                                }
                            }
                            material_file.albedo_rgba8 = desc.albedo_rgba8;
                            material_file.flags = desc.flags;
                            material_file.alpha_cutoff = desc.alpha_cutoff;
                            material_file.reflectivity = desc.reflectivity;
                            material_file.roughness = desc.roughness;
                            material_file.emissive = desc.emissive;
                            material_file.winding = desc.winding;
                            material_file.blend = desc.blend;
                            material_file.raster_shadow_mode = desc.raster_shadow_mode;
                            material_file.shadow_alpha = desc.shadow_alpha;
                            material_file.shadow_opacity = desc.shadow_opacity;
                            material_file.shadow_softness = desc.shadow_softness;
                            saved.materials.push_back(material_file);
                            ++local_stats.materials;
                        }
                    }
                }

                u32 transform_index = 0u;
                bool parented = false;
                bool approximate = false;
                const LocalTransform local = save_transform_for_node(scene,
                                                                     nodes[i].node,
                                                                     transforms[i].local,
                                                                     parented,
                                                                     approximate);
                if (parented) {
                    ++local_stats.parented_mesh_instances;
                    ++local_stats.flattened_parent_relations;
                    ++local_stats.baked_world_transforms;
                }
                if (approximate)
                    ++local_stats.approximate_world_transforms;
                if (!append_save_transform(saved, local, transform_index, error)) {
                    ok = false;
                    return;
                }

                u32 group_name_offset = 0u;
                if (hooks.mesh_instance_group_name) {
                    const std::string_view group_name =
                        hooks.mesh_instance_group_name(hooks.user, nodes[i].entity, nodes[i].node);
                    if (!group_name.empty()) {
                        if (!add_save_string(saved, group_name, group_name_offset, error)) {
                            ok = false;
                            return;
                        }
                        ++local_stats.mesh_instance_group_names;
                    }
                }

                SceneFileMeshInstance instance{};
                instance.transform_index = transform_index;
                instance.mesh_name_offset = mesh_name_offset;
                instance.material_name_offset = material_name_offset;
                instance.group_name_offset = group_name_offset;
                instance.flags = renderable.flags;
                instance.mobility = renderable.mobility;
                const u32 mesh_instance_index = static_cast<u32>(saved.mesh_instances.size());
                saved.mesh_instances.push_back(instance);
                saved.object_refs[nodes[i].entity.raw] = SceneFileObjectName{
                    .kind = SceneFileObjectKind::MeshInstance,
                    .object_index = mesh_instance_index,
                };
                if (!append_save_object_name(saved,
                                             SceneFileObjectKind::MeshInstance,
                                             mesh_instance_index,
                                             scene.registry(),
                                             nodes[i].entity,
                                             error)) {
                    ok = false;
                    return;
                }
                ++local_stats.mesh_instances;
            }
        });

    if (!ok) {
        out_bytes.clear();
        if (stats)
            *stats = local_stats;
        return false;
    }

    if (!append_save_authoring_nodes(scene, saved, local_stats, error)) {
        out_bytes.clear();
        if (stats)
            *stats = local_stats;
        return false;
    }

    if (!write_save_blob(saved, out_bytes, error)) {
        out_bytes.clear();
        if (stats)
            *stats = local_stats;
        return false;
    }

    if (stats)
        *stats = local_stats;
    return true;
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

SceneLoadRequest& SceneLoadRequest::bind_mesh(std::string_view mesh_name,
                                              ::psynder::render::MeshId mesh,
                                              ::psynder::render::MaterialId material) {
    owned_bindings_.push_back(OwnedMeshBinding{
        .mesh_name = std::string{mesh_name},
        .mesh = mesh,
        .material = material,
    });
    binding_views_.clear();
    material_binding_views_.clear();
    return *this;
}

SceneLoadRequest& SceneLoadRequest::bind_material(std::string_view material_name,
                                                  ::psynder::render::MaterialId material) {
    owned_material_bindings_.push_back(OwnedMaterialBinding{
        .material_name = std::string{material_name},
        .material = material,
    });
    material_binding_views_.clear();
    return *this;
}

SceneLoadRequest& SceneLoadRequest::on_progress(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
    return *this;
}

SceneLoadRequest& SceneLoadRequest::on_ready(ReadyCallback callback) {
    ready_callback_ = std::move(callback);
    return *this;
}

SceneLoadRequest& SceneLoadRequest::on_error(ErrorCallback callback) {
    error_callback_ = std::move(callback);
    return *this;
}

void SceneLoadRequest::load_async(std::string_view virtual_path) {
    virtual_path_.assign(virtual_path.data(), virtual_path.size());
    scene_file_ = {};
    mesh_entities_.clear();
    base_transforms_.clear();
    result_ = {};
    error_.clear();
    binding_views_.clear();
    stage_ = SceneLoadStage::Queued;
    emit_progress(SceneLoadStage::Queued, 0.0f);
    file_request_.load_async(virtual_path_);
    stage_ = SceneLoadStage::Reading;
    emit_progress(SceneLoadStage::Reading, 0.10f);
}

bool SceneLoadRequest::update(Scene& scene, SceneLoadRuntimeHooks hooks) {
    if (stage_ == SceneLoadStage::Idle || stage_ == SceneLoadStage::Ready ||
        stage_ == SceneLoadStage::Failed) {
        return false;
    }

    if (file_request_.failed()) {
        error_ = file_request_.error();
        stage_ = SceneLoadStage::Failed;
        emit_progress(SceneLoadStage::Failed, 1.0f);
        if (error_callback_)
            error_callback_(error_);
        return false;
    }

    SceneFileLoaded loaded;
    if (!file_request_.consume(loaded))
        return false;

    emit_progress(SceneLoadStage::Parsing, 0.70f);
    scene_file_ = std::move(loaded);
    const SceneFileView& view = scene_file_.view;

    emit_progress(SceneLoadStage::Instantiating, 0.85f);
    scene.prewarm_capacity(scene_file_prewarm_config(view));
    if (hooks.reserve_render_capacity) {
        hooks.reserve_render_capacity(
            hooks.user, static_cast<u32>(view.mesh_instances.size()), 1u);
    }

    mesh_entities_.assign(view.mesh_instances.size(), {});
    rebuild_binding_views();
    bind_scene_assets(scene, view, hooks);
    SceneFileInstantiateResult instantiate{};
    {
        const ScopedImmediateStructural structural_scope{scene};
        instantiate = instantiate_scene_file(scene,
                                             view,
                                             binding_views_,
                                             material_binding_views_,
                                             mesh_entities_);
    }
    if (instantiate.missing_mesh_bindings != 0u) {
        PSY_LOG_WARN("{}: {} cooked mesh binding(s) were missing",
                     virtual_path_.empty() ? "psyscene" : virtual_path_,
                     instantiate.missing_mesh_bindings);
    }
    if (instantiate.missing_material_bindings != 0u) {
        PSY_LOG_WARN("{}: {} cooked material binding(s) were missing",
                     virtual_path_.empty() ? "psyscene" : virtual_path_,
                     instantiate.missing_material_bindings);
    }

    base_transforms_.resize(view.mesh_instances.size());
    for (usize i = 0; i < view.mesh_instances.size(); ++i) {
        base_transforms_[i] = scene_file_transform(view, view.mesh_instances[i].transform_index);
    }

    result_ = {
        .instantiate = instantiate,
        .mesh_entities = std::span<const Entity>{mesh_entities_.data(), mesh_entities_.size()},
        .base_transforms =
            std::span<const LocalTransform>{base_transforms_.data(), base_transforms_.size()},
    };
    stage_ = SceneLoadStage::Ready;
    emit_progress(SceneLoadStage::Ready, 1.0f);
    if (ready_callback_)
        ready_callback_(result_);
    return true;
}

void SceneLoadRequest::emit_progress(SceneLoadStage stage, f32 fraction) {
    if (progress_callback_)
        progress_callback_(SceneLoadProgress{.stage = stage, .fraction = fraction});
}

void SceneLoadRequest::rebuild_binding_views() {
    binding_views_.clear();
    binding_views_.reserve(owned_bindings_.size());
    for (const OwnedMeshBinding& binding : owned_bindings_) {
        binding_views_.push_back(SceneMeshBinding{
            .mesh_name = binding.mesh_name,
            .mesh = binding.mesh,
            .material = binding.material,
        });
    }
    material_binding_views_.clear();
    material_binding_views_.reserve(owned_material_bindings_.size());
    for (const OwnedMaterialBinding& binding : owned_material_bindings_) {
        material_binding_views_.push_back(SceneMaterialBinding{
            .material_name = binding.material_name,
            .material = binding.material,
        });
    }
}

void SceneLoadRequest::bind_scene_assets(Scene& scene,
                                         const SceneFileView& view,
                                         SceneLoadRuntimeHooks hooks) {
    if (hooks.resolve_material) {
        scene.materials().reserve(static_cast<u32>(view.materials.size()));
        for (const SceneFileMaterial& material_file : view.materials) {
            const std::string_view material_name{
                scene_file_string(view, material_file.name_offset)};
            if (material_name.empty())
                continue;
            const auto material_bound =
                std::find_if(material_binding_views_.begin(),
                             material_binding_views_.end(),
                             [&](const SceneMaterialBinding& binding) {
                                 return binding.material_name == material_name;
                             });
            if (material_bound != material_binding_views_.end())
                continue;
            const ::psynder::render::MaterialId material =
                hooks.resolve_material(hooks.user, scene, view, material_file);
            if (material.valid()) {
                material_binding_views_.push_back(SceneMaterialBinding{
                    .material_name = material_name,
                    .material = material,
                });
            }
        }
    }

    if (!hooks.resolve_mesh)
        return;

    for (const SceneFileMeshInstance& mesh_file : view.mesh_instances) {
        const std::string_view mesh_name{scene_file_string(view, mesh_file.mesh_name_offset)};
        if (mesh_name.empty())
            continue;
        const auto mesh_bound = std::find_if(binding_views_.begin(),
                                             binding_views_.end(),
                                             [&](const SceneMeshBinding& binding) {
                                                 return binding.mesh_name == mesh_name;
                                             });
        if (mesh_bound != binding_views_.end())
            continue;
        const ::psynder::render::MeshId mesh = hooks.resolve_mesh(hooks.user, mesh_name);
        if (mesh.valid()) {
            binding_views_.push_back(SceneMeshBinding{
                .mesh_name = mesh_name,
                .mesh = mesh,
                .material = {},
            });
        }
    }
}

}  // namespace psynder::scene
