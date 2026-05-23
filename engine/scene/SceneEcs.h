// SPDX-License-Identifier: MIT
// Psynder — ECS components that bind scene nodes to render submissions.

#pragma once

#include "core/Types.h"
#include "core/Log.h"
#include "math/Bounds.h"
#include "math/MathExt.h"
#include "render/Geometry.h"
#include "render/Material.h"
#include "scene/EcsRegistry.h"
#include "scene/Environment.h"
#include "scene/SceneGraph.h"
#include "scene/SceneRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::scene {

enum class GeometryKind : u8 {
    None = 0,
    Mesh = 1,
    AnalyticSphere = 2,
};

enum class ObjectMobility : u8 {
    Static = 0,
    Dynamic = 1,
};

enum class RenderableFlags : u32 {
    None = 0,
    Visible = 1u << 0,
    CastsShadowOverride = 1u << 1,
    ReceivesShadowOverride = 1u << 2,
};

[[nodiscard]] constexpr u32 renderable_flags_bits(RenderableFlags flags) noexcept {
    return static_cast<u32>(flags);
}

[[nodiscard]] constexpr RenderableFlags operator|(RenderableFlags a,
                                                  RenderableFlags b) noexcept {
    return static_cast<RenderableFlags>(renderable_flags_bits(a) | renderable_flags_bits(b));
}

[[nodiscard]] constexpr u32 operator&(RenderableFlags a, RenderableFlags b) noexcept {
    return renderable_flags_bits(a) & renderable_flags_bits(b);
}

constexpr RenderableFlags& operator|=(RenderableFlags& a, RenderableFlags b) noexcept {
    a = a | b;
    return a;
}

enum class RenderableValidationIssue : u32 {
    None = 0u,
    NotVisible = 1u << 0,
    NoGeometry = 1u << 1,
    DynamicStaticBake = 1u << 2,
};

[[nodiscard]] constexpr u32 renderable_validation_issue_bits(
    RenderableValidationIssue issue) noexcept {
    return static_cast<u32>(issue);
}

[[nodiscard]] constexpr RenderableValidationIssue operator|(
    RenderableValidationIssue a,
    RenderableValidationIssue b) noexcept {
    return static_cast<RenderableValidationIssue>(renderable_validation_issue_bits(a) |
                                                  renderable_validation_issue_bits(b));
}

[[nodiscard]] constexpr u32 operator&(RenderableValidationIssue a,
                                      RenderableValidationIssue b) noexcept {
    return renderable_validation_issue_bits(a) & renderable_validation_issue_bits(b);
}

constexpr RenderableValidationIssue& operator|=(RenderableValidationIssue& a,
                                                RenderableValidationIssue b) noexcept {
    a = a | b;
    return a;
}

struct RenderableValidation {
    RenderableValidationIssue issues = RenderableValidationIssue::None;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return issues == RenderableValidationIssue::None;
    }
    [[nodiscard]] constexpr bool has(RenderableValidationIssue issue) const noexcept {
        return (issues & issue) != 0u;
    }
};

PSYNDER_COMPONENT(TransformComponent) {
    LocalTransform local{};
};

PSYNDER_COMPONENT(SceneNodeComponent) {
    SceneNode node{};
    Entity entity{};
};

PSYNDER_COMPONENT(CameraComponent) {
    f32 fov_y_rad = 60.0f * math::kDegToRad;
    f32 aspect = 0.0f;  // <= 0 means use the active render target aspect.
    f32 near_z = 0.1f;
    f32 far_z = 100.0f;
    u32 tile_w = 64;
    u32 tile_h = 64;
    u8 active = 1;
    u8 _pad[3] = {};
};

struct CameraDesc {
    math::Vec3 position{0.0f, 0.0f, 2.5f};
    math::Vec3 look_at{0.0f, 0.0f, 0.0f};
    math::Vec3 up{0.0f, 1.0f, 0.0f};
    f32 fov_y_rad = 60.0f * math::kDegToRad;
    f32 aspect = 0.0f;  // <= 0 means use the active render target aspect.
    f32 near_z = 0.1f;
    f32 far_z = 100.0f;
    u32 tile_w = 64;
    u32 tile_h = 64;
    bool active = true;
};

PSYNDER_COMPONENT(RenderableComponent) {
    GeometryKind geometry = GeometryKind::None;
    u32 geometry_id = 0u;
    ::psynder::render::MaterialId material{};
    RenderableFlags flags = RenderableFlags::Visible;
    ObjectMobility mobility = ObjectMobility::Dynamic;
    math::Aabb local_bounds{};
};

struct SceneRenderItem {
    Entity entity{};
    SceneNode node{};
    math::Mat4 world{};
    math::Aabb world_bounds{};
    GeometryKind geometry = GeometryKind::None;
    u32 geometry_id = 0u;
    ::psynder::render::MaterialId material{};
    RenderableFlags flags = RenderableFlags::None;
    ObjectMobility mobility = ObjectMobility::Dynamic;
};

