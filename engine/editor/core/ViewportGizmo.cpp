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

[[nodiscard]] math::Vec2 mouse_pos(const platform::MouseState& mouse) noexcept {
    return {mouse.x, mouse.y};
}

[[nodiscard]] f32 shortest_framebuffer_extent(math::Vec2 framebuffer_size) noexcept {
    if (framebuffer_size.x <= 0.0f || framebuffer_size.y <= 0.0f)
        return 0.0f;
    return framebuffer_size.x < framebuffer_size.y ? framebuffer_size.x : framebuffer_size.y;
}

[[nodiscard]] f32 translate_units_per_px(math::Vec2 framebuffer_size) noexcept {
    const f32 min_extent = shortest_framebuffer_extent(framebuffer_size);
    return min_extent > 0.0f ? (2.0f / min_extent) : 0.01f;
}

[[nodiscard]] f32 rotate_radians_from_delta(const platform::MouseState& mouse) noexcept {
    return mouse.dx * 0.01f;
}

[[nodiscard]] f32 scale_factor_from_delta(const platform::MouseState& mouse) noexcept {
    f32 factor = 1.0f + (mouse.dx - mouse.dy) * 0.01f;
    if (factor < 0.01f)
        factor = 0.01f;
    return factor;
}

[[nodiscard]] bool text_equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (usize i = 0u; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

[[nodiscard]] bool same_transform(const scene::LocalTransform& a, const scene::LocalTransform& b) noexcept {
    return std::fabs(a.translation.x - b.translation.x) <= 0.0001f &&
           std::fabs(a.translation.y - b.translation.y) <= 0.0001f &&
           std::fabs(a.translation.z - b.translation.z) <= 0.0001f &&
           std::fabs(a.rotation.x - b.rotation.x) <= 0.0001f &&
           std::fabs(a.rotation.y - b.rotation.y) <= 0.0001f &&
           std::fabs(a.rotation.z - b.rotation.z) <= 0.0001f &&
           std::fabs(a.rotation.w - b.rotation.w) <= 0.0001f &&
           std::fabs(a.scale.x - b.scale.x) <= 0.0001f &&
           std::fabs(a.scale.y - b.scale.y) <= 0.0001f &&
           std::fabs(a.scale.z - b.scale.z) <= 0.0001f;
}

[[nodiscard]] GizmoMode effective_mode(const GizmoFrame& frame, const GizmoState* state) noexcept {
    return state ? state->mode : frame.mode;
}

[[nodiscard]] GizmoFrame with_mode(GizmoFrame frame, GizmoMode mode) noexcept {
    frame.mode = mode;
    return frame;
}

}  // namespace

std::string_view gizmo_mode_name(GizmoMode mode) noexcept {
    switch (mode) {
        case GizmoMode::Translate:
            return "translate";
        case GizmoMode::Rotate:
            return "rotate";
        case GizmoMode::Scale:
            return "scale";
    }
    return "translate";
}

std::string_view gizmo_mode_label(GizmoMode mode) noexcept {
    switch (mode) {
        case GizmoMode::Translate:
            return "Translate";
        case GizmoMode::Rotate:
            return "Rotate";
        case GizmoMode::Scale:
            return "Scale";
    }
    return "Translate";
}

bool parse_gizmo_mode(std::string_view text, GizmoMode& out_mode) noexcept {
    if (text_equals_ci(text, "translate") || text_equals_ci(text, "move")) {
        out_mode = GizmoMode::Translate;
        return true;
    }
    if (text_equals_ci(text, "rotate") || text_equals_ci(text, "rotation")) {
        out_mode = GizmoMode::Rotate;
        return true;
    }
    if (text_equals_ci(text, "scale")) {
        out_mode = GizmoMode::Scale;
        return true;
    }
    return false;
}

GizmoMode next_gizmo_mode(GizmoMode mode) noexcept {
    switch (mode) {
        case GizmoMode::Translate:
            return GizmoMode::Rotate;
        case GizmoMode::Rotate:
            return GizmoMode::Scale;
        case GizmoMode::Scale:
            return GizmoMode::Translate;
    }
    return GizmoMode::Translate;
}

GizmoMode previous_gizmo_mode(GizmoMode mode) noexcept {
    switch (mode) {
        case GizmoMode::Translate:
            return GizmoMode::Scale;
        case GizmoMode::Rotate:
            return GizmoMode::Translate;
        case GizmoMode::Scale:
            return GizmoMode::Rotate;
    }
    return GizmoMode::Translate;
}

void set_gizmo_mode(GizmoState& state, GizmoMode mode) noexcept {
    if (state.mode == mode)
        return;
    state.mode = mode;
    cancel_gizmo_drag(state);
}

