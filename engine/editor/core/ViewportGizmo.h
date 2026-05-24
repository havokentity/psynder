// SPDX-License-Identifier: MIT
// Psynder — editor-core viewport gizmo foundation.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "scene/SceneGraph.h"

#include <string_view>

namespace psynder::scene {
class Scene;
}  // namespace psynder::scene

namespace psynder::editor::viewport {

enum class GizmoMode : u8 {
    Translate,
    Rotate,
    Scale,
};

enum class GizmoAxis : u8 {
    None,
    X,
    Y,
    Z,
    Uniform,
};

struct GizmoDragState {
    Entity entity{};
    GizmoMode mode = GizmoMode::Translate;
    GizmoAxis axis = GizmoAxis::None;
    scene::LocalTransform origin{};
    scene::LocalTransform current{};
    math::Vec2 start_mouse{0.0f, 0.0f};
    math::Vec2 last_mouse{0.0f, 0.0f};
    bool active = false;
};

struct GizmoState {
    GizmoMode mode = GizmoMode::Translate;
    GizmoDragState drag{};
};

struct GizmoFrame {
    scene::Scene* scene = nullptr;
    Entity selected_entity{};
    math::Mat4 view_projection{};
    platform::MouseState mouse{};
    math::Vec2 framebuffer_size{0.0f, 0.0f};
    GizmoMode mode = GizmoMode::Translate;
    bool apply_transform = true;
};

struct GizmoTransformIntent {
    Entity entity{};
    GizmoMode mode = GizmoMode::Translate;
    scene::LocalTransform before{};
    scene::LocalTransform after{};
    bool valid = false;
};

struct GizmoResult {
    // Legacy editor-state selection id. Prefer scene_entity for ECS-backed
    // viewport integrations.
    u32 selected_entity = 0u;
    Entity scene_entity{};
    bool visible = false;
    bool hot = false;
    bool applied = false;
    GizmoTransformIntent transform{};
};

[[nodiscard]] std::string_view gizmo_mode_name(GizmoMode mode) noexcept;
[[nodiscard]] std::string_view gizmo_mode_label(GizmoMode mode) noexcept;
[[nodiscard]] bool parse_gizmo_mode(std::string_view text, GizmoMode& out_mode) noexcept;
[[nodiscard]] GizmoMode next_gizmo_mode(GizmoMode mode) noexcept;
[[nodiscard]] GizmoMode previous_gizmo_mode(GizmoMode mode) noexcept;

void set_gizmo_mode(GizmoState& state, GizmoMode mode) noexcept;
void cancel_gizmo_drag(GizmoState& state) noexcept;
[[nodiscard]] bool begin_gizmo_drag(GizmoState& state,
                                    Entity entity,
                                    GizmoMode mode,
                                    scene::LocalTransform origin,
                                    math::Vec2 mouse,
                                    GizmoAxis axis = GizmoAxis::Uniform) noexcept;
[[nodiscard]] bool update_gizmo_drag(GizmoState& state,
                                     scene::LocalTransform current,
                                     math::Vec2 mouse) noexcept;
[[nodiscard]] GizmoTransformIntent end_gizmo_drag(GizmoState& state) noexcept;

[[nodiscard]] scene::LocalTransform preview_transform(scene::LocalTransform before,
                                                      GizmoMode mode,
                                                      const platform::MouseState& mouse,
                                                      math::Vec2 framebuffer_size) noexcept;

// Draw/apply the current selection's viewport gizmo. ECS callers should pass
// frame.scene, frame.selected_entity (or selection::select_scene_entity), the
// active camera view-projection, and framebuffer-space mouse state. When the
// selected entity is alive and has a TransformComponent, drag input emits a
// transform intent and applies it through Scene::set_transform by default.
[[nodiscard]] GizmoResult draw_apply_gizmo(const GizmoFrame& frame) noexcept;
[[nodiscard]] GizmoResult draw_apply_gizmo(const GizmoFrame& frame, GizmoState* state) noexcept;

}  // namespace psynder::editor::viewport
