// SPDX-License-Identifier: MIT
// Psynder — lane-06 internal editor-facing ECS snapshot helpers.

#pragma once

#include "EcsRegistry_Internal.h"

#include <cstddef>
#include <span>
#include <vector>

namespace psynder::scene::detail {

// Minimal schema/value record for editor inspectors. Field-level reflection is
// intentionally not invented here; this snapshots the component registration
// record plus the raw SoA bytes for the selected entity row.
struct EcsEditorComponentSnapshot {
    ComponentId id = 0;
    u32 size = 0;
    u32 align = 0;
    const char* name = "";
    u32 value_offset = 0;
    u32 value_size = 0;
};

struct EcsEditorEntitySnapshot {
    Entity entity{};
    u32 component_begin = 0;
    u32 component_count = 0;
    bool alive = false;
};

struct EcsEditorSelectionSnapshot {
    std::vector<EcsEditorEntitySnapshot> entities;
    std::vector<EcsEditorComponentSnapshot> components;
    std::vector<std::byte> value_bytes;

    void clear() {
        entities.clear();
        components.clear();
        value_bytes.clear();
    }

    [[nodiscard]] std::span<const EcsEditorComponentSnapshot> components_for(
        const EcsEditorEntitySnapshot& entity) const noexcept {
        if (entity.component_count == 0u)
            return {};
        return std::span<const EcsEditorComponentSnapshot>{
            components.data() + entity.component_begin,
            entity.component_count,
        };
    }

    [[nodiscard]] std::span<const std::byte> bytes_for(
        const EcsEditorComponentSnapshot& component) const noexcept {
        if (component.value_size == 0u)
            return {};
        return std::span<const std::byte>{
            value_bytes.data() + component.value_offset,
            component.value_size,
        };
    }
};

void snapshot_selected_entities(std::span<const Entity> selected, EcsEditorSelectionSnapshot& out);

[[nodiscard]] EcsEditorSelectionSnapshot snapshot_selected_entities(std::span<const Entity> selected);

}  // namespace psynder::scene::detail
