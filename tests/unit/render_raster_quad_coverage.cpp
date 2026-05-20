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
u32 background_holes_in_centre(const u32* indices, u32 index_count, CullMode cull) {
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
u32 floor_coverage_lower_band(CullMode cull) {
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

// Render a single large FLAT camera-facing quad (in the z=0 plane) whose extent
// reaches WELL beyond the lateral frustum edges, then count BACKGROUND pixels
// inside the central `frac` sample box. With full-frustum clipping the quad's
// on-screen portion must fill solid (no holes) and the rasterizer must not crash
// or emit garbage from Q24.8 edge-function overflow on the far-off-screen verts.
//
// The camera sits at z=5 looking down -Z, so a large half_xy pushes all four
// corners thousands of pixels past the L/R/T/B clip planes. The sample box is
// the central `frac` of the screen, which always projects inside the quad.
u32 background_holes_big_quad(f32 half_xy, f32 frac) {
    constexpr u32 W = 160, H = 160;
    Image img(W, H);

    const Vertex verts[] = {
        Vertex{math::Vec3{-half_xy, -half_xy, 0},
               math::Vec3{0, 0, 1},
               math::Vec2{0, 0},
               math::Vec2{0, 0},
               0xFFFFFFFFu},
        Vertex{math::Vec3{half_xy, -half_xy, 0},
               math::Vec3{0, 0, 1},
               math::Vec2{1, 0},
               math::Vec2{0, 0},
               0xFFFFFFFFu},
        Vertex{math::Vec3{half_xy, half_xy, 0},
               math::Vec3{0, 0, 1},
               math::Vec2{1, 1},
               math::Vec2{0, 0},
               0xFFFFFFFFu},
        Vertex{math::Vec3{-half_xy, half_xy, 0},
               math::Vec3{0, 0, 1},
               math::Vec2{0, 1},
               math::Vec2{0, 0},
               0xFFFFFFFFu},
    };
    const u32 idx[] = {0, 1, 2, 0, 2, 3};

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
    d.indices = idx;
    d.index_count = 6;
    d.model = math::identity4();
    d.cull = CullMode::None;  // about clipping, not culling

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    const u32 lo_y = static_cast<u32>(H * (0.5f - frac * 0.5f));
    const u32 hi_y = static_cast<u32>(H * (0.5f + frac * 0.5f));
    const u32 lo_x = static_cast<u32>(W * (0.5f - frac * 0.5f));
    const u32 hi_x = static_cast<u32>(W * (0.5f + frac * 0.5f));
    u32 holes = 0;
    for (u32 y = lo_y; y < hi_y; ++y) {
        for (u32 x = lo_x; x < hi_x; ++x) {
            if (img.pixels[static_cast<std::size_t>(y) * W + x] == 0xFF000000u)
                ++holes;
        }
    }
    return holes;
}

// Render a long floor quad that recedes from just in front of the camera to WELL
// beyond the far plane, then count COVERED (white) pixels in the lower-centre
// band. The far edge crosses the clip-space far plane (w - z < 0), which the old
// near-plane-only path never clipped; full-frustum clipping must split it and
// keep the in-frustum near portion so the lower screen still fills solid.
u32 floor_crosses_far_coverage(CullMode cull) {
    constexpr u32 W = 160, H = 160;
    Image img(W, H);

    // Camera at (0,1.5,4) looking down toward -Z. Far plane is 100 units out.
    // Floor at y=-1 from z=2 (well inside) to z=-400 (far past the far plane).
    const Vertex verts[] = {
        Vertex{math::Vec3{-12, -1, 2}, math::Vec3{0, 1, 0}, math::Vec2{0, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{12, -1, 2}, math::Vec3{0, 1, 0}, math::Vec2{1, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{12, -1, -400}, math::Vec3{0, 1, 0}, math::Vec2{1, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{-12, -1, -400}, math::Vec3{0, 1, 0}, math::Vec2{0, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
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

// Render a flat axis-aligned triangle whose two off-screen verts sit far beyond
// one chosen frustum edge, and count BACKGROUND pixels inside an on-screen box
// near the opposite (on-screen) side. `edge` selects which side blows out:
// 0 = left, 1 = right, 2 = bottom, 3 = top. The on-screen apex plus a very wide
// far base guarantees the sample box is covered iff clipping kept the on-screen
// portion intact (no Q24.8 overflow garbage).
u32 background_holes_offscreen_edge(int edge) {
    constexpr u32 W = 160, H = 160;
    Image img(W, H);

    // Apex near screen centre (z = 0); base pushed far out along the chosen
    // edge direction. Huge magnitudes ensure the off-screen verts are well
    // outside the L/R/T/B clip planes.
    const f32 big = 4000.0f;
    math::Vec3 apex{0, 0, 0};
    math::Vec3 b0, b1;
    switch (edge) {
        case 0:  // left
            b0 = math::Vec3{-big, -big, 0};
            b1 = math::Vec3{-big, big, 0};
            break;
        case 1:  // right
            b0 = math::Vec3{big, big, 0};
            b1 = math::Vec3{big, -big, 0};
            break;
        case 2:  // bottom
            b0 = math::Vec3{-big, -big, 0};
            b1 = math::Vec3{big, -big, 0};
            break;
        default:  // top
            b0 = math::Vec3{big, big, 0};
            b1 = math::Vec3{-big, big, 0};
            break;
    }
    const Vertex verts[] = {
        Vertex{apex, math::Vec3{0, 0, 1}, math::Vec2{0.5f, 0.5f}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{b0, math::Vec3{0, 0, 1}, math::Vec2{0, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{b1, math::Vec3{0, 0, 1}, math::Vec2{1, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
    };
    const u32 idx[] = {0, 1, 2};

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
    d.vertex_count = 3;
    d.indices = idx;
    d.index_count = 3;
    d.model = math::identity4();
    d.cull = CullMode::None;

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    // Sample a small box just inside the apex, on the on-screen side. The apex
    // is at the centre; nudge the box toward the blown-out edge so it lands
    // squarely on covered interior.
    f32 cx = 0.5f, cy = 0.5f;
    switch (edge) {
        case 0:
            cx = 0.30f;
            break;  // left edge blown out -> covered region extends left
        case 1:
            cx = 0.70f;
            break;
        case 2:
            cy = 0.70f;
            break;
        default:
            cy = 0.30f;
            break;
    }
    const u32 lo_y = static_cast<u32>(H * (cy - 0.08f));
    const u32 hi_y = static_cast<u32>(H * (cy + 0.08f));
    const u32 lo_x = static_cast<u32>(W * (cx - 0.08f));
    const u32 hi_x = static_cast<u32>(W * (cx + 0.08f));
    u32 holes = 0;
    for (u32 y = lo_y; y < hi_y; ++y) {
        for (u32 x = lo_x; x < hi_x; ++x) {
            if (img.pixels[static_cast<std::size_t>(y) * W + x] == 0xFF000000u)
                ++holes;
        }
    }
    return holes;
}

}  // namespace

// HiZ coplanar-quad guard (the original "holes pop" regression): a
// camera-facing quad must fill with NO interior holes. Rendered two-sided so
// the assertion is winding-independent — the HiZ early-reject bug was unrelated
// to culling, and this is the test that would have caught it.
TEST_CASE("HiZ: coplanar camera-facing quad fills with no holes", "[raster][coverage][quad]") {
    const u32 idx[] = {0, 1, 2, 0, 2, 3};
    REQUIRE(background_holes_in_centre(idx, 6, CullMode::None) == 0);
}

// Back-face culling keeps exactly one winding and culls its mirror — proves
// culling is real (not globally two-sided) while still hole-free on the kept
// side.
TEST_CASE("back-face culling: one winding fills, the mirror is culled", "[raster][coverage][cull]") {
    const u32 a_idx[] = {0, 1, 2, 0, 2, 3};
    const u32 b_idx[] = {0, 2, 1, 0, 3, 2};
    const u32 a = background_holes_in_centre(a_idx, 6, CullMode::Back);
    const u32 b = background_holes_in_centre(b_idx, 6, CullMode::Back);
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
    const u32 covered = floor_coverage_lower_band(CullMode::None);
    REQUIRE(covered > 3000);
}

// Full-frustum clip guard — left/right/top/bottom edges. A huge camera-facing
// quad whose corners sit far outside every lateral clip plane must still fill
// its on-screen portion solid. Before full-frustum clipping these far-off-screen
// verts reached the Q24.8 edge functions with enormous screen coordinates and
// risked fixed-point overflow (holes / garbage); clipping bounds the inputs. The
// central 60% box (~96x96 = 9216 px) always projects inside the quad.
TEST_CASE("full-frustum clip: a quad far past the lateral planes fills with no holes",
          "[raster][coverage][clip]") {
    // half-extent 200 at z=0 with the camera at z=5: corners project thousands
    // of pixels off-screen on all four sides. The central 60% box must be solid.
    const u32 holes = background_holes_big_quad(200.0f, 0.60f);
    REQUIRE(holes == 0);
}

// Full-frustum clip guard — far plane. A floor that recedes well past the far
// plane must still fill the lower screen. The near-plane-only path never clipped
// the far plane at all; full-frustum clipping splits the far edge and keeps the
// in-frustum near portion. Two-sided to stay winding-independent. The lower-
// centre band is ~53x64 = 3392 px; the near floor fills essentially all of it.
TEST_CASE("full-frustum clip: a floor crossing the far plane still fills",
          "[raster][coverage][clip]") {
    const u32 covered = floor_crosses_far_coverage(CullMode::None);
    REQUIRE(covered > 3000);
}

// Full-frustum clip guard — each lateral edge individually. A flat triangle with
// its apex on-screen and a very wide base blown far past one chosen frustum edge
// (left/right/bottom/top) must fill a small on-screen box near the apex with no
// holes and no crash. Exercises each plane's clip in isolation with extreme
// (4000-unit) off-screen coordinates that would overflow Q24.8 without the clip.
TEST_CASE("full-frustum clip: triangles blown past each lateral edge stay hole-free",
          "[raster][coverage][clip]") {
    REQUIRE(background_holes_offscreen_edge(0) == 0);  // left
    REQUIRE(background_holes_offscreen_edge(1) == 0);  // right
    REQUIRE(background_holes_offscreen_edge(2) == 0);  // bottom
    REQUIRE(background_holes_offscreen_edge(3) == 0);  // top
}
