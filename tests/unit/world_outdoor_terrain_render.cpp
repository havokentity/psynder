// SPDX-License-Identifier: MIT
// Psynder — Lane 11 Wave-E unit tests: `TerrainRaymarch::render` paints
// through the public API into a caller-bound framebuffer.
//
// Wave A's `TerrainRaymarch::render` was a no-op stub; sample_06 (M6) had
// to dip into `world::outdoor::detail::*` directly to paint the
// framebuffer. Wave E wires the public API to actually render. These
// tests pin:
//
//   * `render()` paints non-default pixels into the bound framebuffer
//     when given a known heightmap and a sane view/proj.
//   * `render()` writes a sub-far depth value into the depth slot for at
//     least one pixel that hit terrain — required so subsequent raster
//     submits Z-test against the terrain silhouette.
//   * `render()` is a no-op (no crash, no writes) when no target has been
//     bound. This is the contract that lets callers `set_target(nullptr)`
//     to disable terrain raymarching mid-frame without an extra guard.
//   * `set_target(rm, nullptr)` after a non-null bind reverts to the
//     no-op behavior.

#include <catch2/catch_test_macros.hpp>

#include "math/Math.h"
#include "render/Framebuffer.h"
#include "world/outdoor/Terrain.h"
#include "world/outdoor/TerrainTarget.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace pwo = psynder::world::outdoor;

namespace {

// A flat heightmap at a known world-Y. Drives a deterministic test: any
// hit position is on a horizontal plane and we know its exact depth.
std::vector<std::uint16_t> flat_heightmap(std::uint32_t w, std::uint32_t h, std::uint16_t level) {
    return std::vector<std::uint16_t>(static_cast<std::size_t>(w) * h, level);
}

// Build a small framebuffer with RGBA8 + 24-bit-float-packed depth, all
// zero-initialized so any non-zero pixel proves the renderer wrote it.
struct TestFb {
    std::vector<std::uint32_t> pixels;
    std::vector<std::uint32_t> depth;
    psynder::render::Framebuffer fb{};
    TestFb(std::uint32_t w, std::uint32_t h)
        : pixels(static_cast<std::size_t>(w) * h, 0u)
        , depth(static_cast<std::size_t>(w) * h, 0xFFFFFFFFu) {
        fb.width = w;
        fb.height = h;
        fb.pitch = w * 4u;
        fb.format = psynder::render::PixelFormat::RGBA8;
        fb.pixels = reinterpret_cast<std::uint8_t*>(pixels.data());
        fb.depth = depth.data();
    }
};

// Look straight down at the centre of a square map from `eye_y` metres
// above the ground. Matches sample_06's general framing.
psynder::math::Mat4 downward_view(psynder::math::Vec3 eye) {
    return psynder::math::look_at_rh(eye,
                                     psynder::math::Vec3{eye.x, 0.0f, eye.z + 0.001f},  // tiny +Z bias
                                     psynder::math::Vec3{0.0f, 0.0f, -1.0f});  // "up" pointing -Z
}

}  // namespace

TEST_CASE("TerrainRaymarch::render paints pixels into the bound framebuffer",
          "[world_outdoor][terrain_render][wave_e]") {
    // 64×64m flat terrain at y = 10m.
    constexpr std::uint32_t kHm = 64;
    auto raw = flat_heightmap(kHm, kHm, 100);  // 100 * 0.1 = 10m

    pwo::HeightmapDesc desc{};
    desc.size_x = kHm;
    desc.size_z = kHm;
    desc.spacing = 1.0f;
    desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwo::TerrainRaymarch rm;
    rm.set_heightmap(desc);

    constexpr std::uint32_t kFbW = 32;
    constexpr std::uint32_t kFbH = 24;
    TestFb tfb(kFbW, kFbH);

    pwo::set_target(rm, &tfb.fb);

    // Eye well above the plane, looking down at it.
    const psynder::math::Vec3 eye{
        static_cast<float>(kHm) * 0.5f,
        25.0f,  // 15m above the ground
        static_cast<float>(kHm) * 0.5f,
    };
    const psynder::math::Mat4 view = downward_view(eye);
    const psynder::math::Mat4 proj =
        psynder::math::perspective_rh(60.0f * psynder::math::kDegToRad,
                                      static_cast<float>(kFbW) / static_cast<float>(kFbH),
                                      1.0f,
                                      200.0f);

    rm.render(view, proj);

    // Count painted pixels. With the camera centred above a flat plane,
    // every ray hits — but we keep a healthy floor so tiny camera-derive
    // quirks don't cause flake. (Empirically the centre 80%+ of the FB
    // hits; we require ≥ 25% to be conservative.)
    std::size_t painted = 0;
    for (auto px : tfb.pixels)
        if (px != 0u)
            ++painted;
    REQUIRE(painted >= (kFbW * kFbH) / 4u);

    // At least one painted pixel must have a depth value strictly below
    // far-Z (we initialized depth to 0xFFFFFFFF, which is greater than the
    // pack_depth_u24 output for any z < 1).
    bool any_sub_far = false;
    for (std::size_t i = 0; i < tfb.depth.size(); ++i) {
        if (tfb.pixels[i] != 0u && tfb.depth[i] < 0xFFFFFFFFu) {
            any_sub_far = true;
            break;
        }
    }
    REQUIRE(any_sub_far);
}