struct ScenePrewarmConfig {
    u32 scene_entities = 0u;
    u32 renderables = 0u;
    u32 cameras = 0u;
    u32 analytic_spheres = 0u;
    u32 render_items = 0u;
    u32 deferred_structural_changes = 0u;
};

struct ScenePoolStats {
    u32 entity_capacity = 0u;
    u32 chunk_live_count = 0u;
    u32 node_capacity = 0u;
    u32 live_nodes = 0u;
    u32 free_nodes = 0u;
    u32 render_item_capacity = 0u;
};

struct SceneGroupId {
    u32 index = 0xFFFFFFFFu;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0xFFFFFFFFu; }
};

struct SceneCameraView {
    Entity entity{};
    SceneNode node{};
    math::Mat4 view{};
    math::Mat4 projection{};
    u32 tile_w = 64;
    u32 tile_h = 64;
};

inline bool renderable_is_visible(const RenderableComponent& renderable) noexcept {
    return (renderable.flags & RenderableFlags::Visible) != 0u;
}

inline bool renderable_has_geometry(const RenderableComponent& renderable) noexcept {
    return renderable.geometry != GeometryKind::None;
}

inline bool renderable_is_static(const RenderableComponent& renderable) noexcept {
    return renderable.mobility == ObjectMobility::Static;
}

inline bool renderable_is_dynamic(const RenderableComponent& renderable) noexcept {
    return renderable.mobility == ObjectMobility::Dynamic;
}

inline RenderableComponent make_renderable(GeometryKind geometry,
                                           u32 geometry_id,
                                           ::psynder::render::MaterialId material,
                                           const math::Aabb& local_bounds,
                                           ObjectMobility mobility = ObjectMobility::Dynamic,
                                           RenderableFlags flags = RenderableFlags::Visible) noexcept {
    RenderableComponent renderable{};
    renderable.geometry = geometry;
    renderable.geometry_id = geometry_id;
    renderable.material = material;
    renderable.flags = flags;
    renderable.mobility = mobility;
    renderable.local_bounds = local_bounds;
    return renderable;
}

inline RenderableComponent make_static_renderable(GeometryKind geometry,
                                                  u32 geometry_id,
                                                  ::psynder::render::MaterialId material,
                                                  const math::Aabb& local_bounds,
                                                  RenderableFlags flags = RenderableFlags::Visible) noexcept {
    return make_renderable(geometry, geometry_id, material, local_bounds, ObjectMobility::Static, flags);
}

inline RenderableComponent make_dynamic_renderable(GeometryKind geometry,
                                                   u32 geometry_id,
                                                   ::psynder::render::MaterialId material,
                                                   const math::Aabb& local_bounds,
                                                   RenderableFlags flags = RenderableFlags::Visible) noexcept {
    return make_renderable(geometry, geometry_id, material, local_bounds, ObjectMobility::Dynamic, flags);
}

inline RenderableValidation validate_renderable_for_static_bake(const RenderableComponent& renderable) noexcept {
    RenderableValidation validation{};
    if (!renderable_is_visible(renderable))
        validation.issues |= RenderableValidationIssue::NotVisible;
    if (!renderable_has_geometry(renderable))
        validation.issues |= RenderableValidationIssue::NoGeometry;
    if (!renderable_is_static(renderable))
        validation.issues |= RenderableValidationIssue::DynamicStaticBake;
    return validation;
}

inline bool renderable_can_participate_in_static_bake(const RenderableComponent& renderable) noexcept {
    return validate_renderable_for_static_bake(renderable).ok();
}

inline bool update_renderable(EcsRegistry& registry, Entity entity, const RenderableComponent& renderable) {
    if (!registry.alive(entity) || !registry.get<RenderableComponent>(entity))
        return false;
    registry.add<RenderableComponent>(entity, renderable);
    return true;
}

inline bool set_renderable_mobility(EcsRegistry& registry, Entity entity, ObjectMobility mobility) {
    auto* renderable = registry.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.mobility = mobility;
    registry.add<RenderableComponent>(entity, updated);
    return true;
}

inline bool mark_renderable_static(EcsRegistry& registry, Entity entity) {
    return set_renderable_mobility(registry, entity, ObjectMobility::Static);
}

inline bool mark_renderable_dynamic(EcsRegistry& registry, Entity entity) {
    return set_renderable_mobility(registry, entity, ObjectMobility::Dynamic);
}

inline bool get_renderable_mobility(EcsRegistry& registry, Entity entity, ObjectMobility& out) {
    auto* renderable = registry.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    out = renderable->mobility;
    return true;
}

inline bool set_renderable_material(EcsRegistry& registry, Entity entity, ::psynder::render::MaterialId material) {
    auto* renderable = registry.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.material = material;
    registry.add<RenderableComponent>(entity, updated);
    return true;
}

