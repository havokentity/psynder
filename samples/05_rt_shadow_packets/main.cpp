// SPDX-License-Identifier: MIT
// Psynder — Sample 05 / M5 demo. RT shadow packets with colored lights.
//
// The scene: a dark ground plane and five colored cubes scattered around it,
// lit by three orbiting point lights (red / green / blue). The sample hands
// its TLAS, camera, lights, and instance colors to the hybrid scene renderer;
// the engine owns primary visibility, 8-wide shadow packets, lighting, and
// upsample. The skybox is a vertical gradient.
//
// To keep the smoke run fast (CI budgets <8 min), the lit pass runs at
// quarter resolution and is bilinear-upsampled into the final framebuffer.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/SceneRenderer.h"
#include "render/rt/Bvh.h"
#include "render/rt/FrameRenderer.h"
#include "ui/console/ConsoleOverlay.h"
#include "ui/imm/DebugHud.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

// ─── Render config ───────────────────────────────────────────────────────
// Internal framebuffer (small — raytracing per pixel is expensive). The
// shadow pass runs at a quarter of these dimensions and is bilinear-
// upsampled into the final image.
constexpr u32 kFbW = 512;
constexpr u32 kFbH = 288;
constexpr u32 kShadowW = kFbW / 2;  // 256
constexpr u32 kShadowH = kFbH / 2;  // 144
constexpr u32 kNumLights = 3;
constexpr u32 kNumCubes = 5;

// Pack RGBA8 little-endian.
PSY_FORCEINLINE u32 pack_rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

// ─── Scene geometry ──────────────────────────────────────────────────────
// Generate a unit cube's 12 triangles centered at the origin with half-
// extent 0.5 (so it spans [-0.5, +0.5] on each axis).
void emit_unit_cube(std::vector<render::rt::Triangle>& out) {
    using math::Vec3;
    // 8 corners of the unit cube.
    const Vec3 c000{-0.5f, -0.5f, -0.5f}, c100{+0.5f, -0.5f, -0.5f};
    const Vec3 c010{-0.5f, +0.5f, -0.5f}, c110{+0.5f, +0.5f, -0.5f};
    const Vec3 c001{-0.5f, -0.5f, +0.5f}, c101{+0.5f, -0.5f, +0.5f};
    const Vec3 c011{-0.5f, +0.5f, +0.5f}, c111{+0.5f, +0.5f, +0.5f};
    // -Z face (back)
    out.push_back({c000, c110, c100});
    out.push_back({c000, c010, c110});
    // +Z face (front)
    out.push_back({c001, c101, c111});
    out.push_back({c001, c111, c011});
    // -X face (left)
    out.push_back({c000, c001, c011});
    out.push_back({c000, c011, c010});
    // +X face (right)
    out.push_back({c100, c111, c101});
    out.push_back({c100, c110, c111});
    // -Y face (bottom)
    out.push_back({c000, c101, c001});
    out.push_back({c000, c100, c101});
    // +Y face (top)
    out.push_back({c010, c011, c111});
    out.push_back({c010, c111, c110});
}

// Two big triangles that form a ground quad on the XZ plane at y = 0.
void emit_ground(std::vector<render::rt::Triangle>& out, f32 half) {
    using math::Vec3;
    const Vec3 p00{-half, 0.0f, -half}, p10{+half, 0.0f, -half};
    const Vec3 p01{-half, 0.0f, +half}, p11{+half, 0.0f, +half};
    out.push_back({p00, p11, p10});
    out.push_back({p00, p01, p11});
}

struct CubeInstance {
    math::Vec3 center;
    f32 size;   // edge length
    u32 color;  // RGBA8
};

// Five cubes laid out in a loose ring around the origin.
std::array<CubeInstance, kNumCubes> make_cube_instances() {
    std::array<CubeInstance, kNumCubes> cubes{};
    cubes[0] = {{0.0f, 0.5f, 0.0f}, 1.0f, pack_rgba8(220, 90, 90)};
    cubes[1] = {{2.2f, 0.6f, 0.8f}, 1.2f, pack_rgba8(90, 220, 90)};
    cubes[2] = {{-2.0f, 0.5f, -1.2f}, 1.0f, pack_rgba8(90, 90, 220)};
    cubes[3] = {{1.4f, 0.4f, -2.4f}, 0.8f, pack_rgba8(220, 200, 80)};
    cubes[4] = {{-1.6f, 0.55f, 2.0f}, 1.1f, pack_rgba8(220, 80, 220)};
    return cubes;
}

// ─── Mat4 helpers (just the ops we need; engine API is frozen). ──────────
math::Mat4 mat4_trs(math::Vec3 t, f32 scale) {
    math::Mat4 m{};
    m.m[0] = scale;
    m.m[1] = 0;
    m.m[2] = 0;
    m.m[3] = 0;
    m.m[4] = 0;
    m.m[5] = scale;
    m.m[6] = 0;
    m.m[7] = 0;
    m.m[8] = 0;
    m.m[9] = 0;
    m.m[10] = scale;
    m.m[11] = 0;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    m.m[15] = 1;
    return m;
}

// ─── Camera ──────────────────────────────────────────────────────────────
using Camera = render::rt::FrameCamera;

