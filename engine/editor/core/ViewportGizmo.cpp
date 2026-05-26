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

[[nodiscard]] math::Vec2 project_to_screen(math::Vec3 world,
                                           const math::Mat4& view_projection,
                                           math::Vec2 screen_size) noexcept {
    const math::Vec4 clip =
        math::mul(view_projection, math::Vec4{world.x, world.y, world.z, 1.0f});
    if (clip.w <= 0.0001f)
        return {std::nanf(""), std::nanf("")};
    const f32 ndc_x = clip.x / clip.w;
    const f32 ndc_y = clip.y / clip.w;
    return {
        (ndc_x * 0.5f + 0.5f) * screen_size.x,
        (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_size.y,
    };
}

[[nodiscard]] f32 distance_to_segment(math::Vec2 p, math::Vec2 a, math::Vec2 b) noexcept {
    const f32 abx = b.x - a.x;
    const f32 aby = b.y - a.y;
    const f32 apx = p.x - a.x;
    const f32 apy = p.y - a.y;
    const f32 ab_len2 = abx * abx + aby * aby;
    if (ab_len2 <= 0.0001f)
        return std::sqrt(apx * apx + apy * apy);
    f32 t = (apx * abx + apy * aby) / ab_len2;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    const f32 cx = a.x + abx * t - p.x;
    const f32 cy = a.y + aby * t - p.y;
    return std::sqrt(cx * cx + cy * cy);
}

[[nodiscard]] math::Vec2 fixed_screen_tip(math::Vec2 origin, math::Vec2 projected) noexcept {
    constexpr f32 kArmLengthPx = 64.0f;
    if (std::isnan(projected.x) || std::isnan(projected.y))
        return origin;
    const f32 dx = projected.x - origin.x;
    const f32 dy = projected.y - origin.y;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f)
        return {origin.x + kArmLengthPx, origin.y};
    const f32 scale = kArmLengthPx / len;
    return {origin.x + dx * scale, origin.y + dy * scale};
}

struct TranslateAxisPick {
    GizmoAxis axis = GizmoAxis::None;
    math::Vec2 screen_axis{0.0f, 0.0f};
    math::Vec2 screen_axis_b{0.0f, 0.0f};
};

[[nodiscard]] math::Vec2 normalized_screen_axis(math::Vec2 origin, math::Vec2 tip) noexcept {
    const f32 dx = tip.x - origin.x;
    const f32 dy = tip.y - origin.y;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f)
        return {0.0f, 0.0f};
    return {dx / len, dy / len};
}

