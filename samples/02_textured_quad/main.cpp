// SPDX-License-Identifier: MIT
// Psynder — Sample 02 / M2 demo. Rotating "crate room" — a small set of
// unit cubes spinning on the floor, viewed through a perspective camera.
//
// Wave-B brings the rasterizer up to tiled-binner + bilinear + Z (lane 07
// landed those in this wave). Sample 02 is the demo target for M2 per
// DESIGN.md §13: rotating crate room with the new pipeline. The crates now
// carry a real per-pixel procedural wooden-crate texture: each cube vertex
// is white and the whole RGBA8 chunk is bound on the DrawItem via
// `lightmap_texels`/`w`/`h`, so `end_frame` routes the draw onto the
// rasterizer's per-pixel bilinear surface_cached path (it samples the
// chunk through each face's 0..1 `uv` and multiplies by the white vertex
// colour ⇒ the texture dominates). The geometry still exercises every hot
// path in the rasterizer (vertex transform → triangle setup → tile bin →
// perspective-correct interpolation → Z reject) plus the textured fetch.
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

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "ui/console/ConsoleOverlay.h"
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

// Unit cube — 24 verts (4 per face × 6 faces). Every vertex colour is white
// so the surface_cached path's `vertexColor × chunk` reduces to the crate
// texture itself (the chunk dominates; see kCrateWhite + build_crate_texture
// below). Each face's `uv` spans the full 0..1 range, so the whole crate
// chunk maps across every face. Z faces forward (-Z), so the front of each
// crate faces the camera at angle 0.
constexpr u32 pack_rgba(u8 r, u8 g, u8 b, u8 a = 255) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

// White ⇒ out = texture. The rasterizer multiplies the interpolated vertex
// colour by the sampled chunk, so a flat white vertex colour lets the
// procedural crate texture come through unmodulated.
constexpr u32 kCrateWhite = 0xFFFFFFFFu;

// 24 vertices for a unit cube centered at origin, ±0.5 on each axis.
const std::array<render::raster::Vertex, 24> kCubeVerts{{
    // +X face (BL → TL → TR → BR in face-local space)
    {{0.5f, -0.5f, -0.5f}, {1, 0, 0}, {0, 1}, {0, 0}, kCrateWhite},
    {{0.5f, 0.5f, -0.5f}, {1, 0, 0}, {0, 0}, {0, 0}, kCrateWhite},
    {{0.5f, 0.5f, 0.5f}, {1, 0, 0}, {1, 0}, {0, 0}, kCrateWhite},
    {{0.5f, -0.5f, 0.5f}, {1, 0, 0}, {1, 1}, {0, 0}, kCrateWhite},
    // -X face
    {{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}, {0, 1}, {0, 0}, kCrateWhite},
    {{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}, {0, 0}, {0, 0}, kCrateWhite},
    {{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}, {1, 0}, {0, 0}, kCrateWhite},
    {{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}, {1, 1}, {0, 0}, kCrateWhite},
    // +Y face (top)
    {{-0.5f, 0.5f, -0.5f}, {0, 1, 0}, {0, 1}, {0, 0}, kCrateWhite},
    {{-0.5f, 0.5f, 0.5f}, {0, 1, 0}, {0, 0}, {0, 0}, kCrateWhite},
    {{0.5f, 0.5f, 0.5f}, {0, 1, 0}, {1, 0}, {0, 0}, kCrateWhite},
    {{0.5f, 0.5f, -0.5f}, {0, 1, 0}, {1, 1}, {0, 0}, kCrateWhite},
    // -Y face (bottom)
    {{-0.5f, -0.5f, 0.5f}, {0, -1, 0}, {0, 1}, {0, 0}, kCrateWhite},
    {{-0.5f, -0.5f, -0.5f}, {0, -1, 0}, {0, 0}, {0, 0}, kCrateWhite},
    {{0.5f, -0.5f, -0.5f}, {0, -1, 0}, {1, 0}, {0, 0}, kCrateWhite},
    {{0.5f, -0.5f, 0.5f}, {0, -1, 0}, {1, 1}, {0, 0}, kCrateWhite},
    // +Z face
    {{-0.5f, -0.5f, 0.5f}, {0, 0, 1}, {0, 1}, {0, 0}, kCrateWhite},
    {{0.5f, -0.5f, 0.5f}, {0, 0, 1}, {1, 1}, {0, 0}, kCrateWhite},
    {{0.5f, 0.5f, 0.5f}, {0, 0, 1}, {1, 0}, {0, 0}, kCrateWhite},
    {{-0.5f, 0.5f, 0.5f}, {0, 0, 1}, {0, 0}, {0, 0}, kCrateWhite},
    // -Z face (front when camera looks down -Z)
    {{0.5f, -0.5f, -0.5f}, {0, 0, -1}, {0, 1}, {0, 0}, kCrateWhite},
    {{-0.5f, -0.5f, -0.5f}, {0, 0, -1}, {1, 1}, {0, 0}, kCrateWhite},
    {{-0.5f, 0.5f, -0.5f}, {0, 0, -1}, {1, 0}, {0, 0}, kCrateWhite},
    {{0.5f, 0.5f, -0.5f}, {0, 0, -1}, {0, 0}, {0, 0}, kCrateWhite},
}};

