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
u32 background_holes_in_centre(const u32* indices, u32 index_count, u8 cull) {
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
    d.cull = cull;

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

// Render a large floor quad that STRADDLES the near plane — its near edge sits
// behind the camera (clip-space z + w < 0), the rest extends far in front — and
// return the number of COVERED (white) pixels in the lower-centre band where the
// floor must project. Near-plane clipping has to split the straddling triangles
// and keep the in-front part; the pre-fix code dropped any triangle with a
// vertex behind the near plane, which collapses this count to ~0.
u32 floor_coverage_lower_band(u8 cull) {
    constexpr u32 W = 160, H = 160;
    Image img(W, H);

    // Floor at y = -1. The near edge (z = 6) is behind the eye (z = 4), so those
    // triangles straddle the near plane; the far edge (z = -24) is well in front.
    const Vertex verts[] = {
        Vertex{math::Vec3{-8, -1, 6}, math::Vec3{0, 1, 0}, math::Vec2{0, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{8, -1, 6}, math::Vec3{0, 1, 0}, math::Vec2{1, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{8, -1, -24}, math::Vec3{0, 1, 0}, math::Vec2{1, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{-8, -1, -24}, math::Vec3{0, 1, 0}, math::Vec2{0, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
    };
    const u32 idx[] = {0, 1, 2, 0, 2, 3};

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 1.5f, 4}, math::Vec3{0, -1, -4}, math::Vec3{0, 1, 0});
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
    d.indices = idx;
    d.index_count = 6;
    d.model = math::identity4();
    d.cull = cull;

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    u32 covered = 0;
    for (u32 y = static_cast<u32>(H * 0.62f); y < static_cast<u32>(H * 0.95f); ++y) {
        for (u32 x = static_cast<u32>(W * 0.30f); x < static_cast<u32>(W * 0.70f); ++x) {
            if (img.pixels[static_cast<std::size_t>(y) * W + x] == 0xFFFFFFFFu)
                ++covered;
        }
    }
    return covered;
}

}  // namespace

// HiZ coplanar-quad guard (the original "holes pop" regression): a
// camera-facing quad must fill with NO interior holes. Rendered two-sided so
// the assertion is winding-independent — the HiZ early-reject bug was unrelated
// to culling, and this is the test that would have caught it.
TEST_CASE("HiZ: coplanar camera-facing quad fills with no holes", "[raster][coverage][quad]") {
    const u32 idx[] = {0, 1, 2, 0, 2, 3};
    REQUIRE(background_holes_in_centre(idx, 6, /*cull=None*/ 2) == 0);
}

// Back-face culling keeps exactly one winding and culls its mirror — proves
// culling is real (not globally two-sided) while still hole-free on the kept
// side.
TEST_CASE("back-face culling: one winding fills, the mirror is culled", "[raster][coverage][cull]") {
    const u32 a_idx[] = {0, 1, 2, 0, 2, 3};
    const u32 b_idx[] = {0, 2, 1, 0, 3, 2};
    const u32 a = background_holes_in_centre(a_idx, 6, /*cull=Back*/ 0);
    const u32 b = background_holes_in_centre(b_idx, 6, /*cull=Back*/ 0);
    // Exactly one winding is front-facing (0 holes); the other is fully culled
    // (its central box is all background).
    REQUIRE((a == 0) != (b == 0));
    REQUIRE((a == 0 ? b : a) > 1000);
}

// Near-plane clipping guard: a floor quad whose near edge sits BEHIND the camera
// must still fill the lower screen. The pre-fix path dropped any triangle with a
// vertex behind the near plane, which blacked out floors/walls in interior (FPS)
// views — exactly the "room is mostly black with holes at the corners" bug.
// Rendered two-sided so the assertion is winding-independent (this is about
// clipping, not culling). The lower-centre band is ~53x64 = 3392 px; a correctly
// clipped floor fills essentially all of it, while a dropped straddle yields ~0.
TEST_CASE("near-plane clip: a straddling floor still fills (no dropped half)",
          "[raster][coverage][clip]") {
    const u32 covered = floor_coverage_lower_band(/*cull=None*/ 2);
    REQUIRE(covered > 3000);
}
