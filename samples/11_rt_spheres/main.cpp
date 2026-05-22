// SPDX-License-Identifier: MIT
// Psynder — Sample 11. Reflective sphere room you fly/walk through.
//
// The scene: an enclosed room (floor + four walls, each a triangle mesh) of
// mirror and saturated-diffuse spheres plus a couple of boxes, all built as
// `render::rt::Bvh8` BLAS instances inside one `render::rt::Tlas`. Two point
// lights orbit the room. The sample hands its TLAS, materials, reflectivity,
// camera, and lights to the hybrid rendering system, which owns the primary
// pass, shadow packets, optional single-bounce reflections, and upsample.
//
// Navigation is the shared `psynder::samples::CharacterController`: FreeCam by
// default so you can fly around the spheres (press V for grounded FPS, `noclip
// 1` at the console to leave the room). Its eye + forward build the primary-
// ray basis; world bounds are pinned to the room interior.
//
// To keep the smoke run fast (CI budget) the trace runs at quarter resolution
// and is bilinear-upsampled into the final framebuffer.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "common/CharacterController.h"
#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/App.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/RenderingSystem.h"
#include "render/rt/Bvh.h"
#include "render/rt/FrameRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace psynder::render::rt {
void ensure_denoise_console_commands_registered();
}

namespace {

// ─── Render config ───────────────────────────────────────────────────────
// Internal framebuffer (small — raytracing per pixel is expensive). The lit
// pass runs at half these dimensions on each axis (quarter the pixels) and is
// bilinear-upsampled into the final image.
constexpr u32 kFbW = 512;
constexpr u32 kFbH = 288;
constexpr u32 kTraceW = kFbW / 2;  // 256
constexpr u32 kTraceH = kFbH / 2;  // 144
constexpr u32 kNumLights = 2;

// Room interior half-extents (metres). The floor sits at y = 0; the ceiling
// is open so the sky shows through reflections that escape upward.
constexpr f32 kRoomHalf = 7.0f;    // ±X / ±Z walls
constexpr f32 kWallHeight = 5.0f;  // wall top

// Pack RGBA8 little-endian.
PSY_FORCEINLINE u32 pack_rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}
PSY_FORCEINLINE u32 clamp_u8(f32 v) noexcept {
    if (v < 0.0f)
        return 0u;
    if (v > 255.0f)
        return 255u;
    return static_cast<u32>(v);
}

// ─── Surface materials ─────────────────────────────────────────────────────
struct Material {
    f32 r, g, b;       // base albedo, 0..1
    f32 reflectivity;  // 0 = pure diffuse, 1 = pure mirror
};

// ─── Scene geometry helpers ────────────────────────────────────────────────
// Unit cube (half-extent 0.5, spans [-0.5,+0.5]) as 12 triangles.
void emit_unit_cube(std::vector<render::rt::Triangle>& out) {
    using math::Vec3;
    const Vec3 c000{-0.5f, -0.5f, -0.5f}, c100{+0.5f, -0.5f, -0.5f};
    const Vec3 c010{-0.5f, +0.5f, -0.5f}, c110{+0.5f, +0.5f, -0.5f};
    const Vec3 c001{-0.5f, -0.5f, +0.5f}, c101{+0.5f, -0.5f, +0.5f};
    const Vec3 c011{-0.5f, +0.5f, +0.5f}, c111{+0.5f, +0.5f, +0.5f};
    out.push_back({c000, c110, c100});
    out.push_back({c000, c010, c110});
    out.push_back({c001, c101, c111});
    out.push_back({c001, c111, c011});
    out.push_back({c000, c001, c011});
    out.push_back({c000, c011, c010});
    out.push_back({c100, c111, c101});
    out.push_back({c100, c110, c111});
    out.push_back({c000, c101, c001});
    out.push_back({c000, c100, c101});
    out.push_back({c010, c011, c111});
    out.push_back({c010, c111, c110});
}

