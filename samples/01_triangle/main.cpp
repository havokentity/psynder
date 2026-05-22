// SPDX-License-Identifier: MIT
// Psynder — Sample 01 / M1 demo. Rotating textured triangle.
//
// The sample exists to drive lane 07's rasterizer through its public API
// (Rasterizer::Get().submit(DrawItem)). While lane 07's tiled scanline
// implementation is being built, we ALSO carry a tiny self-contained
// software walker right here in the sample so the demo paints visible
// pixels today on the stub rasterizer. When lane 07 ships the real
// rasterizer, the fallback walker can be deleted — the DrawItem we submit
// already matches the contract in engine/render/raster/Raster.h.
//
// Texture: 32×32 PPM under assets/crate.ppm. Loaded once at startup and
// sampled with nearest-neighbour filtering (M1 spec — bilinear lands at M2).
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Same, space-separated (matches the cmake helper
//                            invocation in cmake/Goldens.cmake).
//   --smoke-capture-out PATH Write the final rendered framebuffer to PATH
//                            as a valid 24-bit RGB PNG. Used by the
//                            psynder_add_golden_cell() ctest cells.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "editor/core/SampleHook.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/Texture.h"
#include "render/raster/Raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// Search for the asset file in a few well-known relative paths so the
// sample works whether you launch from build/bin, the source tree, or a
// CI working directory.
std::string find_asset(std::string_view rel) {
    namespace fs = std::filesystem;
    const std::array<fs::path, 5> roots{
        fs::path{},
        fs::path{"."},
        fs::path{".."},
        fs::path{"../.."},
        fs::path{"../../.."},
    };
    for (const auto& root : roots) {
        fs::path candidate =
            root.empty() ? fs::path{rel} : root / "samples" / "01_triangle" / fs::path{rel};
        if (fs::exists(candidate))
            return candidate.string();
        candidate = root / fs::path{rel};
        if (fs::exists(candidate))
            return candidate.string();
    }
    return std::string{rel};
}

// Fallback texture if asset load fails — magenta/black checkerboard so the
// failure is loud, not silent.
render::Texture2D make_fallback_texture() {
    std::vector<u32> pixels(16 * 16);
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const bool magenta = ((x ^ y) & 1u) != 0u;
            pixels[static_cast<usize>(y) * 16u + x] = magenta ? 0xFFFF00FFu : 0xFF000000u;
        }
    return render::Texture2D::from_rgba8(16, 16, std::move(pixels));
}

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

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 01 (textured triangle)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;
    return desc;
}

int sample_main(const app::AppArgs& base_args, app::WindowApp& app_host) {
    const app::AppArgs& args = base_args;
    const u32 smoke_frames = args.smoke_frames;
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();

    // Load the crate texture (procedurally-generated 32×32 PPM committed
    // alongside the sample).
    render::Texture2D crate = make_fallback_texture();
    const std::string tex_path = find_asset("assets/crate.ppm");
    render::TextureLoad crate_request = render::load_ppm_texture_async(tex_path);
    bool crate_resolved = false;
    bool crate_failed = false;
    PSY_LOG_INFO("sample_01: queued async texture load {}", tex_path);

    render::Framebuffer& fb = app_host.framebuffer();

    // Triangle vertices in mesh-local space (XY plane, Z forward, UV in [0,1]).
    const std::array<render::raster::Vertex, 3> verts{{
        {{-0.6f, -0.4f, 0.0f}, {0, 0, 1}, {0.0f, 1.0f}, {0, 0}, 0xFFFFFFFFu},
        {{0.6f, -0.4f, 0.0f}, {0, 0, 1}, {1.0f, 1.0f}, {0, 0}, 0xFFFFFFFFu},
        {{0.0f, 0.6f, 0.0f}, {0, 0, 1}, {0.5f, 0.0f}, {0, 0}, 0xFFFFFFFFu},
    }};
    const std::array<u32, 3> indices{0, 1, 2};

    // Drive lane 07's public API with a real DrawItem each frame. Even
    // though Rasterizer::submit() is a stub today, we want the wiring on
    // the books so when lane 07 lands, this sample needs zero changes.
    auto& rasterizer = render::raster::Rasterizer::Get();

    PSY_LOG_INFO("Psynder sample 01 running{}",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    while (!window->should_close()) {
        window->poll_events();

        // Smoke runs pin time to frame index so the captured PNG is bit-
        // identical across runs / hosts (golden-image determinism).
        const f64 t = smoke_frames > 0 ? static_cast<f64>(frame) * (1.0 / 60.0)
                                       : platform::Clock::seconds(platform::Clock::ticks_now() - t0);

        render::raster::clear_framebuffer(fb, 0xFF202028u);  // dark slate

        // ── Build a ViewState + DrawItem and submit through the public API ──
        render::raster::ViewState view{};
        view.target = fb;
        view.view = math::identity4();
        view.projection = math::identity4();
        view.tile_w = 64;
        view.tile_h = 64;
        rasterizer.begin_frame(view);

        render::raster::DrawItem item{};
        item.vertices = verts.data();
        item.vertex_count = static_cast<u32>(verts.size());
        item.indices = indices.data();
        item.index_count = static_cast<u32>(indices.size());
        item.model = math::rotate_quat(
            math::quat_from_axis_angle(math::Vec3{0, 0, 1}, static_cast<f32>(t) * 0.8f));
        rasterizer.submit(item);
        rasterizer.end_frame();

        // ── Fallback walker so we paint visible pixels on the stub raster ──
        const f32 angle = static_cast<f32>(t) * 0.8f;
        const f32 cs = std::cos(angle), sn = std::sin(angle);
        const f32 cx = 0.5f * static_cast<f32>(desc.render_width);
        const f32 cy = 0.5f * static_cast<f32>(desc.render_height);
        const f32 scale = 0.45f * static_cast<f32>(desc.render_height);

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
        project(verts[0], a);
        project(verts[1], b);
        project(verts[2], c);
        if (!crate_resolved && crate_request.take_if_ready(crate)) {
            crate_resolved = true;
            PSY_LOG_INFO("sample_01: async texture ready {} ({}x{})",
                         tex_path,
                         crate.width(),
                         crate.height());
        } else if (!crate_failed && crate_request.status() == render::TextureLoadStatus::Failed) {
            crate_failed = true;
            PSY_LOG_WARN("sample_01: failed to load {} — using fallback checker", tex_path);
        }
        raster_triangle_nearest(fb, a, b, c, crate.view());

        // Engine overlay suite: `~` console + F1 debug HUD + F2 badge.
        if (auto* in = platform::input()) {
            editor::frame_overlays(*in, fb);
        }

        window->present(fb);

        if (smoke_frames > 0 && ++frame >= smoke_frames) {
            PSY_LOG_INFO("sample_01: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    if (!app_host.write_capture_if_requested("sample_01"))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

struct TriangleSample {
    static constexpr std::string_view log_name() noexcept { return "sample_01"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 01"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return sample_main(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(TriangleSample)
