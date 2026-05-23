// SPDX-License-Identifier: MIT
// Psynder — cooked scene file contract and async runtime loader.

#pragma once

#include "asset/Vault.h"
#include "core/Types.h"
#include "math/Math.h"
#include "render/Geometry.h"
#include "render/Material.h"
#include "scene/SceneEcs.h"

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::scene {

inline constexpr u32 kPsySceneMagic = 0x4E435350u;  // "PSCN", little endian.
inline constexpr u16 kPsySceneVersion = 1u;
inline constexpr u32 kPsySceneAlignment = 64u;

enum class SceneFileChunkType : u32 {
    Strings = 0x53525453u,           // STRS
    Environment = 0x564E4553u,       // SENV
    TransformTranslation = 0x54584654u,  // TXFT
    TransformRotation = 0x52584654u,     // TXFR
    TransformScale = 0x53584654u,        // TXFS
    Cameras = 0x4D414353u,          // SCAM
    MeshInstances = 0x53454D53u,    // SMES
    Materials = 0x54414D53u,        // SMAT
};

struct SceneFileHeader {
    u32 magic = kPsySceneMagic;
    u16 version = kPsySceneVersion;
    u16 header_bytes = sizeof(SceneFileHeader);
    u32 file_bytes = 0u;
    u32 chunk_count = 0u;
    u32 transform_count = 0u;
    u32 camera_count = 0u;
    u32 mesh_instance_count = 0u;
    u32 flags = 0u;
    u32 reserved[8] = {};
};

struct SceneFileChunk {
    SceneFileChunkType type = SceneFileChunkType::Strings;
    u32 offset = 0u;
    u32 bytes = 0u;
    u32 stride = 0u;
};

struct SceneFileEnvironment {
    u32 clear_color_rgba8 = 0xFF000000u;
    u8 clear_color = 1u;
    u8 clear_depth = 1u;
    u8 _pad[2] = {};
};

struct SceneFileCamera {
    u32 transform_index = 0u;
    math::Vec3 look_at{0.0f, 0.0f, 0.0f};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    f32 fov_y_rad = 60.0f * math::kDegToRad;
    f32 near_z = 0.1f;
    f32 far_z = 100.0f;
    u32 tile_w = 64u;
    u32 tile_h = 64u;
    u8 active = 1u;
    u8 _pad[15] = {};
};

struct SceneFileMeshInstance {
    u32 transform_index = 0u;
    u32 mesh_name_offset = 0u;
    u32 material_name_offset = 0u;
    RenderableFlags flags = RenderableFlags::Visible;
    ObjectMobility mobility = ObjectMobility::Dynamic;
    u8 _pad[3] = {};
};

struct SceneFileMaterial {
    u32 name_offset = 0u;
    u32 base_color_texture_name_offset = 0u;
    u32 albedo_rgba8 = 0xFFFFFFFFu;
    ::psynder::render::MaterialFlags flags = ::psynder::render::Material_DefaultFlags;
    f32 alpha_cutoff = 0.5f;
    f32 reflectivity = 0.0f;
    f32 roughness = 1.0f;
    f32 emissive = 0.0f;
    ::psynder::render::MaterialWinding winding = ::psynder::render::MaterialWinding::Ccw;
    ::psynder::render::MaterialBlendMode blend = ::psynder::render::MaterialBlendMode::Opaque;
    ::psynder::render::MaterialRasterShadowMode raster_shadow_mode =
        ::psynder::render::MaterialRasterShadowMode::None;
    ::psynder::render::MaterialShadowAlphaMode shadow_alpha =
        ::psynder::render::MaterialShadowAlphaMode::Opaque;
    f32 shadow_opacity = 0.5f;
    f32 shadow_softness = 0.5f;
    u8 _pad[20] = {};
};

struct SceneFileView {
    const SceneFileHeader* header = nullptr;
    std::span<const char> strings;
    std::span<const SceneFileEnvironment> environments;
    std::span<const math::Vec3> translations;
    std::span<const math::Quat> rotations;
    std::span<const math::Vec3> scales;
    std::span<const SceneFileCamera> cameras;
    std::span<const SceneFileMeshInstance> mesh_instances;
    std::span<const SceneFileMaterial> materials;
};

struct SceneFileLoaded {
    detail::AlignedVector<u8> bytes;
    SceneFileView view{};
};

struct SceneMeshBinding {
    std::string_view mesh_name;
    ::psynder::render::MeshId mesh{};
    ::psynder::render::MaterialId material{};
};

struct SceneMaterialBinding {
    std::string_view material_name;
    ::psynder::render::MaterialId material{};
};

struct SceneFileInstantiateResult {
    u32 cameras = 0u;
    u32 mesh_instances = 0u;
    u32 missing_mesh_bindings = 0u;
    u32 missing_material_bindings = 0u;
};

[[nodiscard]] bool parse_scene_file(std::span<const u8> bytes,
                                    SceneFileView& out,
                                    std::string* error = nullptr);

[[nodiscard]] const char* scene_file_string(const SceneFileView& scene_file,
                                            u32 offset) noexcept;
[[nodiscard]] LocalTransform scene_file_transform(const SceneFileView& scene_file,
                                                  u32 index) noexcept;
[[nodiscard]] ScenePrewarmConfig scene_file_prewarm_config(
    const SceneFileView& scene_file) noexcept;

[[nodiscard]] SceneFileInstantiateResult instantiate_scene_file(
    Scene& scene,
    const SceneFileView& scene_file,
    std::span<const SceneMeshBinding> mesh_bindings,
    std::span<Entity> out_mesh_entities = {});
[[nodiscard]] SceneFileInstantiateResult instantiate_scene_file(
    Scene& scene,
    const SceneFileView& scene_file,
    std::span<const SceneMeshBinding> mesh_bindings,
    std::span<const SceneMaterialBinding> material_bindings,
    std::span<Entity> out_mesh_entities = {});

namespace scene_file_detail {
struct LoadState;
}  // namespace scene_file_detail

class SceneFileRequest {
   public:
    enum class State : u8 {
        Idle,
        Loading,
        Ready,
        Failed,
    };

    SceneFileRequest() = default;
    SceneFileRequest(const SceneFileRequest&) = delete;
    SceneFileRequest& operator=(const SceneFileRequest&) = delete;

    void load_async(std::string_view virtual_path);
    [[nodiscard]] State state() const;
    [[nodiscard]] bool ready() const { return state() == State::Ready; }
    [[nodiscard]] bool failed() const { return state() == State::Failed; }
    [[nodiscard]] std::string error() const;

    bool consume(SceneFileLoaded& out);

   private:
    static void on_loaded(asset::Blob blob, void* user) noexcept;

    std::shared_ptr<scene_file_detail::LoadState> state_;
};

enum class SceneLoadStage : u8 {
    Idle,
    Queued,
    Reading,
    Parsing,
    Instantiating,
    Ready,
    Failed,
};

struct SceneLoadProgress {
    SceneLoadStage stage = SceneLoadStage::Idle;
    f32 fraction = 0.0f;
};

struct SceneLoadRuntimeHooks {
    void* user = nullptr;
    void (*reserve_render_capacity)(void* user, u32 renderables, u32 meshes) = nullptr;
    ::psynder::render::MeshId (*resolve_mesh)(void* user, std::string_view mesh_name) = nullptr;
    ::psynder::render::MaterialId (*resolve_material)(void* user,
                                                      Scene& scene,
                                                      const SceneFileView& scene_file,
                                                      const SceneFileMaterial& material) = nullptr;
};

struct SceneLoadResult {
    SceneFileInstantiateResult instantiate{};
    std::span<const Entity> mesh_entities;
    std::span<const LocalTransform> base_transforms;
};

class SceneLoadRequest {
   public:
    using ProgressCallback = std::function<void(const SceneLoadProgress&)>;
    using ReadyCallback = std::function<void(const SceneLoadResult&)>;
    using ErrorCallback = std::function<void(std::string_view)>;

    SceneLoadRequest() = default;
    SceneLoadRequest(const SceneLoadRequest&) = delete;
    SceneLoadRequest& operator=(const SceneLoadRequest&) = delete;
    SceneLoadRequest(SceneLoadRequest&&) = delete;
    SceneLoadRequest& operator=(SceneLoadRequest&&) = delete;

    SceneLoadRequest& bind_mesh(std::string_view mesh_name,
                                ::psynder::render::MeshId mesh,
                                ::psynder::render::MaterialId material = {});
    SceneLoadRequest& bind_material(std::string_view material_name,
                                    ::psynder::render::MaterialId material);
    SceneLoadRequest& on_progress(ProgressCallback callback);
    SceneLoadRequest& on_ready(ReadyCallback callback);
    SceneLoadRequest& on_error(ErrorCallback callback);

    void load_async(std::string_view virtual_path);
    bool update(Scene& scene, SceneLoadRuntimeHooks hooks = {});

    [[nodiscard]] SceneLoadStage stage() const noexcept { return stage_; }
    [[nodiscard]] bool ready() const noexcept { return stage_ == SceneLoadStage::Ready; }
    [[nodiscard]] bool failed() const noexcept { return stage_ == SceneLoadStage::Failed; }
    [[nodiscard]] bool pending() const noexcept {
        return stage_ == SceneLoadStage::Queued || stage_ == SceneLoadStage::Reading ||
               stage_ == SceneLoadStage::Parsing || stage_ == SceneLoadStage::Instantiating;
    }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }
    [[nodiscard]] const SceneLoadResult& result() const noexcept { return result_; }
    [[nodiscard]] const SceneFileLoaded& loaded_file() const noexcept { return scene_file_; }

   private:
    struct OwnedMeshBinding {
        std::string mesh_name;
        ::psynder::render::MeshId mesh{};
        ::psynder::render::MaterialId material{};
    };
    struct OwnedMaterialBinding {
        std::string material_name;
        ::psynder::render::MaterialId material{};
    };

    void emit_progress(SceneLoadStage stage, f32 fraction);
    void rebuild_binding_views();
    void bind_scene_assets(Scene& scene, const SceneFileView& view, SceneLoadRuntimeHooks hooks);

    std::string virtual_path_{};
    SceneFileRequest file_request_{};
    SceneFileLoaded scene_file_{};
    std::vector<OwnedMeshBinding> owned_bindings_{};
    std::vector<OwnedMaterialBinding> owned_material_bindings_{};
    std::vector<SceneMeshBinding> binding_views_{};
    std::vector<SceneMaterialBinding> material_binding_views_{};
    std::vector<Entity> mesh_entities_{};
    std::vector<LocalTransform> base_transforms_{};
    SceneLoadResult result_{};
    ProgressCallback progress_callback_{};
    ReadyCallback ready_callback_{};
    ErrorCallback error_callback_{};
    std::string error_{};
    SceneLoadStage stage_ = SceneLoadStage::Idle;
};

static_assert(sizeof(SceneFileHeader) == 64u);
static_assert(sizeof(SceneFileChunk) == 16u);
static_assert(sizeof(SceneFileEnvironment) == 8u);
static_assert(sizeof(SceneFileCamera) == 64u);
static_assert(sizeof(SceneFileMeshInstance) == 20u);
static_assert(sizeof(SceneFileMaterial) == 64u);

}  // namespace psynder::scene
