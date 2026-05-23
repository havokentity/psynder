// SPDX-License-Identifier: MIT
// Psynder - Sample 02 / M2 demo. Rotating textured crate room.
//
// Uses engine geometry and texture generators, then submits mesh instances
// through the shared scene/rendering path.
//
#include "platform/App.h"
#include "render/GeometryTools.h"
#include "render/Texture.h"
#include "render/TextureGenerators.h"

#include <array>

using namespace psynder;

namespace {

// Crate placements inside the room. Four cubes form a small clump in front
// of the camera; the camera looks down -Z so all four are visible.
const std::array<math::Vec3, 4> kCratePositions{{
    {-1.3f, 0.0f, -3.0f},
    {1.3f, 0.0f, -3.0f},
    {-0.6f, 0.0f, -5.0f},
    {0.7f, 1.2f, -5.5f},
}};

struct TexturedQuadSample : app::BasicSceneApp {
    static constexpr const char* log_name = "sample_02";
    static constexpr const char* display_name = "Psynder sample 02 (crate room)";

    render::Texture2D crate_texture{};
    std::array<Entity, kCratePositions.size()> crates{};

    void started(app::WindowApp& app) {
        crate_texture = render::texture_generators::wooden_crate();

        auto& scene_ref = scene();
        scene_ref.prewarm_capacity(
            {.scene_entities = 8u, .renderables = 4u, .cameras = 1u, .render_items = 4u});
        app.reserve_scene_capacity(4u, 1u);
        scene_ref.environment().set_clear_color(0xFF182030u);
        (void)scene_ref.spawn_camera({.position = math::Vec3{0.0f, 1.5f, 1.5f},
                                      .look_at = math::Vec3{0.0f, 0.0f, -3.0f}});

        render::MeshDesc cube_mesh_desc = render::geometry_tools::unit_cube();
        cube_mesh_desc.base_color = crate_texture.view();
        const render::MeshId cube_mesh = app.rendering_system().cached_mesh(cube_mesh_desc);

        render::MaterialDesc crate_material{};
        crate_material.flags = render::MaterialFlags::RasterVisible;
        const render::MaterialId crate_material_id = scene_ref.materials().create(crate_material);

        std::array<scene::LocalTransform, kCratePositions.size()> crate_transforms{};
        for (usize i = 0; i < kCratePositions.size(); ++i)
            crate_transforms[i].translation = kCratePositions[i];
        scene_ref.spawn_mesh_batch(cube_mesh, crate_material_id, crate_transforms, crates);
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        const auto edit_mode = ctx.app.engine_frame_update(ctx.dt);
        const f32 t = edit_mode == editor::Mode::Edit ? 0.0f : static_cast<f32>(ctx.seconds);

        for (usize i = 0; i < kCratePositions.size(); ++i) {
            scene::LocalTransform transform{};
            transform.translation = kCratePositions[i];
            transform.rotation = math::quat_from_axis_angle(
                math::Vec3{0, 1, 0}, t * (0.35f + 0.12f * static_cast<f32>(i)));
            cr.scene().set_transform(crates[i], transform);
        }
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TexturedQuadSample)
