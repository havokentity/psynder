// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: a camera-facing quad must fill solid with no
// holes. Regression guard for the "polygons drop out / holes pop" class of
// bug: a quad is two triangles sharing a diagonal; if either triangle is
// wrongly culled (winding) or mis-clipped, the quad shows a hole. We drive
// the real engine rasterizer (begin/submit/end) and scan the framebuffer.
//
// The golden-image tests do NOT cover this — they use their own software
// renderer, not the engine raster path — which is how the hole bug shipped.

#include "core/console/Console.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

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

// Draw a square quad facing the camera, with the given index winding, and
// return the number of BACKGROUND pixels found inside the central 40% box
// (which projects well inside the quad). 0 = fully covered, no holes.
u32 background_holes_in_centre(const u32* indices, u32 index_count) {
    constexpr u32 W = 160, H = 160;
    Image img(W, H);

    // White quad centred at the origin, in the z=0 plane, facing +Z toward a
    // camera at z=5 looking down -Z. Half-extent 2.0 fills most of the view.
    const Vertex verts[] = {
        Vertex{math::Vec3{-2, -2, 0}, math::Vec3{0, 0, 1}, math::Vec2{0, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{2, -2, 0}, math::Vec3{0, 0, 1}, math::Vec2{1, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{2, 2, 0}, math::Vec3{0, 0, 1}, math::Vec2{1, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{-2, 2, 0}, math::Vec3{0, 0, 1}, math::Vec2{0, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
    };

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 5}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
    v.projection = math::perspective_rh(60.0f * math::kDegToRad,
                                        static_cast<f32>(W) / static_cast<f32>(H),
                                        0.1f,
                                        100.0f);
    v.target = img.fb;
    v.tile_w = 64;
    v.tile_h = 64;

    clear_framebuffer(img.fb, 0xFF000000u);

    DrawItem d{};
    d.vertices = verts;
    d.vertex_count = 4;
    d.indices = indices;
    d.index_count = index_count;
    d.model = math::identity4();

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    u32 holes = 0;
    for (u32 y = static_cast<u32>(H * 0.3f); y < static_cast<u32>(H * 0.7f); ++y) {
        for (u32 x = static_cast<u32>(W * 0.3f); x < static_cast<u32>(W * 0.7f); ++x) {
            if (img.pixels[static_cast<std::size_t>(y) * W + x] == 0xFF000000u)
                ++holes;
        }
    }
    return holes;
}

}  // namespace

TEST_CASE("camera-facing quad fills solid with no holes (CCW fan)", "[raster][coverage][quad]") {
    const u32 idx[] = {0, 1, 2, 0, 2, 3};
    REQUIRE(background_holes_in_centre(idx, 6) == 0);
}

TEST_CASE("camera-facing quad fills solid regardless of winding (two-sided)",
          "[raster][coverage][quad]") {
    // The reversed winding must fill identically — two-sided rasterization
    // means neither winding may drop a triangle and leave a hole.
    const u32 idx[] = {0, 2, 1, 0, 3, 2};
    REQUIRE(background_holes_in_centre(idx, 6) == 0);
}