// Unit sphere (radius 1, centred at origin) as a tessellated UV mesh. The RT
// core only takes triangles, so spheres are approximated; the per-pixel hit
// normal comes from the triangle so the silhouette is faceted but the shading
// reads as round at this resolution.
void emit_unit_sphere(std::vector<render::rt::Triangle>& out, u32 stacks, u32 slices) {
    using math::Vec3;
    auto vert = [](u32 i, u32 j, u32 stacks, u32 slices) -> Vec3 {
        const f32 v = static_cast<f32>(i) / static_cast<f32>(stacks);  // 0..1 pole→pole
        const f32 u = static_cast<f32>(j) / static_cast<f32>(slices);  // 0..1 around
        const f32 phi = v * math::kPi;                                 // polar angle 0..π
        const f32 theta = u * math::kTwoPi;                            // azimuth 0..2π
        const f32 sp = std::sin(phi);
        return {sp * std::cos(theta), std::cos(phi), sp * std::sin(theta)};
    };
    for (u32 i = 0; i < stacks; ++i) {
        for (u32 j = 0; j < slices; ++j) {
            const Vec3 a = vert(i, j, stacks, slices);
            const Vec3 b = vert(i + 1, j, stacks, slices);
            const Vec3 c = vert(i + 1, j + 1, stacks, slices);
            const Vec3 d = vert(i, j + 1, stacks, slices);
            // Two triangles per quad (skip degenerate ones at the poles).
            if (i != 0)
                out.push_back({a, b, d});
            if (i + 1 != stacks)
                out.push_back({b, c, d});
        }
    }
}

// One floor quad + four walls on the XZ-bounded room, as triangles. Normals
// face inward (toward the room interior) thanks to winding order.
void emit_room(std::vector<render::rt::Triangle>& out, f32 half, f32 height) {
    using math::Vec3;
    const f32 h = half;
    // Floor (y = 0), upward normal.
    const Vec3 f00{-h, 0.0f, -h}, f10{+h, 0.0f, -h}, f01{-h, 0.0f, +h}, f11{+h, 0.0f, +h};
    out.push_back({f00, f11, f10});
    out.push_back({f00, f01, f11});
    // -Z wall (normal +Z, facing interior).
    {
        const Vec3 a{-h, 0.0f, -h}, b{+h, 0.0f, -h}, c{+h, height, -h}, d{-h, height, -h};
        out.push_back({a, c, b});
        out.push_back({a, d, c});
    }
    // +Z wall (normal -Z).
    {
        const Vec3 a{-h, 0.0f, +h}, b{+h, 0.0f, +h}, c{+h, height, +h}, d{-h, height, +h};
        out.push_back({a, b, c});
        out.push_back({a, c, d});
    }
    // -X wall (normal +X).
    {
        const Vec3 a{-h, 0.0f, -h}, b{-h, 0.0f, +h}, c{-h, height, +h}, d{-h, height, -h};
        out.push_back({a, b, c});
        out.push_back({a, c, d});
    }
    // +X wall (normal -X).
    {
        const Vec3 a{+h, 0.0f, -h}, b{+h, 0.0f, +h}, c{+h, height, +h}, d{+h, height, -h};
        out.push_back({a, c, b});
        out.push_back({a, d, c});
    }
}

// ─── Mat4 helper: uniform translate + scale (engine API is frozen). ────────
math::Mat4 mat4_trs(math::Vec3 t, f32 scale) {
    math::Mat4 m{};
    m.m[0] = scale;
    m.m[5] = scale;
    m.m[10] = scale;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    m.m[15] = 1.0f;
    return m;
}

// ─── Lights ──────────────────────────────────────────────────────────────
using Camera = render::rt::FrameCamera;
using Light = render::rt::FrameLight;