inline bool set_renderable_flags(EcsRegistry& registry, Entity entity, RenderableFlags flags) {
    auto* renderable = registry.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.flags = flags;
    registry.add<RenderableComponent>(entity, updated);
    return true;
}

inline bool set_renderable_geometry(EcsRegistry& registry,
                                    Entity entity,
                                    GeometryKind geometry,
                                    u32 geometry_id,
                                    const math::Aabb& local_bounds) {
    auto* renderable = registry.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.geometry = geometry;
    updated.geometry_id = geometry_id;
    updated.local_bounds = local_bounds;
    registry.add<RenderableComponent>(entity, updated);
    return true;
}

inline void prewarm_scene_capacity(EcsRegistry& registry, SceneGraph& graph, const ScenePrewarmConfig& config) {
    const u32 scene_entity_count = std::max(config.scene_entities, config.renderables);
    graph.reserve_nodes(scene_entity_count);
    graph.reserve_analytic_spheres(config.analytic_spheres);

    registry.reserve_entities(scene_entity_count);
    registry.reserve_archetype<TransformComponent>(scene_entity_count);
    registry.reserve_archetype<SceneNodeComponent, TransformComponent>(scene_entity_count);
    if (config.cameras != 0u) {
        registry.reserve_archetype<CameraComponent, SceneNodeComponent, TransformComponent>(
            config.cameras);
    }
    if (config.renderables != 0u) {
        registry.reserve_archetype<RenderableComponent, SceneNodeComponent, TransformComponent>(
            config.renderables);
    }

    const u32 structural_ops = config.deferred_structural_changes != 0u
                                   ? config.deferred_structural_changes
                                   : scene_entity_count * 2u + config.renderables;
    const u32 structural_bytes = scene_entity_count * static_cast<u32>(sizeof(TransformComponent) +
                                                                       sizeof(SceneNodeComponent)) +
                                 config.renderables * static_cast<u32>(sizeof(RenderableComponent));
    registry.reserve_structural_changes(structural_ops, structural_bytes);
}

inline Entity create_scene_entity(EcsRegistry& registry,
                                  SceneGraph& graph,
                                  SceneNode parent = kInvalidSceneNode,
                                  const LocalTransform& local = {}) {
    const Entity entity = registry.create();
    const SceneNode node = graph.create_node(parent, local);
    registry.add<TransformComponent>(entity, TransformComponent{local});
    registry.add<SceneNodeComponent>(entity, SceneNodeComponent{node, entity});
    return entity;
}

inline bool set_scene_entity_transform(EcsRegistry& registry,
                                       SceneGraph& graph,
                                       Entity entity,
                                       const LocalTransform& local) {
    auto* transform = registry.get<TransformComponent>(entity);
    auto* node = registry.get<SceneNodeComponent>(entity);
    if (!transform || !node || !graph.alive(node->node))
        return false;
    transform->local = local;
    graph.set_local_transform(node->node, local);
    return true;
}

[[nodiscard]] inline math::Quat camera_rotation_towards(math::Vec3 position,
                                                        math::Vec3 target,
                                                        math::Vec3 up) noexcept {
    const math::Vec3 forward = math::normalize(math::sub(target, position));
    const math::Vec3 safe_forward =
        math::length(forward) > 0.0f ? forward : math::Vec3{0.0f, 0.0f, -1.0f};
    math::Vec3 right = math::normalize(math::cross(safe_forward, up));
    if (math::length(right) <= 0.0f)
        right = math::Vec3{1.0f, 0.0f, 0.0f};
    const math::Vec3 camera_up = math::cross(right, safe_forward);
    const math::Vec3 back = math::mul(safe_forward, -1.0f);

    const f32 m00 = right.x;
    const f32 m01 = camera_up.x;
    const f32 m02 = back.x;
    const f32 m10 = right.y;
    const f32 m11 = camera_up.y;
    const f32 m12 = back.y;
    const f32 m20 = right.z;
    const f32 m21 = camera_up.z;
    const f32 m22 = back.z;
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

inline bool find_active_camera(EcsRegistry& registry,
                               SceneGraph& graph,
                               f32 render_target_aspect,
                               SceneCameraView& out) {
    bool found = false;
    registry.query<reads<SceneNodeComponent, CameraComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes, std::span<const CameraComponent> cameras) {
            if (found)
                return;
            const usize n = std::min(nodes.size(), cameras.size());
            for (usize i = 0; i < n; ++i) {
                const CameraComponent& camera = cameras[i];
                const SceneNode node = nodes[i].node;
                if (camera.active == 0u || !graph.alive(node))
                    continue;
                const f32 aspect = camera.aspect > 0.0f ? camera.aspect : render_target_aspect;
                out.entity = nodes[i].entity;
                out.node = node;
                out.view = math::inverse_affine(graph.world_matrix(node));
                out.projection = math::perspective_rh(camera.fov_y_rad,
                                                      aspect > 0.0f ? aspect : 1.0f,
                                                      camera.near_z,
                                                      camera.far_z);
                out.tile_w = camera.tile_w;
                out.tile_h = camera.tile_h;
                found = true;
                return;
            }
        });
    return found;
}

