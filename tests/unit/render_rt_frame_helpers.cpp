// SPDX-License-Identifier: MIT
// Psynder — shared RT frame helper tests.

#include "render/rt/FrameRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace psynder;

namespace {

bool approx_eq(f32 a, f32 b, f32 tol = 1e-5f) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

TEST_CASE("render rt frame helpers: center primary ray follows camera forward",
          "[render_rt][frame_helpers]") {
    const render::rt::FrameCamera cam = render::rt::make_frame_camera({0.0f, 0.0f, 0.0f},
                                                                      {0.0f, 0.0f, 1.0f},
                                                                      16.0f / 9.0f,
                                                                      60.0f * math::kDegToRad);
    const render::rt::Ray ray = render::rt::primary_ray(cam, 0.5f, 0.5f);

    REQUIRE(approx_eq(ray.direction.x, 0.0f));
    REQUIRE(approx_eq(ray.direction.y, 0.0f));
    REQUIRE(approx_eq(ray.direction.z, 1.0f));
}

TEST_CASE("render rt frame helpers: camera handles vertical forward vector",
          "[render_rt][frame_helpers]") {
    const render::rt::FrameCamera cam = render::rt::make_frame_camera({0.0f, 0.0f, 0.0f},
                                                                      {0.0f, 1.0f, 0.0f},
                                                                      1.0f,
                                                                      60.0f * math::kDegToRad);

    REQUIRE(approx_eq(math::dot(cam.right, cam.right), 1.0f));
    REQUIRE(approx_eq(math::dot(cam.up, cam.forward), 0.0f));
}

TEST_CASE("render rt frame helpers: reflect mirrors incident direction around normal",
          "[render_rt][frame_helpers]") {
    const math::Vec3 reflected = render::rt::reflect({1.0f, -1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});

    REQUIRE(approx_eq(reflected.x, 1.0f));
    REQUIRE(approx_eq(reflected.y, 1.0f));
    REQUIRE(approx_eq(reflected.z, 0.0f));
}

TEST_CASE("render rt frame renderer: reflection bounce cvar clamps to implemented paths",
          "[render_rt][frame_helpers]") {
    render::rt::FrameRendererConsoleOverrides overrides{};
    overrides.reflection_bounces = "9";
    render::rt::apply_frame_renderer_console_overrides(overrides);

    render::rt::FrameRenderConfig config =
        render::rt::frame_render_config_from_console(16u, 16u, 16u, 16u, 8u);
    REQUIRE(config.reflection_bounces == 1u);

    overrides.reflection_bounces = "0";
    render::rt::apply_frame_renderer_console_overrides(overrides);
    config = render::rt::frame_render_config_from_console(16u, 16u, 16u, 16u, 8u);
    REQUIRE(config.reflection_bounces == 0u);

    overrides.reflection_bounces = "1";
    render::rt::apply_frame_renderer_console_overrides(overrides);
}

TEST_CASE("render rt frame scheduler: explicit row batch clamps to work",
          "[render_rt][frame_helpers]") {
    render::rt::FrameRowScheduleConfig config{};
    config.batch_rows = 64;

    REQUIRE(render::rt::frame_row_schedule_grain(17, config) == 17);
}

TEST_CASE("render rt frame scheduler: serial row dispatch covers requested range once",
          "[render_rt][frame_helpers]") {
    render::rt::FrameRowScheduleConfig config{};
    config.parallel = false;

    std::vector<u32> rows;
    const render::rt::FrameRowScheduleStats stats =
        render::rt::parallel_frame_rows(5, config, [&](u32 y0, u32 y1) {
            rows.push_back(y0);
            rows.push_back(y1);
        });

    REQUIRE(stats.rows == 5);
    REQUIRE(stats.scheduled_jobs == 1);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0] == 0);
    REQUIRE(rows[1] == 5);
}
