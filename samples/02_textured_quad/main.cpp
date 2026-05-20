// SPDX-License-Identifier: MIT
// Psynder — Sample 02 / M2 demo. Rotating "crate room" — a small set of
// unit cubes spinning on the floor, viewed through a perspective camera.
//
// Wave-B brings the rasterizer up to tiled-binner + bilinear + Z (lane 07
// landed those in this wave). Sample 02 is the demo target for M2 per
// DESIGN.md §13: rotating crate room with the new pipeline. The crates use
// per-face vertex colours rather than texture binds because the public
// rasterizer surface (DrawItem) still routes textures via `MaterialId`,
// and lane 05 / 24's material binding plumbing arrives in Wave-C — the
// shape is correct regardless, and the geometry exercises every hot path
// in the rasterizer (vertex transform → triangle setup → tile bin →
// perspective-correct interpolation → Z reject).
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Same, space-separated (matches cmake helper
//                            invocation in cmake/Goldens.cmake).
//   --smoke-capture-out PATH Write the last rendered framebuffer to PATH
//                            (or PATH may be `=`-suffixed). The file is a
//                            valid 24-bit RGB PNG. Used by the
//                            psynder_add_golden_cell() ctest cells to
//                            produce the actual-image-this-run output.

#include "common/MeshWinding.h"
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "ui/imm/DebugHud.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

struct Args {
    u32 smoke_frames = 0;
    std::string capture_out;
};

u32 parse_uint(std::string_view v) noexcept {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9')
            return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kSmoke = "--smoke-frames=";
    constexpr std::string_view kSmokeSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kSmoke)) {
            a.smoke_frames = parse_uint(s.substr(kSmoke.size()));
        } else if (s == kSmokeSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

// Unit cube — 24 verts (4 per face × 6 faces), per-face colour packed into
// the Vertex::color slot so the perspective-correct interpolator routes it
// through the (r/w, g/w, b/w) channels — same path the texture pipeline
// will eventually use. Z faces forward (-Z), so the front of each crate
// faces the camera at angle 0.
constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

constexpr u32 kColPosX = pack_rgba(220, 110, 60);   // warm orange
constexpr u32 kColNegX = pack_rgba(160, 90, 50);    // dim orange
constexpr u32 kColPosY = pack_rgba(220, 200, 130);  // top lighter
constexpr u32 kColNegY = pack_rgba(80, 60, 40);     // floor darker
constexpr u32 kColPosZ = pack_rgba(200, 100, 60);   // mid orange
constexpr u32 kColNegZ = pack_rgba(140, 80, 50);    // back dim

// 24 vertices for a unit cube centered at origin, ±0.5 on each axis.
const std::array<render::raster::Vertex, 24> kCubeVerts{{
    // +X face (BL → TL → TR → BR in face-local space)
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0, 1}, {0, 0}, kColPosX},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0, 0}, {0, 0}, kColPosX},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {1, 0}, {0, 0}, kColPosX},
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {1, 1}, {0, 0}, kColPosX},
    // -X face
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0, 1}, {0, 0}, kColNegX},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0, 0}, {0, 0}, kColNegX},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {1, 0}, {0, 0}, kColNegX},
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {1, 1}, {0, 0}, kColNegX},
    // +Y face (top)
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0, 1}, {0, 0}, kColPosY},
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0, 0}, {0, 0}, kColPosY},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {1, 0}, {0, 0}, kColPosY},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {1, 1}, {0, 0}, kColPosY},
    // -Y face (bottom)
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0, 1}, {0, 0}, kColNegY},
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}, {0, 0}, kColNegY},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}, {0, 0}, kColNegY},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {1, 1}, {0, 0}, kColNegY},
    // +Z face
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0, 1}, {0, 0}, kColPosZ},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {1, 1}, {0, 0}, kColPosZ},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {1, 0}, {0, 0}, kColPosZ},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0, 0}, {0, 0}, kColPosZ},
    // -Z face (front when camera looks down -Z)
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}, {0, 0}, kColNegZ},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}, {0, 0}, kColNegZ},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0}, {0, 0}, kColNegZ},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0, 0}, {0, 0}, kColNegZ},
}};

