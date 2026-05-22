// SPDX-License-Identifier: MIT
// Psynder — Sample 12. Raytracing showcase: a dense scene through the BVH8
// TLAS to stress the pure-CPU raytracer harder than sample_05.
//
// The scene: a dark ground plane plus a parametric field of cubes and
// spheres of varying height (two BLAS meshes — a unit cube and a unit
// sphere — reused across dozens of TLAS instances; that reuse is exactly
// what the TLAS is for). The field is lit by six orbiting colored point
// lights, each casting traced shadows. The sample now hands its TLAS,
// camera, lights, and instance materials to the hybrid scene renderer;
// the engine owns primary visibility, AO, 8-wide shadow packets, shading,
// and upsample.
//
// To keep the run real-time (and the smoke run fast), the lit pass runs at
// quarter resolution and is bilinear-upsampled into the final framebuffer.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.
//   --rt-ao=0|1              Override sample AO CVar for capture/perf checks.
//   --rt-ao-debug=0|1        Show AO visibility as grayscale when AO is enabled.
//   --rt-ao-samples=N        Override AO sample count for capture/perf checks.
//   --rt-ao-radius=F         Override AO radius for capture/perf checks.
//   --rt-ao-strength=F       Override AO ambient strength for capture/perf checks.
//   --rt-ao-lit-strength=F   Override AO direct-light strength for capture/perf checks.
//   --rt-cores=N             Override sample RT worker chunk target for smoke/perf checks.

#include "core/AppArgs.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "jobs/JobSystem.h"
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace psynder::render::rt {
void ensure_denoise_console_commands_registered();
}

namespace {

// ─── CLI parsing ─────────────────────────────────────────────────────────
struct Args : app::AppArgs {
    int rt_ao = -1;
    int rt_ao_debug = -1;
    std::string rt_cores_hint;
    std::string rt_ao_samples;
    std::string rt_ao_radius;
    std::string rt_ao_strength;
    std::string rt_ao_lit_strength;
};

int parse_bool_arg(std::string_view v) noexcept {
    u32 value = 0;
    return app::parse_u32_decimal(v, value) && value != 0u ? 1 : 0;
}

Args parse_sample12_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kAoEq = "--rt-ao=";
    constexpr std::string_view kAoSp = "--rt-ao";
    constexpr std::string_view kAoDebugEq = "--rt-ao-debug=";
    constexpr std::string_view kAoDebugSp = "--rt-ao-debug";
    constexpr std::string_view kAoSamplesEq = "--rt-ao-samples=";
    constexpr std::string_view kAoSamplesSp = "--rt-ao-samples";
    constexpr std::string_view kAoRadiusEq = "--rt-ao-radius=";
    constexpr std::string_view kAoRadiusSp = "--rt-ao-radius";
    constexpr std::string_view kAoStrengthEq = "--rt-ao-strength=";
    constexpr std::string_view kAoStrengthSp = "--rt-ao-strength";
    constexpr std::string_view kAoLitStrengthEq = "--rt-ao-lit-strength=";
    constexpr std::string_view kAoLitStrengthSp = "--rt-ao-lit-strength";
    constexpr std::string_view kRtCoresEq = "--rt-cores=";
    constexpr std::string_view kRtCoresSp = "--rt-cores";
    for (int i = 1; i < argc; ++i) {
        if (app::consume_common_arg(argc, argv, i, a))
            continue;
        std::string_view s{argv[i]};
        if (s.starts_with(kAoEq)) {
            a.rt_ao = parse_bool_arg(s.substr(kAoEq.size()));
        } else if (s == kAoSp && i + 1 < argc) {
            a.rt_ao = parse_bool_arg(std::string_view{argv[++i]});
        } else if (s.starts_with(kAoDebugEq)) {
            a.rt_ao_debug = parse_bool_arg(s.substr(kAoDebugEq.size()));
        } else if (s == kAoDebugSp && i + 1 < argc) {
            a.rt_ao_debug = parse_bool_arg(std::string_view{argv[++i]});
        } else if (s.starts_with(kAoSamplesEq)) {
            a.rt_ao_samples = std::string(s.substr(kAoSamplesEq.size()));
        } else if (s == kAoSamplesSp && i + 1 < argc) {
            a.rt_ao_samples = argv[++i];
        } else if (s.starts_with(kAoRadiusEq)) {
            a.rt_ao_radius = std::string(s.substr(kAoRadiusEq.size()));
        } else if (s == kAoRadiusSp && i + 1 < argc) {
            a.rt_ao_radius = argv[++i];
        } else if (s.starts_with(kAoStrengthEq)) {
            a.rt_ao_strength = std::string(s.substr(kAoStrengthEq.size()));
        } else if (s == kAoStrengthSp && i + 1 < argc) {
            a.rt_ao_strength = argv[++i];
        } else if (s.starts_with(kAoLitStrengthEq)) {
            a.rt_ao_lit_strength = std::string(s.substr(kAoLitStrengthEq.size()));
        } else if (s == kAoLitStrengthSp && i + 1 < argc) {
            a.rt_ao_lit_strength = argv[++i];
        } else if (s.starts_with(kRtCoresEq)) {
            a.rt_cores_hint = std::string(s.substr(kRtCoresEq.size()));
        } else if (s == kRtCoresSp && i + 1 < argc) {
            a.rt_cores_hint = argv[++i];
        }
    }
    return a;
}

