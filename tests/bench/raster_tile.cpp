// SPDX-License-Identifier: MIT
// Psynder — bench: per-tile rasterizer cost across the three ADR-002
// specializations. CI gate enforces > 2% regression must justify
// (docs/wave-a-bar.md item 4). Wave-A bench runs with --smoke for the
// quick smoke validation; longer runs report ns/pixel.

#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/TestMesh.h"

#include "core/console/Console.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

struct Image {
    std::vector<u32> pixels;
    std::vector<u32> depth;
    Framebuffer      fb{};
    explicit Image(u32 w, u32 h)
        : pixels(static_cast<std::size_t>(w) * h, 0xFF000000u),
          depth(static_cast<std::size_t>(w) * h, 0) {
        fb.width  = w;
        fb.height = h;
        fb.pitch  = w * 4;
        fb.format = PixelFormat::RGBA8;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth  = depth.data();
    }
};

f64 bench_one(Image& img, u32 tile_size, u32 iters) {
    console::Console::Get().RegisterCVar("r_tile_size", "64", "", 0);
    console::Console::Get().SetCVarOverride("r_tile_size", std::to_string(tile_size));

    auto mesh = test_mesh::fullscreen_quad();

    ViewState v{};
    v.view       = math::look_at_rh(math::Vec3{0,0,2}, math::Vec3{0,0,0}, math::Vec3{0,1,0});
    v.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                        static_cast<f32>(img.fb.width) /
                                        static_cast<f32>(img.fb.height),
                                        0.1f, 100.0f);
    v.target     = img.fb;
    v.tile_w     = tile_size;
    v.tile_h     = tile_size;

    DrawItem d{};
    d.vertices     = mesh.vertices;
    d.vertex_count = mesh.vertex_count;
    d.indices      = mesh.indices;
    d.index_count  = mesh.index_count;
    d.model        = math::identity4();

    // Warm-up
    auto& r = Rasterizer::Get();
    clear_framebuffer(img.fb, 0xFF000000u);
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    auto t0 = std::chrono::steady_clock::now();
    for (u32 i = 0; i < iters; ++i) {
        clear_framebuffer(img.fb, 0xFF000000u);
        r.begin_frame(v);
        r.submit(d);
        r.end_frame();
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<f64, std::milli>(t1 - t0).count() /
           static_cast<f64>(iters);
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
    }

    constexpr u32 W = 640;
    constexpr u32 H = 360;
    const u32 iters = smoke ? 1u : 50u;

    Image img(W, H);

    std::printf("psynder_bench raster_tile (%ux%u, %u iters)\n", W, H, iters);
    for (u32 ts : {32u, 64u, 128u}) {
        const f64 ms = bench_one(img, ts, iters);
        std::printf("  tile=%3u: %.3f ms/frame  (%.2f Mpix/s)\n",
                    ts, ms,
                    (static_cast<f64>(W) * H / (ms / 1000.0)) / 1.0e6);
    }
    return 0;
}