inline Entity find_active_camera_entity(EcsRegistry& registry) {
    Entity out{};
    registry.query<reads<SceneNodeComponent, CameraComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes, std::span<const CameraComponent> cameras) {
            if (out.valid())
                return;
            const usize n = std::min(nodes.size(), cameras.size());
            for (usize i = 0; i < n; ++i) {
                if (cameras[i].active == 0u)
                    continue;
                out = nodes[i].entity;
                return;
            }
        });
    return out;
}

inline void gather_scene_render_items(EcsRegistry& registry,
                                      const SceneGraph& graph,
                                      std::vector<SceneRenderItem>& out) {
    out.clear();
    std::mutex append_mutex;
    registry.query<reads<SceneNodeComponent, RenderableComponent>, writes<>>(
        [&](std::span<const SceneNodeComponent> nodes, std::span<const RenderableComponent> renderables) {
            const usize n = std::min(nodes.size(), renderables.size());
            constexpr usize kMaxChunkRows = 256u;
            std::array<SceneRenderItem, kMaxChunkRows> chunk_items{};
            usize chunk_count = 0u;
            for (usize i = 0; i < n; ++i) {
                const RenderableComponent& r = renderables[i];
                if ((r.flags & RenderableFlags::Visible) == 0u ||
                    r.geometry == GeometryKind::None)
                    continue;
                const SceneNode node = nodes[i].node;
                if (!graph.alive(node))
                    continue;
                SceneRenderItem item{};
                item.entity = nodes[i].entity;
                item.node = node;
                item.world = graph.world_matrix(node);
                item.world_bounds = math::transform(r.local_bounds, item.world);
                item.geometry = r.geometry;
                item.geometry_id = r.geometry_id;
                item.material = r.material;
                item.flags = r.flags;
                item.mobility = r.mobility;
                chunk_items[chunk_count++] = item;
            }
            if (chunk_count != 0u) {
                std::scoped_lock lock{append_mutex};
                out.reserve(out.size() + chunk_count);
                out.insert(out.end(), chunk_items.begin(), chunk_items.begin() + chunk_count);
            }
        });
}

class Scene {
   public:
    using SpawnMeshFn = Entity (*)(void* user,
                                   Scene& scene,
                                   const ::psynder::render::MeshDesc& mesh_desc,
                                   const LocalTransform& local,
                                   SceneNode parent,
                                   RenderableFlags flags,
                                   ObjectMobility mobility);
    using SpawnMeshInstanceFn = Entity (*)(void* user,
                                           Scene& scene,
                                           ::psynder::render::MeshId mesh,
                                           ::psynder::render::MaterialId material,
                                           const LocalTransform& local,
                                           SceneNode parent,
                                           RenderableFlags flags,
                                           ObjectMobility mobility);
    using SpawnMeshBatchFn = u32 (*)(void* user,
                                     Scene& scene,
                                     ::psynder::render::MeshId mesh,
                                     ::psynder::render::MaterialId material,
                                     std::span<const LocalTransform> local,
                                     std::span<Entity> out_entities,
                                     SceneNode parent,
                                     RenderableFlags flags,
                                     ObjectMobility mobility);

    struct GroupAuthored {
        LocalTransform local{};
    };

    struct GroupTransformEdit {
        Scene* scene = nullptr;
        Entity entity{};
        math::Vec3 translation{};
        math::Quat rotation{};
        math::Vec3 scale{1.0f, 1.0f, 1.0f};

        GroupTransformEdit() = default;
        GroupTransformEdit(Scene& scene_ref, Entity entity_ref, const LocalTransform& local) noexcept
            : scene(&scene_ref)
            , entity(entity_ref)
            , translation(local.translation)
            , rotation(local.rotation)
            , scale(local.scale) {}
        GroupTransformEdit(const GroupTransformEdit&) = delete;
        GroupTransformEdit& operator=(const GroupTransformEdit&) = delete;
        GroupTransformEdit(GroupTransformEdit&& other) noexcept
            : scene(other.scene)
            , entity(other.entity)
            , translation(other.translation)
            , rotation(other.rotation)
            , scale(other.scale) {
            other.scene = nullptr;
        }
        GroupTransformEdit& operator=(GroupTransformEdit&& other) noexcept {
            if (this != &other) {
                flush();
                scene = other.scene;
                entity = other.entity;
                translation = other.translation;
                rotation = other.rotation;
                scale = other.scale;
                other.scene = nullptr;
            }
            return *this;
        }
        ~GroupTransformEdit() { flush(); }

        [[nodiscard]] LocalTransform local() const noexcept {
            return {.translation = translation, .rotation = rotation, .scale = scale};
        }
        void flush() noexcept {
            if (scene && entity.valid()) {
                scene->set_transform(entity, local());
                scene = nullptr;
            }
        }
    };

