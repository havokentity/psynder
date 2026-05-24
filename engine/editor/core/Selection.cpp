// SPDX-License-Identifier: MIT
// Psynder — editor-core selection helper implementation.

#include "Selection.h"

#include "editor/core/EditorState.h"
#include "scene/SceneEcs.h"

#include <atomic>

namespace psynder::editor::selection {

namespace {

std::atomic<u32>& selected_scene_raw_storage() noexcept {
    static std::atomic<u32> selected{0u};
    return selected;
}

[[nodiscard]] bool scene_entity_alive(scene::Scene* scene, Entity entity) noexcept {
    return scene && entity.valid() && scene->registry().alive(entity);
}

}  // namespace

bool select_entity(u32 editor_entity_id) noexcept {
    if (editor_entity_id == kNoEntity) {
        clear_selection();
        return false;
    }

    const detail::EntityRec* rec = detail::find_entity(editor_entity_id);
    if (!rec || !rec->alive) {
        clear_selection();
        return false;
    }

    auto& s = detail::get_state();
    s.selection.clear();
    s.selection.push_back(editor_entity_id);
    selected_scene_raw_storage().store(0u, std::memory_order_release);
    return true;
}

bool select_scene_entity(scene::Scene* scene, Entity entity) noexcept {
    if (!scene_entity_alive(scene, entity)) {
        clear_selection();
        return false;
    }

    detail::get_state().selection.clear();
    selected_scene_raw_storage().store(entity.raw, std::memory_order_release);
    return true;
}

bool select_scene_entity_raw(scene::Scene* scene, u32 entity_raw) noexcept {
    return select_scene_entity(scene, Entity{entity_raw});
}

void mirror_scene_entity_raw(u32 entity_raw) noexcept {
    detail::get_state().selection.clear();
    selected_scene_raw_storage().store(entity_raw, std::memory_order_release);
}

void clear_selection() noexcept {
    detail::get_state().selection.clear();
    selected_scene_raw_storage().store(0u, std::memory_order_release);
}

u32 selected_entity() noexcept {
    const auto& selection = detail::get_state().selection;
    return selection.empty() ? kNoEntity : selection.front();
}

Entity selected_scene_entity() noexcept {
    return Entity{selected_scene_raw_storage().load(std::memory_order_acquire)};
}

u32 selected_scene_entity_raw() noexcept {
    return selected_scene_entity().raw;
}

bool has_selection() noexcept {
    return selected_entity() != kNoEntity;
}

bool has_scene_selection(scene::Scene* scene) noexcept {
    const Entity entity = selected_scene_entity();
    if (!entity.valid())
        return false;
    return scene ? scene_entity_alive(scene, entity) : true;
}

bool is_selected(u32 editor_entity_id) noexcept {
    return editor_entity_id != kNoEntity && selected_entity() == editor_entity_id;
}

bool is_scene_selected(Entity entity) noexcept {
    return entity.valid() && selected_scene_entity() == entity;
}

bool is_scene_selected_raw(u32 entity_raw) noexcept {
    return is_scene_selected(Entity{entity_raw});
}

}  // namespace psynder::editor::selection
