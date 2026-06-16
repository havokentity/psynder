// SPDX-License-Identifier: MIT
// Psynder — cooked scene file contract and async runtime loader.

#pragma once

#include "asset/Vault.h"
#include "core/Types.h"
#include "math/Math.h"
#include "render/Geometry.h"
#include "render/Material.h"
#include "scene/GameplayComponents.h"
#include "scene/PhysicsComponents.h"
#include "scene/SceneEcs.h"
#include "scene/ScriptComponents.h"
#include "scene/TrackComponent.h"

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::scene {

inline constexpr u32 kPsySceneMagic = 0x4E435350u;  // "PSCN", little endian.
// v2 adds the PhysicsBodies (SPHY) chunk (authoring RigidBody / Vehicle /
// Helicopter / Character components). Older v1 files have no SPHY chunk and load
// unchanged — parse_scene_file accepts any version <= kPsySceneVersion.
// v3 adds the RenderSettings (SRND) chunk (render mode + sun/ambient/shadow/RT
// quality). Older v1/v2 files have no SRND chunk; load_span treats a missing
// chunk as empty, so they load with the RenderSettings defaults (sun disabled).
// v4 adds the GameplayAi (SGAI) chunk (no-code authoring of Faction / Hitbox /
// WeaponMode + the AI trio AiAgent / Perception / Patrol). Older v1..v3 files
// have no SGAI chunk; load_span treats a missing chunk as empty, so they load
// with no gameplay/AI authoring components attached.
// v5 adds the visual-scripting pair: ScriptGraphs (SSCG) records keyed by
// authoring_node_index + a raw byte pool ScriptGraphBlobs (SCGB) holding the
// concatenated psygraph serialized blobs each record slices into. Older v1..v4
// files have neither chunk; load_span treats a missing chunk as empty, so they
// load with no authored graphs attached.
// v6 adds the VehicleExt (SVHX) chunk (Wave 8 terrain-on-vehicle authoring): the
// speed governor + speed-scaled steering authority + the ground binding (Plane /
// Heightfield + the procedural rolling-hills surface) for a VehicleComponent.
// These are NEW VehicleComponent fields that did not exist when the SPHY chunk's
// fixed-stride SceneFilePhysicsVehicle was frozen, so they ride a SEPARATE chunk
// (keyed by authoring_node_index, exactly like SPHY) rather than growing the SPHY
// stride and breaking every existing scene. Older v1..v5 files have no SVHX chunk;
// load_span treats a missing chunk as empty, so a pre-Wave-8 vehicle loads with
// the governor off + Plane ground at y=0 -- bit-for-bit its old flat behaviour.
// v7 adds the Tracks (STRK) chunk (Wave 11 racer DoD): an authored closed-loop
// race track (a fixed set of cubic Bezier segments + track half-width + the
// start/finish lap gate) for a TrackComponent, keyed by authoring_node_index
// exactly like SPHY / SGAI. Older v1..v6 files have no STRK chunk; load_span
// treats a missing chunk as empty, so they load with no authored track attached
// -- a pure addition that cannot change any pre-Wave-11 scene.
inline constexpr u16 kPsySceneVersion = 7u;
inline constexpr u32 kPsySceneAlignment = 64u;

enum class SceneFileChunkType : u32 {
    Strings = 0x53525453u,           // STRS
    Environment = 0x564E4553u,       // SENV
    TransformTranslation = 0x54584654u,  // TXFT
    TransformRotation = 0x52584654u,     // TXFR
    TransformScale = 0x53584654u,        // TXFS
    Cameras = 0x4D414353u,          // SCAM
    MeshInstances = 0x53454D53u,    // SMES
    Lights = 0x54494C53u,           // SLIT
    ObjectNames = 0x4D414E53u,      // SNAM
    Materials = 0x54414D53u,        // SMAT
    BehaviorSpinOps = 0x50534253u,  // SBSP
    BehaviorTranslateOps = 0x4C544253u,  // SBTL
    AuthoringNodes = 0x54554153u,   // SAUT
    GameplayEntities = 0x504D4753u,  // SGMP
    PhysicsBodies = 0x59485053u,    // SPHY
    RenderSettings = 0x444E5253u,   // SRND
    GameplayAi = 0x49414753u,       // SGAI
    ScriptGraphs = 0x47435353u,     // SSCG
    ScriptGraphBlobs = 0x42474353u,  // SCGB
    VehicleExt = 0x58485653u,        // SVHX
    Tracks = 0x4B525453u,            // STRK
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

// Cooked render settings (v3+ SRND chunk). Mirrors scene::RenderSettings field
// for field; render_mode is stored as a u8 (scene::RenderMode). Older files
// have no SRND chunk and load with the runtime defaults (sun disabled).
struct SceneFileRenderSettings {
    u8 render_mode = 0u;  // scene::RenderMode (Raster=0, Raytraced=1, Hybrid=2)
    u8 sun_enabled = 0u;
    u8 shadows_enabled = 1u;
    u8 rt_ao = 1u;
    math::Vec3 sun_direction{-0.4f, -0.8f, -0.45f};
    u32 sun_color_rgba8 = 0xFFFFFFFFu;
    f32 sun_intensity = 1.0f;
    u32 ambient_color_rgba8 = 0xFF404040u;
    f32 ambient_intensity = 1.0f;
    f32 shadow_softness = 0.5f;
    f32 shadow_opacity = 0.7f;
    u32 rt_trace_downscale = 2u;
    u32 rt_reflection_bounces = 1u;
    u32 rt_samples = 1u;
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
    u32 group_name_offset = 0u;
    RenderableFlags flags = RenderableFlags::Visible;
    ObjectMobility mobility = ObjectMobility::Dynamic;
    u8 _pad[3] = {};
};

struct SceneFileLight {
    u32 transform_index = 0u;
    LightKind kind = LightKind::Point;
    u8 casts_shadow = 0u;
    u8 _pad0[2] = {};
    u32 color_rgba8 = 0xFFFFFFFFu;
    f32 intensity = 3.0f;
    f32 range = 8.0f;
    f32 inner_cone_deg = 20.0f;
    f32 outer_cone_deg = 45.0f;
    u8 _pad[32] = {};
};

enum class SceneFileObjectKind : u32 {
    Empty = 0u,
    Camera = 1u,
    MeshInstance = 2u,
    Light = 3u,
};

struct SceneFileObjectName {
    SceneFileObjectKind kind = SceneFileObjectKind::MeshInstance;
    u32 object_index = 0u;
    u32 name_offset = 0u;
    u32 reserved = 0u;
};

inline constexpr u32 kSceneFileAuthoringRoot = 0xFFFFFFFFu;

struct SceneFileAuthoringNode {
    SceneFileObjectKind kind = SceneFileObjectKind::Empty;
    u32 object_index = 0u;
    u32 transform_index = 0u;
    u32 parent_index = kSceneFileAuthoringRoot;
    u32 name_offset = 0u;
    u32 reserved[3] = {};
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

struct SceneFileBehaviorSpinOp {
    u32 name_offset = 0u;
    u32 target_group_name_offset = 0u;
    math::Vec3 axis{0.0f, 1.0f, 0.0f};
    f32 speed_base = 0.0f;
    f32 speed_step = 0.0f;
    f32 phase_base = 0.0f;
    f32 phase_step = 0.0f;
    u32 flags = 1u;
    u8 _pad[24] = {};
};

struct SceneFileBehaviorTranslateOp {
    u32 name_offset = 0u;
    u32 target_group_name_offset = 0u;
    math::Vec3 axis{0.0f, 1.0f, 0.0f};
    f32 amount_base = 0.0f;
    f32 amount_step = 0.0f;
    u32 flags = 1u;
    u8 _pad[32] = {};
};

struct SceneFileGameplayEntity {
    u32 authoring_node_index = 0u;
    u32 component_mask = 0u;
    GameplayRole role = GameplayRole::None;
    u8 _pad0[3] = {};
    u32 flags = 0u;
    PlayerControllerComponent player_controller{};
    HealthComponent health{};
    WeaponComponent weapon{};
};

// Authoring physics components persisted with the scene (DESIGN.md SS10.1).
// Keyed by authoring_node_index (the SAUT entry it attaches to), mirroring
// SceneFileGameplayEntity. component_mask selects which of the four authoring
// payloads below are present. Only AUTHORING fields are stored — the runtime_*
// handle fields on the live components stay 0 on load (never serialized).
struct SceneFilePhysicsRigidBody {
    ColliderShape shape = ColliderShape::Box;
    u8 _pad[3] = {};
    f32 mass = 1.0f;
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
};

struct SceneFilePhysicsVehicle {
    math::Vec3 half_extent{1.0f, 0.4f, 2.0f};
    f32 mass = 1200.0f;
    f32 engine_max_torque = 400.0f;
    f32 drag = 0.30f;
    f32 wheel_radius = 0.34f;
    f32 suspension = 0.35f;
    f32 stiffness = 35000.0f;
    f32 damping = 4500.0f;
    u8 is_player = 1u;
    u8 _pad[3] = {};
};

struct SceneFilePhysicsHelicopter {
    math::Vec3 half_extent{1.2f, 0.6f, 2.0f};
    f32 mass = 900.0f;
    f32 max_thrust_n = 14000.0f;
    f32 pitch_torque = 8000.0f;
    f32 roll_torque = 8000.0f;
    f32 yaw_torque = 4000.0f;
    f32 angular_damping = 2.0f;
    u8 hover_assist = 1u;
    u8 is_player = 1u;
    u8 _pad[2] = {};
};

struct SceneFilePhysicsCharacter {
    f32 height = 1.8f;
    f32 radius = 0.35f;
    f32 move_speed = 4.5f;
};

struct SceneFilePhysicsBody {
    u32 authoring_node_index = 0u;
    u32 component_mask = 0u;
    SceneFilePhysicsRigidBody rigid_body{};
    SceneFilePhysicsVehicle vehicle{};
    SceneFilePhysicsHelicopter helicopter{};
    SceneFilePhysicsCharacter character{};
};

// VehicleComponent EXTENSION fields persisted with the scene (v6 SVHX chunk;
// Wave 8 terrain-on-vehicle authoring). Keyed by authoring_node_index, mirroring
// SceneFilePhysicsBody. These are the VehicleComponent fields added AFTER the
// SPHY chunk's SceneFilePhysicsVehicle stride was frozen (speed governor, speed-
// scaled steering authority, and the ground binding: Plane vs Heightfield + the
// procedural rolling-hills surface). A record is emitted ONLY for an entity that
// already has a VehicleComponent in SPHY; on load the matching SPHY vehicle is
// patched with these fields. Older files have no SVHX chunk, so the vehicle keeps
// the defaults below (governor off, full steering authority, Plane ground at 0).
struct SceneFileVehicleExt {
    u32 authoring_node_index = 0u;
    f32 max_speed = 0.0f;
    f32 steer_full_speed = 0.0f;
    f32 steer_taper_speed = 0.0f;
    f32 steer_min_authority = 1.0f;
    u8 ground_mode = 0u;  // scene::VehicleGroundMode (Plane=0, Heightfield=1)
    u8 _pad[3] = {};
    f32 plane_y = 0.0f;
    f32 hf_base_y = 0.0f;
    f32 hf_amplitude = 4.0f;
    f32 hf_frequency = 0.05f;
};

// One cooked cubic Bezier track segment (v7 STRK chunk). Mirrors
// scene::TrackSegment field-for-field (four control points + half-width). The
// trailing pad keeps the serialized stride stable and the parent record a clean
// fixed-size POD the chunk validator accepts.
struct SceneFileTrackSegment {
    math::Vec3 p0{0.0f, 0.0f, 0.0f};
    math::Vec3 p1{0.0f, 0.0f, 0.0f};
    math::Vec3 p2{0.0f, 0.0f, 0.0f};
    math::Vec3 p3{0.0f, 0.0f, 0.0f};
    f32 half_width = 6.0f;
    u32 _pad = 0u;
};

// An authored closed-loop race TRACK persisted with the scene (v7 STRK chunk;
// Wave 11 racer DoD). Keyed by authoring_node_index, mirroring SceneFilePhysicsBody
// / SceneFileGameplayAi. Stores ONLY the authoring fields of scene::TrackComponent
// (the Bezier geometry + width + auto-driver tuning + the start/finish lap gate);
// the runtime cursor + lap bookkeeping are RUNTIME-only and never serialized (they
// reset to their POD defaults on load). Older files have no STRK chunk, so they
// load with no track attached.
struct SceneFileTrack {
    u32 authoring_node_index = 0u;
    u32 segment_count = 0u;
    f32 target_speed = 11.0f;
    f32 look_ahead = 12.0f;
    f32 steer_gain = 0.7f;
    f32 steer_clamp = 0.22f;
    f32 throttle_kp = 0.5f;
    u32 _pad = 0u;
    math::Vec3 lap_gate_point{0.0f, 0.0f, 0.0f};
    math::Vec3 lap_gate_normal{1.0f, 0.0f, 0.0f};
    SceneFileTrackSegment segments[TrackComponent::kMaxSegments] = {};
};

// Authoring gameplay + AI components persisted with the scene (v4 SGAI chunk).
// Keyed by authoring_node_index (the SAUT entry it attaches to), mirroring
// SceneFileGameplayEntity / SceneFilePhysicsBody. component_mask selects which of
// the payloads below are present. These mirror scene/GameplayComponents.h (the
// scene-level proxies), which in turn mirror gameplay::/ai::; PlayRuntime maps
// the live proxies into the real combat/AI components when a Play session begins.
// Only AUTHORING fields are stored; the live combat/AI runtime state (acquired
// target, cooldowns, last-seen memory) is never serialized.
struct SceneFileGameplayFaction {
    u32 faction = 0u;
};

struct SceneFileGameplayHitbox {
    math::Vec3 offset{0.0f, 0.0f, 0.0f};
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
    f32 radius = 0.0f;
    u32 enabled = 1u;
};

struct SceneFileGameplayWeaponMode {
    u8 kind = 0u;  // scene::WeaponFireKind (Hitscan=0, Projectile=1)
    u8 _pad[3] = {};
    f32 projectile_speed = 40.0f;
    f32 projectile_life = 3.0f;
};

struct SceneFileAiAgent {
    u8 state = 0u;  // scene::AiState (Idle=0..Dead=4)
    u8 _pad[3] = {};
    f32 sight_range = 20.0f;
    f32 fov_cos = 0.5f;
    f32 attack_range = 12.0f;
    f32 think_interval = 0.15f;
    f32 move_speed = 3.5f;
};

struct SceneFileAiPatrol {
    math::Vec3 waypoints[8] = {};
    u32 count = 0u;
    f32 wait_time = 1.0f;
    f32 arrive_radius = 0.5f;
    u32 _pad = 0u;
};

struct SceneFileGameplayAi {
    u32 authoring_node_index = 0u;
    u32 component_mask = 0u;
    SceneFileGameplayFaction faction{};
    SceneFileGameplayHitbox hitbox{};
    SceneFileGameplayWeaponMode weapon_mode{};
    SceneFileAiAgent ai_agent{};
    SceneFileAiPatrol patrol{};
    // Perception is a tag-only proxy (no authored fields); its presence is
    // carried purely by the component_mask bit, so it needs no payload struct.
};

// Authored visual-script graph record (v5 SSCG chunk). Keyed by
// authoring_node_index (the SAUT entry the entity's ScriptGraphComponent
// attaches to), mirroring SceneFileGameplayAi / SceneFilePhysicsBody. The graph
// itself is an opaque psygraph serialized blob; this fixed-stride record slices
// it out of the ScriptGraphBlobs (SCGB) byte pool by [blob_offset, blob_bytes).
// Storing the variable-length bytes in a separate pool keeps SSCG a clean POD
// array the chunk validator accepts (every chunk must be a whole array of a
// fixed-size type).
struct SceneFileScriptGraph {
    u32 authoring_node_index = 0u;
    u32 blob_offset = 0u;  // byte offset into the SCGB blob pool
    u32 blob_bytes = 0u;   // length of this graph's blob in the SCGB pool
    u32 reserved = 0u;
};

struct SceneFileView {
    const SceneFileHeader* header = nullptr;
    std::span<const char> strings;
    std::span<const SceneFileEnvironment> environments;
    std::span<const SceneFileRenderSettings> render_settings;
    std::span<const math::Vec3> translations;
    std::span<const math::Quat> rotations;
    std::span<const math::Vec3> scales;
    std::span<const SceneFileCamera> cameras;
    std::span<const SceneFileMeshInstance> mesh_instances;
    std::span<const SceneFileLight> lights;
    std::span<const SceneFileObjectName> object_names;
    std::span<const SceneFileMaterial> materials;
    std::span<const SceneFileBehaviorSpinOp> behavior_spin_ops;
    std::span<const SceneFileBehaviorTranslateOp> behavior_translate_ops;
    std::span<const SceneFileAuthoringNode> authoring_nodes;
    std::span<const SceneFileGameplayEntity> gameplay_entities;
    std::span<const SceneFilePhysicsBody> physics_bodies;
    std::span<const SceneFileVehicleExt> vehicle_exts;
    std::span<const SceneFileGameplayAi> gameplay_ai;
    std::span<const SceneFileScriptGraph> script_graphs;
    std::span<const u8> script_graph_blobs;
    std::span<const SceneFileTrack> tracks;
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
    u32 lights = 0u;
    u32 missing_mesh_bindings = 0u;
    u32 missing_material_bindings = 0u;
};

struct SceneFileSaveHooks {
    void* user = nullptr;
    // Required for mesh renderables; cooked v1 stores mesh references by name.
    std::string_view (*mesh_name)(void* user, ::psynder::render::MeshId mesh) = nullptr;
    std::string_view (*material_name)(void* user, ::psynder::render::MaterialId material) = nullptr;
    std::string_view (*material_base_color_texture_name)(
        void* user,
        ::psynder::render::MaterialId material,
        const ::psynder::render::MaterialDesc& material_desc) = nullptr;
    // Optional override for the material string written to SMAT and mesh refs.
    // Empty falls back to material_name().
    std::string_view (*material_preset_name)(
        void* user,
        ::psynder::render::MaterialId material,
        const ::psynder::render::MaterialDesc& material_desc) = nullptr;
    // Cooked v1 keeps this mesh group string for behavior targeting and older
    // content. General editor labels and hierarchy are stored in SAUT.
    std::string_view (*mesh_instance_group_name)(void* user,
                                                Entity entity,
                                                SceneNode node) = nullptr;
};

struct SceneFileSaveStats {
    u32 cameras = 0u;
    u32 mesh_instances = 0u;
    u32 lights = 0u;
    u32 materials = 0u;
    u32 skipped_non_mesh_renderables = 0u;
    u32 skipped_dead_nodes = 0u;
    u32 missing_mesh_names = 0u;
    u32 missing_material_names = 0u;
    u32 material_preset_names = 0u;
    u32 mesh_instance_group_names = 0u;
    u32 parented_cameras = 0u;
    u32 parented_mesh_instances = 0u;
    u32 parented_lights = 0u;
    u32 flattened_parent_relations = 0u;
    u32 baked_world_transforms = 0u;
    u32 approximate_world_transforms = 0u;
    u32 authoring_nodes = 0u;
    u32 gameplay_entities = 0u;
    u32 physics_bodies = 0u;
    u32 vehicle_exts = 0u;
    u32 gameplay_ai = 0u;
    u32 script_graphs = 0u;
    u32 script_graph_blob_bytes = 0u;
    u32 tracks = 0u;
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

[[nodiscard]] bool save_scene_file(Scene& scene,
                                   const SceneFileSaveHooks& hooks,
                                   detail::AlignedVector<u8>& out_bytes,
                                   SceneFileSaveStats* stats = nullptr,
                                   std::string* error = nullptr);

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
static_assert(sizeof(SceneFileRenderSettings) == 52u);
static_assert(sizeof(SceneFileCamera) == 64u);
static_assert(sizeof(SceneFileMeshInstance) == 24u);
static_assert(sizeof(SceneFileLight) == 60u);
static_assert(sizeof(SceneFileObjectName) == 16u);
static_assert(sizeof(SceneFileMaterial) == 64u);
static_assert(sizeof(SceneFileBehaviorSpinOp) == 64u);
static_assert(sizeof(SceneFileBehaviorTranslateOp) == 64u);
static_assert(sizeof(SceneFileGameplayEntity) == 76u);
static_assert(sizeof(SceneFilePhysicsRigidBody) == 28u);
static_assert(sizeof(SceneFilePhysicsVehicle) == 44u);
static_assert(sizeof(SceneFilePhysicsHelicopter) == 40u);
static_assert(sizeof(SceneFilePhysicsCharacter) == 12u);
static_assert(sizeof(SceneFilePhysicsBody) == 132u);
static_assert(sizeof(SceneFileVehicleExt) == 40u);
static_assert(sizeof(SceneFileGameplayFaction) == 4u);
static_assert(sizeof(SceneFileGameplayHitbox) == 32u);
static_assert(sizeof(SceneFileGameplayWeaponMode) == 12u);
static_assert(sizeof(SceneFileAiAgent) == 24u);
static_assert(sizeof(SceneFileAiPatrol) == 112u);
static_assert(sizeof(SceneFileGameplayAi) == 192u);
static_assert(sizeof(SceneFileScriptGraph) == 16u);
static_assert(sizeof(SceneFileTrackSegment) == 56u);
static_assert(sizeof(SceneFileTrack) == 504u);

}  // namespace psynder::scene