void cancel_gizmo_drag(GizmoState& state) noexcept {
    state.drag = GizmoDragState{};
}

bool begin_gizmo_drag(GizmoState& state,
                      Entity entity,
                      GizmoMode mode,
                      scene::LocalTransform origin,
                      math::Vec2 mouse,
                      GizmoAxis axis) noexcept {
    if (!entity.valid())
        return false;
    state.mode = mode;
    state.drag = GizmoDragState{
        .entity = entity,
        .mode = mode,
        .axis = axis,
        .origin = origin,
        .current = origin,
        .start_mouse = mouse,
        .last_mouse = mouse,
        .active = true,
    };
    return true;
}

bool update_gizmo_drag(GizmoState& state, scene::LocalTransform current, math::Vec2 mouse) noexcept {
    if (!state.drag.active)
        return false;
    state.drag.current = current;
    state.drag.last_mouse = mouse;
    return true;
}

GizmoTransformIntent end_gizmo_drag(GizmoState& state) noexcept {
    GizmoTransformIntent intent{};
    if (state.drag.active && !same_transform(state.drag.origin, state.drag.current)) {
        intent = GizmoTransformIntent{
            .entity = state.drag.entity,
            .mode = state.drag.mode,
            .before = state.drag.origin,
            .after = state.drag.current,
            .valid = state.drag.entity.valid(),
        };
    }
    cancel_gizmo_drag(state);
    return intent;
}

scene::LocalTransform preview_transform(scene::LocalTransform before,
                                        GizmoMode mode,
                                        const platform::MouseState& mouse,
                                        math::Vec2 framebuffer_size) noexcept {
    scene::LocalTransform after = before;

    switch (mode) {
        case GizmoMode::Translate:
            after.translation.x += mouse.dx * translate_units_per_px(framebuffer_size);
            after.translation.y -= mouse.dy * translate_units_per_px(framebuffer_size);
            break;
        case GizmoMode::Rotate: {
            const f32 angle = rotate_radians_from_delta(mouse);
            if (std::fabs(angle) > 0.0001f) {
                const math::Quat delta = math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, angle);
                after.rotation = math::quat_normalize(math::quat_mul(delta, before.rotation));
            }
            break;
        }
        case GizmoMode::Scale: {
            after.scale = math::mul(before.scale, scale_factor_from_delta(mouse));
            break;
        }
    }

    return after;
}

namespace {

void draw_scene_gizmo(GizmoResult& result, const GizmoFrame& frame, scene::LocalTransform local) noexcept {
    const math::Vec2 mouse_screen = mouse_pos(frame.mouse);
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
    return draw_apply_gizmo(frame, nullptr);
}

GizmoResult draw_apply_gizmo(const GizmoFrame& frame, GizmoState* state) noexcept {
    GizmoResult result{};
    const GizmoMode mode = effective_mode(frame, state);
    const GizmoFrame active_frame = with_mode(frame, mode);

    const Entity selected =
        active_frame.selected_entity.valid() ? active_frame.selected_entity : selection::selected_scene_entity();
    if (!selected.valid())
        return draw_legacy_editor_record_gizmo(active_frame);

    result.scene_entity = selected;
    result.selected_entity = selected.raw;

    if (!active_frame.scene || !active_frame.scene->registry().alive(selected)) {
        if (selection::is_scene_selected(selected))
            selection::clear_selection();
        if (state)
            cancel_gizmo_drag(*state);
        result.scene_entity = {};
        result.selected_entity = selection::kNoEntity;
        return result;
    }

    const auto* transform = active_frame.scene->registry().get<scene::TransformComponent>(selected);
    if (!transform)
        return result;

    result.visible = true;
    const scene::LocalTransform before = transform->local;
    draw_scene_gizmo(result, active_frame, before);

    if (state && state->drag.active && state->drag.entity != selected)
        cancel_gizmo_drag(*state);

    if (result.hot && active_frame.mouse.left && state && !state->drag.active) {
        (void)begin_gizmo_drag(*state, selected, mode, before, mouse_pos(active_frame.mouse));
    }

    if (result.hot && active_frame.mouse.left && has_mouse_delta(active_frame.mouse)) {
        const scene::LocalTransform after =
            preview_transform(before, mode, active_frame.mouse, active_frame.framebuffer_size);
        result.transform = GizmoTransformIntent{
            .entity = selected,
            .mode = mode,
            .before = before,
            .after = after,
            .valid = true,
        };

        if (active_frame.apply_transform)
            result.applied = active_frame.scene->set_transform(selected, result.transform.after);
        if (state && result.applied)
            (void)update_gizmo_drag(*state, result.transform.after, mouse_pos(active_frame.mouse));
    }

    return result;
}

}  // namespace psynder::editor::viewport
