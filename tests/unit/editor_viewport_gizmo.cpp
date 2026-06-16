// SPDX-License-Identifier: MIT
// Psynder - editor viewport gizmo drag/latch behavior.

#include "editor/core/ViewportGizmo.h"
#include "scene/SceneEcs.h"

#include <catch2/catch_test_macros.hpp>

using namespace psynder;
using namespace psynder::editor::viewport;

namespace {

constexpr f32 kEps = 1e-4f;
constexpr math::Vec2 kFramebuffer{200.0f, 200.0f};
constexpr math::Vec2 kScreenOrigin{100.0f, 100.0f};
constexpr math::Vec2 kMouseOnX{132.0f, 100.0f};
constexpr math::Vec2 kMouseOnY{100.0f, 68.0f};
constexpr math::Vec2 kMouseOnZ{100.0f, 132.0f};

PSY_FORCEINLINE bool approx_eq(f32 a, f32 b) noexcept {
    const f32 d = a - b;
    return d > -kEps && d < kEps;
}

math::Mat4 z_down_view_projection() {
    // Column-major matrix: clip.x = world.x, clip.y = world.y - world.z,
    // clip.w = 1. In screen space, +X points right, +Y points up, and +Z
    // points down, giving every translate axis a distinct visible arm.
    return {{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    }};
}

platform::MouseState mouse_at(math::Vec2 pos, bool left, f32 dx = 0.0f, f32 dy = 0.0f) {
    platform::MouseState mouse{};
    mouse.x = pos.x;
    mouse.y = pos.y;
    mouse.dx = dx;
    mouse.dy = dy;
    mouse.left = left;
    return mouse;
}

struct SceneFixture {
    scene::EcsRegistry& registry = scene::EcsRegistry::Get();
    scene::Scene scene{registry};
    Entity entity{};

    SceneFixture() {
        registry.clear();
        entity = scene.create_entity();
    }

    ~SceneFixture() {
        registry.clear();
    }
};

GizmoFrame frame_for(SceneFixture& fixture, const platform::MouseState& mouse) {
    GizmoFrame frame{};
    frame.scene = &fixture.scene;
    frame.selected_entity = fixture.entity;
    frame.view_projection = z_down_view_projection();
    frame.mouse = mouse;
    frame.framebuffer_size = kFramebuffer;
    frame.mode = GizmoMode::Translate;
    frame.apply_transform = true;
    return frame;
}

scene::LocalTransform transform_of(SceneFixture& fixture) {
    return fixture.scene.transform(fixture.entity);
}

}  // namespace

TEST_CASE("viewport gizmo: mouse-up clears drag latch before a new axis pick",
          "[editor][viewport][gizmo]") {
    SceneFixture fixture;
    GizmoState state{};

    const GizmoResult first_down =
        draw_apply_gizmo(frame_for(fixture, mouse_at(kMouseOnX, true)), &state);
    REQUIRE(first_down.hot);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::X);

    (void)draw_apply_gizmo(frame_for(fixture, mouse_at(kMouseOnX, false)), &state);
    REQUIRE_FALSE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::None);
    REQUIRE_FALSE(state.drag.entity.valid());

    const GizmoResult second_down =
        draw_apply_gizmo(frame_for(fixture, mouse_at(kMouseOnY, true)), &state);
    REQUIRE(second_down.hot);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::Y);
}

TEST_CASE("viewport gizmo: active drag uses stored projected screen axis",
          "[editor][viewport][gizmo]") {
    SceneFixture fixture;
    GizmoState state{};

    (void)draw_apply_gizmo(frame_for(fixture, mouse_at(kMouseOnX, true, 20.0f, 0.0f)), &state);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::X);

    const scene::LocalTransform after_x_drag = transform_of(fixture);
    REQUIRE(approx_eq(after_x_drag.translation.x, 0.2f));
    REQUIRE(approx_eq(after_x_drag.translation.y, 0.0f));
    REQUIRE(approx_eq(after_x_drag.translation.z, 0.0f));

    // Move vertically while still latched to the X axis. If the frame's
    // current hover or uniform fallback direction were used, this would drift
    // in Y; the stored X screen axis should project this movement to zero.
    (void)draw_apply_gizmo(
        frame_for(fixture, mouse_at({kScreenOrigin.x + 20.0f, kScreenOrigin.y + 24.0f},
                                    true,
                                    0.0f,
                                    24.0f)),
        &state);

    const scene::LocalTransform after_vertical_motion = transform_of(fixture);
    REQUIRE(approx_eq(after_vertical_motion.translation.x, after_x_drag.translation.x));
    REQUIRE(approx_eq(after_vertical_motion.translation.y, 0.0f));
    REQUIRE(approx_eq(after_vertical_motion.translation.z, 0.0f));
}

TEST_CASE("viewport gizmo: Z drag follows the visible arrow direction",
          "[editor][viewport][gizmo]") {
    SceneFixture fixture;
    GizmoState state{};

    (void)draw_apply_gizmo(frame_for(fixture, mouse_at(kMouseOnZ, true)), &state);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::Z);

    (void)draw_apply_gizmo(frame_for(fixture, mouse_at({kMouseOnZ.x, kMouseOnZ.y + 20.0f},
                                                      true,
                                                      0.0f,
                                                      20.0f)),
                           &state);

    const scene::LocalTransform after_z_drag = transform_of(fixture);
    REQUIRE(approx_eq(after_z_drag.translation.x, 0.0f));
    REQUIRE(approx_eq(after_z_drag.translation.y, 0.0f));
    REQUIRE(approx_eq(after_z_drag.translation.z, 0.2f));
}

TEST_CASE("viewport gizmo: plane handle moves on two axes",
          "[editor][viewport][gizmo]") {
    SceneFixture fixture;
    GizmoState state{};

    const math::Vec2 xy_plane{120.0f, 80.0f};
    (void)draw_apply_gizmo(frame_for(fixture, mouse_at(xy_plane, true)), &state);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::XY);

    (void)draw_apply_gizmo(frame_for(fixture, mouse_at({xy_plane.x + 20.0f, xy_plane.y - 10.0f},
                                                      true,
                                                      20.0f,
                                                      -10.0f)),
                           &state);

    const scene::LocalTransform after_xy_drag = transform_of(fixture);
    REQUIRE(approx_eq(after_xy_drag.translation.x, 0.2f));
    REQUIRE(approx_eq(after_xy_drag.translation.y, 0.1f));
    REQUIRE(approx_eq(after_xy_drag.translation.z, 0.0f));
}

TEST_CASE("viewport gizmo: active axis drag owns mouse until release",
          "[editor][viewport][gizmo]") {
    SceneFixture fixture;
    GizmoState state{};

    const GizmoResult first_down =
        draw_apply_gizmo(frame_for(fixture, mouse_at(kMouseOnX, true)), &state);
    REQUIRE(first_down.hot);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::X);

    const GizmoResult off_handle_drag =
        draw_apply_gizmo(frame_for(fixture, mouse_at({180.0f, 160.0f}, true, 20.0f, 60.0f)),
                         &state);
    REQUIRE(off_handle_drag.hot);
    REQUIRE(state.drag.active);
    REQUIRE(state.drag.axis == GizmoAxis::X);

    (void)draw_apply_gizmo(frame_for(fixture, mouse_at({180.0f, 160.0f}, false)), &state);
    REQUIRE_FALSE(state.drag.active);
}
