// SPDX-License-Identifier: MIT
// Psynder — cooked scene file contract and async runtime loader.

#pragma once

#include "asset/Vault.h"
#include "core/Types.h"
#include "math/Math.h"
#include "render/Geometry.h"
#include "render/Material.h"
#include "scene/SceneEcs.h"

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

struct SceneFileView {
    const SceneFileHeader* header = nullptr;
    std::span<const char> strings;
    std::span<const SceneFileEnvironment> environments;
    std::span<const math::Vec3> translations;
    std::span<const math::Quat> rotations;
    std::span<const math::Vec3> scales;
    std::span<const SceneFileCamera> cameras;
    std::span<const SceneFileMeshInstance> mesh_instances;
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

struct SceneFileInstantiateResult {
    u32 cameras = 0u;
    u32 mesh_instances = 0u;
    u32 missing_mesh_bindings = 0u;
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

static_assert(sizeof(SceneFileHeader) == 64u);
static_assert(sizeof(SceneFileChunk) == 16u);
static_assert(sizeof(SceneFileEnvironment) == 8u);
static_assert(sizeof(SceneFileCamera) == 64u);
static_assert(sizeof(SceneFileMeshInstance) == 20u);

}  // namespace psynder::scene
