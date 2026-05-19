// SPDX-License-Identifier: MIT
// Psynder — Sample 02 / M2 demo. Rotating "crate room" — a few axis-aligned
// boxes with the same crate texture, viewed through a perspective camera.
//
// M2 ships once lane 07's tiled rasterizer lands bilinear + mipmaps + Z. Per
// the lane 25 brief, this sample is allowed to land **returning early** if
// lane 07's full bilinear pipeline hasn't shipped yet — it just needs to
// build, link, and exit cleanly so CI keeps running. When the rasterizer is
// real, the body below kicks in: build the room geometry once, submit a
// DrawItem per face per frame, present.
//
// CLI flags:
//   --smoke-frames=N    Headless CI run for N frames then exit.
//   --force-full        Skip the M2-readiness gate and run the full path
//                       anyway (useful for local-dev once lane 07 lands).

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

struct Args {
    u32  smoke_frames = 0;
    bool force_full   = false;
};

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kSmoke = "--smoke-frames=";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kSmoke)) {
            u32 out = 0;
            for (char c : s.substr(kSmoke.size())) {
                if (c < '0' || c > '9') { out = 0; break; }
                out = out * 10u + static_cast<u32>(c - '0');
            }
            a.smoke_frames = out;
        } else if (s == "--force-full") {
            a.force_full = true;
        }
    }
    return a;
}