Camera make_orbit_camera(f32 t_seconds, f32 aspect) {
    const f32 radius = 7.5f;
    const f32 height = 3.5f;
    const f32 angle = t_seconds * 0.18f;
    const math::Vec3 eye{std::cos(angle) * radius, height, std::sin(angle) * radius};
    const math::Vec3 target{0.0f, 0.4f, 0.0f};
    return render::rt::make_frame_camera(eye, math::sub(target, eye), aspect, 45.0f * math::kDegToRad);
}

// ─── Lights ──────────────────────────────────────────────────────────────
using Light = render::rt::FrameLight;

void orbit_lights(f32 t_seconds, std::array<Light, kNumLights>& lights) {
    const f32 base_y = 2.6f;
    // Three lights, each with its own orbit radius / speed / phase.
    const f32 radii[3] = {4.0f, 5.0f, 3.2f};
    const f32 speeds[3] = {0.55f, -0.35f, 0.80f};
    const f32 phases[3] = {0.0f, 2.094f, 4.188f};  // 0, 120°, 240°
    const f32 heights[3] = {base_y, base_y + 0.6f, base_y - 0.3f};
    const f32 cols[3][3] = {
        {1.0f, 0.30f, 0.25f},  // red
        {0.25f, 1.0f, 0.35f},  // green
        {0.30f, 0.45f, 1.0f},  // blue
    };
    for (u32 i = 0; i < kNumLights; ++i) {
        const f32 a = t_seconds * speeds[i] + phases[i];
        lights[i].position = {std::cos(a) * radii[i], heights[i], std::sin(a) * radii[i]};
        lights[i].r = cols[i][0];
        lights[i].g = cols[i][1];
        lights[i].b = cols[i][2];
        lights[i].intensity = 9.0f;
        lights[i].range = 14.0f;
    }
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 05 (RT shadow packets)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;
    return desc;
}

int sample_main(const app::AppArgs& base_args, app::WindowApp& app_host) {
    const app::AppArgs& args = base_args;
    const u32 smoke_frames = args.smoke_frames;
    render::rt::ensure_frame_renderer_console_registered();
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();

    // -- Build static scene geometry. ------------------------------------
    std::array<CubeInstance, kNumCubes> cube_instances = make_cube_instances();

    std::vector<render::rt::Triangle> cube_tris;
    emit_unit_cube(cube_tris);

    std::vector<render::rt::Triangle> ground_tris;
    emit_ground(ground_tris, /*half=*/8.0f);

    render::rt::Bvh8 cube_blas;
    cube_blas.build(cube_tris.data(), static_cast<u32>(cube_tris.size()));
    render::rt::Bvh8 ground_blas;
    ground_blas.build(ground_tris.data(), static_cast<u32>(ground_tris.size()));

    std::array<render::rt::Tlas::InstanceDesc, kNumCubes + 1> insts{};
    std::array<u32, kNumCubes + 1> instance_colors{};
    for (u32 i = 0; i < kNumCubes; ++i) {
        insts[i].blas = &cube_blas;
        insts[i].transform = mat4_trs(cube_instances[i].center, cube_instances[i].size);
        instance_colors[i] = cube_instances[i].color;
    }
    insts[kNumCubes].blas = &ground_blas;
    insts[kNumCubes].transform = math::identity4();
    instance_colors[kNumCubes] = pack_rgba8(60, 60, 70);

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    std::vector<u32>& final_pixels = app_host.pixels();
    render::Framebuffer& fb = app_host.framebuffer();
    render::SceneRenderer renderer;
    ui::imm::DebugHudFrameHistory hud_history{};

    PSY_LOG_INFO("Psynder sample 05 running{}",
                 smoke_frames > 0 ? fmt::format(" -- smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;
    u64 prev_frame_ticks = t0;
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;
    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);
    std::array<Light, kNumLights> lights{};

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        hud_history.push(frame_ms);

        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
            break;
        }

        const editor::Mode edit_mode =
            platform::input() ? editor::sample_step(*platform::input(), fb, frame_ms * 0.001f)
                              : editor::Mode::Play;
        const f64 t = (edit_mode == editor::Mode::Edit) ? 0.0
                      : smoke_frames > 0
                          ? static_cast<f64>(frame) * (1.0 / 60.0)
                          : platform::Clock::seconds(platform::Clock::ticks_now() - t0);

        orbit_lights(static_cast<f32>(t), lights);
        const Camera cam = make_orbit_camera(static_cast<f32>(t), aspect);

        render::rt::FrameRenderConfig rt_config =
            render::rt::frame_render_config_from_console(kFbW, kFbH, kShadowW, kShadowH, 16u);
        rt_config.attenuation_quadratic = 0.08f;
        render::rt::FrameRenderInput rt_input{};
        rt_input.tlas = &tlas;
        rt_input.camera = cam;
        rt_input.lights = lights.data();
        rt_input.light_count = static_cast<u32>(lights.size());
        rt_input.materials.instance_rgba8 = instance_colors.data();
        rt_input.materials.instance_count = static_cast<u32>(instance_colors.size());
        rt_input.materials.default_rgba8 = pack_rgba8(60, 60, 70);
        renderer.render_rt(rt_input, rt_config, final_pixels.data());

        ui::imm::draw_debug_hud(fb, hud_history.make_stats(frame_ms, 1, 0, 0));
        ui::console::draw(fb);
        window->present(fb);

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_05: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_05");
    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
struct RtShadowPacketsSample {
    static constexpr std::string_view log_name() noexcept { return "sample_05"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 05"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return sample_main(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(RtShadowPacketsSample)