void apply_rt_arg_overrides(const Args& args) {
    render::rt::FrameRendererConsoleOverrides overrides{};
    overrides.cores_hint = args.rt_cores_hint;
    overrides.ambient_occlusion = args.rt_ao;
    overrides.ao_debug = args.rt_ao_debug;
    overrides.ao_samples = args.rt_ao_samples;
    overrides.ao_radius = args.rt_ao_radius;
    overrides.ao_strength = args.rt_ao_strength;
    overrides.ao_lit_strength = args.rt_ao_lit_strength;
    render::rt::apply_frame_renderer_console_overrides(overrides);
}

// ─── Render config ───────────────────────────────────────────────────────
// Internal framebuffer (small — raytracing per pixel is expensive). The
// RT frame renderer traces at half width / half height and is bilinear-
// upsampled into the final image. Same resolution as sample_05 so the
// per-frame budget stays real-time even with the much larger instance set.
constexpr u32 kFbW = 1024;
constexpr u32 kFbH = 512;
constexpr u32 kShadowW = kFbW / 2;  // 512
constexpr u32 kShadowH = kFbH / 2;  // 256
constexpr u32 kRtTile = 16;
constexpr u32 kNumLights = 6;

// Field layout: a kFieldDim × kFieldDim grid of pillars on the ground.
// Each cell is either a cube or a sphere, chosen by parity, at a height
// driven by a smooth radial function. With kFieldDim = 7 that is 49 field
// instances + 1 ground = 50 TLAS instances total.
constexpr u32 kFieldDim = 7;
constexpr u32 kFieldCount = kFieldDim * kFieldDim;
constexpr u32 kNumInstances = kFieldCount + 1u;  // + ground
constexpr f32 kCellSpacing = 2.0f;

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

// Generate a unit-radius UV sphere centered at the origin (radius 0.5 so it
// matches the unit cube's footprint and the same per-instance scale maps
// cleanly to a diameter). `stacks` latitude bands × `slices` longitude
// segments triangulated into a closed shell.
void emit_unit_sphere(std::vector<render::rt::Triangle>& out, u32 stacks, u32 slices) {
    using math::Vec3;
    const f32 radius = 0.5f;
    auto vert = [&](u32 stack, u32 slice) -> Vec3 {
        const f32 v = static_cast<f32>(stack) / static_cast<f32>(stacks);  // 0..1 (pole→pole)
        const f32 u = static_cast<f32>(slice) / static_cast<f32>(slices);  // 0..1 around
        const f32 phi = v * math::kPi;                                     // 0..pi latitude
        const f32 theta = u * 2.0f * math::kPi;                            // 0..2pi longitude
        const f32 sp = std::sin(phi);
        return Vec3{radius * sp * std::cos(theta), radius * std::cos(phi), radius * sp * std::sin(theta)};
    };
    for (u32 st = 0; st < stacks; ++st) {
        for (u32 sl = 0; sl < slices; ++sl) {
            const Vec3 a = vert(st, sl);
            const Vec3 b = vert(st + 1, sl);
            const Vec3 c = vert(st + 1, sl + 1);
            const Vec3 d = vert(st, sl + 1);
            // Top cap degenerates the first band's lower edge, bottom cap
            // the last band's upper edge; the engine BVH tolerates the few
            // sliver triangles and the visual cost is nil.
            if (st != 0)
                out.push_back({a, b, c});
            if (st != stacks - 1)
                out.push_back({a, c, d});
        }
    }
}

