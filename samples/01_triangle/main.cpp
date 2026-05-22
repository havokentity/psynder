// SPDX-License-Identifier: MIT
// Psynder — Sample 01 / M1 demo. Rotating textured triangle.
//
// Texture: 32×32 PPM under assets/crate.ppm. Loaded once at startup and
// sampled by the engine raster path.
//
#include "platform/App.h"

using namespace psynder;

namespace {

struct TriangleSample {
    static constexpr const char* log_name = "sample_01";
    static constexpr const char* display_name = "Psynder sample 01 (textured triangle)";
    static constexpr const char* asset_root = "samples/01_triangle";

    render::TextureAsset crate{};

    scene::Scene scene{};
    Entity triangle_entity{};

    void started(app::WindowApp& app) {
        scene.environment().set_clear_color(0xFF202028u);
        crate.load_ppm("assets/crate.ppm");

        render::MaterialDesc triangle_material{};
        triangle_material.flags = render::MaterialFlags::RasterVisible;
        const render::MaterialId triangle_material_id = scene.materials().create(triangle_material);
        const render::MeshDesc triangle_mesh_desc = render::geometry_tools::textured_triangle(&crate);
        triangle_entity = app.create_mesh_entity(scene, triangle_mesh_desc, triangle_material_id).entity;
        app.set_scene(scene);
    }

    void frame(app::WindowFrameContext& ctx) {
        scene::LocalTransform triangle_transform{};
        triangle_transform.rotation =
            math::quat_from_axis_angle(math::Vec3{0, 0, 1}, static_cast<f32>(ctx.seconds) * 0.8f);
        scene.set_transform(triangle_entity, triangle_transform);
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TriangleSample)
