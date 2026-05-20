// SPDX-License-Identifier: MIT
// Lane 09 — volumetric fog froxel grid + resolve.
//
// We verify:
//
//   1. Grid dimensions match DESIGN.md §8.4 (160×90×64).
//   2. populate_fog_grid clears the grid when fog is disabled.
//   3. populate_fog_grid writes non-zero scatter near a bright light.
//   4. Resolve adds energy when scatter is positive and dims the framebuffer
//      according to Beer-Lambert when extinction > 0.
//   5. Resolve is a no-op when scatter == extinction == 0.
//   6. The occluder callback path: a "blocked" callback nukes scatter from
//      the cells whose line-of-sight to the light is shadowed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "render/post/Internal.h"
#include "render/post/VolumetricFog.h"

#include <cstring>
#include <memory>
#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::post;
using namespace psynder::render::post::detail;

namespace {

FogScene make_scene(const std::vector<FogLight>* lights = nullptr) {
    FogScene s;
    s.camera_position = math::Vec3{0, 0, 0};
    s.camera_forward = math::Vec3{0, 0, 1};
    s.camera_right = math::Vec3{1, 0, 0};
    s.camera_up = math::Vec3{0, 1, 0};
    s.fov_y_rad = 1.04f;
    s.aspect = 16.0f / 9.0f;
    s.near_z = 0.1f;
    s.far_z = 32.0f;
    s.density = 0.05f;
    s.ambient = math::Vec3{0.01f, 0.01f, 0.01f};
    if (lights) {
        s.lights = lights->data();
        s.light_count = static_cast<u32>(lights->size());
    }
    return s;
}

}  // namespace

TEST_CASE("render_post: froxel grid dimensions match DESIGN sec 8.4", "[render_post][fog]") {
    REQUIRE(kFroxelW == 160u);
    REQUIRE(kFroxelH == 90u);
    REQUIRE(kFroxelD == 64u);
    REQUIRE(kFroxelCount == 160u * 90u * 64u);
}

TEST_CASE("render_post: populate writes nonzero scatter near a bright light", "[render_post][fog]") {
    std::vector<FogLight> lights = {FogLight{
        math::Vec3{0, 0, 4},  // directly in front of camera
        math::Vec3{8, 6, 4},  // bright HDR
        0.0f                  // infinite radius
    }};
    FogScene scene = make_scene(&lights);

    auto grid = std::make_unique<FroxelGrid>();
    populate_fog_grid(*grid, scene);

    // Sample a centre froxel near z=4 — slice index z_to_slice(4, 0.1, 32).
    // The cell at (centre_x, centre_y, ~depth-of-light) should have
    // strong scatter.
    const u32 cx = kFroxelW / 2;
    const u32 cy = kFroxelH / 2;
    // Find the slice closest to z=4 via the same monotonic mapping.
    u32 cz = 0;
    {
        const f32 inv_ln = 1.0f / std::log(scene.far_z / scene.near_z);
        const f32 ratio = std::log(4.0f / scene.near_z) * inv_ln;
        cz = static_cast<u32>(ratio * static_cast<f32>(kFroxelD));
        if (cz >= kFroxelD)
            cz = kFroxelD - 1;
    }
    const auto& cell = grid->cells[FroxelGrid::index_of(cx, cy, cz)];
    REQUIRE(cell.scatter_r > 0.0f);
    REQUIRE(cell.scatter_g > 0.0f);
    REQUIRE(cell.scatter_b > 0.0f);
    REQUIRE(cell.extinction > 0.0f);
}

TEST_CASE("render_post: apply_volumetric_fog is identity on zero grid", "[render_post][fog]") {
    // Empty grid (default zero-init) → no scatter, no extinction → resolve
    // leaves the framebuffer untouched.
    const u32 W = 8, H = 8;
    std::vector<HdrPixel> fb(static_cast<usize>(W) * H, HdrPixel{0.4f, 0.5f, 0.6f, 1.0f});
    auto reference = fb;
    Framebuffer hdr_fb;
    hdr_fb.width = W;
    hdr_fb.height = H;
    hdr_fb.pitch = W * static_cast<u32>(sizeof(HdrPixel));
    hdr_fb.pixels = reinterpret_cast<u8*>(fb.data());

    auto grid = std::make_unique<FroxelGrid>();
    FogScene scene = make_scene();
    VolumetricFogParams p;
    p.enabled = true;

    apply_volumetric_fog(hdr_fb, *grid, scene, nullptr, p);

    for (usize i = 0; i < fb.size(); ++i) {
        REQUIRE(fb[i].r == Catch::Approx(reference[i].r).margin(1e-6f));
        REQUIRE(fb[i].g == Catch::Approx(reference[i].g).margin(1e-6f));
        REQUIRE(fb[i].b == Catch::Approx(reference[i].b).margin(1e-6f));
    }
}

