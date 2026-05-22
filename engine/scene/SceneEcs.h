// SPDX-License-Identifier: MIT
// Psynder — ECS components that bind scene nodes to render submissions.

#pragma once

#include "core/Types.h"
#include "math/Bounds.h"
#include "render/Material.h"
#include "scene/SceneGraph.h"
#include "scene/World.h"

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

inline bool update_renderable(World& world, Entity entity, const RenderableComponent& renderable) {
    if (!world.alive(entity) || !world.get<RenderableComponent>(entity))
        return false;
    world.add<RenderableComponent>(entity, renderable);
    return true;
}

inline bool set_renderable_mobility(World& world, Entity entity, ObjectMobility mobility) {
    auto* renderable = world.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.mobility = mobility;
    world.add<RenderableComponent>(entity, updated);
    return true;
}

inline bool mark_renderable_static(World& world, Entity entity) {
    return set_renderable_mobility(world, entity, ObjectMobility::Static);
}

inline bool mark_renderable_dynamic(World& world, Entity entity) {
    return set_renderable_mobility(world, entity, ObjectMobility::Dynamic);
}

inline bool get_renderable_mobility(World& world, Entity entity, ObjectMobility& out) {
    auto* renderable = world.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    out = renderable->mobility;
    return true;
}

inline bool set_renderable_material(World& world, Entity entity, ::psynder::render::MaterialId material) {
    auto* renderable = world.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.material = material;
    world.add<RenderableComponent>(entity, updated);
    return true;
}

inline bool set_renderable_flags(World& world, Entity entity, u32 flags) {
    auto* renderable = world.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.flags = flags;
    world.add<RenderableComponent>(entity, updated);
    return true;
}

inline bool set_renderable_geometry(World& world,
                                    Entity entity,
                                    GeometryKind geometry,
                                    u32 geometry_id,
                                    const math::Aabb& local_bounds) {
    auto* renderable = world.get<RenderableComponent>(entity);
    if (!renderable)
        return false;
    RenderableComponent updated = *renderable;
    updated.geometry = geometry;
    updated.geometry_id = geometry_id;
    updated.local_bounds = local_bounds;
    world.add<RenderableComponent>(entity, updated);
    return true;
}

inline void prewarm_scene_capacity(World& world, SceneGraph& graph, const ScenePrewarmConfig& config) {
    const u32 scene_entity_count = std::max(config.scene_entities, config.renderables);
    graph.reserve_nodes(scene_entity_count);
    graph.reserve_analytic_spheres(config.analytic_spheres);

    world.reserve_entities(scene_entity_count);
    world.reserve_archetype<TransformComponent>(scene_entity_count);
    world.reserve_archetype<SceneNodeComponent, TransformComponent>(scene_entity_count);
    if (config.renderables != 0u) {
        world.reserve_archetype<RenderableComponent, SceneNodeComponent, TransformComponent>(
            config.renderables);
    }

    const u32 structural_ops = config.deferred_structural_changes != 0u
                                   ? config.deferred_structural_changes
                                   : scene_entity_count * 2u + config.renderables;
    const u32 structural_bytes = scene_entity_count * static_cast<u32>(sizeof(TransformComponent) +
                                                                       sizeof(SceneNodeComponent)) +
                                 config.renderables * static_cast<u32>(sizeof(RenderableComponent));
    world.reserve_structural_changes(structural_ops, structural_bytes);
}

inline Entity create_scene_entity(World& world,
                                  SceneGraph& graph,
                                  SceneNode parent = kInvalidSceneNode,
                                  const LocalTransform& local = {}) {
    const Entity entity = world.create();
    const SceneNode node = graph.create_node(parent, local);
    world.add<TransformComponent>(entity, TransformComponent{local});
    world.add<SceneNodeComponent>(entity, SceneNodeComponent{node, entity});
    return entity;
}

