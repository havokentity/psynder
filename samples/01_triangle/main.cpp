// SPDX-License-Identifier: MIT
// Psynder — Sample 01 / M1 demo. Rotating textured triangle.
//
// Texture: 32×32 PPM under assets/crate.ppm. Loaded once at startup and
// sampled by the engine raster path.
//
#include "platform/App.h"
#include "render/EntityHelpers.h"
#include "render/GeometryTools.h"

using namespace psynder;

namespace {

struct TriangleSample {
    static constexpr const char* log_name = "sample_01";
    static constexpr const char* display_name = "Psynder sample 01 (textured triangle)";
    static constexpr const char* asset_root = "samples/01_triangle";

    render::TextureAsset crate{};

    Entity triangle_entity{};

    void started(app::WindowApp& app) {
        auto& scene = app.loaded_scene();
        scene.environment().set_clear_color(0xFF202028u);
        crate.load_ppm("assets/crate.ppm");

        triangle_entity = render::entity_helpers::create_mesh_entity(app.rendering_system(),
                                                                     scene,
                                                                     render::geometry_tools::textured_triangle(&crate))
                              .entity;
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
