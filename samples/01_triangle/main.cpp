// SPDX-License-Identifier: MIT
// Psynder — Sample 01 / M1 demo. Rotating textured triangle.
//
// Texture: 32×32 PPM under assets/crate.ppm. Loaded once at startup and
// sampled by the engine raster path.
//
#include "core/Types.h"
#include "math/Math.h"
#include "platform/App.h"
#include "render/Color.h"
#include "render/SceneRenderer.h"
#include "render/Texture.h"
#include "scene/SceneEcs.h"

#include <array>

using namespace psynder;

namespace {

const std::array<render::raster::Vertex, 3> kTriangleVerts{{
    {{-0.6f, -0.4f, 0.0f}, {0, 0, 1}, {0.0f, 1.0f}, {0, 0}, 0xFFFFFFFFu},
    {{0.6f, -0.4f, 0.0f}, {0, 0, 1}, {1.0f, 1.0f}, {0, 0}, 0xFFFFFFFFu},
    {{0.0f, 0.6f, 0.0f}, {0, 0, 1}, {0.5f, 0.0f}, {0, 0}, 0xFFFFFFFFu},
}};
const std::array<u32, 3> kTriangleIndices{0, 2, 1};

struct TriangleSample {
    static constexpr const char* log_name = "sample_01";
    static constexpr const char* display_name = "Psynder sample 01 (textured triangle)";
    static constexpr const char* asset_root = "samples/01_triangle";

    render::TextureAsset crate{};

    scene::RuntimeScene scene{};
    render::SceneRenderer renderer{};
    render::MeshId triangle_mesh{};
    Entity triangle_entity{};

    static app::FrameClear frame_clear(const app::WindowFrameContext&) noexcept {
        return app::FrameClear::color_only(render::rgba8(0x28, 0x20, 0x20));
    }

    void started(app::WindowApp&) {
        crate.load_ppm("assets/crate.ppm");

        render::MeshDesc triangle_mesh_desc{};
        triangle_mesh_desc.vertices = kTriangleVerts.data();
        triangle_mesh_desc.vertex_count = static_cast<u32>(kTriangleVerts.size());
        triangle_mesh_desc.indices = kTriangleIndices.data();
        triangle_mesh_desc.index_count = static_cast<u32>(kTriangleIndices.size());
        triangle_mesh_desc.base_color_asset = &crate;
        triangle_mesh_desc.local_bounds = math::Aabb{{-0.6f, -0.4f, 0.0f}, {0.6f, 0.6f, 0.0f}};
        triangle_mesh = renderer.meshes().create_mesh(triangle_mesh_desc);

        render::MaterialDesc triangle_material{};
        triangle_material.flags = render::Material_RasterVisible;
        const render::MaterialId triangle_material_id = scene.materials().create(triangle_material);
        triangle_entity =
            scene.create_renderable(renderer.make_mesh_renderable(triangle_mesh, triangle_material_id));
    }

    void frame(app::WindowFrameContext& ctx) {
        // Submit through the hybrid scene renderer; raster is an internal backend.
        render::raster::ViewState view{};
        view.target = ctx.framebuffer;
        view.view = math::identity4();
        view.projection = math::identity4();
        view.tile_w = 64;
        view.tile_h = 64;
        scene::LocalTransform triangle_transform{};
        triangle_transform.rotation =
            math::quat_from_axis_angle(math::Vec3{0, 0, 1}, static_cast<f32>(ctx.seconds) * 0.8f);
        scene.set_transform(triangle_entity, triangle_transform);
        renderer.render_raster(scene, view);
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TriangleSample)