// ─── Procedural wooden-crate texture ─────────────────────────────────────
// Deterministic RGBA8 chunk (kCrateTexDim², pitch == width) that reads as a
// shipping crate: vertical wooden planks with darker grooves between them,
// a heavy dark frame around the border, two diagonal cross-battens, and a
// metal bolt in each corner. No RNG — a couple of cheap integer hashes give
// the planks per-column grain + light per-texel speckle so the wood doesn't
// look like flat bands. The buffer is owned by main() and outlives the
// render loop; its data pointer is handed to each crate's DrawItem.
constexpr u32 kCrateTexDim = 128;

// Small deterministic 2D value hash → [0,1). Cheap, repeatable, no global
// state; used only to add fine grain/speckle so the result isn't banded.
f32 hash2(u32 x, u32 y) noexcept {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
}

u8 clamp_u8(i32 v) noexcept {
    return static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

std::vector<u32> build_crate_texture() {
    const u32 dim = kCrateTexDim;
    std::vector<u32> tex(static_cast<usize>(dim) * dim, 0u);

    // Palette (warm wood + dark iron frame). Plank base is a mid wood brown;
    // each plank gets a small per-column shade offset so adjacent boards read
    // as separate boards.
    constexpr i32 kWoodR = 150, kWoodG = 103, kWoodB = 56;      // base plank
    constexpr i32 kGrooveR = 64, kGrooveG = 40, kGrooveB = 20;  // gap shadow
    constexpr i32 kFrameR = 78, kFrameG = 50, kFrameB = 26;     // border/batten
    constexpr i32 kBoltR = 92, kBoltG = 96, kBoltB = 104;       // iron bolt

    const u32 frame = dim / 12;                 // border thickness (~10 px @128)
    const u32 plank_w = dim / 6;                // 6 vertical planks across the face
    const u32 groove = std::max(2u, dim / 64);  // dark gap between planks
    const f32 dimf = static_cast<f32>(dim);

    for (u32 y = 0; y < dim; ++y) {
        for (u32 x = 0; x < dim; ++x) {
            // Per-plank shade so neighbouring boards differ; deterministic
            // off the plank index. ±18 luma swing across the 6 planks.
            const u32 plank_idx = x / plank_w;
            const i32 plank_shade =
                static_cast<i32>((plank_idx * 37u) % 37u) - 18 + static_cast<i32>(plank_idx * 6u);
            // Long vertical grain: a slow cosine down the board + fine speckle.
            const f32 grain =
                std::cos((static_cast<f32>(x) + static_cast<f32>(plank_idx) * 11.0f) * 0.9f) * 10.0f;
            const f32 speckle = (hash2(x, y) - 0.5f) * 22.0f;
            i32 r = kWoodR + plank_shade + static_cast<i32>(grain + speckle);
            i32 g = kWoodG + plank_shade + static_cast<i32>(grain * 0.8f + speckle);
            i32 b = kWoodB + plank_shade + static_cast<i32>(grain * 0.5f + speckle);

            // Dark groove between planks (skip the leading edge at x==0).
            const u32 col_in_plank = x % plank_w;
            const bool in_groove = (col_in_plank < groove) && (plank_idx > 0);

            // Diagonal cross-battens (two bars forming an X across the
            // interior). Bar half-width scales with the face.
            const f32 fx = static_cast<f32>(x) / dimf;
            const f32 fy = static_cast<f32>(y) / dimf;
            const f32 bar = 0.06f;
            const bool on_batten = (std::fabs(fx - fy) < bar) || (std::fabs(fx - (1.0f - fy)) < bar);

            // Heavy border frame around the whole face.
            const bool in_frame =
                (x < frame) || (y < frame) || (x >= dim - frame) || (y >= dim - frame);

            // Corner bolts: small discs centred inside each corner of the
            // frame so the crate reads as bolted-together.
            const f32 bolt_in = static_cast<f32>(frame) * 0.5f;
            const f32 corners[4][2] = {
                {bolt_in, bolt_in},
                {dimf - bolt_in, bolt_in},
                {bolt_in, dimf - bolt_in},
                {dimf - bolt_in, dimf - bolt_in},
            };
            bool on_bolt = false;
            const f32 bolt_rad = static_cast<f32>(frame) * 0.30f;
            for (const auto& c : corners) {
                const f32 dx = static_cast<f32>(x) + 0.5f - c[0];
                const f32 dy = static_cast<f32>(y) + 0.5f - c[1];
                if (dx * dx + dy * dy <= bolt_rad * bolt_rad) {
                    on_bolt = true;
                    break;
                }
            }

            if (on_bolt) {
                // Tiny top-left shading so the bolt reads as a rounded head.
                const i32 sh = static_cast<i32>((hash2(x, y) - 0.5f) * 30.0f);
                r = kBoltR + sh;
                g = kBoltG + sh;
                b = kBoltB + sh;
            } else if (in_groove) {
                r = kGrooveR;
                g = kGrooveG;
                b = kGrooveB;
            } else if (in_frame || on_batten) {
                // Frame/batten share the dark iron-stained timber tone, with
                // the same fine speckle so they aren't dead flat.
                const i32 sp = static_cast<i32>((hash2(x, y) - 0.5f) * 16.0f);
                r = kFrameR + sp;
                g = kFrameG + sp;
                b = kFrameB + sp;
            }

            tex[static_cast<usize>(y) * dim + x] =
                pack_rgba(clamp_u8(r), clamp_u8(g), clamp_u8(b), 255);
        }
    }
    return tex;
}

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
    const app::AppArgs args = app::parse_common_args(argc, argv).args;

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

    // Procedural wooden-crate texture, built once and owned here so its
    // storage outlives every end_frame() that samples it. Each crate's
    // DrawItem points `lightmap_texels` at this buffer (pitch == width), so
    // the rasterizer's per-pixel surface_cached path samples it through the
    // cube's 0..1 face `uv` and multiplies by the white vertex colour.
    const std::vector<u32> crate_tex = build_crate_texture();

    // The rasterizer now back-face culls by default, so the cube must be
    // wound consistently with its per-vertex normals or faces drop out as a
    // crate spins (the old two-sided path hid this). Rewind once from the
    // shared normals; the per-face crate palette is left untouched.
    std::array<u32, kCubeIndices.size()> cube_idx = kCubeIndices;
    samples::fix_winding(kCubeVerts.data(),
                         static_cast<u32>(kCubeVerts.size()),
                         cube_idx.data(),
                         static_cast<u32>(cube_idx.size()));

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

        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
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
            // Bind the procedural crate chunk: forces the SurfaceCached path
            // so the inner loop computes white × crate(uv) per pixel. Buffer
            // is owned by `crate_tex` above and outlives this loop.
            item.lightmap_texels = crate_tex.data();
            item.lightmap_w = kCrateTexDim;
            item.lightmap_h = kCrateTexDim;
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
        ui::console::draw(fb);  // drop-down console (`~`) overlays everything
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