void orbit_lights(f32 t_seconds, std::array<Light, kNumLights>& lights) {
    const f32 base_y = kWallHeight - 1.2f;
    const f32 radii[2] = {4.2f, 3.4f};
    const f32 speeds[2] = {0.45f, -0.6f};
    const f32 phases[2] = {0.0f, math::kPi};
    const f32 cols[2][3] = {
        {1.0f, 0.92f, 0.80f},  // warm
        {0.70f, 0.82f, 1.0f},  // cool
    };
    for (u32 i = 0; i < kNumLights; ++i) {
        const f32 a = t_seconds * speeds[i] + phases[i];
        lights[i].position = {std::cos(a) * radii[i], base_y, std::sin(a) * radii[i]};
        lights[i].r = cols[i][0];
        lights[i].g = cols[i][1];
        lights[i].b = cols[i][2];
        // Tuned so direct lighting lands roughly in 0..1 linear before the
        // final ×255 (ndotl * atten * intensity, atten ≈ 0.5 at mid-room).
        lights[i].intensity = 1.7f;
        lights[i].range = 24.0f;
    }
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 11 (reflective sphere room, CPU RT)";
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
    render::rt::ensure_frame_scheduler_console_registered();
    render::rt::ensure_denoise_console_commands_registered();
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();

    // ── Build the static scene geometry. ────────────────────────────────
    // One Bvh8 each for the shared unit sphere, the shared unit cube, and the
    // room shell. Per-instance placement + scale lives on the TLAS
    // InstanceDesc; the material is keyed off the instance index.
    std::vector<render::rt::Triangle> sphere_tris;
    sphere_tris.reserve(2u * 24u * (14u - 1u));
    emit_unit_sphere(sphere_tris, /*stacks=*/14, /*slices=*/24);
    std::vector<render::rt::Triangle> cube_tris;
    cube_tris.reserve(12);
    emit_unit_cube(cube_tris);
    std::vector<render::rt::Triangle> room_tris;
    room_tris.reserve(10);
    emit_room(room_tris, kRoomHalf, kWallHeight);

    render::rt::Bvh8 sphere_blas;
    sphere_blas.build(sphere_tris.data(), static_cast<u32>(sphere_tris.size()));
    render::rt::Bvh8 cube_blas;
    cube_blas.build(cube_tris.data(), static_cast<u32>(cube_tris.size()));
    render::rt::Bvh8 room_blas;
    room_blas.build(room_tris.data(), static_cast<u32>(room_tris.size()));

    // Scene description: a ring of spheres (mix of mirrors + saturated
    // diffuse), two boxes, and the room. Instance order defines the material
    // table consumed during shading.
    struct InstanceInfo {
        const render::rt::Bvh8* blas;
        math::Vec3 center;
        f32 scale;  // sphere radius / cube edge length
        Material mat;
    };

    std::vector<InstanceInfo> scene;
    scene.reserve(11);
    // 8 spheres in a loose ring + 1 big mirror in the middle.
    scene.push_back(
        {&sphere_blas, {0.0f, 1.3f, 0.0f}, 1.3f, {0.95f, 0.95f, 0.97f, 0.92f}});  // hero mirror
    scene.push_back(
        {&sphere_blas, {3.4f, 0.9f, 0.6f}, 0.9f, {0.90f, 0.20f, 0.18f, 0.05f}});  // red diffuse
    scene.push_back(
        {&sphere_blas, {-3.0f, 0.8f, 1.8f}, 0.8f, {0.18f, 0.75f, 0.30f, 0.05f}});  // green diffuse
    scene.push_back(
        {&sphere_blas, {1.6f, 0.7f, -3.2f}, 0.7f, {0.20f, 0.35f, 0.92f, 0.05f}});  // blue diffuse
    scene.push_back({&sphere_blas, {-2.4f, 1.0f, -2.6f}, 1.0f, {0.85f, 0.86f, 0.90f, 0.85f}});  // mirror
    scene.push_back(
        {&sphere_blas, {-1.4f, 0.6f, 3.6f}, 0.6f, {0.95f, 0.78f, 0.18f, 0.05f}});  // yellow diffuse
    scene.push_back({&sphere_blas, {3.6f, 0.75f, -2.0f}, 0.75f, {0.80f, 0.82f, 0.88f, 0.80f}});  // mirror
    scene.push_back({&sphere_blas, {0.6f, 0.55f, 3.0f}, 0.55f, {0.85f, 0.25f, 0.80f, 0.05f}});  // magenta diffuse
    // Two boxes.
    scene.push_back({&cube_blas, {2.2f, 0.75f, 2.6f}, 1.5f, {0.55f, 0.55f, 0.60f, 0.05f}});  // grey box
    scene.push_back(
        {&cube_blas, {-3.6f, 0.6f, -0.6f}, 1.2f, {0.30f, 0.30f, 0.34f, 0.55f}});  // dark glossy box
    // Room shell (placed last; identity transform).
    const u32 kRoomInstance = static_cast<u32>(scene.size());
    scene.push_back(
        {&room_blas, {0.0f, 0.0f, 0.0f}, 1.0f, {0.42f, 0.42f, 0.46f, 0.04f}});  // matte floor/walls

    std::vector<render::rt::Tlas::InstanceDesc> insts(scene.size());
    render::MaterialLibrary material_library;
    material_library.reserve(static_cast<u32>(scene.size()));
    std::vector<render::MaterialId> instance_materials(scene.size());
    for (usize i = 0; i < scene.size(); ++i) {
        insts[i].blas = scene[i].blas;
        insts[i].transform =
            (i == kRoomInstance) ? math::identity4() : mat4_trs(scene[i].center, scene[i].scale);

        const Material& m = scene[i].mat;
        render::MaterialDesc material{};
        material.albedo_rgba8 =
            pack_rgba8(clamp_u8(m.r * 255.0f), clamp_u8(m.g * 255.0f), clamp_u8(m.b * 255.0f));
        material.reflectivity = m.reflectivity;
        material.flags = render::MaterialFlags::RtVisible | render::MaterialFlags::CastsRtShadow |
                         render::MaterialFlags::ReceivesRtShadow;
        instance_materials[i] = material_library.create(material);
    }

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    // -- CPU framebuffer + shared RT renderer. -----------------------------
    std::vector<u32>& final_pixels = app_host.pixels();
    render::RenderingSystem renderer;

    // -- Shared first-person / free-cam controller (samples/common). ------
    samples::CharacterControllerConfig cc_cfg{};
    cc_cfg.floor_y = 0.0f;
    cc_cfg.eye_height = 1.6f;
    samples::CharacterController controller{cc_cfg};
    const f32 b = kRoomHalf - 0.2f;
    controller.set_bounds(math::Aabb{{-b, 0.2f, -b}, {b, kWallHeight - 0.2f, b}});
    controller.set_mode(samples::ControllerMode::FreeCam);
    controller.set_position({0.0f, 2.2f, 6.2f});
    controller.set_look(0.0f, -0.12f);

    PSY_LOG_INFO("Psynder sample 11 running{}",
                 smoke_frames > 0 ? fmt::format(" -- smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    auto* input = platform::input();
    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;
    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);
    std::array<Light, kNumLights> lights{};

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        if (input && input->key_down(platform::KeyCode::Escape) && !editor::overlays_capturing()) {
            PSY_LOG_INFO("sample_11: escape pressed, exiting");
            break;
        }

        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        const editor::Mode edit_mode = app_host.engine_frame_update(dt);

        f64 t;
        if (edit_mode == editor::Mode::Edit) {
            t = 0.0;
        } else if (smoke_frames > 0) {
            t = static_cast<f64>(frame) * (1.0 / 60.0);
            const f32 ang = static_cast<f32>(frame) * 0.05f;
            const math::Vec3 eye{std::cos(ang) * 5.6f, 2.4f, std::sin(ang) * 5.6f};
            controller.set_position(eye);
            const f32 yaw = std::atan2(-eye.x, eye.z) + math::kPi;
            controller.set_look(yaw, -0.18f);
        } else {
            t = platform::Clock::seconds(now_ticks - t0);
            if (input && !editor::overlays_capturing())
                controller.update(*input, dt);
        }

        orbit_lights(static_cast<f32>(t), lights);
        const Camera cam = render::rt::make_frame_camera(controller.eye(),
                                                         controller.forward(),
                                                         aspect,
                                                         60.0f * math::kDegToRad);

        render::rt::FrameRenderConfig rt_config =
            render::rt::frame_render_config_from_console(kFbW, kFbH, kTraceW, kTraceH, 16u);
        rt_config.ambient_scale = 20.4f;
        rt_config.direct_scale = 255.0f;
        rt_config.attenuation_quadratic = 0.05f;
        rt_config.reflection_scale = 1.0f;

        render::rt::FrameRenderInput rt_input{};
        rt_input.tlas = &tlas;
        rt_input.camera = cam;
        rt_input.lights = lights.data();
        rt_input.light_count = static_cast<u32>(lights.size());
        rt_input.materials.library = &material_library;
        rt_input.materials.instance_materials = instance_materials.data();
        rt_input.materials.instance_material_count = static_cast<u32>(instance_materials.size());
        rt_input.materials.default_rgba8 = pack_rgba8(70, 70, 80);
        renderer.render_rt(rt_input, rt_config, final_pixels.data());

        app_host.engine_frame_post();
        app_host.present();

        if (smoke_frames > 0) {
            const math::Vec3 eye = controller.eye();
            PSY_LOG_INFO(
                "sample_11: frame {} -- eye ({:.2f},{:.2f},{:.2f}) traced {}x{} bounces={}",
                frame,
                eye.x,
                eye.y,
                eye.z,
                kTraceW,
                kTraceH,
                rt_config.reflection_bounces);
        }

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_11: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_11");

    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct RtSpheresSample {
    static constexpr std::string_view log_name() noexcept { return "sample_11"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 11"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    int run(app::WindowApp& app_host, const app::AppArgs& args) {
        return sample_main(args, app_host);
    }
};

PSYNDER_WINDOW_SAMPLE_MAIN(RtSpheresSample)
