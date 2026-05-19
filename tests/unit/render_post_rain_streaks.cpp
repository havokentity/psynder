// SPDX-License-Identifier: MIT
// Lane 09 — rain streak system.
//
// We verify:
//
//   1. seed() spawns the requested count, never more than kMaxStreaks.
//   2. step() applies gravity-driven motion: streak.y decreases when base
//      velocity has a negative y.
//   3. Streaks that fall below the kill plane are recycled to the spawn
//      column on the next step.
//   4. render() is a no-op when params.enabled = false.
//   5. render() blends streak pixels into the HDR buffer such that the
//      affected region's average luminance changes (we just want to know
//      *something* got drawn).
//
// Rendering is screen-space; we set up a tiny FogScene that points at the
// rain column so the streaks project into the framebuffer.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "render/post/Internal.h"
#include "render/post/RainStreaks.h"
#include "render/post/VolumetricFog.h"

#include <vector>

using namespace psynder;
using namespace psynder::render;
using namespace psynder::render::post;
using namespace psynder::render::post::detail;

namespace {

RainParams default_params() {
    RainParams p;
    p.enabled = true;
    p.spawn_centre   = math::Vec3{0, 5, 0};
    p.spawn_extent   = math::Vec3{4, 0, 4};
    p.kill_height_y  = -2.0f;
    p.base_velocity  = math::Vec3{0, -8.0f, 0};
    p.jitter_velocity = math::Vec3{0.5f, 1.0f, 0.5f};
    p.streak_length  = 0.4f;
    p.streak_alpha   = 0.5f;
    p.intensity      = 1.0f;
    p.rng_seed       = 12345u;
    return p;
}

FogScene default_camera() {
    FogScene c;
    c.camera_position = math::Vec3{0, 0, -5};
    c.camera_forward  = math::Vec3{0, 0, 1};
    c.camera_right    = math::Vec3{1, 0, 0};
    c.camera_up       = math::Vec3{0, 1, 0};
    c.fov_y_rad       = 1.04f;
    c.aspect          = 1.0f;     // square test fb keeps the math simple
    c.near_z          = 0.1f;
    c.far_z           = 50.0f;
    c.ambient         = math::Vec3{0.02f, 0.02f, 0.02f};
    c.lights          = nullptr;
    c.light_count     = 0;
    return c;
}

}  // namespace

TEST_CASE("render_post: rain seed populates the pool",
          "[render_post][rain]")
{
    StreakSystem sys;
    sys.seed(1024, default_params());
    REQUIRE(sys.active_count() == 1024);
}

TEST_CASE("render_post: rain seed clamps to capacity",
          "[render_post][rain]")
{
    StreakSystem sys;
    sys.seed(StreakSystem::kMaxStreaks + 100u, default_params());
    REQUIRE(sys.active_count() == StreakSystem::kMaxStreaks);
}

TEST_CASE("render_post: rain step advances streaks downward",
          "[render_post][rain]")
{
    StreakSystem sys;
    auto p = default_params();
    sys.seed(64, p);

    // Snapshot y positions.
    std::vector<f32> before(64);
    for (u32 i = 0; i < 64; ++i) before[i] = sys.at(i).position.y;

    sys.step(0.05f, p);   // 50 ms tick

    // After a step every streak should have lower y, unless it just got
    // recycled (in which case y resets to spawn_centre.y). The "majority
    // moved down" assertion is robust to recycling.
    u32 moved_down = 0;
    for (u32 i = 0; i < 64; ++i) {
        if (sys.at(i).position.y < before[i] + 1e-3f) ++moved_down;
    }
    REQUIRE(moved_down >= 60u);
}

TEST_CASE("render_post: rain step recycles streaks below kill plane",
          "[render_post][rain]")
{
    StreakSystem sys;
    auto p = default_params();
    sys.seed(32, p);
    // Big timestep: every streak passes through the kill plane.
    sys.step(10.0f, p);
    // After the recycle every streak should be near the spawn plane again
    // (not below the kill plane).
    for (u32 i = 0; i < 32; ++i) {
        REQUIRE(sys.at(i).position.y > p.kill_height_y);
    }
}

TEST_CASE("render_post: rain render disabled is a no-op",
          "[render_post][rain]")
{
    const u32 W = 8, H = 8;
    std::vector<HdrPixel> fb(static_cast<usize>(W) * H,
                             HdrPixel{0.3f, 0.4f, 0.5f, 1.0f});
    auto reference = fb;
    Framebuffer hdr_fb;
    hdr_fb.width  = W;
    hdr_fb.height = H;
    hdr_fb.pitch  = W * static_cast<u32>(sizeof(HdrPixel));
    hdr_fb.pixels = reinterpret_cast<u8*>(fb.data());

    StreakSystem sys;
    auto p = default_params();
    sys.seed(16, p);
    p.enabled = false;
    sys.render(hdr_fb, default_camera(), p, nullptr);
    for (usize i = 0; i < fb.size(); ++i) {
        REQUIRE(fb[i].r == reference[i].r);
        REQUIRE(fb[i].g == reference[i].g);
        REQUIRE(fb[i].b == reference[i].b);
    }
}

TEST_CASE("render_post: rain render modifies pixels when enabled",
          "[render_post][rain]")
{
    // 64x64 framebuffer covering a square test camera. Spawn 512 streaks
    // directly in front of the camera; after render the average luminance
    // must increase (alpha blend with non-zero streak colour).
    const u32 W = 64, H = 64;
    std::vector<HdrPixel> fb(static_cast<usize>(W) * H,
                             HdrPixel{0.0f, 0.0f, 0.0f, 1.0f});
    Framebuffer hdr_fb;
    hdr_fb.width  = W;
    hdr_fb.height = H;
    hdr_fb.pitch  = W * static_cast<u32>(sizeof(HdrPixel));
    hdr_fb.pixels = reinterpret_cast<u8*>(fb.data());

    auto p = default_params();
    p.spawn_centre  = math::Vec3{0, 3, 5};   // in front of camera at (0,0,-5)
    p.spawn_extent  = math::Vec3{6, 0, 6};
    p.kill_height_y = -10.0f;
    p.streak_alpha  = 0.9f;
    p.intensity     = 3.0f;
    StreakSystem sys;
    sys.seed(512, p);

    sys.render(hdr_fb, default_camera(), p, nullptr);

    // At least one pixel must have been written.
    bool any_modified = false;
    for (const auto& px : fb) {
        if (px.r > 0.0f || px.g > 0.0f || px.b > 0.0f) {
            any_modified = true;
            break;
        }
    }
    REQUIRE(any_modified);
}

TEST_CASE("render_post: rain streak data size is half a cache line",
          "[render_post][rain]")
{
    // Pinned because the spawn pool is sized for memory budget; if the
    // alignment changes we want the test to flag it.
    REQUIRE(sizeof(Streak) == 32);
    REQUIRE(alignof(Streak) == 32);
}