    struct GroupTransformRecord {
        Entity entity{};
        GroupTransformEdit transform{};
        GroupAuthored authored{};
    };

    class GroupTransformIterator {
       public:
        GroupTransformIterator() = default;
        GroupTransformIterator(Scene& scene_ref,
                               std::span<const Entity> entities,
                               std::span<const LocalTransform> authored,
                               usize index) noexcept
            : scene_(&scene_ref)
            , entities_(entities)
            , authored_(authored)
            , index_(index) {}

        [[nodiscard]] bool operator!=(const GroupTransformIterator& other) const noexcept {
            return index_ != other.index_;
        }
        GroupTransformIterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        [[nodiscard]] GroupTransformRecord operator*() const {
            const Entity entity = entities_[index_];
            return GroupTransformRecord{
                .entity = entity,
                .transform = GroupTransformEdit{*scene_, entity, scene_->transform(entity)},
                .authored = GroupAuthored{authored_[index_]},
            };
        }

       private:
        Scene* scene_ = nullptr;
        std::span<const Entity> entities_{};
        std::span<const LocalTransform> authored_{};
        usize index_ = 0u;
    };

    class GroupTransforms {
       public:
        GroupTransforms() = default;
        GroupTransforms(Scene& scene_ref,
                        std::span<const Entity> entities,
                        std::span<const LocalTransform> authored) noexcept
            : scene_(&scene_ref)
            , entities_(entities)
            , authored_(authored) {}

        [[nodiscard]] GroupTransformIterator begin() const noexcept {
            return scene_ ? GroupTransformIterator{*scene_, entities_, authored_, 0u}
                          : GroupTransformIterator{};
        }
        [[nodiscard]] GroupTransformIterator end() const noexcept {
            return scene_ ? GroupTransformIterator{*scene_, entities_, authored_, entities_.size()}
                          : GroupTransformIterator{};
        }

       private:
        Scene* scene_ = nullptr;
        std::span<const Entity> entities_{};
        std::span<const LocalTransform> authored_{};
    };

    class SceneGroupQuery {
       public:
        SceneGroupQuery() = default;
        SceneGroupQuery(Scene& scene_ref,
                        std::span<const Entity> entities,
                        std::span<const LocalTransform> authored) noexcept
            : scene_(&scene_ref)
            , entities_(entities)
            , authored_(authored) {}

        [[nodiscard]] bool empty() const noexcept { return entities_.empty(); }
        [[nodiscard]] usize size() const noexcept { return entities_.size(); }
        [[nodiscard]] std::span<const Entity> entities() const noexcept { return entities_; }
        [[nodiscard]] GroupTransforms transforms() const noexcept {
            return scene_ ? GroupTransforms{*scene_, entities_, authored_} : GroupTransforms{};
        }

       private:
        Scene* scene_ = nullptr;
        std::span<const Entity> entities_{};
        std::span<const LocalTransform> authored_{};
    };

    explicit Scene(EcsRegistry& registry = EcsRegistry::Get()) noexcept
        : registry_(&registry)
        , environment_(runtime_.environment) {
        registry_->set_structural_deferred(false);
    }

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    Scene(Scene&& other) noexcept { move_from(other); }
    Scene& operator=(Scene&& other) noexcept {
        if (this != &other)
            move_from(other);
        return *this;
    }