// M2-readiness probe. The rasterizer is a stub today; once lane 07 lands
// real bilinear + Z, this gate flips to true and the sample runs the full
// crate-room path. The probe is intentionally trivial — we don't want to
// poke at internal state, just check whether end_frame() draws ANY pixels
// into a framebuffer it was given. The detection is heuristic; --force-full
// overrides it.
bool rasterizer_ready_for_m2() {
    using namespace render;
    std::array<u32, 64 * 64> px{};
    for (u32& p : px) p = 0xDEADBEEFu;

    Framebuffer fb{};
    fb.width  = 64;
    fb.height = 64;
    fb.pitch  = 64 * 4;
    fb.format = PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(px.data());

    raster::ViewState view{};
    view.target     = fb;
    view.view       = math::identity4();
    view.projection = math::identity4();

    // A trivial centered triangle covering most of the framebuffer.
    const std::array<raster::Vertex, 3> verts{{
        { {-0.8f, -0.8f, 0.0f}, {0,0,1}, {0,1}, {0,0}, 0xFF00FF00u },
        { { 0.8f, -0.8f, 0.0f}, {0,0,1}, {1,1}, {0,0}, 0xFF00FF00u },
        { { 0.0f,  0.8f, 0.0f}, {0,0,1}, {0.5f,0}, {0,0}, 0xFF00FF00u },
    }};
    const std::array<u32, 3> indices{0,1,2};

    raster::DrawItem item{};
    item.vertices     = verts.data();
    item.vertex_count = static_cast<u32>(verts.size());
    item.indices      = indices.data();
    item.index_count  = static_cast<u32>(indices.size());
    item.model        = math::identity4();

    auto& r = raster::Rasterizer::Get();
    r.begin_frame(view);
    r.submit(item);
    r.end_frame();

    // Any pixel changed away from 0xDEADBEEF means the rasterizer wrote
    // something. The stub never writes; lane 07's real impl will.
    for (u32 p : px) if (p != 0xDEADBEEFu) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    const bool ready = args.force_full || rasterizer_ready_for_m2();
    if (!ready) {
        PSY_LOG_INFO("sample_02: lane 07 tiled-raster + bilinear not yet ready; "
                     "exiting clean (--force-full to override).");
        // Touch the window factory anyway so the platform layer is exercised
        // even on the early-return path — that's the M2 smoke contract.
        platform::WindowDesc desc{};
        desc.title         = "Psynder — sample 02 (crate room, stub)";
        desc.window_width  = 640;
        desc.window_height = 360;
        desc.render_width  = 320;
        desc.render_height = 180;
        if (auto* w = platform::create_window(desc)) {
            w->poll_events();
            platform::destroy_window(w);
        }
        return EXIT_SUCCESS;
    }

    // ─── Full M2 path ───────────────────────────────────────────────────
    platform::WindowDesc desc{};
    desc.title         = "Psynder — sample 02 (crate room)";
    desc.window_width  = 1280;
    desc.window_height = 720;
    desc.render_width  = 640;
    desc.render_height = 360;
    desc.scale_mode    = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_02: failed to create window");
        return EXIT_FAILURE;
    }

    std::vector<u32> pixels(static_cast<usize>(desc.render_width) * desc.render_height, 0);
    std::vector<u32> depth (static_cast<usize>(desc.render_width) * desc.render_height, 0xFFFFFFFFu);
    render::Framebuffer fb{};
    fb.width  = desc.render_width;
    fb.height = desc.render_height;
    fb.pitch  = desc.render_width * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(pixels.data());
    fb.depth  = depth.data();

    // Unit cube, 8 vertices, 12 triangles (24 indices for two-tri strips per
    // face × 6 faces — 36 indices). Texture wraps per face.
    const std::array<render::raster::Vertex, 24> verts{{
        // +X face
        { { 1,-1,-1}, { 1,0,0}, {0,1}, {0,0}, 0xFFFFFFFFu },
        { { 1, 1,-1}, { 1,0,0}, {0,0}, {0,0}, 0xFFFFFFFFu },
        { { 1, 1, 1}, { 1,0,0}, {1,0}, {0,0}, 0xFFFFFFFFu },
        { { 1,-1, 1}, { 1,0,0}, {1,1}, {0,0}, 0xFFFFFFFFu },
        // -X face
        { {-1,-1, 1}, {-1,0,0}, {0,1}, {0,0}, 0xFFFFFFFFu },
        { {-1, 1, 1}, {-1,0,0}, {0,0}, {0,0}, 0xFFFFFFFFu },
        { {-1, 1,-1}, {-1,0,0}, {1,0}, {0,0}, 0xFFFFFFFFu },
        { {-1,-1,-1}, {-1,0,0}, {1,1}, {0,0}, 0xFFFFFFFFu },
        // +Y face
        { {-1, 1,-1}, {0, 1,0}, {0,1}, {0,0}, 0xFFFFFFFFu },
        { {-1, 1, 1}, {0, 1,0}, {0,0}, {0,0}, 0xFFFFFFFFu },
        { { 1, 1, 1}, {0, 1,0}, {1,0}, {0,0}, 0xFFFFFFFFu },
        { { 1, 1,-1}, {0, 1,0}, {1,1}, {0,0}, 0xFFFFFFFFu },
        // -Y face
        { {-1,-1, 1}, {0,-1,0}, {0,1}, {0,0}, 0xFFFFFFFFu },
        { {-1,-1,-1}, {0,-1,0}, {0,0}, {0,0}, 0xFFFFFFFFu },
        { { 1,-1,-1}, {0,-1,0}, {1,0}, {0,0}, 0xFFFFFFFFu },
        { { 1,-1, 1}, {0,-1,0}, {1,1}, {0,0}, 0xFFFFFFFFu },
        // +Z face
        { {-1,-1, 1}, {0,0, 1}, {0,1}, {0,0}, 0xFFFFFFFFu },
        { { 1,-1, 1}, {0,0, 1}, {1,1}, {0,0}, 0xFFFFFFFFu },
        { { 1, 1, 1}, {0,0, 1}, {1,0}, {0,0}, 0xFFFFFFFFu },
        { {-1, 1, 1}, {0,0, 1}, {0,0}, {0,0}, 0xFFFFFFFFu },
        // -Z face
        { { 1,-1,-1}, {0,0,-1}, {0,1}, {0,0}, 0xFFFFFFFFu },
        { {-1,-1,-1}, {0,0,-1}, {1,1}, {0,0}, 0xFFFFFFFFu },
        { {-1, 1,-1}, {0,0,-1}, {1,0}, {0,0}, 0xFFFFFFFFu },
        { { 1, 1,-1}, {0,0,-1}, {0,0}, {0,0}, 0xFFFFFFFFu },
    }};
    const std::array<u32, 36> indices{
         0, 1, 2,  0, 2, 3,
         4, 5, 6,  4, 6, 7,
         8, 9,10,  8,10,11,
        12,13,14, 12,14,15,
        16,17,18, 16,18,19,
        20,21,22, 20,22,23,
    };

    auto& rasterizer = render::raster::Rasterizer::Get();

    PSY_LOG_INFO("Psynder sample 02 running{}",
                 args.smoke_frames > 0
                     ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                     : std::string{});

    const u64 t0    = platform::Clock::ticks_now();
    u32       frame = 0;

    // Crate positions inside the room.
    const std::array<math::Vec3, 4> crate_positions{{
        {-2.5f, 0, -3.0f}, { 2.5f, 0, -3.0f},
        {-1.0f, 0, -5.5f}, { 1.0f, 0, -5.5f},
    }};

    while (!window->should_close()) {
        window->poll_events();

        render::raster::clear_framebuffer(fb, 0xFF182030u);
        // Clear depth to far.
        if (fb.depth) {
            for (usize i = 0, n = static_cast<usize>(fb.width) * fb.height; i < n; ++i) {
                fb.depth[i] = 0xFFFFFFFFu;
            }
        }

        const f32 t = static_cast<f32>(platform::Clock::seconds(
                          platform::Clock::ticks_now() - t0));

        render::raster::ViewState view{};
        view.target     = fb;
        view.view       = math::look_at_rh(
                              math::Vec3{0, 1.5f, 2.0f}, math::Vec3{0, 0, -3},
                              math::Vec3{0, 1, 0});
        view.projection = math::perspective_rh(
                              60.0f * math::kDegToRad,
                              static_cast<f32>(desc.render_width) /
                              static_cast<f32>(desc.render_height),
                              0.1f, 100.0f);
        view.tile_w = 64;
        view.tile_h = 64;

        rasterizer.begin_frame(view);

        for (const math::Vec3& pos : crate_positions) {
            render::raster::DrawItem item{};
            item.vertices     = verts.data();
            item.vertex_count = static_cast<u32>(verts.size());
            item.indices      = indices.data();
            item.index_count  = static_cast<u32>(indices.size());
            const math::Mat4 tr = math::translate(pos);
            const math::Mat4 ro = math::rotate_quat(
                math::quat_from_axis_angle(math::Vec3{0,1,0}, t * 0.4f));
            item.model = math::mul(tr, ro);
            rasterizer.submit(item);
        }

        rasterizer.end_frame();
        window->present(fb);

        if (args.smoke_frames > 0 && ++frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_02: smoke target reached ({}); exiting",
                         args.smoke_frames);
            break;
        }
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
