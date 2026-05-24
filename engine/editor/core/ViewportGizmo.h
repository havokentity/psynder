// SPDX-License-Identifier: MIT
// Psynder — editor-core viewport gizmo foundation.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "scene/SceneGraph.h"

namespace psynder::scene {
class Scene;
}  // namespace psynder::scene

namespace psynder::editor::viewport {

enum class GizmoMode : u8 {
    Translate,
    Rotate,
    Scale,
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

// Draw/apply the current selection's viewport gizmo. ECS callers should pass
// frame.scene, frame.selected_entity (or selection::select_scene_entity), the
// active camera view-projection, and framebuffer-space mouse state. When the
// selected entity is alive and has a TransformComponent, drag input emits a
// transform intent and applies it through Scene::set_transform by default.
[[nodiscard]] GizmoResult draw_apply_gizmo(const GizmoFrame& frame) noexcept;

}  // namespace psynder::editor::viewport