// 36 indices — two triangles per face, wound CCW in the post-flip screen
// frame (matches the front-face convention in TriSetup.cpp).
constexpr std::array<u32, 36> kCubeIndices{
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

// Crate placements inside the room. Four cubes form a small clump in front
// of the camera; the camera looks down -Z so all four are visible.
const std::array<math::Vec3, 4> kCratePositions{{
    {-1.3f, 0.0f, -3.0f},
    {1.3f, 0.0f, -3.0f},
    {-0.6f, 0.0f, -5.0f},
    {0.7f, 1.2f, -5.5f},
}};

void clear_depth_far(render::Framebuffer& fb) noexcept {
    if (!fb.depth)
        return;
    // 1.0 packed the same way pack_depth() does inside TileRaster.cpp —
    // the top 24 bits of the f32 1.0 representation, low byte zeroed.
    u32 packed_far = 0;
    const f32 one = 1.0f;
    std::memcpy(&packed_far, &one, sizeof(packed_far));
    packed_far &= 0xFFFFFF00u;
    const usize n = static_cast<usize>(fb.width) * fb.height;
    for (usize i = 0; i < n; ++i)
        fb.depth[i] = packed_far;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 02 (crate room)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_02: failed to create window");
        return EXIT_FAILURE;
    }

    std::vector<u32> pixels(static_cast<usize>(desc.render_width) * desc.render_height, 0);
    std::vector<u32> depth(static_cast<usize>(desc.render_width) * desc.render_height, 0);

    render::Framebuffer fb{};
    fb.width = desc.render_width;
    fb.height = desc.render_height;
    fb.pitch = desc.render_width * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(pixels.data());
    fb.depth = depth.data();

    auto& rasterizer = render::raster::Rasterizer::Get();

    // The rasterizer now back-face culls by default, so the cube must be
    // wound consistently with its per-vertex normals or faces drop out as a
    // crate spins (the old two-sided path hid this). Rewind once from the
    // shared normals; the per-face crate palette is left untouched.
    std::array<u32, kCubeIndices.size()> cube_idx = kCubeIndices;
    samples::fix_winding(kCubeVerts.data(), cube_idx.data(), static_cast<u32>(cube_idx.size()));

    PSY_LOG_INFO("Psynder sample 02 running{}{}",
                 args.smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", args.smoke_frames)
                                       : std::string{},
                 args.capture_out.empty() ? std::string{}
                                          : fmt::format(" — capture to {}", args.capture_out));

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    // 60-sample ring of recent frame times (ms) for the debug HUD's strip
    // chart. Wraps via `frame % kFrameHistory`; the HUD's own internal
    // averaging looks at `avg_frame_ms` so we keep this lightweight.
    // Sized `u32` so `frame + 1u` and the `min` against it stay in the
    // same domain (the comparable variant in PR #114 mixed `usize` and
    // `u32`, which clang would have narrowed silently — pre-emptively
    // avoiding that here).
    constexpr u32 kFrameHistory = 60;
    std::array<f32, kFrameHistory> frame_ms_ring{};
    u64 prev_frame_ticks = t0;
    // Smoke-mode default frame time stand-in (60 FPS budget = 1/60 s).
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;

    while (!window->should_close()) {
        window->poll_events();

        if (auto* in = platform::input(); in && in->key_down(platform::KeyCode::Escape)) {
            break;
        }

        // Per-frame wall-clock delta for HUD stats. Smoke runs are
        // frame-indexed so we use the 60 FPS budget stand-in to keep the
        // chart deterministic across hosts.
        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            args.smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        frame_ms_ring[frame % kFrameHistory] = frame_ms;

        // Clear colour + depth.
        render::raster::clear_framebuffer(fb, 0xFF182030u);
        clear_depth_far(fb);

        // Drive the spin off the wall clock so non-smoke runs animate; for
        // smoke runs we use a fixed phase (per-frame * step) so frame N is
        // deterministic and reproducible across hosts. The cmake helper
        // pins captures at a specific frame count so what we hand the
        // golden gate is deterministic. Frozen in EDIT mode so the user
        // sees a stable scene while inspecting / spawning props.
        const editor::Mode edit_mode =
            platform::input() ? editor::sample_step(*platform::input(), fb) : editor::Mode::Play;
        // EDIT mode pins `t` to a constant so the scene is frozen for
        // inspection. Smoke mode advances per frame for deterministic
        // captures; otherwise we tick off the wall clock.
        const f32 t =
            (edit_mode == editor::Mode::Edit) ? 0.0f
            : args.smoke_frames > 0
                ? static_cast<f32>(frame) * 0.05f
                : static_cast<f32>(platform::Clock::seconds(platform::Clock::ticks_now() - t0));

        render::raster::ViewState view{};
        view.target = fb;
        view.view = math::look_at_rh(math::Vec3{0, 1.5f, 1.5f},   // eye
                                     math::Vec3{0, 0.0f, -3.0f},  // target
                                     math::Vec3{0, 1.0f, 0.0f});  // up
        view.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                               static_cast<f32>(desc.render_width) /
                                                   static_cast<f32>(desc.render_height),
                                               0.1f,
                                               100.0f);
        view.tile_w = 64;
        view.tile_h = 64;

        rasterizer.begin_frame(view);

        for (usize i = 0; i < kCratePositions.size(); ++i) {
            const math::Vec3 pos = kCratePositions[i];
            // Each crate spins at a slightly different rate to make the
            // depth interactions visible.
            const f32 spin = t * (0.35f + 0.12f * static_cast<f32>(i));
            const math::Mat4 tr = math::translate(pos);
            const math::Mat4 ro =
                math::rotate_quat(math::quat_from_axis_angle(math::Vec3{0, 1, 0}, spin));

            render::raster::DrawItem item{};
            item.vertices = kCubeVerts.data();
            item.vertex_count = static_cast<u32>(kCubeVerts.size());
            item.indices = cube_idx.data();
            item.index_count = static_cast<u32>(cube_idx.size());
            item.model = math::mul(tr, ro);
            rasterizer.submit(item);
        }

        rasterizer.end_frame();

        // Debug HUD overlay — toggle via `r_debug_hud full` console var.
        // The HUD reads the per-frame stats we filled at the top of the
        // loop; if the cvar is `off` the call early-returns. Stays drawn
        // after the rasterizer so it composites on top of the scene.
        {
            ui::imm::DebugHudStats stats{};
            stats.frame_ms = frame_ms;
            stats.avg_frame_ms = [&]() noexcept {
                // Walk only the populated prefix of the ring. `n` matches
                // `kFrameHistory`'s `u32` type so we don't mix domains.
                const u32 n = std::min<u32>(frame + 1u, kFrameHistory);
                if (n == 0u)
                    return 0.0f;
                f32 sum = 0.0f;
                for (u32 i = 0; i < n; ++i)
                    sum += frame_ms_ring[i];
                return sum / static_cast<f32>(n);
            }();
            stats.draw_calls = kCratePositions.size();
            stats.triangles = kCratePositions.size() * 12u;
            stats.active_voices = 0;
            ui::imm::draw_debug_hud(fb, stats);
        }
        window->present(fb);

        ++frame;
        if (args.smoke_frames > 0 && frame >= args.smoke_frames) {
            PSY_LOG_INFO("sample_02: smoke target reached ({}); exiting", args.smoke_frames);
            break;
        }
    }

    // Capture the final frame to PNG if requested. We do this AFTER the
    // last frame's submit/end_frame so the image we hand the golden gate
    // is fully rendered, not a torn intermediate.
    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_02: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_02: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