// Two big triangles that form a ground quad on the XZ plane at y = 0.
void emit_ground(std::vector<render::rt::Triangle>& out, f32 half) {
    using math::Vec3;
    const Vec3 p00{-half, 0.0f, -half}, p10{+half, 0.0f, -half};
    const Vec3 p01{-half, 0.0f, +half}, p11{+half, 0.0f, +half};
    out.push_back({p00, p11, p10});
    out.push_back({p00, p01, p11});
}

// One field cell: which BLAS to use, where to put it, how big, what color.
struct FieldInstance {
    math::Vec3 center;
    f32 size;     // edge length / sphere diameter
    u32 color;    // RGBA8
    bool sphere;  // true → sphere BLAS, false → cube BLAS
};

// Build the parametric field: a centered grid of pillars whose heights rise
// toward the middle, alternating cube / sphere by cell parity, with a hue
// that cycles across the grid so the lights pick out colored highlights.
std::array<FieldInstance, kFieldCount> make_field_instances() {
    std::array<FieldInstance, kFieldCount> field{};
    const f32 half = static_cast<f32>(kFieldDim - 1u) * 0.5f;
    for (u32 gz = 0; gz < kFieldDim; ++gz) {
        for (u32 gx = 0; gx < kFieldDim; ++gx) {
            const u32 i = gz * kFieldDim + gx;
            const f32 fx = (static_cast<f32>(gx) - half) * kCellSpacing;
            const f32 fz = (static_cast<f32>(gz) - half) * kCellSpacing;
            // Radial falloff → taller pillars near the center.
            const f32 rr = std::sqrt(fx * fx + fz * fz);
            const f32 h = 0.6f + 1.7f * std::exp(-0.18f * rr * rr * 0.25f);
            const bool sphere = ((gx + gz) & 1u) != 0u;
            const f32 size = sphere ? 0.95f : 0.85f;
            // Hue ramp across the grid (cheap HSV-ish via cosine lobes).
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kFieldCount);
            const f32 cr = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.00f));
            const f32 cg = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.33f));
            const f32 cb = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.66f));
            FieldInstance fi{};
            fi.sphere = sphere;
            fi.size = size;
            // Rest the object on the ground: cube center at h/2-ish gives a
            // pillar of height h; spheres sit so their base touches y=0.
            fi.center = {fx, h * 0.5f, fz};
            fi.color = pack_rgba8(clamp_u8(40.0f + 200.0f * cr),
                                  clamp_u8(40.0f + 200.0f * cg),
                                  clamp_u8(40.0f + 200.0f * cb));
            field[i] = fi;
        }
    }
    return field;
}

// ─── Mat4 helpers (just the ops we need; engine API is frozen). ──────────
// Non-uniform scale on Y lets the cube pillars stretch into columns while
// keeping the X/Z footprint at `size`.
math::Mat4 mat4_trs(math::Vec3 t, f32 scale_xz, f32 scale_y) {
    math::Mat4 m{};
    m.m[0] = scale_xz;
    m.m[1] = 0;
    m.m[2] = 0;
    m.m[3] = 0;
    m.m[4] = 0;
    m.m[5] = scale_y;
    m.m[6] = 0;
    m.m[7] = 0;
    m.m[8] = 0;
    m.m[9] = 0;
    m.m[10] = scale_xz;
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
    // Pull back further than sample_05 to frame the whole field.
    const f32 radius = 13.0f;
    const f32 height = 6.5f;
    const f32 angle = t_seconds * 0.16f;
    const math::Vec3 eye{std::cos(angle) * radius, height, std::sin(angle) * radius};
    const math::Vec3 target{0.0f, 1.2f, 0.0f};
    return render::rt::make_frame_camera(eye, math::sub(target, eye), aspect, 48.0f * math::kDegToRad);
}

// ─── Lights ──────────────────────────────────────────────────────────────
using Light = render::rt::FrameLight;

