// SPDX-License-Identifier: MIT
// Psynder — lane-06 internal editor-facing ECS snapshot helpers.

#include "EcsEditorSnapshot_Internal.h"

#include "Registry.h"
#include "scene/PhysicsComponents.h"
#include "scene/SceneEcs.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace psynder::scene::detail {

namespace {

constexpr u32 entity_index_of(Entity e) noexcept {
    return e.index() == 0 ? 0xFFFFFFFFu : e.index() - 1u;
}

[[nodiscard]] EcsEditorWellKnownComponentKind well_known_component_kind(ComponentId id) {
    if (id == component_id<TransformComponent>())
        return EcsEditorWellKnownComponentKind::Transform;
    if (id == component_id<SceneNodeComponent>())
        return EcsEditorWellKnownComponentKind::SceneNode;
    if (id == component_id<EntityNameComponent>())
        return EcsEditorWellKnownComponentKind::EntityName;
    if (id == component_id<CameraComponent>())
        return EcsEditorWellKnownComponentKind::Camera;
    if (id == component_id<LightComponent>())
        return EcsEditorWellKnownComponentKind::Light;
    if (id == component_id<RenderableComponent>())
        return EcsEditorWellKnownComponentKind::Renderable;
    if (id == component_id<RigidBodyComponent>())
        return EcsEditorWellKnownComponentKind::RigidBody;
    if (id == component_id<VehicleComponent>())
        return EcsEditorWellKnownComponentKind::Vehicle;
    if (id == component_id<HelicopterComponent>())
        return EcsEditorWellKnownComponentKind::Helicopter;
    if (id == component_id<CharacterControllerComponent>())
        return EcsEditorWellKnownComponentKind::CharacterController;
    return EcsEditorWellKnownComponentKind::Unknown;
}

[[nodiscard]] EcsEditorEntityKind entity_kind(EcsRegistry& registry, Entity entity) {
    if (registry.get<CameraComponent>(entity))
        return EcsEditorEntityKind::Camera;
    if (registry.get<LightComponent>(entity))
        return EcsEditorEntityKind::Light;
    if (registry.get<RenderableComponent>(entity))
        return EcsEditorEntityKind::Renderable;
    return EcsEditorEntityKind::Empty;
}

[[nodiscard]] std::string default_entity_name(EcsEditorEntityKind kind, Entity entity) {
    switch (kind) {
        case EcsEditorEntityKind::Camera:
            return "Camera";
        case EcsEditorEntityKind::Light:
            return "Light";
        case EcsEditorEntityKind::Renderable:
            return "Renderable";
        case EcsEditorEntityKind::Empty:
            break;
    }
    return "Entity " + std::to_string(entity.raw);
}

[[nodiscard]] std::string editor_entity_name(EcsRegistry& registry,
                                             Entity entity,
                                             EcsEditorEntityKind kind) {
    if (const auto* name = registry.get<EntityNameComponent>(entity)) {
        if (!entity_name_empty(*name))
            return std::string{entity_name_view(*name)};
    }
    return default_entity_name(kind, entity);
}

}  // namespace

void EcsRegistryImpl::snapshot_selected_entities(std::span<const Entity> selected,
                                                 EcsEditorSelectionSnapshot& out) const {
    out.clear();
    out.entities.reserve(selected.size());

    std::lock_guard<std::mutex> lk(mutex_);
    for (Entity entity : selected) {
        EcsEditorEntitySnapshot entity_snapshot{};
        entity_snapshot.entity = entity;
        entity_snapshot.component_begin = static_cast<u32>(out.components.size());

        const u32 entity_idx = entity_index_of(entity);
        if (entity_idx == 0xFFFFFFFFu || entity_idx >= entities_.size()) {
            out.entities.push_back(entity_snapshot);
            continue;
        }

        const EntitySlot& slot = entities_[entity_idx];
        if (!slot.alive || slot.generation != entity.gen()) {
            out.entities.push_back(entity_snapshot);
            continue;
        }

        entity_snapshot.alive = true;

        const Archetype& arche = archetypes_[slot.archetype_id];
        const Chunk* chunk = arche.chunk(slot.chunk_index);
        const auto component_ids = arche.components();
        const auto column_sizes = arche.column_sizes();
        entity_snapshot.component_count = static_cast<u32>(component_ids.size());
        out.components.reserve(out.components.size() + component_ids.size());

        for (usize i = 0; i < component_ids.size(); ++i) {
            const u32 column = static_cast<u32>(i);
            const ComponentId component_id = component_ids[column];
            const ComponentRecord record = ComponentRegistry::Get().lookup(component_id);
            const u32 value_size = column_sizes[column];
            const u32 value_offset = static_cast<u32>(out.value_bytes.size());

            EcsEditorComponentSnapshot component_snapshot{};
            component_snapshot.id = component_id;
            component_snapshot.size = record.size != 0u ? record.size : value_size;
            component_snapshot.align = record.align;
            component_snapshot.name = record.name ? record.name : "";
            component_snapshot.kind = well_known_component_kind(component_id);
            component_snapshot.value_offset = value_offset;
            component_snapshot.value_size = value_size;
            out.components.push_back(component_snapshot);

            out.value_bytes.resize(out.value_bytes.size() + value_size);
            if (value_size == 0u)
                continue;

            const std::byte* src = arche.column_base(chunk, column) +
                                   static_cast<usize>(slot.row_in_chunk) * value_size;
            std::memcpy(out.value_bytes.data() + value_offset, src, value_size);
            if (component_snapshot.kind == EcsEditorWellKnownComponentKind::SceneNode &&
                value_size == sizeof(SceneNodeComponent)) {
                SceneNodeComponent node{};
                std::memcpy(&node, src, sizeof(node));
                entity_snapshot.node = node.node;
            } else if (component_snapshot.kind == EcsEditorWellKnownComponentKind::EntityName &&
                       value_size == sizeof(EntityNameComponent)) {
                EntityNameComponent name{};
                std::memcpy(&name, src, sizeof(name));
                entity_snapshot.name = std::string{entity_name_view(name)};
            } else if (component_snapshot.kind == EcsEditorWellKnownComponentKind::Camera) {
                entity_snapshot.kind = EcsEditorEntityKind::Camera;
            } else if (component_snapshot.kind == EcsEditorWellKnownComponentKind::Light &&
                       entity_snapshot.kind != EcsEditorEntityKind::Camera) {
                entity_snapshot.kind = EcsEditorEntityKind::Light;
            } else if (component_snapshot.kind == EcsEditorWellKnownComponentKind::Renderable &&
                       entity_snapshot.kind == EcsEditorEntityKind::Empty) {
                entity_snapshot.kind = EcsEditorEntityKind::Renderable;
            }
        }

        if (entity_snapshot.name.empty())
            entity_snapshot.name = default_entity_name(entity_snapshot.kind, entity);

        out.entities.push_back(entity_snapshot);
    }
}

