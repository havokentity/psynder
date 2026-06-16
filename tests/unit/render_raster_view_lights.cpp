// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: the ViewState dynamic-light list must actually
// reach the fragment shader. This is the regression guard for the root-cause
// rendering bug where the editor raster path never fed scene lights, so
// everything shaded flat white and `light_add` had zero visible effect.
//
// We drive the real engine rasterizer (begin/submit/end) twice over a white,
// untextured, camera-facing quad:
//   * default ViewState (no view lights)  -> begin_frame keeps full-white
//     ambient + no lights, so the shader returns 1.0 and the quad is white.
//   * ViewState carrying ONE directional light + dim ambient -> the shader
//     evaluates the packet and the lit centre pixel is meaningfully darker
//     than the unlit white case.
// If the view-light fields never reached begin_frame -> the inner DrawCmd, the
// two renders would be byte-identical and this test fails — exactly the bug.

#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/RasterLighting.h"

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

// Render a white, untextured quad facing +Z (camera at z=5 looking down -Z) and
// return the centre pixel. When `lights != nullptr` the view carries them; the
// quad's normal faces the camera so a -Z directional light lands full-strength.
u32 render_quad_centre(const RasterLight* lights, u32 light_count, math::Vec3 ambient) {
    constexpr u32 W = 96, H = 96;
    Image img(W, H);

    const Vertex verts[] = {
        Vertex{math::Vec3{-2, -2, 0}, math::Vec3{0, 0, 1}, math::Vec2{0, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{2, -2, 0}, math::Vec3{0, 0, 1}, math::Vec2{1, 0}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{2, 2, 0}, math::Vec3{0, 0, 1}, math::Vec2{1, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
        Vertex{math::Vec3{-2, 2, 0}, math::Vec3{0, 0, 1}, math::Vec2{0, 1}, math::Vec2{0, 0}, 0xFFFFFFFFu},
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
    v.lights = lights;
    v.light_count = light_count;
    v.ambient_linear = ambient;

    clear_framebuffer(img.fb, 0xFF000000u);

    DrawItem d{};
    d.vertices = verts;
    d.vertex_count = 4;
    d.indices = idx;
    d.index_count = 6;
    d.model = math::identity4();
    d.cull = CullMode::None;  // about lighting, not culling

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    return img.pixels[static_cast<std::size_t>(H / 2) * W + (W / 2)];
}

PSY_FORCEINLINE u32 red_of(u32 rgba8) noexcept {
    return rgba8 & 0xFFu;
}

}  // namespace

// Default path: no view lights -> begin_frame's historical default (full-white
// ambient, no dynamic lights) shades the white quad white. This is the byte-
// identical behaviour samples/goldens rely on; if it drifts, the gate leaked.
TEST_CASE("view lights: no lights renders the quad full white (default path)",
          "[render][raster][lights]") {
    const u32 centre = render_quad_centre(nullptr, 0, math::Vec3{1.0f, 1.0f, 1.0f});
    // Opaque white: RGB all ~255.
    REQUIRE(red_of(centre) >= 250u);
    REQUIRE(((centre >> 8) & 0xFFu) >= 250u);
    REQUIRE(((centre >> 16) & 0xFFu) >= 250u);
}

// The load-bearing assertion: a ViewState-supplied directional light reaches the
// fragment shader. The lit case uses zero ambient + a half-intensity -Z light on
// a +Z-facing quad (n·l == 1), so the centre shades to ~0.5 (mid grey) — clearly
// distinct from the full-white unlit case. Equal results would mean the view
// lights never made it into begin_frame -> DrawCmd::light_packet, i.e. the bug.
TEST_CASE("view lights: a directional light through the view dims the quad",
          "[render][raster][lights]") {
    const u32 unlit = render_quad_centre(nullptr, 0, math::Vec3{1.0f, 1.0f, 1.0f});

    RasterLight key{};
    key.kind = RasterLightKind::Directional;
    key.direction_world = math::Vec3{0.0f, 0.0f, -1.0f};  // illuminates the +Z face
    key.color_linear = math::Vec3{1.0f, 1.0f, 1.0f};
    key.intensity = 0.5f;
    const u32 lit = render_quad_centre(&key, 1, math::Vec3{0.0f, 0.0f, 0.0f});

    // Both pixels are covered (not background).
    REQUIRE(unlit != 0xFF000000u);
    REQUIRE(lit != 0xFF000000u);
    // The view light reached shading: the lit centre is distinctly darker.
    REQUIRE(red_of(lit) < red_of(unlit));
    REQUIRE(red_of(unlit) - red_of(lit) > 40u);
    // n·l == 1 at intensity 0.5 with zero ambient -> ~0.5 -> ~127.
    REQUIRE(red_of(lit) > 90u);
    REQUIRE(red_of(lit) < 165u);
}
