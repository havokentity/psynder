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

#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "editor/core/SampleHook.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// ─── CLI parsing ─────────────────────────────────────────────────────────
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
    constexpr std::string_view kFlag = "--smoke-frames=";
    constexpr std::string_view kFlagSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kFlag)) {
            a.smoke_frames = parse_uint(s.substr(kFlag.size()));
        } else if (s == kFlagSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

// ─── Tiny PPM (P6) loader — RGB8, no comments ────────────────────────────
struct Texture {
    u32 width = 0;
    u32 height = 0;
    std::vector<u32> pixels;  // RGBA8, alpha = 0xFF
};

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

bool load_ppm(const std::string& path, Texture& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[3] = {0, 0, 0};
    if (std::fread(magic, 1, 2, f) != 2 || magic[0] != 'P' || magic[1] != '6') {
        std::fclose(f);
        return false;
    }
    int w = 0, h = 0, maxv = 0;
    if (std::fscanf(f, "%d %d %d", &w, &h, &maxv) != 3 || maxv != 255 || w <= 0 || h <= 0) {
        std::fclose(f);
        return false;
    }
    // Skip exactly one whitespace byte before binary payload.
    std::fgetc(f);
    const usize uw = static_cast<usize>(w);
    const usize uh = static_cast<usize>(h);
    std::vector<u8> rgb(uw * uh * 3);
    if (std::fread(rgb.data(), 1, rgb.size(), f) != rgb.size()) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    out.width = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.pixels.resize(uw * uh);
    for (usize i = 0, n = uw * uh; i < n; ++i) {
        u32 r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        out.pixels[i] = r | (g << 8) | (b << 16) | (0xFFu << 24);
    }
    return true;
}

// Fallback texture if asset load fails — magenta/black checkerboard so the
// failure is loud, not silent.
Texture make_fallback_texture() {
    Texture t;
    t.width = t.height = 16;
    t.pixels.resize(16 * 16);
    for (u32 y = 0; y < 16; ++y)
        for (u32 x = 0; x < 16; ++x) {
            const bool magenta = ((x ^ y) & 1u) != 0u;
            t.pixels[static_cast<usize>(y) * 16u + x] = magenta ? 0xFFFF00FFu : 0xFF000000u;
        }
    return t;
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
    render::Framebuffer& fb, const V2& a, const V2& b, const V2& c, const Texture& tex) {
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
                tex.pixels[static_cast<usize>(ty) * tex.width + tx];
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const u32 smoke_frames = args.smoke_frames;

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 01 (textured triangle)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = 640;
    desc.render_height = 360;
    desc.scale_mode = platform::ScaleMode::Integer;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_01: failed to create window");
        return EXIT_FAILURE;
    }

    // Load the crate texture (procedurally-generated 32×32 PPM committed
    // alongside the sample).
    Texture crate{};
    const std::string tex_path = find_asset("assets/crate.ppm");
    if (!load_ppm(tex_path, crate)) {
        PSY_LOG_WARN("sample_01: failed to load {} — using fallback checker", tex_path);
        crate = make_fallback_texture();
    } else {
        PSY_LOG_INFO("sample_01: loaded crate texture {} ({}x{})", tex_path, crate.width, crate.height);
    }

    // CPU-side framebuffer at the internal render resolution.
    std::vector<u32> pixels(static_cast<usize>(desc.render_width) * desc.render_height, 0);
    render::Framebuffer fb{};
    fb.width = desc.render_width;
    fb.height = desc.render_height;
    fb.pitch = desc.render_width * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(pixels.data());

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
        raster_triangle_nearest(fb, a, b, c, crate);

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

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_01: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_01: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
