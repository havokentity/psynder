// SPDX-License-Identifier: MIT
// Psynder — Sample 01 / M1 demo. Rotating textured triangle.
//
// Texture: 32×32 PPM under assets/crate.ppm. Loaded once at startup and
// sampled by the engine raster path.
//
#include "core/BuildMeta.h"
#include "platform/App.h"

using namespace psynder;

PSYNDER_RUNTIME_BUNDLE("01_triangle");

namespace {

struct TriangleSample : app::BasicSceneApp {
    static constexpr const char* log_name = "sample_01";
    static constexpr const char* display_name = "Psynder sample 01 (textured triangle)";
    static constexpr const char* asset_root = "samples/01_triangle";

    render::TextureAsset crate{};

    Entity triangle_entity{};

    void started(app::WindowApp& app) {
        auto& scene_ref = scene();
        scene_ref.environment().set_clear_color(0xFF202028u);
        (void)scene_ref.spawn_camera();
        crate.load_ppm("assets/crate.ppm");

        const render::MeshId triangle_mesh =
            app.rendering_system().builtin_mesh(render::BuiltInMesh::TexturedTriangle, &crate);
        triangle_entity = scene_ref.spawn_mesh_instance(triangle_mesh);
    }

    void frame(app::WindowFrameContext& ctx, app::WindowFrameCacheReady& cr) {
        cr.scene().set_transform(
            triangle_entity,
            {.rotation =
                 math::quat_from_axis_angle({0, 0, 1}, static_cast<f32>(ctx.seconds) * 0.8f)});
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TriangleSample)