void orbit_lights(f32 t_seconds, std::array<Light, kNumLights>& lights) {
    const f32 base_y = 3.4f;
    // Six lights, each with its own orbit radius / speed / phase / color so
    // the traced shadows fan out across the field from several directions.
    const f32 radii[kNumLights] = {6.0f, 8.5f, 5.0f, 9.5f, 7.0f, 4.0f};
    const f32 speeds[kNumLights] = {0.50f, -0.32f, 0.74f, 0.21f, -0.58f, 0.95f};
    const f32 phases[kNumLights] = {0.0f, 1.047f, 2.094f, 3.142f, 4.189f, 5.236f};
    const f32 heights[kNumLights] =
        {base_y, base_y + 1.0f, base_y - 0.4f, base_y + 1.6f, base_y + 0.3f, base_y - 0.8f};
    const f32 cols[kNumLights][3] = {
        {1.00f, 0.30f, 0.25f},  // red
        {0.25f, 1.00f, 0.35f},  // green
        {0.30f, 0.45f, 1.00f},  // blue
        {1.00f, 0.80f, 0.25f},  // amber
        {0.85f, 0.30f, 1.00f},  // magenta
        {0.25f, 0.95f, 1.00f},  // cyan
    };
    for (u32 i = 0; i < kNumLights; ++i) {
        const f32 a = t_seconds * speeds[i] + phases[i];
        lights[i].position = {std::cos(a) * radii[i], heights[i], std::sin(a) * radii[i]};
        lights[i].r = cols[i][0];
        lights[i].g = cols[i][1];
        lights[i].b = cols[i][2];
        lights[i].intensity = 8.0f;
        lights[i].range = 20.0f;
    }
}

}  // namespace

platform::WindowDesc make_window_desc(const app::AppArgs&) noexcept {
    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 12 (raytracing showcase)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;
    return desc;
}