inline bool set_scene_entity_transform(World& world,
                                       SceneGraph& graph,
                                       Entity entity,
                                       const LocalTransform& local) {
    auto* transform = world.get<TransformComponent>(entity);
    auto* node = world.get<SceneNodeComponent>(entity);
    if (!transform || !node || !graph.alive(node->node))
        return false;
    transform->local = local;
    graph.set_local_transform(node->node, local);
    return true;
}

inline void gather_scene_render_items(World& world,
                                      const SceneGraph& graph,
                                      std::vector<SceneRenderItem>& out) {
    out.clear();
    std::mutex append_mutex;
    world.query<reads<SceneNodeComponent, RenderableComponent>, writes<>>(
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

class RuntimeScene {
   public:
    explicit RuntimeScene(World& world = World::Get()) noexcept : world_(&world) {}

    [[nodiscard]] World& world() noexcept { return *world_; }
    [[nodiscard]] const World& world() const noexcept { return *world_; }
    [[nodiscard]] SceneGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const SceneGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] ::psynder::render::MaterialLibrary& materials() noexcept { return materials_; }
    [[nodiscard]] const ::psynder::render::MaterialLibrary& materials() const noexcept {
        return materials_;
    }

    void prewarm_capacity(const ScenePrewarmConfig& config) {
        prewarm_scene_capacity(*world_, graph_, config);
        render_item_capacity_ =
            std::max(render_item_capacity_, std::max(config.render_items, config.renderables));
    }

    void reserve_render_items(std::vector<SceneRenderItem>& out) const {
        out.reserve(render_item_capacity_);
    }

    [[nodiscard]] Entity create_entity(const LocalTransform& local = {},
                                       SceneNode parent = kInvalidSceneNode) {
        return create_scene_entity(*world_, graph_, parent, local);
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
        if (auto* node_component = world_->get<SceneNodeComponent>(entity))
            node = node_component->node;
        world_->destroy(entity);
        return node.valid() ? graph_.destroy_node(node) : false;
    }

    bool set_transform(Entity entity, const LocalTransform& local) {
        return set_scene_entity_transform(*world_, graph_, entity, local);
    }

    bool attach_renderable(Entity entity, const RenderableComponent& renderable) {
        if (!world_->alive(entity) || !world_->get<SceneNodeComponent>(entity))
            return false;
        world_->add<RenderableComponent>(entity, renderable);
        return true;
    }

    bool update_renderable(Entity entity, const RenderableComponent& renderable) {
        return ::psynder::scene::update_renderable(*world_, entity, renderable);
    }

    bool set_renderable_mobility(Entity entity, ObjectMobility mobility) {
        return ::psynder::scene::set_renderable_mobility(*world_, entity, mobility);
    }

    bool mark_renderable_static(Entity entity) {
        return ::psynder::scene::mark_renderable_static(*world_, entity);
    }

    bool mark_renderable_dynamic(Entity entity) {
        return ::psynder::scene::mark_renderable_dynamic(*world_, entity);
    }

    bool get_renderable_mobility(Entity entity, ObjectMobility& out) {
        return ::psynder::scene::get_renderable_mobility(*world_, entity, out);
    }

    RenderableValidation validate_renderable_for_static_bake(Entity entity) {
        auto* renderable = world_->get<RenderableComponent>(entity);
        if (!renderable)
            return RenderableValidation{RenderableValidation_NoGeometry};
        return ::psynder::scene::validate_renderable_for_static_bake(*renderable);
    }

    SceneGraphUpdateStats update_transforms(u32 parallel_threshold = 128u) {
        return graph_.update_world_transforms(parallel_threshold);
    }

    void gather_render_items(std::vector<SceneRenderItem>& out) {
        reserve_render_items(out);
        gather_scene_render_items(*world_, graph_, out);
    }

   private:
    World* world_ = nullptr;
    SceneGraph graph_{};
    ::psynder::render::MaterialLibrary materials_{};
    u32 render_item_capacity_ = 0u;
};

}  // namespace psynder::scene
