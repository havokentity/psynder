// SPDX-License-Identifier: MIT
// Psynder — lane-06 internal editor-facing ECS snapshot helpers.

#include "EcsEditorSnapshot_Internal.h"

#include "Registry.h"

#include <cstring>

namespace psynder::scene::detail {

namespace {

constexpr u32 entity_index_of(Entity e) noexcept {
    return e.index() == 0 ? 0xFFFFFFFFu : e.index() - 1u;
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
            component_snapshot.value_offset = value_offset;
            component_snapshot.value_size = value_size;
            out.components.push_back(component_snapshot);

            out.value_bytes.resize(out.value_bytes.size() + value_size);
            if (value_size == 0u)
                continue;

            const std::byte* src = arche.column_base(chunk, column) +
                                   static_cast<usize>(slot.row_in_chunk) * value_size;
            std::memcpy(out.value_bytes.data() + value_offset, src, value_size);
        }

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

}  // namespace psynder::scene::detail
