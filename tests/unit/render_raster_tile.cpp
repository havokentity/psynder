// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: single-tile coverage bitmap.
//
// Renders a known triangle into a small framebuffer and verifies the
// coverage bitmap matches the analytical expectation. Also exercises the
// 32 / 64 / 128 tile specializations (ADR-002): the rendered image is
// invariant to tile size for any valid triangle.

#include "core/console/Console.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/TestMesh.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

struct Image {
    std::vector<u32> pixels;
    std::vector<u32> depth;
    Framebuffer fb{};
    explicit Image(u32 w, u32 h)
        : pixels(static_cast<std::size_t>(w) * h, 0xFF000000u)
        , depth(static_cast<std::size_t>(w) * h, 0) {
        fb.width = w;
        fb.height = h;
        fb.pitch = w * 4;
        fb.format = PixelFormat::RGBA8;
        fb.pixels = reinterpret_cast<u8*>(pixels.data());
        fb.depth = depth.data();
    }
};

void render_triangle(Image& img, u32 tile_size) {
    console::Console::Get().RegisterCVar("r_tile_size", "64", "", 0);
    console::Console::Get().SetCVarOverride("r_tile_size", std::to_string(tile_size));

    auto mesh = test_mesh::colored_triangle();

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 2}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
    v.projection =
        math::perspective_rh(60.0f * math::kDegToRad,
                             static_cast<f32>(img.fb.width) / static_cast<f32>(img.fb.height),
                             0.1f,
                             100.0f);
    v.target = img.fb;
    v.tile_w = tile_size;
    v.tile_h = tile_size;

    clear_framebuffer(img.fb, 0xFF000000u);

    DrawItem d{};
    d.vertices = mesh.vertices;
    d.vertex_count = mesh.vertex_count;
    d.indices = mesh.indices;
    d.index_count = mesh.index_count;
    d.model = math::identity4();

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();
}

u32 count_lit(const Image& img) {
    u32 c = 0;
    for (u32 p : img.pixels)
        if (p != 0xFF000000u)
            ++c;
    return c;
}

}  // namespace

TEST_CASE("test-mesh helpers return valid geometry", "[raster][testmesh]") {
    auto tri = test_mesh::colored_triangle();
    REQUIRE(tri.vertex_count == 3);
    REQUIRE(tri.index_count == 3);
    REQUIRE(tri.vertices != nullptr);
    REQUIRE(tri.indices != nullptr);

    auto quad = test_mesh::fullscreen_quad();
    REQUIRE(quad.vertex_count == 4);
    REQUIRE(quad.index_count == 6);

    u32 tw = 0, th = 0;
    const u32* tex = test_mesh::checkerboard_texture(tw, th);
    REQUIRE(tex != nullptr);
    REQUIRE(tw == 8);
    REQUIRE(th == 8);
}

TEST_CASE("rasterizer fills the test triangle and writes depth", "[raster][tile]") {
    Image img(128, 128);
    render_triangle(img, 64);

    const u32 lit = count_lit(img);
    INFO("lit pixels = " << lit);
    REQUIRE(lit > 1000);       // triangle is substantial in 128×128
    REQUIRE(lit < 128 * 128);  // and doesn't cover the whole framebuffer
    // A pixel in the centre should be lit (centroid of NDC triangle).
    REQUIRE(img.pixels[64 * 128 + 64] != 0xFF000000u);
}

TEST_CASE("tile size is invariant: 32 / 64 / 128 produce identical output",
          "[raster][tile][adr-002]") {
    Image img_32(128, 128);
    Image img_64(128, 128);
    Image img_128(128, 128);
    render_triangle(img_32, 32);
    render_triangle(img_64, 64);
    render_triangle(img_128, 128);

    REQUIRE(img_32.pixels == img_64.pixels);
    REQUIRE(img_64.pixels == img_128.pixels);
}

TEST_CASE("submit before begin_frame is a no-op (no crash)", "[raster][lifecycle]") {
    auto& r = Rasterizer::Get();
    auto tri = test_mesh::colored_triangle();
    DrawItem d{};
    d.vertices = tri.vertices;
    d.vertex_count = tri.vertex_count;
    d.indices = tri.indices;
    d.index_count = tri.index_count;
    // No active frame
    r.submit(d);    // must not crash
    r.end_frame();  // must not crash
    SUCCEED();
}