void snapshot_selected_entities(std::span<const Entity> selected, EcsEditorSelectionSnapshot& out) {
    EcsRegistryImpl::Get().snapshot_selected_entities(selected, out);
}

EcsEditorSelectionSnapshot snapshot_selected_entities(std::span<const Entity> selected) {
    EcsEditorSelectionSnapshot out;
    snapshot_selected_entities(selected, out);
    return out;
}

void snapshot_scene_authoring(Scene& scene, EcsEditorSceneSnapshot& out) {
    out.clear();
    out.environment = sanitize_environment_settings(scene.environment().settings());
    out.render_settings = sanitize_render_settings(scene.render_settings());

    EcsRegistry& registry = scene.registry();
    const u32 total = registry.snapshot_live_entities(std::span<Entity>{});
    std::vector<Entity> entities(total);
    const u32 copied = registry.snapshot_live_entities(entities);
    entities.resize(std::min<u32>(total, copied));

    std::unordered_map<u32, Entity> node_to_entity;
    node_to_entity.reserve(entities.size());
    out.hierarchy.reserve(entities.size());

    for (Entity entity : entities) {
        const auto* node_component = registry.get<SceneNodeComponent>(entity);
        if (!node_component || !scene.graph().alive(node_component->node))
            continue;
        node_to_entity.emplace(node_component->node.raw, entity);
    }

    for (Entity entity : entities) {
        const auto* node_component = registry.get<SceneNodeComponent>(entity);
        if (!node_component || !scene.graph().alive(node_component->node))
            continue;

        EcsEditorEntitySnapshot snapshot{};
        snapshot.entity = entity;
        snapshot.node = node_component->node;
        snapshot.parent_node = scene.graph().parent(snapshot.node);
        if (snapshot.parent_node.valid()) {
            if (const auto it = node_to_entity.find(snapshot.parent_node.raw);
                it != node_to_entity.end()) {
                snapshot.parent_entity = it->second;
            }
        }
        snapshot.alive = true;
        snapshot.kind = entity_kind(registry, entity);
        snapshot.name = editor_entity_name(registry, entity, snapshot.kind);

        for (SceneNode parent = snapshot.parent_node; parent.valid();
             parent = scene.graph().parent(parent)) {
            ++snapshot.depth;
            if (snapshot.depth > 1024u)
                break;
        }
        out.hierarchy.push_back(std::move(snapshot));
    }

    for (EcsEditorEntitySnapshot& parent : out.hierarchy) {
        parent.child_count = 0u;
        for (const EcsEditorEntitySnapshot& child : out.hierarchy) {
            if (child.parent_node == parent.node)
                ++parent.child_count;
        }
    }

    std::sort(out.hierarchy.begin(),
              out.hierarchy.end(),
              [](const EcsEditorEntitySnapshot& a, const EcsEditorEntitySnapshot& b) {
                  if (a.depth != b.depth)
                      return a.depth < b.depth;
                  return a.node.raw < b.node.raw;
              });
}

EcsEditorSceneSnapshot snapshot_scene_authoring(Scene& scene) {
    EcsEditorSceneSnapshot out;
    snapshot_scene_authoring(scene, out);
    return out;
}

}  // namespace psynder::scene::detail