int sample_main(const Args& parsed_args, app::WindowApp& app_host) {
    const Args& args = parsed_args;
    const u32 smoke_frames = args.smoke_frames;
    render::rt::ensure_frame_renderer_console_registered();
    apply_rt_arg_overrides(args);
    render::rt::ensure_denoise_console_commands_registered();
    const platform::WindowDesc desc = make_window_desc(args);
    auto* window = &app_host.window();

    // ── Build the static scene geometry. ────────────────────────────────
    // Just THREE BLAS meshes — a unit cube, a unit sphere, and the ground —
    // are reused across all kFieldCount field instances via per-instance
    // TLAS transforms. That BLAS reuse is the point of the TLAS: dozens of
    // visible objects, a handful of meshes.
    std::array<FieldInstance, kFieldCount> field = make_field_instances();

    std::vector<render::rt::Triangle> cube_tris;
    emit_unit_cube(cube_tris);

    std::vector<render::rt::Triangle> sphere_tris;
    emit_unit_sphere(sphere_tris, /*stacks=*/10, /*slices=*/14);

    std::vector<render::rt::Triangle> ground_tris;
    emit_ground(ground_tris, /*half=*/14.0f);

    render::rt::Bvh8 cube_blas;
    cube_blas.build(cube_tris.data(), static_cast<u32>(cube_tris.size()));
    render::rt::Bvh8 sphere_blas;
    sphere_blas.build(sphere_tris.data(), static_cast<u32>(sphere_tris.size()));
    render::rt::Bvh8 ground_blas;
    ground_blas.build(ground_tris.data(), static_cast<u32>(ground_tris.size()));

    // TLAS: kFieldCount field instances + 1 ground.
    std::array<render::rt::Tlas::InstanceDesc, kNumInstances> insts{};
    for (u32 i = 0; i < kFieldCount; ++i) {
        const FieldInstance& fi = field[i];
        insts[i].blas = fi.sphere ? &sphere_blas : &cube_blas;
        // Cubes stretch vertically into pillars (footprint stays `size`);
        // spheres scale uniformly so they stay round.
        const f32 scale_y = fi.sphere ? fi.size : (fi.center.y * 2.0f);
        insts[i].transform = mat4_trs(fi.center, fi.size, scale_y);
    }
    insts[kFieldCount].blas = &ground_blas;
    insts[kFieldCount].transform = math::identity4();

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    // ── CPU framebuffers. ───────────────────────────────────────────────
    std::vector<u32>& final_pixels = app_host.pixels();
    std::array<u32, kNumInstances> instance_colors{};
    for (u32 i = 0; i < kFieldCount; ++i)
        instance_colors[i] = field[i].color;
    instance_colors[kFieldCount] = pack_rgba8(55, 55, 65);
    render::SceneRenderer renderer;

    render::Framebuffer& fb = app_host.framebuffer();

    PSY_LOG_INFO("Psynder sample 12 running{} — {} TLAS instances, {} lights",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{},
                 kNumInstances,
                 kNumLights);
    const render::rt::FrameRenderConfig startup_rt_config =
        render::rt::frame_render_config_from_console(kFbW, kFbH, kShadowW, kShadowH, kRtTile);
    PSY_LOG_INFO(
        "sample_12 AO: enabled={}, debug={}, samples={}, radius={}, strength={}, lit_strength={}, "
        "denoise={}",
        startup_rt_config.ambient_occlusion ? 1 : 0,
        startup_rt_config.ao_debug ? 1 : 0,
        startup_rt_config.ao_samples,
        startup_rt_config.ao_radius,
        startup_rt_config.ao_strength,
        startup_rt_config.ao_lit_strength,
        startup_rt_config.ao_denoise ? 1 : 0);

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);

    std::array<Light, kNumLights> lights{};

    ui::imm::DebugHudFrameHistory hud_history{};
    u64 prev_frame_ticks = t0;
    // Smoke-mode frame-time stand-in (60 FPS budget = 1/60 s).
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;

    while (!window->should_close()) {
        window->poll_events();

        // Per-frame wall-clock delta for HUD stats. Smoke runs are
        // frame-indexed so use the 60 FPS budget stand-in for determinism.
        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        hud_history.push(frame_ms);

        // ESC quits — unless the console is open, where Esc closes it instead.
        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
            break;
        }

        // Editor F2/~ toggle + PLAY/EDIT badge bottom-right. EDIT mode
        // pins time so the user can inspect the BVH with a frozen scene.
        const editor::Mode edit_mode =
            platform::input() ? editor::sample_step(*platform::input(), fb, frame_ms * 0.001f)
                              : editor::Mode::Play;

        // Smoke runs pin time to frame index so the captured PNG is
        // deterministic across hosts.
        const f64 t = (edit_mode == editor::Mode::Edit) ? 0.0
                      : smoke_frames > 0
                          ? static_cast<f64>(frame) * (1.0 / 60.0)
                          : platform::Clock::seconds(platform::Clock::ticks_now() - t0);

        orbit_lights(static_cast<f32>(t), lights);
        const Camera cam = make_orbit_camera(static_cast<f32>(t), aspect);
        apply_rt_arg_overrides(args);
        render::rt::FrameRenderInput rt_input{};
        rt_input.tlas = &tlas;
        rt_input.camera = cam;
        rt_input.lights = lights.data();
        rt_input.light_count = static_cast<u32>(lights.size());
        rt_input.materials.instance_rgba8 = instance_colors.data();
        rt_input.materials.instance_count = static_cast<u32>(instance_colors.size());
        rt_input.materials.default_rgba8 = pack_rgba8(55, 55, 65);

        const render::rt::FrameRenderConfig rt_config =
            render::rt::frame_render_config_from_console(kFbW, kFbH, kShadowW, kShadowH, kRtTile);

        render::rt::FrameRenderStats rt_stats{};
        renderer.render_rt(rt_input, rt_config, final_pixels.data(), &rt_stats);

        if (smoke_frames > 0 && frame == 0) {
            PSY_LOG_INFO("sample_12 RT renderer: workers={}, hint={}, scheduled_jobs={}, tiles={}",
                         jobs::JobSystem::Get().worker_count(),
                         rt_config.cores_hint,
                         rt_stats.scheduled_jobs,
                         rt_stats.tile_count);
            if (rt_config.ambient_occlusion) {
                PSY_LOG_INFO("sample_12 AO stats: hit_pixels={}, ao_min={}, ao_avg={}",
                             rt_stats.hit_pixels,
                             rt_stats.ao_min,
                             rt_stats.ao_avg);
            }
        }

        ui::imm::draw_debug_hud(fb, hud_history.make_stats(frame_ms, 1, 0, 0));

        ui::console::draw(fb);  // drop-down console (`~`) overlays everything
        window->present(fb);

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_12: smoke target reached ({}); {} instances, {} lights; exiting",
                         smoke_frames,
                         kNumInstances,
                         kNumLights);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_12");

    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

struct RtShowcaseSample {
    static constexpr std::string_view log_name() noexcept { return "sample_12"; }
    static constexpr std::string_view display_name() noexcept { return "Psynder sample 12"; }

    static platform::WindowDesc window_desc(const app::AppArgs& args) noexcept {
        return make_window_desc(args);
    }

    static Args parse_args(int argc, char** argv) { return parse_sample12_args(argc, argv); }

    int run(app::WindowApp& app_host, const Args& args) { return sample_main(args, app_host); }
};

PSYNDER_WINDOW_SAMPLE_MAIN(RtShowcaseSample)