TEST_CASE("TerrainRaymarch::render is a no-op when no target is bound",
          "[world_outdoor][terrain_render][wave_e]") {
    constexpr std::uint32_t kHm = 16;
    auto raw = flat_heightmap(kHm, kHm, 200);

    pwo::HeightmapDesc desc{};
    desc.size_x = kHm;
    desc.size_z = kHm;
    desc.spacing = 1.0f;
    desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwo::TerrainRaymarch rm;
    rm.set_heightmap(desc);
    // Intentionally do NOT call set_target — render() should silently
    // skip without dereferencing the null target.

    const psynder::math::Mat4 view = downward_view(psynder::math::Vec3{8.0f, 30.0f, 8.0f});
    const psynder::math::Mat4 proj =
        psynder::math::perspective_rh(60.0f * psynder::math::kDegToRad, 4.0f / 3.0f, 1.0f, 200.0f);

    // Must not crash. The test framework's address sanitizers (when
    // enabled) will flag any null-deref.
    REQUIRE_NOTHROW(rm.render(view, proj));
}

TEST_CASE("TerrainRaymarch::render reverts to no-op when target is cleared",
          "[world_outdoor][terrain_render][wave_e]") {
    constexpr std::uint32_t kHm = 32;
    auto raw = flat_heightmap(kHm, kHm, 150);

    pwo::HeightmapDesc desc{};
    desc.size_x = kHm;
    desc.size_z = kHm;
    desc.spacing = 1.0f;
    desc.height_scale = 0.1f;
    desc.heights = raw.data();

    pwo::TerrainRaymarch rm;
    rm.set_heightmap(desc);

    constexpr std::uint32_t kFbW = 16;
    constexpr std::uint32_t kFbH = 12;
    TestFb tfb(kFbW, kFbH);

    // Bind, render — pixels paint. Then clear, render again — no further
    // pixels paint (because clearing the target re-disables the path).
    pwo::set_target(rm, &tfb.fb);

    const psynder::math::Vec3 eye{static_cast<float>(kHm) * 0.5f, 30.0f, static_cast<float>(kHm) * 0.5f};
    const psynder::math::Mat4 view = downward_view(eye);
    const psynder::math::Mat4 proj =
        psynder::math::perspective_rh(60.0f * psynder::math::kDegToRad,
                                      static_cast<float>(kFbW) / static_cast<float>(kFbH),
                                      1.0f,
                                      200.0f);

    rm.render(view, proj);

    // Snapshot the framebuffer state, clear the target, and render again.
    // The snapshot must be unchanged after the second render.
    const auto snap_pixels = tfb.pixels;
    const auto snap_depth = tfb.depth;

    pwo::set_target(rm, nullptr);

    // Smash the framebuffer to a sentinel pattern so we can prove
    // `render()` did NOT write to it (vs. writing the same values).
    std::memset(tfb.pixels.data(), 0xAB, tfb.pixels.size() * sizeof(std::uint32_t));
    std::memset(tfb.depth.data(), 0xCD, tfb.depth.size() * sizeof(std::uint32_t));

    rm.render(view, proj);

    for (std::size_t i = 0; i < tfb.pixels.size(); ++i) {
        REQUIRE(tfb.pixels[i] == 0xABABABABu);
        REQUIRE(tfb.depth[i] == 0xCDCDCDCDu);
    }

    // Silence unused-locals when the snapshots aren't asserted directly:
    // the smash-and-check above proves the same invariant more strongly.
    (void)snap_pixels;
    (void)snap_depth;
}