    [[nodiscard]] EcsRegistry& registry() noexcept { return *registry_; }
    [[nodiscard]] const EcsRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] SceneGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const SceneGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] ::psynder::render::MaterialLibrary& materials() noexcept { return materials_; }
    [[nodiscard]] const ::psynder::render::MaterialLibrary& materials() const noexcept {
        return materials_;
    }
    [[nodiscard]] Environment& environment() noexcept { return environment_; }
    [[nodiscard]] const Environment& environment() const noexcept { return environment_; }
    [[nodiscard]] SceneRuntime& runtime() noexcept { return runtime_; }
    [[nodiscard]] const SceneRuntime& runtime() const noexcept { return runtime_; }

    void bind_mesh_spawner(void* user,
                           SpawnMeshFn fn,
                           SpawnMeshInstanceFn instance_fn = nullptr,
                           SpawnMeshBatchFn batch_fn = nullptr) noexcept {
        mesh_spawner_user_ = user;
        mesh_spawner_ = fn;
        mesh_instance_spawner_ = instance_fn;
        mesh_batch_spawner_ = batch_fn;
    }

    void clear_mesh_spawner(void* user) noexcept {
        if (mesh_spawner_user_ == user) {
            mesh_spawner_user_ = nullptr;
            mesh_spawner_ = nullptr;
            mesh_instance_spawner_ = nullptr;
            mesh_batch_spawner_ = nullptr;
        }
    }

    [[nodiscard]] bool can_spawn_mesh() const noexcept { return mesh_spawner_ != nullptr; }
    [[nodiscard]] bool can_spawn_mesh_instance() const noexcept {
        return mesh_instance_spawner_ != nullptr;
    }

    void set_structural_deferred(bool on) noexcept { registry_->set_structural_deferred(on); }
    [[nodiscard]] bool structural_deferred() const noexcept {
        return registry_->structural_deferred();
    }
    void apply_structural_changes() { registry_->apply_structural_changes(); }

    void prewarm_capacity(const ScenePrewarmConfig& config) {
        prewarm_scene_capacity(*registry_, graph_, config);
        render_item_capacity_ =
            std::max(render_item_capacity_, std::max(config.render_items, config.renderables));
        pool_growth_warnings_ = true;
        record_pool_watermark();
    }

    void reserve_render_items(std::vector<SceneRenderItem>& out) const {
        out.reserve(render_item_capacity_);
    }

    [[nodiscard]] ScenePoolStats pool_stats() const noexcept {
        return {registry_->entity_capacity(),
                registry_->chunk_live_count(),
                graph_.node_capacity(),
                graph_.live_node_count(),
                graph_.free_node_count(),
                render_item_capacity_};
    }

    [[nodiscard]] Entity create_entity(const LocalTransform& local = {},
                                       SceneNode parent = kInvalidSceneNode) {
        const Entity entity = create_scene_entity(*registry_, graph_, parent, local);
        warn_pool_growth("create_entity");
        return entity;
    }

    [[nodiscard]] SceneNode node(Entity entity) const noexcept {
        if (auto* node_component = registry_->get<SceneNodeComponent>(entity))
            return node_component->node;
        return kInvalidSceneNode;
    }

    [[nodiscard]] Entity create_camera(const CameraComponent& camera = {},
                                       const LocalTransform& local = {},
                                       SceneNode parent = kInvalidSceneNode) {
        const Entity entity = create_entity(local, parent);
        attach_camera(entity, camera);
        return entity;
    }

    [[nodiscard]] Entity spawn_camera(const CameraDesc& desc = {},
                                      SceneNode parent = kInvalidSceneNode) {
        CameraComponent camera{};
        camera.fov_y_rad = desc.fov_y_rad;
        camera.aspect = desc.aspect;
        camera.near_z = desc.near_z;
        camera.far_z = desc.far_z;
        camera.tile_w = desc.tile_w;
        camera.tile_h = desc.tile_h;
        camera.active = desc.active ? 1u : 0u;

        LocalTransform transform{};
        transform.translation = desc.position;
        transform.rotation = camera_rotation_towards(desc.position, desc.look_at, desc.up);

        const Entity entity = create_camera(camera, transform, parent);
        if (desc.active)
            set_active_camera(entity);
        return entity;
    }

    [[nodiscard]] Entity create_renderable(const RenderableComponent& renderable,
                                           const LocalTransform& local = {},
                                           SceneNode parent = kInvalidSceneNode) {
        const Entity entity = create_entity(local, parent);
        attach_renderable(entity, renderable);
        return entity;
    }

    [[nodiscard]] Entity spawn_mesh(
        const ::psynder::render::MeshDesc& mesh_desc,
        const LocalTransform& local = {},
        SceneNode parent = kInvalidSceneNode,
        RenderableFlags flags = RenderableFlags::Visible,
        ObjectMobility mobility = ObjectMobility::Dynamic) {
        if (!mesh_spawner_)
            return {};
        return mesh_spawner_(mesh_spawner_user_, *this, mesh_desc, local, parent, flags, mobility);
    }

    [[nodiscard]] Entity spawn_mesh_instance(
        ::psynder::render::MeshId mesh,
        ::psynder::render::MaterialId material = {},
        const LocalTransform& local = {},
        SceneNode parent = kInvalidSceneNode,
        RenderableFlags flags = RenderableFlags::Visible,
        ObjectMobility mobility = ObjectMobility::Dynamic) {
        if (!mesh_instance_spawner_)
            return {};
        return mesh_instance_spawner_(
            mesh_spawner_user_, *this, mesh, material, local, parent, flags, mobility);
    }

    u32 spawn_mesh_batch(::psynder::render::MeshId mesh,
                         ::psynder::render::MaterialId material,
                         std::span<const LocalTransform> local,
                         std::span<Entity> out_entities,
                         SceneNode parent = kInvalidSceneNode,
                         RenderableFlags flags = RenderableFlags::Visible,
                         ObjectMobility mobility = ObjectMobility::Dynamic) {
        if (local.empty() || out_entities.empty())
            return 0u;
        const usize count = std::min(local.size(), out_entities.size());
        const std::span<const LocalTransform> local_slice{local.data(), count};
        const std::span<Entity> out_slice{out_entities.data(), count};
        if (mesh_batch_spawner_) {
            return mesh_batch_spawner_(mesh_spawner_user_,
                                       *this,
                                       mesh,
                                       material,
                                       local_slice,
                                       out_slice,
                                       parent,
                                       flags,
                                       mobility);
        }
        u32 spawned = 0u;
        for (usize i = 0; i < count; ++i) {
            out_slice[i] = spawn_mesh_instance(mesh, material, local_slice[i], parent, flags, mobility);
            if (out_slice[i].valid())
                ++spawned;
        }
        return spawned;
    }

    bool despawn_entity(Entity entity) { return destroy_entity(entity); }

    u32 despawn_batch(std::span<const Entity> entities) {
        u32 despawned = 0u;
        for (Entity entity : entities) {
            if (despawn_entity(entity))
                ++despawned;
        }
        return despawned;
    }

    bool destroy_entity(Entity entity) {
        SceneNode node{};
        if (auto* node_component = registry_->get<SceneNodeComponent>(entity))
            node = node_component->node;
        registry_->destroy(entity);
        return node.valid() ? graph_.destroy_node(node) : false;
    }

    bool set_transform(Entity entity, const LocalTransform& local) {
        return set_scene_entity_transform(*registry_, graph_, entity, local);
    }

    [[nodiscard]] LocalTransform transform(Entity entity) {
        if (const auto* transform = registry_->get<TransformComponent>(entity))
            return transform->local;
        return {};
    }

    [[nodiscard]] SceneGroupId group_id(std::string_view group_name) {
        if (group_name.empty())
            return {};
        return group_slot(group_name);
    }

    void add_to_group(std::string_view group_name, Entity entity, const LocalTransform& authored) {
        if (group_name.empty() || !entity.valid())
            return;
        add_to_group(group_id(group_name), entity, authored);
    }

    void add_to_group(SceneGroupId group, Entity entity, const LocalTransform& authored) {
        if (!group.valid() || !entity.valid() || group.index >= groups_.size())
            return;
        groups_[group.index].entities.push_back(entity);
        groups_[group.index].authored_locals.push_back(authored);
    }

    [[nodiscard]] SceneGroupQuery query_group(std::string_view group_name) {
        for (SceneGroupStorage& group : groups_) {
            if (group.name == group_name) {
                return SceneGroupQuery{
                    *this,
                    std::span<const Entity>{group.entities.data(), group.entities.size()},
                    std::span<const LocalTransform>{group.authored_locals.data(),
                                                    group.authored_locals.size()},
                };
            }
        }
        return {};
    }

    [[nodiscard]] SceneGroupQuery query_group(SceneGroupId group) {
        if (!group.valid() || group.index >= groups_.size())
            return {};
        SceneGroupStorage& storage = groups_[group.index];
        return SceneGroupQuery{
            *this,
            std::span<const Entity>{storage.entities.data(), storage.entities.size()},
            std::span<const LocalTransform>{storage.authored_locals.data(),
                                            storage.authored_locals.size()},
        };
    }

    bool attach_camera(Entity entity, const CameraComponent& camera = {}) {
        if (!registry_->alive(entity) || !registry_->get<SceneNodeComponent>(entity))
            return false;
        registry_->add<CameraComponent>(entity, camera);
        warn_pool_growth("attach_camera");
        return true;
    }

    bool set_active_camera(Entity entity) {
        if (!registry_->alive(entity) || !registry_->get<CameraComponent>(entity))
            return false;
        registry_->query<reads<>, writes<CameraComponent>>(
            [](std::span<CameraComponent> cameras) {
                for (CameraComponent& camera : cameras)
                    camera.active = 0u;
            });
        CameraComponent updated = *registry_->get<CameraComponent>(entity);
        updated.active = 1u;
        registry_->add<CameraComponent>(entity, updated);
        return true;
    }

    bool attach_renderable(Entity entity, const RenderableComponent& renderable) {
        if (!registry_->alive(entity) || !registry_->get<SceneNodeComponent>(entity))
            return false;
        registry_->add<RenderableComponent>(entity, renderable);
        warn_pool_growth("attach_renderable");
        return true;
    }

    bool update_renderable(Entity entity, const RenderableComponent& renderable) {
        return ::psynder::scene::update_renderable(*registry_, entity, renderable);
    }

    bool set_renderable_mobility(Entity entity, ObjectMobility mobility) {
        return ::psynder::scene::set_renderable_mobility(*registry_, entity, mobility);
    }

    bool mark_renderable_static(Entity entity) {
        return ::psynder::scene::mark_renderable_static(*registry_, entity);
    }

    bool mark_renderable_dynamic(Entity entity) {
        return ::psynder::scene::mark_renderable_dynamic(*registry_, entity);
    }

    bool get_renderable_mobility(Entity entity, ObjectMobility& out) {
        return ::psynder::scene::get_renderable_mobility(*registry_, entity, out);
    }

    RenderableValidation validate_renderable_for_static_bake(Entity entity) {
        auto* renderable = registry_->get<RenderableComponent>(entity);
        if (!renderable)
            return RenderableValidation{RenderableValidationIssue::NoGeometry};
        return ::psynder::scene::validate_renderable_for_static_bake(*renderable);
    }

    SceneGraphUpdateStats update_transforms(u32 parallel_threshold = 128u) {
        return graph_.update_world_transforms(parallel_threshold);
    }

    [[nodiscard]] bool active_camera_view(f32 render_target_aspect, SceneCameraView& out) {
        update_transforms();
        return find_active_camera(*registry_, graph_, render_target_aspect, out);
    }

    [[nodiscard]] Entity active_camera_entity() {
        return find_active_camera_entity(*registry_);
    }

    void gather_render_items(std::vector<SceneRenderItem>& out) {
        reserve_render_items(out);
        gather_scene_render_items(*registry_, graph_, out);
    }

   private:
    void move_from(Scene& other) noexcept {
        registry_ = other.registry_;
        graph_ = std::move(other.graph_);
        materials_ = std::move(other.materials_);
        runtime_ = std::move(other.runtime_);
        environment_ = std::move(other.environment_);
        environment_.bind_runtime(runtime_.environment);
        render_item_capacity_ = other.render_item_capacity_;
        mesh_spawner_user_ = other.mesh_spawner_user_;
        mesh_spawner_ = other.mesh_spawner_;
        mesh_instance_spawner_ = other.mesh_instance_spawner_;
        mesh_batch_spawner_ = other.mesh_batch_spawner_;
        pool_growth_warnings_ = other.pool_growth_warnings_;
        last_entity_capacity_ = other.last_entity_capacity_;
        last_chunk_live_count_ = other.last_chunk_live_count_;
        last_node_capacity_ = other.last_node_capacity_;
        groups_ = std::move(other.groups_);

        other.registry_ = &EcsRegistry::Get();
        other.environment_.bind_runtime(other.runtime_.environment);
        other.render_item_capacity_ = 0u;
        other.mesh_spawner_user_ = nullptr;
        other.mesh_spawner_ = nullptr;
        other.mesh_instance_spawner_ = nullptr;
        other.mesh_batch_spawner_ = nullptr;
        other.pool_growth_warnings_ = false;
        other.last_entity_capacity_ = 0u;
        other.last_chunk_live_count_ = 0u;
        other.last_node_capacity_ = 0u;
        other.groups_.clear();
    }

    void record_pool_watermark() noexcept {
        last_entity_capacity_ = registry_->entity_capacity();
        last_chunk_live_count_ = registry_->chunk_live_count();
        last_node_capacity_ = graph_.node_capacity();
    }

    void warn_pool_growth(const char* op) noexcept {
        if (!pool_growth_warnings_)
            return;
        const u32 entity_capacity = registry_->entity_capacity();
        const u32 chunk_live_count = registry_->chunk_live_count();
        const u32 node_capacity = graph_.node_capacity();
        if (entity_capacity > last_entity_capacity_ || chunk_live_count > last_chunk_live_count_ ||
            node_capacity > last_node_capacity_) {
            PSY_LOG_WARN(
                "scene: pooled storage grew during {} (entities {}->{}, chunks {}->{}, nodes "
                "{}->{}); prewarm more capacity to avoid runtime allocation",
                op,
                last_entity_capacity_,
                entity_capacity,
                last_chunk_live_count_,
                chunk_live_count,
                last_node_capacity_,
                node_capacity);
            last_entity_capacity_ = entity_capacity;
            last_chunk_live_count_ = chunk_live_count;
            last_node_capacity_ = node_capacity;
        }
    }

    struct SceneGroupStorage {
        std::string name;
        std::vector<Entity> entities;
        std::vector<LocalTransform> authored_locals;
    };

    [[nodiscard]] SceneGroupId group_slot(std::string_view group_name) {
        for (u32 i = 0; i < static_cast<u32>(groups_.size()); ++i) {
            if (groups_[i].name == group_name)
                return SceneGroupId{i};
        }
        SceneGroupStorage group{};
        group.name.assign(group_name.data(), group_name.size());
        groups_.push_back(std::move(group));
        return SceneGroupId{static_cast<u32>(groups_.size() - 1u)};
    }

    EcsRegistry* registry_ = nullptr;
    SceneGraph graph_{};
    ::psynder::render::MaterialLibrary materials_{};
    SceneRuntime runtime_{};
    Environment environment_{};
    u32 render_item_capacity_ = 0u;
    void* mesh_spawner_user_ = nullptr;
    SpawnMeshFn mesh_spawner_ = nullptr;
    SpawnMeshInstanceFn mesh_instance_spawner_ = nullptr;
    SpawnMeshBatchFn mesh_batch_spawner_ = nullptr;
    bool pool_growth_warnings_ = false;
    u32 last_entity_capacity_ = 0u;
    u32 last_chunk_live_count_ = 0u;
    u32 last_node_capacity_ = 0u;
    std::vector<SceneGroupStorage> groups_{};
};

}  // namespace psynder::scene
