// SPDX-License-Identifier: MIT
// Psynder — shared RT frame helper tests.

#include "render/rt/FrameRenderer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <array>
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
    REQUIRE(config.reflection_bounces == 8u);

    overrides.reflection_bounces = "0";
    render::rt::apply_frame_renderer_console_overrides(overrides);
    config = render::rt::frame_render_config_from_console(16u, 16u, 16u, 16u, 8u);
    REQUIRE(config.reflection_bounces == 0u);

    overrides.reflection_bounces = "1";
    render::rt::apply_frame_renderer_console_overrides(overrides);
}

TEST_CASE("render rt frame renderer: material library drives TLAS instance color",
          "[render_rt][frame_helpers]") {
    render::MaterialLibrary materials;
    render::MaterialDesc desc{};
    desc.albedo_rgba8 = 0xFF2040C0u;
    desc.reflectivity = 0.0f;
    const render::MaterialId material = materials.create(desc);

    render::rt::Triangle tri{
        math::Vec3{-1.0f, -1.0f, 5.0f},
        math::Vec3{1.0f, -1.0f, 5.0f},
        math::Vec3{0.0f, 1.0f, 5.0f},
    };
    render::rt::Bvh8 blas;
    blas.build(&tri, 1u);
    render::rt::Tlas::InstanceDesc instance{&blas, math::identity4()};
    render::rt::Tlas tlas;
    tlas.build(&instance, 1u);

    render::rt::FrameRenderInput input{};
    input.tlas = &tlas;
    input.camera = render::rt::make_frame_camera({0.0f, 0.0f, 0.0f},
                                                 {0.0f, 0.0f, 1.0f},
                                                 1.0f,
                                                 30.0f * math::kDegToRad);
    input.materials.library = &materials;
    input.materials.instance_materials = &material;
    input.materials.instance_material_count = 1u;

    render::rt::FrameRenderConfig config{};
    config.output_width = 8u;
    config.output_height = 8u;
    config.trace_width = 8u;
    config.trace_height = 8u;
    config.parallel = false;
    config.ambient_scale = 255.0f;
    config.direct_scale = 0.0f;
    config.reflection_bounces = 0u;

    std::array<u32, 8u * 8u> pixels{};
    render::rt::FrameRenderer renderer;
    renderer.render(input, config, pixels.data());

    const u32 center = pixels[4u * 8u + 4u];
    REQUIRE((center & 0xFFu) == 0xC0u);
    REQUIRE(((center >> 8) & 0xFFu) == 0x40u);
    REQUIRE(((center >> 16) & 0xFFu) == 0x20u);
}

TEST_CASE("render rt frame renderer: material cast flag controls shadow packets",
          "[render_rt][frame_helpers]") {
    render::rt::Triangle receiver_tri{
        math::Vec3{-1.5f, -1.0f, 5.0f},
        math::Vec3{1.5f, -1.0f, 5.0f},
        math::Vec3{0.0f, 1.5f, 5.0f},
    };
    render::rt::Triangle blocker_tri{
        math::Vec3{-0.4f, 0.4f, 3.0f},
        math::Vec3{0.4f, 0.4f, 3.0f},
        math::Vec3{0.0f, 1.2f, 3.0f},
    };
    render::rt::Bvh8 receiver_blas;
    receiver_blas.build(&receiver_tri, 1u);
    render::rt::Bvh8 blocker_blas;
    blocker_blas.build(&blocker_tri, 1u);

    std::array<render::rt::Tlas::InstanceDesc, 2> instances{{
        {&receiver_blas, math::identity4()},
        {&blocker_blas, math::identity4()},
    }};
    render::rt::Tlas tlas;
    tlas.build(instances.data(), static_cast<u32>(instances.size()));

    auto render_center = [&](u32 blocker_flags) {
        render::MaterialLibrary materials;
        render::MaterialDesc receiver_desc{};
        receiver_desc.albedo_rgba8 = 0xFFFFFFFFu;
        receiver_desc.flags = render::Material_RtVisible | render::Material_ReceivesRtShadow;
        const render::MaterialId receiver = materials.create(receiver_desc);

        render::MaterialDesc blocker_desc{};
        blocker_desc.albedo_rgba8 = 0xFFFFFFFFu;
        blocker_desc.flags = blocker_flags;
        const render::MaterialId blocker = materials.create(blocker_desc);
        std::array<render::MaterialId, 2> ids{{receiver, blocker}};

        render::rt::FrameLight light{};
        light.position = {0.0f, 2.0f, 0.0f};
        light.r = 1.0f;
        light.g = 1.0f;
        light.b = 1.0f;
        light.intensity = 4.0f;
        light.range = 10.0f;

        render::rt::FrameRenderInput input{};
        input.tlas = &tlas;
        input.camera = render::rt::make_frame_camera({0.0f, 0.0f, 0.0f},
                                                     {0.0f, 0.0f, 1.0f},
                                                     1.0f,
                                                     30.0f * math::kDegToRad);
        input.lights = &light;
        input.light_count = 1u;
        input.materials.library = &materials;
        input.materials.instance_materials = ids.data();
        input.materials.instance_material_count = static_cast<u32>(ids.size());

        render::rt::FrameRenderConfig config{};
        config.output_width = 8u;
        config.output_height = 8u;
        config.trace_width = 8u;
        config.trace_height = 8u;
        config.parallel = false;
        config.ambient_scale = 0.0f;
        config.direct_scale = 80.0f;
        config.reflection_bounces = 0u;

        std::array<u32, 8u * 8u> pixels{};
        render::rt::FrameRenderer renderer;
        renderer.render(input, config, pixels.data());
        return pixels[4u * 8u + 4u] & 0xFFu;
    };

    const u32 shadowed =
        render_center(render::Material_RtVisible | render::Material_CastsRtShadow);
    const u32 unshadowed = render_center(render::Material_RtVisible);
    REQUIRE(shadowed < unshadowed);
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
