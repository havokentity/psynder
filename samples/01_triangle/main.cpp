// SPDX-License-Identifier: MIT
// Psynder — Sample 01 / M1 demo. Rotating textured triangle.
//
// The sample drives the engine-facing hybrid scene renderer. While lane 07's
// tiled scanline implementation is still maturing, we ALSO carry a tiny
// self-contained software walker here so the demo paints visible pixels today.
//
// Texture: 32×32 PPM under assets/crate.ppm. Loaded once at startup and
// sampled with nearest-neighbour filtering (M1 spec — bilinear lands at M2).
//
#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "platform/App.h"
#include "render/Color.h"
#include "render/Framebuffer.h"
#include "render/SceneRenderer.h"
#include "render/Texture.h"
#include "scene/SceneEcs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace psynder;

namespace {

const std::array<render::raster::Vertex, 3> kTriangleVerts{{
    {{-0.6f, -0.4f, 0.0f}, {0, 0, 1}, {0.0f, 1.0f}, {0, 0}, 0xFFFFFFFFu},
    {{0.6f, -0.4f, 0.0f}, {0, 0, 1}, {1.0f, 1.0f}, {0, 0}, 0xFFFFFFFFu},
    {{0.0f, 0.6f, 0.0f}, {0, 0, 1}, {0.5f, 0.0f}, {0, 0}, 0xFFFFFFFFu},
}};
const std::array<u32, 3> kTriangleIndices{0, 1, 2};

// ─── Self-contained nearest-filter triangle walker ───────────────────────
// Edge function — positive on one side of the directed edge.
PSY_FORCEINLINE f32 edge(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

struct V2 {
    f32 x, y, u, v;
};

void raster_triangle_nearest(
    render::Framebuffer& fb, const V2& a, const V2& b, const V2& c, const render::TextureView& tex) {
    const f32 area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (std::abs(area) < 1e-4f)
        return;
    const f32 inv_area = 1.0f / area;

    const i32 min_x = std::max(0, static_cast<i32>(std::floor(std::min({a.x, b.x, c.x}))));
    const i32 max_x = std::min(static_cast<i32>(fb.width) - 1,
                               static_cast<i32>(std::ceil(std::max({a.x, b.x, c.x}))));
    const i32 min_y = std::max(0, static_cast<i32>(std::floor(std::min({a.y, b.y, c.y}))));
    const i32 max_y = std::min(static_cast<i32>(fb.height) - 1,
                               static_cast<i32>(std::ceil(std::max({a.y, b.y, c.y}))));
    if (max_x < min_x || max_y < min_y)
        return;

    auto* pixels = reinterpret_cast<u32*>(fb.pixels);
    const u32 tw = tex.width, th = tex.height;
    for (i32 y = min_y; y <= max_y; ++y) {
        const f32 py = static_cast<f32>(y) + 0.5f;
        for (i32 x = min_x; x <= max_x; ++x) {
            const f32 px = static_cast<f32>(x) + 0.5f;
            const f32 w0 = edge(b.x, b.y, c.x, c.y, px, py) * inv_area;
            const f32 w1 = edge(c.x, c.y, a.x, a.y, px, py) * inv_area;
            const f32 w2 = 1.0f - w0 - w1;
            // Allow both winding orders.
            const bool inside_ccw = w0 >= 0 && w1 >= 0 && w2 >= 0;
            const bool inside_cw = w0 <= 0 && w1 <= 0 && w2 <= 0;
            if (!inside_ccw && !inside_cw)
                continue;

            const f32 u = w0 * a.u + w1 * b.u + w2 * c.u;
            const f32 v = w0 * a.v + w1 * b.v + w2 * c.v;
            // Repeat-wrap UV before integer sampling.
            f32 uu = u - std::floor(u);
            f32 vv = v - std::floor(v);
            const u32 tx = std::min(tw - 1, static_cast<u32>(uu * static_cast<f32>(tw)));
            const u32 ty = std::min(th - 1, static_cast<u32>(vv * static_cast<f32>(th)));
            pixels[static_cast<usize>(y) * fb.width + static_cast<usize>(x)] =
                tex.texels[static_cast<usize>(ty) * tex.pitch + tx];
        }
    }
}

struct TriangleSample {
    static constexpr const char* log_name = "sample_01";
    static constexpr const char* display_name = "Psynder sample 01 (textured triangle)";
    static constexpr const char* asset_root = "samples/01_triangle";

    render::Texture2D crate = render::fallback_checker_texture();
    render::TextureLoad crate_request{};
    std::string tex_path{};
    bool crate_resolved = false;
    bool crate_failed = false;

    scene::World world{};
    scene::RuntimeScene scene{world};
    render::SceneRenderer renderer{};
    render::MeshId triangle_mesh{};
    Entity triangle_entity{};

    static app::FrameClear frame_clear(const app::WindowFrameContext&) noexcept {
        return app::FrameClear::color_only(render::rgba8(0x28, 0x20, 0x20));
    }

    void started(app::WindowApp&) {
        world.set_structural_deferred(false);

        tex_path = "assets/crate.ppm";
        crate_request = render::load_ppm_texture_async(tex_path);
        PSY_LOG_INFO("sample_01: queued async texture load {}", tex_path);

        render::MeshDesc triangle_mesh_desc{};
        triangle_mesh_desc.vertices = kTriangleVerts.data();
        triangle_mesh_desc.vertex_count = static_cast<u32>(kTriangleVerts.size());
        triangle_mesh_desc.indices = kTriangleIndices.data();
        triangle_mesh_desc.index_count = static_cast<u32>(kTriangleIndices.size());
        triangle_mesh_desc.base_color = crate.view();
        triangle_mesh_desc.local_bounds = math::Aabb{{-0.6f, -0.4f, 0.0f}, {0.6f, 0.6f, 0.0f}};
        triangle_mesh = renderer.meshes().create_mesh(triangle_mesh_desc);

        render::MaterialDesc triangle_material{};
        triangle_material.flags = render::Material_RasterVisible;
        const render::MaterialId triangle_material_id = scene.materials().create(triangle_material);
        triangle_entity =
            scene.create_renderable(renderer.make_mesh_renderable(triangle_mesh, triangle_material_id));
    }

    void frame(app::WindowFrameContext& ctx) {
        resolve_texture_if_ready();

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

        // ── Fallback walker so we paint visible pixels on the stub raster ──
        const f32 angle = static_cast<f32>(ctx.seconds) * 0.8f;
        const f32 cs = std::cos(angle), sn = std::sin(angle);
        const f32 cx = 0.5f * static_cast<f32>(ctx.framebuffer.width);
        const f32 cy = 0.5f * static_cast<f32>(ctx.framebuffer.height);
        const f32 scale = 0.45f * static_cast<f32>(ctx.framebuffer.height);

        V2 a, b, c;
        auto project = [&](const render::raster::Vertex& vtx, V2& out) {
            // Rotate around Z, then scale into screen space.
            const f32 x = vtx.position.x * cs - vtx.position.y * sn;
            const f32 y = vtx.position.x * sn + vtx.position.y * cs;
            out.x = cx + x * scale;
            // Flip Y because pixel-space grows downward.
            out.y = cy - y * scale;
            out.u = vtx.uv.x;
            out.v = vtx.uv.y;
        };
        project(kTriangleVerts[0], a);
        project(kTriangleVerts[1], b);
        project(kTriangleVerts[2], c);
        raster_triangle_nearest(ctx.framebuffer, a, b, c, crate.view());
    }

    void resolve_texture_if_ready() {
        if (!crate_resolved && crate_request.take_if_ready(crate)) {
            crate_resolved = true;
            renderer.meshes().update_base_color(triangle_mesh, crate.view());
            PSY_LOG_INFO("sample_01: async texture ready {} ({}x{})",
                         tex_path,
                         crate.width(),
                         crate.height());
        } else if (!crate_failed && crate_request.status() == render::TextureLoadStatus::Failed) {
            crate_failed = true;
            PSY_LOG_WARN("sample_01: failed to load {} — using fallback checker", tex_path);
        }
    }
};

}  // namespace

PSYNDER_WINDOW_SAMPLE_MAIN(TriangleSample)