[[nodiscard]] f32 cross2(math::Vec2 a, math::Vec2 b, math::Vec2 c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

[[nodiscard]] bool point_in_triangle(math::Vec2 p,
                                     math::Vec2 a,
                                     math::Vec2 b,
                                     math::Vec2 c) noexcept {
    const f32 c1 = cross2(a, b, p);
    const f32 c2 = cross2(b, c, p);
    const f32 c3 = cross2(c, a, p);
    const bool has_neg = c1 < 0.0f || c2 < 0.0f || c3 < 0.0f;
    const bool has_pos = c1 > 0.0f || c2 > 0.0f || c3 > 0.0f;
    return !(has_neg && has_pos);
}

[[nodiscard]] bool point_in_quad(math::Vec2 p,
                                 math::Vec2 a,
                                 math::Vec2 b,
                                 math::Vec2 c,
                                 math::Vec2 d) noexcept {
    return point_in_triangle(p, a, b, c) || point_in_triangle(p, a, c, d);
}

[[nodiscard]] math::Vec2 plane_corner(math::Vec2 origin,
                                      math::Vec2 a,
                                      math::Vec2 b,
                                      f32 da,
                                      f32 db) noexcept {
    return {origin.x + (a.x - origin.x) * da + (b.x - origin.x) * db,
            origin.y + (a.y - origin.y) * da + (b.y - origin.y) * db};
}

void pick_translate_plane(TranslateAxisPick& picked,
                          math::Vec2 mouse,
                          math::Vec2 origin,
                          math::Vec2 a,
                          math::Vec2 b,
                          GizmoAxis axis) noexcept {
    constexpr f32 kNear = 0.22f;
    constexpr f32 kFar = 0.43f;
    const math::Vec2 axis_a = normalized_screen_axis(origin, a);
    const math::Vec2 axis_b = normalized_screen_axis(origin, b);
    const f32 area = std::fabs(axis_a.x * axis_b.y - axis_a.y * axis_b.x);
    if (area < 0.2f)
        return;
    const math::Vec2 p0 = plane_corner(origin, a, b, kNear, kNear);
    const math::Vec2 p1 = plane_corner(origin, a, b, kFar, kNear);
    const math::Vec2 p2 = plane_corner(origin, a, b, kFar, kFar);
    const math::Vec2 p3 = plane_corner(origin, a, b, kNear, kFar);
    if (!point_in_quad(mouse, p0, p1, p2, p3))
        return;
    picked.axis = axis;
    picked.screen_axis = axis_a;
    picked.screen_axis_b = axis_b;
}

[[nodiscard]] TranslateAxisPick pick_translate_axis(const GizmoFrame& frame,
                                                    math::Vec3 origin,
                                                    math::Vec2 mouse) noexcept {
    constexpr f32 kHitDistPx = 8.0f;
    const math::Vec2 screen_size = shortest_framebuffer_extent(frame.framebuffer_size) > 0.0f
                                       ? frame.framebuffer_size
                                       : math::Vec2{1024.0f, 768.0f};
    const math::Vec2 o = project_to_screen(origin, frame.view_projection, screen_size);
    if (std::isnan(o.x) || std::isnan(o.y))
        return {};

    const math::Vec2 tip_x = fixed_screen_tip(
        o, project_to_screen(math::add(origin, math::Vec3{1.0f, 0.0f, 0.0f}),
                             frame.view_projection,
                             screen_size));
    const math::Vec2 tip_y = fixed_screen_tip(
        o, project_to_screen(math::add(origin, math::Vec3{0.0f, 1.0f, 0.0f}),
                             frame.view_projection,
                             screen_size));
    const math::Vec2 tip_z = fixed_screen_tip(
        o, project_to_screen(math::add(origin, math::Vec3{0.0f, 0.0f, 1.0f}),
                             frame.view_projection,
                             screen_size));

    TranslateAxisPick picked{};
    pick_translate_plane(picked, mouse, o, tip_x, tip_y, GizmoAxis::XY);
    if (picked.axis != GizmoAxis::None)
        return picked;
    pick_translate_plane(picked, mouse, o, tip_x, tip_z, GizmoAxis::XZ);
    if (picked.axis != GizmoAxis::None)
        return picked;
    pick_translate_plane(picked, mouse, o, tip_y, tip_z, GizmoAxis::YZ);
    if (picked.axis != GizmoAxis::None)
        return picked;

    f32 best = kHitDistPx;
    const f32 dx = distance_to_segment(mouse, o, tip_x);
    if (dx < best) {
        best = dx;
        picked.axis = GizmoAxis::X;
        picked.screen_axis = normalized_screen_axis(o, tip_x);
    }
    const f32 dy = distance_to_segment(mouse, o, tip_y);
    if (dy < best) {
        best = dy;
        picked.axis = GizmoAxis::Y;
        picked.screen_axis = normalized_screen_axis(o, tip_y);
    }
    const f32 dz = distance_to_segment(mouse, o, tip_z);
    if (dz < best) {
        picked.axis = GizmoAxis::Z;
        picked.screen_axis = normalized_screen_axis(o, tip_z);
    }
    return picked;
}

[[nodiscard]] f32 projected_mouse_delta(const platform::MouseState& mouse,
                                        math::Vec2 screen_axis) noexcept {
    const f32 len2 = screen_axis.x * screen_axis.x + screen_axis.y * screen_axis.y;
    if (len2 <= 0.0001f)
        return mouse.dx;
    return mouse.dx * screen_axis.x + mouse.dy * screen_axis.y;
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
                      GizmoAxis axis,
                      math::Vec2 screen_axis,
                      math::Vec2 screen_axis_b) noexcept {
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
        .screen_axis = screen_axis,
        .screen_axis_b = screen_axis_b,
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
                                        math::Vec2 framebuffer_size,
                                        GizmoAxis axis,
                                        math::Vec2 screen_axis,
                                        math::Vec2 screen_axis_b) noexcept {
    scene::LocalTransform after = before;

    switch (mode) {
        case GizmoMode::Translate: {
            const f32 units = translate_units_per_px(framebuffer_size);
            const f32 axis_delta = projected_mouse_delta(mouse, screen_axis);
            if (axis == GizmoAxis::X) {
                after.translation.x += axis_delta * units;
            } else if (axis == GizmoAxis::Y) {
                after.translation.y += axis_delta * units;
            } else if (axis == GizmoAxis::Z) {
                after.translation.z += axis_delta * units;
            } else if (axis == GizmoAxis::XY) {
                after.translation.x += axis_delta * units;
                after.translation.y += projected_mouse_delta(mouse, screen_axis_b) * units;
            } else if (axis == GizmoAxis::XZ) {
                after.translation.x += axis_delta * units;
                after.translation.z += projected_mouse_delta(mouse, screen_axis_b) * units;
            } else if (axis == GizmoAxis::YZ) {
                after.translation.y += axis_delta * units;
                after.translation.z += projected_mouse_delta(mouse, screen_axis_b) * units;
            } else {
                after.translation.x += mouse.dx * units;
                after.translation.y -= mouse.dy * units;
            }
            break;
        }
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

    if (state && state->drag.active && !active_frame.mouse.left)
        cancel_gizmo_drag(*state);

    if (result.hot && active_frame.mouse.left && state && !state->drag.active) {
        GizmoAxis axis = GizmoAxis::Uniform;
        math::Vec2 screen_axis{0.0f, 0.0f};
        math::Vec2 screen_axis_b{0.0f, 0.0f};
        if (mode == GizmoMode::Translate) {
            const TranslateAxisPick pick =
                pick_translate_axis(active_frame, before.translation, mouse_pos(active_frame.mouse));
            axis = pick.axis;
            screen_axis = pick.screen_axis;
            screen_axis_b = pick.screen_axis_b;
            if (axis == GizmoAxis::None) {
                axis = GizmoAxis::Uniform;
                screen_axis = {};
                screen_axis_b = {};
            }
        }
        (void)begin_gizmo_drag(
            *state,
            selected,
            mode,
            before,
            mouse_pos(active_frame.mouse),
            axis,
            screen_axis,
            screen_axis_b);
    }

    const bool active_drag =
        state && state->drag.active && state->drag.entity == selected && active_frame.mouse.left;
    result.hot = result.hot || active_drag;
    if ((result.hot || active_drag) && active_frame.mouse.left &&
        has_mouse_delta(active_frame.mouse)) {
        const scene::LocalTransform after =
            preview_transform(before,
                              mode,
                              active_frame.mouse,
                              active_frame.framebuffer_size,
                              active_drag ? state->drag.axis : GizmoAxis::Uniform,
                              active_drag ? state->drag.screen_axis : math::Vec2{},
                              active_drag ? state->drag.screen_axis_b : math::Vec2{});
        result.transform = GizmoTransformIntent{
            .entity = selected,
            .mode = mode,
            .before = active_drag ? state->drag.origin : before,
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
