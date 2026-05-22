// SPDX-License-Identifier: MIT
// Psynder — ECS components that bind scene nodes to render submissions.

#pragma once

#include "core/Types.h"
#include "math/Bounds.h"
#include "render/Material.h"
#include "scene/SceneGraph.h"
#include "scene/EcsRegistry.h"

#include <algorithm>
#include <array>
#include <mutex>
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

enum RenderableFlags : u32 {
    Renderable_Visible = 1u << 0,
    Renderable_CastsShadowOverride = 1u << 1,
    Renderable_ReceivesShadowOverride = 1u << 2,

    Renderable_DefaultFlags = Renderable_Visible,
};

enum RenderableValidationIssue : u32 {
    RenderableValidation_None = 0u,
    RenderableValidation_NotVisible = 1u << 0,
    RenderableValidation_NoGeometry = 1u << 1,
    RenderableValidation_DynamicStaticBake = 1u << 2,
};

struct RenderableValidation {
    u32 issues = RenderableValidation_None;

    [[nodiscard]] constexpr bool ok() const noexcept { return issues == RenderableValidation_None; }
    [[nodiscard]] constexpr bool has(RenderableValidationIssue issue) const noexcept {
        return (issues & static_cast<u32>(issue)) != 0u;
    }
};

PSYNDER_COMPONENT(TransformComponent) {
    LocalTransform local{};
};

PSYNDER_COMPONENT(SceneNodeComponent) {
    SceneNode node{};
    Entity entity{};
};

PSYNDER_COMPONENT(RenderableComponent) {
    GeometryKind geometry = GeometryKind::None;
    u32 geometry_id = 0u;
    ::psynder::render::MaterialId material{};
    u32 flags = Renderable_DefaultFlags;
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
    u32 flags = 0u;
    ObjectMobility mobility = ObjectMobility::Dynamic;
};

struct ScenePrewarmConfig {
    u32 scene_entities = 0u;
    u32 renderables = 0u;
    u32 analytic_spheres = 0u;
    u32 render_items = 0u;
    u32 deferred_structural_changes = 0u;
};

inline bool renderable_is_visible(const RenderableComponent& renderable) noexcept {
    return (renderable.flags & Renderable_Visible) != 0u;
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
                                           u32 flags = Renderable_DefaultFlags) noexcept {
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
                                                  u32 flags = Renderable_DefaultFlags) noexcept {
    return make_renderable(geometry, geometry_id, material, local_bounds, ObjectMobility::Static, flags);
}

inline RenderableComponent make_dynamic_renderable(GeometryKind geometry,
                                                   u32 geometry_id,
                                                   ::psynder::render::MaterialId material,
                                                   const math::Aabb& local_bounds,
                                                   u32 flags = Renderable_DefaultFlags) noexcept {
    return make_renderable(geometry, geometry_id, material, local_bounds, ObjectMobility::Dynamic, flags);
}

inline RenderableValidation validate_renderable_for_static_bake(const RenderableComponent& renderable) noexcept {
    RenderableValidation validation{};
    if (!renderable_is_visible(renderable))
        validation.issues |= RenderableValidation_NotVisible;
    if (!renderable_has_geometry(renderable))
        validation.issues |= RenderableValidation_NoGeometry;
    if (!renderable_is_static(renderable))
        validation.issues |= RenderableValidation_DynamicStaticBake;
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

inline bool set_renderable_flags(EcsRegistry& registry, Entity entity, u32 flags) {
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
                if ((r.flags & Renderable_Visible) == 0u || r.geometry == GeometryKind::None)
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
    explicit Scene(EcsRegistry& registry = EcsRegistry::Get()) noexcept : registry_(&registry) {
        registry_->set_structural_deferred(false);
    }

    [[nodiscard]] EcsRegistry& registry() noexcept { return *registry_; }
    [[nodiscard]] const EcsRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] SceneGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const SceneGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] ::psynder::render::MaterialLibrary& materials() noexcept { return materials_; }
    [[nodiscard]] const ::psynder::render::MaterialLibrary& materials() const noexcept {
        return materials_;
    }

    void set_structural_deferred(bool on) noexcept { registry_->set_structural_deferred(on); }
    void apply_structural_changes() { registry_->apply_structural_changes(); }

    void prewarm_capacity(const ScenePrewarmConfig& config) {
        prewarm_scene_capacity(*registry_, graph_, config);
        render_item_capacity_ =
            std::max(render_item_capacity_, std::max(config.render_items, config.renderables));
    }

    void reserve_render_items(std::vector<SceneRenderItem>& out) const {
        out.reserve(render_item_capacity_);
    }

    [[nodiscard]] Entity create_entity(const LocalTransform& local = {},
                                       SceneNode parent = kInvalidSceneNode) {
        return create_scene_entity(*registry_, graph_, parent, local);
    }

    [[nodiscard]] Entity create_renderable(const RenderableComponent& renderable,
                                           const LocalTransform& local = {},
                                           SceneNode parent = kInvalidSceneNode) {
        const Entity entity = create_entity(local, parent);
        attach_renderable(entity, renderable);
        return entity;
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

    bool attach_renderable(Entity entity, const RenderableComponent& renderable) {
        if (!registry_->alive(entity) || !registry_->get<SceneNodeComponent>(entity))
            return false;
        registry_->add<RenderableComponent>(entity, renderable);
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
            return RenderableValidation{RenderableValidation_NoGeometry};
        return ::psynder::scene::validate_renderable_for_static_bake(*renderable);
    }

    SceneGraphUpdateStats update_transforms(u32 parallel_threshold = 128u) {
        return graph_.update_world_transforms(parallel_threshold);
    }

    void gather_render_items(std::vector<SceneRenderItem>& out) {
        reserve_render_items(out);
        gather_scene_render_items(*registry_, graph_, out);
    }

   private:
    EcsRegistry* registry_ = nullptr;
    SceneGraph graph_{};
    ::psynder::render::MaterialLibrary materials_{};
    u32 render_item_capacity_ = 0u;
};

}  // namespace psynder::scene
