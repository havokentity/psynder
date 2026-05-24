// SPDX-License-Identifier: MIT
// Psynder — editor-core viewport gizmo foundation implementation.

#include "ViewportGizmo.h"

#include "editor/core/EditorState.h"
#include "editor/core/Selection.h"
#include "scene/SceneEcs.h"
#include "ui/imm/Gizmo.h"

#include <cmath>

namespace psynder::editor::viewport {

namespace {

[[nodiscard]] bool has_mouse_delta(const platform::MouseState& mouse) noexcept {
    return std::fabs(mouse.dx) > 0.0001f || std::fabs(mouse.dy) > 0.0001f;
}

[[nodiscard]] scene::LocalTransform mutated_transform(scene::LocalTransform before,
                                                      GizmoMode mode,
                                                      const platform::MouseState& mouse,
                                                      math::Vec2 framebuffer_size) noexcept {
    scene::LocalTransform after = before;
    const f32 min_extent = (framebuffer_size.x > 0.0f && framebuffer_size.x < framebuffer_size.y)
                               ? framebuffer_size.x
                               : framebuffer_size.y;
    const f32 translate_units_per_px = min_extent > 0.0f ? (2.0f / min_extent) : 0.01f;

    switch (mode) {
        case GizmoMode::Translate:
            after.translation.x += mouse.dx * translate_units_per_px;
            after.translation.y -= mouse.dy * translate_units_per_px;
            break;
        case GizmoMode::Rotate: {
            const f32 angle = mouse.dx * 0.01f;
            if (std::fabs(angle) > 0.0001f) {
                const math::Quat delta = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, angle);
                after.rotation = math::quat_normalize(math::quat_mul(delta, before.rotation));
            }
            break;
        }
        case GizmoMode::Scale: {
            f32 factor = 1.0f + (mouse.dx - mouse.dy) * 0.01f;
            if (factor < 0.01f)
                factor = 0.01f;
            after.scale = math::mul(before.scale, factor);
            break;
        }
    }

    return after;
}

void draw_scene_gizmo(GizmoResult& result, const GizmoFrame& frame, scene::LocalTransform local) noexcept {
    const math::Vec2 mouse_screen{frame.mouse.x, frame.mouse.y};
    switch (frame.mode) {
        case GizmoMode::Translate:
            result.hot = ui::imm::gizmo_translate(local.translation,
                                                  frame.view_projection,
                                                  mouse_screen,
                                                  frame.mouse.left,
                                                  frame.framebuffer_size);
            break;
        case GizmoMode::Rotate:
            result.hot = ui::imm::gizmo_rotate(local.translation,
                                               frame.view_projection,
                                               local.translation,
                                               mouse_screen,
                                               frame.mouse.left,
                                               frame.framebuffer_size);
            break;
        case GizmoMode::Scale:
            result.hot = ui::imm::gizmo_scale(local.scale,
                                              frame.view_projection,
                                              local.translation,
                                              mouse_screen,
                                              frame.mouse.left,
                                              frame.framebuffer_size);
            break;
    }
}

[[nodiscard]] GizmoResult draw_legacy_editor_record_gizmo(const GizmoFrame& frame) noexcept {
    GizmoResult result{};
    result.selected_entity = selection::selected_entity();
    if (result.selected_entity == selection::kNoEntity)
        return result;

    detail::EntityRec* entity = detail::find_entity(result.selected_entity);
    if (!entity || !entity->alive) {
        selection::clear_selection();
        result.selected_entity = selection::kNoEntity;
        return result;
    }

    result.visible = true;

    const math::Vec2 mouse_screen{frame.mouse.x, frame.mouse.y};
    switch (frame.mode) {
        case GizmoMode::Translate:
            result.hot = ui::imm::gizmo_translate(entity->position,
                                                  frame.view_projection,
                                                  mouse_screen,
                                                  frame.mouse.left,
                                                  frame.framebuffer_size);
            break;
        case GizmoMode::Rotate:
            result.hot = ui::imm::gizmo_rotate(entity->position,
                                               frame.view_projection,
                                               entity->position,
                                               mouse_screen,
                                               frame.mouse.left,
                                               frame.framebuffer_size);
            break;
        case GizmoMode::Scale:
            result.hot = ui::imm::gizmo_scale(entity->scale,
                                              frame.view_projection,
                                              entity->position,
                                              mouse_screen,
                                              frame.mouse.left,
                                              frame.framebuffer_size);
            break;
    }

    // Current IMM gizmo helpers draw and hit-test only; they do not emit a
    // world-space transform delta yet. Keep `applied` false until that API
    // lands, so callers can distinguish hover/drag capture from mutation.
    result.applied = false;
    return result;
}

}  // namespace

GizmoResult draw_apply_gizmo(const GizmoFrame& frame) noexcept {
    GizmoResult result{};

    const Entity selected =
        frame.selected_entity.valid() ? frame.selected_entity : selection::selected_scene_entity();
    if (!selected.valid())
        return draw_legacy_editor_record_gizmo(frame);

    result.scene_entity = selected;
    result.selected_entity = selected.raw;

    if (!frame.scene || !frame.scene->registry().alive(selected)) {
        if (selection::is_scene_selected(selected))
            selection::clear_selection();
        result.scene_entity = {};
        result.selected_entity = selection::kNoEntity;
        return result;
    }

    const auto* transform = frame.scene->registry().get<scene::TransformComponent>(selected);
    if (!transform)
        return result;

    result.visible = true;
    const scene::LocalTransform before = transform->local;
    draw_scene_gizmo(result, frame, before);

    if (result.hot && frame.mouse.left && has_mouse_delta(frame.mouse)) {
        result.transform = GizmoTransformIntent{
            .entity = selected,
            .mode = frame.mode,
            .before = before,
            .after = mutated_transform(before, frame.mode, frame.mouse, frame.framebuffer_size),
            .valid = true,
        };

        if (frame.apply_transform)
            result.applied = frame.scene->set_transform(selected, result.transform.after);
    }

    return result;
}

}  // namespace psynder::editor::viewport
