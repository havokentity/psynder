// SPDX-License-Identifier: MIT
// Psynder — Sample 12. Raytracing showcase: a dense scene through the BVH8
// TLAS to stress the pure-CPU raytracer harder than sample_05.
//
// The scene: a dark ground plane plus a parametric field of cubes and
// spheres of varying height (two BLAS meshes — a unit cube and a unit
// sphere — reused across dozens of TLAS instances; that reuse is exactly
// what the TLAS is for). The field is lit by six orbiting colored point
// lights, each casting traced shadows. Per pixel we resolve primary
// visibility against the TLAS, then trace per-light shadow rays gathered
// into 8-wide ShadowPacket8 batches via
// `psynder::render::rt::trace_shadow_packet`. Shading is Lambert plus
// inverse-square attenuation, mirroring sample_05. The skybox is a vertical
// gradient.
//
// To keep the run real-time (and the smoke run fast), the lit pass runs at
// quarter resolution and is bilinear-upsampled into the final framebuffer.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.

#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/rt/Bvh.h"
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

// ─── CLI parsing ─────────────────────────────────────────────────────────
struct Args {
    u32 smoke_frames = 0;
    std::string capture_out;
};

u32 parse_uint(std::string_view v) noexcept {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9')
            return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kFlag = "--smoke-frames=";
    constexpr std::string_view kFlagSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kFlag)) {
            a.smoke_frames = parse_uint(s.substr(kFlag.size()));
        } else if (s == kFlagSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        }
    }
    return a;
}

// ─── Render config ───────────────────────────────────────────────────────
// Internal framebuffer (small — raytracing per pixel is expensive). The
// shadow pass runs at a quarter of these dimensions and is bilinear-
// upsampled into the final image. Same resolution as sample_05 so the
// per-frame budget stays real-time even with the much larger instance set.
constexpr u32 kFbW = 512;
constexpr u32 kFbH = 288;
constexpr u32 kShadowW = kFbW / 2;  // 256
constexpr u32 kShadowH = kFbH / 2;  // 144
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
struct Camera {
    math::Vec3 origin;
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up;
    f32 fov_tan;  // tan(half-fov-vertical)
    f32 aspect;
};

Camera make_orbit_camera(f32 t_seconds, f32 aspect) {
    // Pull back further than sample_05 to frame the whole field.
    const f32 radius = 13.0f;
    const f32 height = 6.5f;
    const f32 angle = t_seconds * 0.16f;
    const math::Vec3 eye{std::cos(angle) * radius, height, std::sin(angle) * radius};
    const math::Vec3 target{0.0f, 1.2f, 0.0f};
    const math::Vec3 world_up{0.0f, 1.0f, 0.0f};

    const math::Vec3 fwd = math::normalize(math::sub(target, eye));
    const math::Vec3 right = math::normalize(math::cross(fwd, world_up));
    const math::Vec3 up = math::cross(right, fwd);

    Camera c{};
    c.origin = eye;
    c.forward = fwd;
    c.right = right;
    c.up = up;
    c.fov_tan = std::tan(48.0f * math::kDegToRad * 0.5f);
    c.aspect = aspect;
    return c;
}

// Build a primary ray from a [0,1]^2 NDC pixel coordinate.
render::rt::Ray primary_ray(const Camera& cam, f32 nx, f32 ny) {
    // Pixel (0,0) is top-left; flip y to point upward in world space.
    const f32 sx = (2.0f * nx - 1.0f) * cam.aspect * cam.fov_tan;
    const f32 sy = (1.0f - 2.0f * ny) * cam.fov_tan;
    math::Vec3 dir{};
    dir.x = cam.forward.x + cam.right.x * sx + cam.up.x * sy;
    dir.y = cam.forward.y + cam.right.y * sx + cam.up.y * sy;
    dir.z = cam.forward.z + cam.right.z * sx + cam.up.z * sy;
    dir = math::normalize(dir);
    render::rt::Ray r{};
    r.origin = cam.origin;
    r.direction = dir;
    r.t_min = 1e-3f;
    r.t_max = 1e3f;
    return r;
}

// ─── Lights ──────────────────────────────────────────────────────────────
struct Light {
    math::Vec3 position;
    f32 intensity;  // per-channel multiplier (already pre-tinted)
    f32 r, g, b;    // 0..1 color
    f32 range;
};

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

