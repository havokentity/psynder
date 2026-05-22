// SPDX-License-Identifier: MIT
// Psynder — ECS components that bind scene nodes to render submissions.

#pragma once

#include "core/Types.h"
#include "math/Bounds.h"
#include "render/Material.h"
#include "scene/SceneGraph.h"
#include "scene/World.h"

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
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const RenderableComponent> renderables) {
            const usize n = std::min(nodes.size(), renderables.size());
            thread_local std::vector<SceneRenderItem> chunk_items;
            chunk_items.clear();
            chunk_items.reserve(n);
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
                chunk_items.push_back(item);
            }
            if (!chunk_items.empty()) {
                std::scoped_lock lock{append_mutex};
                out.reserve(out.size() + chunk_items.size());
                out.insert(out.end(), chunk_items.begin(), chunk_items.end());
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

    SceneGraphUpdateStats update_transforms(u32 parallel_threshold = 128u) {
        return graph_.update_world_transforms(parallel_threshold);
    }

    void gather_render_items(std::vector<SceneRenderItem>& out) {
        gather_scene_render_items(*world_, graph_, out);
    }

   private:
    World* world_ = nullptr;
    SceneGraph graph_{};
    ::psynder::render::MaterialLibrary materials_{};
};

}  // namespace psynder::scene