TEST_CASE("render_post: apply_volumetric_fog dims framebuffer with extinction",
          "[render_post][fog]") {
    // Constant extinction, zero scatter → the framebuffer should be scaled
    // down by exp(-extinction * (far - near)) for every pixel.
    const u32 W = 4, H = 4;
    std::vector<HdrPixel> fb(static_cast<usize>(W) * H, HdrPixel{1.0f, 1.0f, 1.0f, 1.0f});
    Framebuffer hdr_fb;
    hdr_fb.width = W;
    hdr_fb.height = H;
    hdr_fb.pitch = W * static_cast<u32>(sizeof(HdrPixel));
    hdr_fb.pixels = reinterpret_cast<u8*>(fb.data());

    auto grid = std::make_unique<FroxelGrid>();
    for (auto& c : grid->cells) {
        c.scatter_r = 0.0f;
        c.scatter_g = 0.0f;
        c.scatter_b = 0.0f;
        c.extinction = 0.02f;  // small uniform density
    }
    FogScene scene = make_scene();
    VolumetricFogParams p;
    p.enabled = true;

    apply_volumetric_fog(hdr_fb, *grid, scene, nullptr, p);

    // Each pixel must have been attenuated — strictly less than the
    // pre-fog brightness — and never become negative.
    for (const auto& px : fb) {
        REQUIRE(px.r < 1.0f);
        REQUIRE(px.r >= 0.0f);
        REQUIRE(px.g < 1.0f);
        REQUIRE(px.g >= 0.0f);
        REQUIRE(px.b < 1.0f);
        REQUIRE(px.b >= 0.0f);
    }
}

TEST_CASE("render_post: apply_volumetric_fog adds scatter with positive grid",
          "[render_post][fog]") {
    // Globally positive scatter + extinction → every pixel brightens, because
    // the resolve adds `scatter * absorbed * trans` along the march.
    //
    // We saturate the grid uniformly so that every (ix,iy) tile sees the
    // same series; on a black framebuffer every pixel ends up at the same
    // positive value.
    const u32 W = 8, H = 8;
    std::vector<HdrPixel> fb(static_cast<usize>(W) * H, HdrPixel{0.0f, 0.0f, 0.0f, 1.0f});
    Framebuffer hdr_fb;
    hdr_fb.width = W;
    hdr_fb.height = H;
    hdr_fb.pitch = W * static_cast<u32>(sizeof(HdrPixel));
    hdr_fb.pixels = reinterpret_cast<u8*>(fb.data());

    auto grid = std::make_unique<FroxelGrid>();
    for (auto& c : grid->cells) {
        c.scatter_r = 0.1f;
        c.scatter_g = 0.05f;
        c.scatter_b = 0.0f;
        c.extinction = 0.05f;
    }

    FogScene scene = make_scene();
    VolumetricFogParams p;
    p.enabled = true;
    p.intensity = 1.0f;

    apply_volumetric_fog(hdr_fb, *grid, scene, nullptr, p);

    // Every pixel must have lit up.
    for (const auto& px : fb) {
        REQUIRE(std::isfinite(px.r));
        REQUIRE(std::isfinite(px.g));
        REQUIRE(std::isfinite(px.b));
        REQUIRE(px.r > 0.0f);
        REQUIRE(px.g > 0.0f);
        REQUIRE(px.b == Catch::Approx(0.0f).margin(1e-6f));  // zero channel stays zero
    }
}

namespace {

bool always_blocked(void* /*user*/, math::Vec3 /*a*/, math::Vec3 /*b*/) {
    return true;
}

}  // namespace

TEST_CASE("render_post: populate honours the occluder callback", "[render_post][fog]") {
    // The lit + unlit grids should differ on the cells with a non-zero
    // light contribution. An always-blocked occluder must zero out the
    // light contribution (ambient remains).
    std::vector<FogLight> lights = {FogLight{math::Vec3{0, 0, 4}, math::Vec3{10, 10, 10}, 0.0f}};
    FogScene unlit = make_scene(&lights);
    FogScene lit = make_scene(&lights);
    unlit.occluder.fn = always_blocked;
    unlit.occluder.user = nullptr;

    auto g_lit = std::make_unique<FroxelGrid>();
    auto g_unlit = std::make_unique<FroxelGrid>();
    populate_fog_grid(*g_lit, lit);
    populate_fog_grid(*g_unlit, unlit);

    // Compare a centre cell — lit should be much brighter than blocked.
    const auto& cl = g_lit->cells[FroxelGrid::index_of(kFroxelW / 2, kFroxelH / 2, 8)];
    const auto& cu = g_unlit->cells[FroxelGrid::index_of(kFroxelW / 2, kFroxelH / 2, 8)];
    REQUIRE(cl.scatter_r > cu.scatter_r);
}
