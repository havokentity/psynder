// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: perspective-correct vs affine attribute
// interpolation. Renders the same heavily-foreshortened triangle twice, once
// in each mode, and verifies the two outputs disagree by a measurable
// amount — proving the perspective-correct path actually does the divide
// at the quad (DESIGN.md §7.4).

#include "core/console/Console.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::raster;

namespace {

// Strongly foreshortened triangle in world space — one vertex pushed deep
// into z. Perspective-correct and affine must visibly disagree on it.
// Vertex order is BL → TOP → BR so the triangle is front-facing after the
// viewport y-flip (the rasterizer's CCW-in-screen-space front-face rule).
constexpr Vertex kForeshortenedVerts[] = {
    Vertex{math::Vec3{-1.0f, -1.0f, -2.0f},
           math::Vec3{0, 0, 1},
           math::Vec2{0, 0},
           math::Vec2{0, 0},
           0xFF0000FFu /* red    */},
    Vertex{math::Vec3{0.0f, 1.0f, -20.0f},
           math::Vec3{0, 0, 1},
           math::Vec2{0.5f, 1},
           math::Vec2{0, 0},
           0xFFFF0000u /* blue   */},
    Vertex{math::Vec3{1.0f, -1.0f, -2.0f},
           math::Vec3{0, 0, 1},
           math::Vec2{1, 0},
           math::Vec2{0, 0},
           0xFF00FF00u /* green  */},
};
constexpr u32 kForeshortenedIndices[] = {0, 1, 2};

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

void render_once(Image& img, bool affine) {
    // Ensure cvars are registered; the rasterizer registers them on first
    // begin_frame, but test ordering is random — touching the value via
    // SetCVarOverride before that point would no-op. Force registration.
    console::Console::Get().RegisterCVar("r_affine", "0", "", console::CVarFlags::None);
    console::Console::Get().SetCVarOverride("r_affine", affine ? "1" : "0");

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 5}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
    v.projection =
        math::perspective_rh(60.0f * math::kDegToRad,
                             static_cast<f32>(img.fb.width) / static_cast<f32>(img.fb.height),
                             0.1f,
                             100.0f);
    v.target = img.fb;
    v.tile_w = 64;
    v.tile_h = 64;

    clear_framebuffer(img.fb, 0xFF000000u);

    DrawItem d{};
    d.vertices = kForeshortenedVerts;
    d.vertex_count = 3;
    d.indices = kForeshortenedIndices;
    d.index_count = 3;
    d.model = math::identity4();

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();
}

i64 sum_abs_diff(const Image& a, const Image& b) {
    REQUIRE(a.pixels.size() == b.pixels.size());
    i64 acc = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const u32 pa = a.pixels[i];
        const u32 pb = b.pixels[i];
        for (u32 s = 0; s < 24; s += 8) {
            const i32 ca = static_cast<i32>((pa >> s) & 0xFFu);
            const i32 cb = static_cast<i32>((pb >> s) & 0xFFu);
            acc += std::abs(ca - cb);
        }
    }
    return acc;
}

}  // namespace

TEST_CASE("perspective-correct attribute interpolation disagrees with affine", "[raster][persp]") {
    constexpr u32 W = 320;
    constexpr u32 H = 240;

    Image img_persp(W, H);
    Image img_affine(W, H);

    render_once(img_persp, /*affine=*/false);
    render_once(img_affine, /*affine=*/true);

    // Verify both modes actually drew the triangle (centroid pixel is
    // non-background).
    const std::size_t centre = (H / 2) * W + W / 2;
    REQUIRE(img_persp.pixels[centre] != 0xFF000000u);
    REQUIRE(img_affine.pixels[centre] != 0xFF000000u);

    // The two outputs must differ. A heavily-foreshortened triangle
    // produces a measurable disagreement; the threshold is conservative.
    const i64 diff = sum_abs_diff(img_persp, img_affine);
    INFO("sum-abs-diff between persp and affine = " << diff);
    REQUIRE(diff > 1000);
}

TEST_CASE("clear_framebuffer also clears depth to far plane", "[raster][clear]") {
    constexpr u32 W = 64, H = 64;
    Image img(W, H);
    clear_framebuffer(img.fb, 0xFF7F7F7Fu);
    // First pixel must be the clear colour.
    REQUIRE(img.pixels[0] == 0xFF7F7F7Fu);
    // Depth packs 1.0f's high 24 bits + 0 stencil.
    u32 expected;
    constexpr f32 one = 1.0f;
    std::memcpy(&expected, &one, sizeof(expected));
    expected &= 0xFFFFFF00u;
    REQUIRE(img.depth[0] == expected);
}