// ─── Skybox / background ─────────────────────────────────────────────────
// Vertical gradient — deep navy at the horizon, fading to nearly black
// overhead. Sample by ray direction Y.
u32 sample_sky(math::Vec3 dir) {
    const f32 t = 0.5f * (dir.y + 1.0f);  // 0..1
    // Horizon (t small): slightly brighter navy. Zenith (t large): black.
    const f32 horizon[3] = {7.0f, 11.0f, 30.0f};
    const f32 zenith[3] = {1.0f, 1.0f, 7.0f};
    const f32 r = horizon[0] * (1.0f - t) + zenith[0] * t;
    const f32 g = horizon[1] * (1.0f - t) + zenith[1] * t;
    const f32 b = horizon[2] * (1.0f - t) + zenith[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

// ─── Bilinear upsample (low-res lit pass → final framebuffer) ────────────
void upsample_bilinear(const u32* src, u32 sw, u32 sh, u32* dst, u32 dw, u32 dh) {
    const f32 fx_scale = static_cast<f32>(sw) / static_cast<f32>(dw);
    const f32 fy_scale = static_cast<f32>(sh) / static_cast<f32>(dh);
    for (u32 y = 0; y < dh; ++y) {
        const f32 sy = (static_cast<f32>(y) + 0.5f) * fy_scale - 0.5f;
        const i32 y0 = std::max(0, static_cast<i32>(std::floor(sy)));
        const i32 y1 = std::min(static_cast<i32>(sh) - 1, y0 + 1);
        const f32 fy = sy - static_cast<f32>(y0);
        for (u32 x = 0; x < dw; ++x) {
            const f32 sx = (static_cast<f32>(x) + 0.5f) * fx_scale - 0.5f;
            const i32 x0 = std::max(0, static_cast<i32>(std::floor(sx)));
            const i32 x1 = std::min(static_cast<i32>(sw) - 1, x0 + 1);
            const f32 fx = sx - static_cast<f32>(x0);

            const u32 p00 = src[static_cast<usize>(y0) * sw + x0];
            const u32 p10 = src[static_cast<usize>(y0) * sw + x1];
            const u32 p01 = src[static_cast<usize>(y1) * sw + x0];
            const u32 p11 = src[static_cast<usize>(y1) * sw + x1];

            auto unpack = [](u32 p, f32& r, f32& g, f32& b) {
                r = static_cast<f32>(p & 0xFFu);
                g = static_cast<f32>((p >> 8) & 0xFFu);
                b = static_cast<f32>((p >> 16) & 0xFFu);
            };
            f32 r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            unpack(p00, r00, g00, b00);
            unpack(p10, r10, g10, b10);
            unpack(p01, r01, g01, b01);
            unpack(p11, r11, g11, b11);
            const f32 w00 = (1.0f - fx) * (1.0f - fy);
            const f32 w10 = fx * (1.0f - fy);
            const f32 w01 = (1.0f - fx) * fy;
            const f32 w11 = fx * fy;
            const f32 r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
            const f32 g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
            const f32 b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
            dst[static_cast<usize>(y) * dw + x] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const u32 smoke_frames = args.smoke_frames;

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 12 (raytracing showcase)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_12: failed to create window");
        return EXIT_FAILURE;
    }

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
    std::vector<u32> final_pixels(static_cast<usize>(kFbW) * kFbH, 0u);
    std::vector<u32> shadow_pixels(static_cast<usize>(kShadowW) * kShadowH, 0u);

    render::Framebuffer fb{};
    fb.width = kFbW;
    fb.height = kFbH;
    fb.pitch = kFbW * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(final_pixels.data());

    PSY_LOG_INFO("Psynder sample 12 running{} — {} TLAS instances, {} lights",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{},
                 kNumInstances,
                 kNumLights);

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);

    std::array<Light, kNumLights> lights{};

    // Pre-allocate the per-pixel primary-hit info so we can drive shadow
    // packets in row-major batches of 8.
    struct PixelHit {
        bool hit;
        math::Vec3 position;
        math::Vec3 normal;
        f32 r, g, b;  // ambient surface color in 0..1
    };
    std::vector<PixelHit> hits(static_cast<usize>(kShadowW) * kShadowH);

    // 60-sample ring of frame-times (ms) for the debug HUD strip chart.
    constexpr u32 kFrameHistory = 60;
    std::array<f32, kFrameHistory> frame_ms_ring{};
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
        frame_ms_ring[frame % kFrameHistory] = frame_ms;

        // ESC quits.
        if (auto* in = platform::input(); in && in->key_down(platform::KeyCode::Escape)) {
            break;
        }

        // Editor F2/~ toggle + PLAY/EDIT badge bottom-right. EDIT mode
        // pins time so the user can inspect the BVH with a frozen scene.
        const editor::Mode edit_mode =
            platform::input() ? editor::sample_step(*platform::input(), fb) : editor::Mode::Play;

        // Smoke runs pin time to frame index so the captured PNG is
        // deterministic across hosts.
        const f64 t = (edit_mode == editor::Mode::Edit) ? 0.0
                      : smoke_frames > 0
                          ? static_cast<f64>(frame) * (1.0 / 60.0)
                          : platform::Clock::seconds(platform::Clock::ticks_now() - t0);

        orbit_lights(static_cast<f32>(t), lights);
        const Camera cam = make_orbit_camera(static_cast<f32>(t), aspect);

        // ── Pass 1: primary visibility @ low-res (256×144). ─────────────
        // Cast one primary ray per low-res pixel against the TLAS and
        // stash the hit. Pixels that miss draw the sky immediately.
        for (u32 y = 0; y < kShadowH; ++y) {
            for (u32 x = 0; x < kShadowW; ++x) {
                const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kShadowW);
                const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kShadowH);
                const render::rt::Ray ray = primary_ray(cam, nx, ny);
                const render::rt::Hit h = tlas.intersect(ray);

                const usize idx = static_cast<usize>(y) * kShadowW + x;
                PixelHit ph{};
                ph.hit = h.hit;
                if (!h.hit) {
                    shadow_pixels[idx] = sample_sky(ray.direction);
                } else {
                    // Surface color: field[instance] tint, or dim grey for
                    // the ground (the last instance index).
                    u32 color = pack_rgba8(55, 55, 65);  // ground default
                    if (h.instance < kFieldCount) {
                        color = field[h.instance].color;
                    }
                    ph.r = static_cast<f32>(color & 0xFFu) / 255.0f;
                    ph.g = static_cast<f32>((color >> 8) & 0xFFu) / 255.0f;
                    ph.b = static_cast<f32>((color >> 16) & 0xFFu) / 255.0f;
                    // Hit position along the primary ray.
                    ph.position = {
                        ray.origin.x + ray.direction.x * h.t,
                        ray.origin.y + ray.direction.y * h.t,
                        ray.origin.z + ray.direction.z * h.t,
                    };
                    ph.normal = math::normalize(h.normal);
                }
                hits[idx] = ph;
            }
        }

        // ── Pass 2: per-pixel shadow trace (packets of 8). ──────────────
        // For each light, fill ShadowPacket8 with 8 successive hit-pixels'
        // shadow rays, dispatch via trace_shadow_packet, accumulate the
        // lit contribution per pixel.
        std::vector<f32> accum(static_cast<usize>(kShadowW) * kShadowH * 3, 0.0f);

        for (u32 li = 0; li < kNumLights; ++li) {
            const Light& L = lights[li];
            render::rt::ShadowPacket8 pkt{};
            u32 in_pkt = 0;
            u32 pkt_idx[8] = {0};
            // Track which lanes are "real" vs. padding.
            bool pkt_real[8] = {false};

            auto dispatch = [&]() {
                // Fill any padding lanes with a guaranteed-miss ray so the
                // packet kernel has something safe to read.
                for (u32 i = in_pkt; i < 8; ++i) {
                    pkt.rays[i].origin = {0, 1e6f, 0};
                    pkt.rays[i].direction = {0, 1, 0};
                    pkt.rays[i].t_min = 1e-4f;
                    pkt.rays[i].t_max = 1.0f;
                    pkt_real[i] = false;
                }
                render::rt::trace_shadow_packet(tlas, pkt);
                for (u32 i = 0; i < 8; ++i) {
                    if (!pkt_real[i])
                        continue;
                    const usize px = pkt_idx[i];
                    if (pkt.occluded[i])
                        continue;
                    // Lambert + simple inverse-square falloff.
                    const PixelHit& ph = hits[px];
                    math::Vec3 to_light = math::sub(L.position, ph.position);
                    const f32 d2 = math::dot(to_light, to_light);
                    const f32 d = std::sqrt(d2);
                    if (d > L.range || d < 1e-4f)
                        continue;
                    const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                    const f32 ndotl = std::max(0.0f, math::dot(ph.normal, ld));
                    if (ndotl <= 0.0f)
                        continue;
                    const f32 atten = 1.0f / (1.0f + d * d * 0.06f);
                    const f32 k = ndotl * atten * L.intensity;
                    accum[px * 3 + 0] += ph.r * L.r * k;
                    accum[px * 3 + 1] += ph.g * L.g * k;
                    accum[px * 3 + 2] += ph.b * L.b * k;
                }
                in_pkt = 0;
            };

            for (usize px = 0, n = hits.size(); px < n; ++px) {
                const PixelHit& ph = hits[px];
                if (!ph.hit)
                    continue;
                math::Vec3 to_light = math::sub(L.position, ph.position);
                const f32 d2 = math::dot(to_light, to_light);
                const f32 d = std::sqrt(d2);
                if (d > L.range || d < 1e-4f)
                    continue;
                const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                // Back-face cull cheap-out: skip if light is below the
                // surface (saves a shadow ray per backface).
                if (math::dot(ph.normal, ld) <= 0.0f)
                    continue;
                // Origin offset along the normal to avoid self-shadowing.
                const math::Vec3 origin = {
                    ph.position.x + ph.normal.x * 1e-3f,
                    ph.position.y + ph.normal.y * 1e-3f,
                    ph.position.z + ph.normal.z * 1e-3f,
                };
                pkt.rays[in_pkt].origin = origin;
                pkt.rays[in_pkt].direction = ld;
                pkt.rays[in_pkt].t_min = 1e-4f;
                pkt.rays[in_pkt].t_max = d - 1e-3f;
                pkt_idx[in_pkt] = static_cast<u32>(px);
                pkt_real[in_pkt] = true;
                ++in_pkt;
                if (in_pkt == 8)
                    dispatch();
            }
            if (in_pkt > 0)
                dispatch();
        }

        // ── Combine: write final low-res lit color. ─────────────────────
        for (u32 y = 0; y < kShadowH; ++y) {
            for (u32 x = 0; x < kShadowW; ++x) {
                const usize idx = static_cast<usize>(y) * kShadowW + x;
                if (!hits[idx].hit)
                    continue;  // sky already written
                const PixelHit& ph = hits[idx];
                // Ambient floor so unlit faces aren't pure black.
                const f32 amb = 0.10f;
                const f32 r = ph.r * amb * 30.0f + accum[idx * 3 + 0] * 18.0f;
                const f32 g = ph.g * amb * 30.0f + accum[idx * 3 + 1] * 18.0f;
                const f32 b = ph.b * amb * 30.0f + accum[idx * 3 + 2] * 18.0f;
                shadow_pixels[idx] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
            }
        }

        // ── Pass 3: bilinear upsample low-res into the final FB. ────────
        upsample_bilinear(shadow_pixels.data(), kShadowW, kShadowH, final_pixels.data(), kFbW, kFbH);

        // Debug HUD overlay — `r_debug_hud full` enables. Per-frame stats
        // plus an avg over the populated prefix of the ring.
        {
            ui::imm::DebugHudStats stats{};
            stats.frame_ms = frame_ms;
            stats.avg_frame_ms = [&]() noexcept {
                const u32 n = std::min<u32>(frame + 1u, kFrameHistory);
                if (n == 0u)
                    return 0.0f;
                f32 sum = 0.0f;
                for (u32 i = 0; i < n; ++i)
                    sum += frame_ms_ring[i];
                return sum / static_cast<f32>(n);
            }();
            stats.draw_calls = 1;  // single rasterized full-screen blit
            stats.triangles = 0;   // raytraced — no rasterized geometry
            stats.active_voices = 0;
            ui::imm::draw_debug_hud(fb, stats);
        }

        window->present(fb);

        // Bump unconditionally each iteration so the frame_ms_ring populates
        // in interactive runs (smoke_frames == 0) too.
        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_12: smoke target reached ({}); {} instances, {} lights; exiting",
                         smoke_frames,
                         kNumInstances,
                         kNumLights);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             final_pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_12: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_12: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
