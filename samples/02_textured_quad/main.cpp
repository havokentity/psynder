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
    scene::SceneFileRequest scene_request{};
    scene::SceneFileLoaded scene_file{};
    std::vector<Entity> crates{};
    std::vector<scene::LocalTransform> base_transforms{};
    bool instantiated = false;
    bool load_error_reported = false;

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

        scene_request.load_async("assets/crate_room.psyscene");
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        instantiate_if_ready(ctx.app, cr.scene());

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

    void instantiate_if_ready(app::WindowApp& app, scene::Scene& scene_ref) {
        if (instantiated)
            return;
        if (scene_request.failed()) {
            if (!load_error_reported) {
                PSY_LOG_ERROR("sample_02: {}", scene_request.error());
                load_error_reported = true;
            }
            return;
        }
        scene::SceneFileLoaded loaded;
        if (!scene_request.consume(loaded))
            return;

        scene_file = std::move(loaded);
        const scene::SceneFileView& view = scene_file.view;
        scene_ref.prewarm_capacity(scene::scene_file_prewarm_config(view));
        app.reserve_scene_capacity(static_cast<u32>(view.mesh_instances.size()), 1u);

        crates.assign(view.mesh_instances.size(), {});
        const scene::SceneMeshBinding bindings[] = {
            {.mesh_name = "builtin.unit_cube.crate", .mesh = cube_mesh, .material = crate_material_id},
        };
        const scene::SceneFileInstantiateResult result =
            scene::instantiate_scene_file(scene_ref, view, bindings, crates);
        if (result.missing_mesh_bindings != 0u) {
            PSY_LOG_WARN("sample_02: {} cooked mesh binding(s) were missing",
                         result.missing_mesh_bindings);
        }

        base_transforms.resize(crates.size());
        for (usize i = 0; i < view.mesh_instances.size(); ++i) {
            base_transforms[i] =
                scene::scene_file_transform(view, view.mesh_instances[i].transform_index);
        }
        instantiated = true;
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TexturedQuadSample)
