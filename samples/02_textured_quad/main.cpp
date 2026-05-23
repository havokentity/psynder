// SPDX-License-Identifier: MIT
// Psynder - Sample 02 / M2 demo. Rotating textured crate room.
//
// Uses engine geometry and texture generators, then submits mesh instances
// through the shared scene/rendering path.
//
#include "asset/Precook.h"
#include "platform/App.h"
#include "render/GeometryTools.h"
#include "render/Texture.h"
#include "render/TextureGenerators.h"
#include "scene/SceneFile.h"

#include <string_view>
#include <vector>

using namespace psynder;

PSYNDER_RUNTIME_BUNDLE("02_textured_quad");
PSYNDER_PRECOOK_PSYSCENE("assets/crate_room.psyscene.json");

namespace {

struct TexturedQuadSample : app::BasicSceneApp {
    static constexpr const char* log_name = "sample_02";
    static constexpr const char* display_name = "Psynder sample 02 (crate room)";
    static constexpr const char* asset_root = "samples/02_textured_quad";

    render::Texture2D crate_texture{};
    render::MeshId cube_mesh{};
    render::MaterialId crate_material_id{};
    scene::SceneLoadRequest scene_load{};
    std::vector<Entity> crates{};
    std::vector<scene::LocalTransform> base_transforms{};

    void started(app::WindowApp& app) {
        crate_texture = render::texture_generators::wooden_crate();

        auto& scene_ref = scene();
        scene_ref.set_structural_deferred(false);

        render::MeshDesc cube_mesh_desc = render::geometry_tools::unit_cube();
        cube_mesh_desc.base_color = crate_texture.view();
        cube_mesh = app.rendering_system().cached_mesh(cube_mesh_desc);

        render::MaterialDesc crate_material{};
        crate_material.flags = render::MaterialFlags::RasterVisible;
        crate_material_id = scene_ref.materials().create(crate_material);

        scene_load
            .bind_mesh("builtin.unit_cube.crate", cube_mesh, crate_material_id)
            .on_ready([this](const scene::SceneLoadResult& result) {
                crates.assign(result.mesh_entities.begin(), result.mesh_entities.end());
                base_transforms.assign(result.base_transforms.begin(),
                                       result.base_transforms.end());
            })
            .on_error([](std::string_view error) { PSY_LOG_ERROR("sample_02: {}", error); });
        scene_load.load_async("assets/crate_room.psyscene");
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        ctx.app.update_scene_load(scene_load, cr.scene());

        (void)ctx.app.engine_frame_update(ctx.dt);
        const f32 t = static_cast<f32>(ctx.seconds);

        for (usize i = 0; i < crates.size(); ++i) {
            scene::LocalTransform transform = base_transforms[i];
            const math::Quat spin = math::quat_from_axis_angle(
                math::Vec3{0.0f, 1.0f, 0.0f}, t * (0.35f + 0.12f * static_cast<f32>(i)));
            transform.rotation = math::quat_mul(spin, transform.rotation);
            cr.scene().set_transform(crates[i], transform);
        }
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TexturedQuadSample)
