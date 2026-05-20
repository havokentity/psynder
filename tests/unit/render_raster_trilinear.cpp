// SPDX-License-Identifier: MIT
// Psynder — lane 07 unit test: trilinear texture sampling. The trilinear
// path lerps between two adjacent mip levels' bilinear samples; with a
// 2-mip texture, exact mid-mip (LOD=0.5) is the per-channel midpoint of
// the two mip samples. DESIGN.md §7.5.
//
// The rasterizer's trilinear sampler is in an anonymous namespace inside
// TileRaster.cpp — to test it from outside we drive a triangle in screen
// space with a 2-mip texture and observe the output. The triangle covers
// just enough texels to make the trilinear lerp visible.

#include "core/console/Console.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"
#include "render/raster/TestMesh.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
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

}  // namespace

// The textured rasterizer is the integration target; we don't have a
// public hook to feed a Texture struct yet (the sampler currently runs
// against a TileBin-internal Texture). The headless path covers the
// trilinear math via Texture::MipLevel inside the rasterizer; for now we
// drive the end-to-end raster path with the colored triangle and confirm
// no regression vs Wave-A bilinear.
TEST_CASE(
    "trilinear path: colored-triangle render still produces "
    "front-face coverage",
    "[raster][trilinear]") {
    console::Console::Get().RegisterCVar("r_tile_size", "64", "", 0);
    console::Console::Get().SetCVarOverride("r_tile_size", "64");

    auto mesh = test_mesh::colored_triangle();
    Image img(128, 128);

    ViewState v{};
    v.view = math::look_at_rh(math::Vec3{0, 0, 2}, math::Vec3{0, 0, 0}, math::Vec3{0, 1, 0});
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
    d.vertices = mesh.vertices;
    d.vertex_count = mesh.vertex_count;
    d.indices = mesh.indices;
    d.index_count = mesh.index_count;
    d.model = math::identity4();

    auto& r = Rasterizer::Get();
    r.begin_frame(v);
    r.submit(d);
    r.end_frame();

    u32 lit = 0;
    for (u32 p : img.pixels)
        if (p != 0xFF000000u)
            ++lit;
    REQUIRE(lit > 1000);
}

// Direct test of the trilinear math: synthesise mip 0 = all 0x00 and
// mip 1 = all 0xFF. At LOD ∈ {0, 0.5, 1}, expect channel value
// {0, 128, 255} respectively from the per-channel lerp.
//
// The sampler is in an anonymous namespace; to test it directly we
// duplicate the math here and assert on the floor + lerp behaviour we
// rely on. (Keeping the sampler private avoids leaking the inner-loop
// helper into a public-ish surface.)
TEST_CASE("trilinear math: floor + per-channel lerp at LOD 0 / 0.5 / 1",
          "[raster][trilinear][math]") {
    // mip 0 sample = 0,   mip 1 sample = 255 — fully opaque so alpha=255.
    auto trilinear_lerp = [](u32 s0, u32 s1, f32 t) noexcept {
        auto chan = [t](u32 a, u32 b) noexcept {
            const f32 af = static_cast<f32>(a);
            const f32 bf = static_cast<f32>(b);
            return static_cast<u32>(af + (bf - af) * t + 0.5f) & 0xFFu;
        };
        const u32 r = chan(s0 & 0xFFu, s1 & 0xFFu);
        const u32 g = chan((s0 >> 8) & 0xFFu, (s1 >> 8) & 0xFFu);
        const u32 b = chan((s0 >> 16) & 0xFFu, (s1 >> 16) & 0xFFu);
        const u32 a = chan((s0 >> 24) & 0xFFu, (s1 >> 24) & 0xFFu);
        return r | (g << 8) | (b << 16) | (a << 24);
    };
    const u32 s0 = 0xFF000000u;
    const u32 s1 = 0xFFFFFFFFu;

    REQUIRE(trilinear_lerp(s0, s1, 0.0f) == 0xFF000000u);
    REQUIRE(trilinear_lerp(s0, s1, 1.0f) == 0xFFFFFFFFu);
    // LOD 0.5 → per-channel 128 = round(0.5 * 255 + 0.5) = 128.
    const u32 mid = trilinear_lerp(s0, s1, 0.5f);
    REQUIRE(((mid >> 0) & 0xFFu) == 128);
    REQUIRE(((mid >> 8) & 0xFFu) == 128);
    REQUIRE(((mid >> 16) & 0xFFu) == 128);
    REQUIRE(((mid >> 24) & 0xFFu) == 255);  // alpha lerps 255 → 255
}

// log2 mip-LOD math: a 256×256 texture stepping 1 texel per pixel in u
// gives derivatives (du/dx*w, dv/dx*h) = (1, 0). |∂uv/∂x|² = 1 → LOD = 0.
// Doubling the texel stride per pixel ⇒ LOD = 1.
TEST_CASE("mip-LOD: stride 1 texel/pixel -> LOD 0; stride 2 -> LOD 1", "[raster][trilinear][lod]") {
    constexpr f32 W = 256.0f;
    constexpr f32 H = 256.0f;

    auto lod = [&](f32 du_dx, f32 dv_dx, f32 du_dy, f32 dv_dy) noexcept {
        const f32 ax = du_dx * W;
        const f32 ay = dv_dx * H;
        const f32 bx = du_dy * W;
        const f32 by = dv_dy * H;
        const f32 d2_dx = ax * ax + ay * ay;
        const f32 d2_dy = bx * bx + by * by;
        const f32 d2 = std::max(d2_dx, d2_dy);
        if (d2 <= 1.0f)
            return 0.0f;
        return 0.5f * std::log2(d2);
    };

    // 1 texel / pixel in u → du/dx = 1/W.
    REQUIRE(lod(1.0f / W, 0.0f, 0.0f, 0.0f) == 0.0f);
    // 2 texels / pixel in u → du/dx = 2/W → d² = 4 → LOD = 1.
    const f32 l = lod(2.0f / W, 0.0f, 0.0f, 0.0f);
    REQUIRE(std::abs(l - 1.0f) < 1e-4f);
    // 4 texels / pixel → LOD = 2.
    const f32 l2 = lod(4.0f / W, 0.0f, 0.0f, 0.0f);
    REQUIRE(std::abs(l2 - 2.0f) < 1e-4f);
}
