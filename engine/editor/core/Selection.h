// SPDX-License-Identifier: MIT
// Psynder — editor-core selection helper.

#pragma once

#include "core/Types.h"

namespace psynder::scene {
class Scene;
}  // namespace psynder::scene

namespace psynder::editor::selection {

inline constexpr u32 kNoEntity = 0u;

// Store a single editor-stable entity id as the active selection.
// Returns false and clears selection when `editor_entity_id` is not a live
// editor entity. Pass kNoEntity to clear explicitly.
bool select_entity(u32 editor_entity_id) noexcept;

// Select a live scene ECS entity by handle/raw id. These APIs are the
// viewport-facing selection path; the legacy editor-stable id helpers above
// remain for panels that still mirror editor state records.
bool select_scene_entity(scene::Scene* scene, Entity entity) noexcept;
bool select_scene_entity_raw(scene::Scene* scene, u32 entity_raw) noexcept;
void mirror_scene_entity_raw(u32 entity_raw) noexcept;

void clear_selection() noexcept;

[[nodiscard]] u32 selected_entity() noexcept;
[[nodiscard]] Entity selected_scene_entity() noexcept;
[[nodiscard]] u32 selected_scene_entity_raw() noexcept;
[[nodiscard]] bool has_selection() noexcept;
[[nodiscard]] bool has_scene_selection(scene::Scene* scene = nullptr) noexcept;
[[nodiscard]] bool is_selected(u32 editor_entity_id) noexcept;
[[nodiscard]] bool is_scene_selected(Entity entity) noexcept;
[[nodiscard]] bool is_scene_selected_raw(u32 entity_raw) noexcept;

}  // namespace psynder::editor::selection
