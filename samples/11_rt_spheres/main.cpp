// SPDX-License-Identifier: MIT
// Psynder — Sample 11. Reflective sphere room you fly/walk through.
//
// The scene: an enclosed room (floor + four walls, each a triangle mesh) of
// mirror and saturated-diffuse spheres plus a couple of boxes, all built as
// `render::rt::Bvh8` BLAS instances inside one `render::rt::Tlas`. Two point
// lights orbit the room. Per pixel we cast a primary ray against the TLAS,
// shade the hit with Lambert + an 8-wide shadow-ray packet, and — for the
// mirror surfaces — add a single bounce of reflection: reflect the view
// direction about the surface normal, `tlas.intersect` that reflected ray,
// and shade whatever it hits (sky if it escapes the room).
//
// The RT core (engine/render/rt/Bvh.h) exposes only primary `intersect` +
// `trace_shadow_packet` — there is no native recursive/reflection ray — so
// the mirror bounce is implemented here in the sample's shading and kept to
// exactly one bounce so the trace stays real-time.
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
#include "common/PngWriter.h"

#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/rt/Bvh.h"
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

// ─── Camera ──────────────────────────────────────────────────────────────
struct Camera {
    math::Vec3 origin;
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up;
    f32 fov_tan;  // tan(half-fov-vertical)
    f32 aspect;
};

// Build the camera basis from the controller's eye + forward.
Camera make_camera(math::Vec3 eye, math::Vec3 fwd, f32 aspect) {
    const math::Vec3 world_up{0.0f, 1.0f, 0.0f};
    fwd = math::normalize(fwd);
    math::Vec3 right = math::cross(fwd, world_up);
    // Guard against looking straight up/down (forward ∥ world_up).
    if (math::dot(right, right) < 1e-6f)
        right = math::Vec3{1.0f, 0.0f, 0.0f};
    right = math::normalize(right);
    const math::Vec3 up = math::cross(right, fwd);

    Camera c{};
    c.origin = eye;
    c.forward = fwd;
    c.right = right;
    c.up = up;
    c.fov_tan = std::tan(60.0f * math::kDegToRad * 0.5f);
    c.aspect = aspect;
    return c;
}

// Build a primary ray from a [0,1]^2 NDC pixel coordinate.
render::rt::Ray primary_ray(const Camera& cam, f32 nx, f32 ny) {
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

// Reflect incident direction `d` about unit normal `n`: d - 2(d·n)n.
PSY_FORCEINLINE math::Vec3 reflect(math::Vec3 d, math::Vec3 n) noexcept {
    const f32 k = 2.0f * math::dot(d, n);
    return {d.x - k * n.x, d.y - k * n.y, d.z - k * n.z};
}

// ─── Lights ──────────────────────────────────────────────────────────────
struct Light {
    math::Vec3 position;
    f32 intensity;
    f32 r, g, b;  // 0..1 color
    f32 range;
};

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

// ─── Skybox / background ─────────────────────────────────────────────────
// Vertical gradient: warm horizon fading to a deep blue zenith. Sampled by
// ray-direction Y; used for both primary misses and escaped reflection rays.
math::Vec3 sample_sky_lin(math::Vec3 dir) {
    const f32 t = std::clamp(0.5f * (dir.y + 1.0f), 0.0f, 1.0f);
    const f32 horizon[3] = {0.55f, 0.60f, 0.78f};
    const f32 zenith[3] = {0.10f, 0.18f, 0.42f};
    return {
        horizon[0] * (1.0f - t) + zenith[0] * t,
        horizon[1] * (1.0f - t) + zenith[1] * t,
        horizon[2] * (1.0f - t) + zenith[2] * t,
    };
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

            const u32 p00 = src[static_cast<usize>(y0) * sw + static_cast<usize>(x0)];
            const u32 p10 = src[static_cast<usize>(y0) * sw + static_cast<usize>(x1)];
            const u32 p01 = src[static_cast<usize>(y1) * sw + static_cast<usize>(x0)];
            const u32 p11 = src[static_cast<usize>(y1) * sw + static_cast<usize>(x1)];

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
    desc.title = "Psynder — sample 11 (reflective sphere room, CPU RT)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_11: failed to create window");
        return EXIT_FAILURE;
    }

    // ── Build the static scene geometry. ────────────────────────────────
    // One Bvh8 each for the shared unit sphere, the shared unit cube, and the
    // room shell. Per-instance placement + scale lives on the TLAS
    // InstanceDesc; the material is keyed off the instance index.
    std::vector<render::rt::Triangle> sphere_tris;
    emit_unit_sphere(sphere_tris, /*stacks=*/14, /*slices=*/24);
    std::vector<render::rt::Triangle> cube_tris;
    emit_unit_cube(cube_tris);
    std::vector<render::rt::Triangle> room_tris;
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
    std::vector<Material> materials(scene.size());
    for (usize i = 0; i < scene.size(); ++i) {
        insts[i].blas = scene[i].blas;
        insts[i].transform =
            (i == kRoomInstance) ? math::identity4() : mat4_trs(scene[i].center, scene[i].scale);
        materials[i] = scene[i].mat;
    }

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    // ── CPU framebuffers. ───────────────────────────────────────────────
    std::vector<u32> final_pixels(static_cast<usize>(kFbW) * kFbH, 0u);
    std::vector<u32> trace_pixels(static_cast<usize>(kTraceW) * kTraceH, 0u);

    render::Framebuffer fb{};
    fb.width = kFbW;
    fb.height = kFbH;
    fb.pitch = kFbW * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(final_pixels.data());

    // ── Shared first-person / free-cam controller (samples/common). ─────
    // FreeCam by default so you fly around the spheres; V toggles grounded
    // FPS, `noclip 1` lifts the bounds clamp. Bounds = the room interior.
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
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{});

    auto* input = platform::input();
    const u64 t0 = platform::Clock::ticks_now();
    u64 last_ticks = t0;
    u32 frame = 0;

    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);

    std::array<Light, kNumLights> lights{};

    // Per-pixel primary-hit info so we can drive shadow packets in row-major
    // batches of 8 (mirrors sample_05).
    struct PixelHit {
        bool hit;
        math::Vec3 position;
        math::Vec3 normal;
        f32 r, g, b;                 // direct-lit accumulation
        f32 reflectivity;            // 0..1
        f32 refl_r, refl_g, refl_b;  // 1-bounce reflected colour (linear)
    };
    std::vector<PixelHit> hits(static_cast<usize>(kTraceW) * kTraceH);

    constexpr u32 kFrameHistory = 60;
    std::array<f32, kFrameHistory> frame_ms_ring{};
    u64 prev_frame_ticks = t0;
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;

    // Shade a single (already-found) hit's direct lighting (ambient + Lambert
    // + per-light shadow ray). Used for the reflected-ray hit so mirrors show
    // a lit scene, not a flat albedo. Deliberately does NOT recurse another
    // reflection, which bounds the whole trace to a single mirror bounce.
    auto shade_direct = [&](math::Vec3 pos,
                            math::Vec3 nrm,
                            const Material& m,
                            const std::array<Light, kNumLights>& L) -> math::Vec3 {
        math::Vec3 c{0.0f, 0.0f, 0.0f};
        const f32 amb = 0.08f;
        c.x += m.r * amb;
        c.y += m.g * amb;
        c.z += m.b * amb;
        for (u32 li = 0; li < kNumLights; ++li) {
            math::Vec3 to_light = math::sub(L[li].position, pos);
            const f32 d2 = math::dot(to_light, to_light);
            const f32 d = std::sqrt(d2);
            if (d > L[li].range || d < 1e-4f)
                continue;
            const math::Vec3 ld = math::mul(to_light, 1.0f / d);
            const f32 ndotl = std::max(0.0f, math::dot(nrm, ld));
            if (ndotl <= 0.0f)
                continue;
            // Shadow test for the reflected hit (scalar; the primary pass uses
            // packets). Offset the origin off the surface to avoid acne.
            render::rt::Ray sray{};
            sray.origin = {pos.x + nrm.x * 1e-3f, pos.y + nrm.y * 1e-3f, pos.z + nrm.z * 1e-3f};
            sray.direction = ld;
            sray.t_min = 1e-4f;
            sray.t_max = d - 1e-3f;
            if (tlas.occluded(sray))
                continue;
            const f32 atten = 1.0f / (1.0f + d * d * 0.05f);
            const f32 k = ndotl * atten * L[li].intensity;
            c.x += m.r * L[li].r * k;
            c.y += m.g * L[li].g * k;
            c.z += m.b * L[li].b * k;
        }
        return c;
    };

    while (!window->should_close()) {
        window->poll_events();

        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        frame_ms_ring[frame % kFrameHistory] = frame_ms;

        // ESC quits — unless the console is open, where Esc closes it instead.
        if (input && input->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
            PSY_LOG_INFO("sample_11: escape pressed, exiting");
            break;
        }

        const f32 dt =
            (smoke_frames > 0)
                ? 1.0f / 60.0f
                : std::min(0.1f, static_cast<f32>(platform::Clock::seconds(now_ticks - last_ticks)));
        last_ticks = now_ticks;

        // Editor F2/~ toggle + PLAY/EDIT badge. EDIT mode pins time so the
        // user can inspect the BVH with a frozen scene.
        const editor::Mode edit_mode = input ? editor::sample_step(*input, fb) : editor::Mode::Play;

        // Smoke runs pin time + camera to a deterministic orbit path so the
        // capture is identical across hosts (no live input required).
        f64 t;
        if (edit_mode == editor::Mode::Edit) {
            t = 0.0;
        } else if (smoke_frames > 0) {
            t = static_cast<f64>(frame) * (1.0 / 60.0);
            // Drive the controller's pose directly (deterministic flythrough):
            // slow orbit around the room looking at the centre.
            const f32 ang = static_cast<f32>(frame) * 0.05f;
            const math::Vec3 eye{std::cos(ang) * 5.6f, 2.4f, std::sin(ang) * 5.6f};
            controller.set_position(eye);
            // Yaw so forward points at the room centre; slight downward pitch.
            const f32 yaw = std::atan2(-eye.x, eye.z) + math::kPi;
            controller.set_look(yaw, -0.18f);
        } else {
            t = platform::Clock::seconds(now_ticks - t0);
            // Frozen while the console owns input so typing doesn't fly the cam.
            if (input && !ui::console::is_open())
                controller.update(*input, dt);
        }

        orbit_lights(static_cast<f32>(t), lights);
        const Camera cam = make_camera(controller.eye(), controller.forward(), aspect);

        // ── Pass 1: primary visibility + 1-bounce reflection @ low-res. ─
        for (u32 y = 0; y < kTraceH; ++y) {
            for (u32 x = 0; x < kTraceW; ++x) {
                const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kTraceW);
                const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kTraceH);
                const render::rt::Ray ray = primary_ray(cam, nx, ny);
                const render::rt::Hit h = tlas.intersect(ray);

                const usize idx = static_cast<usize>(y) * kTraceW + x;
                PixelHit ph{};
                ph.hit = h.hit;
                if (!h.hit) {
                    const math::Vec3 sky = sample_sky_lin(ray.direction);
                    trace_pixels[idx] = pack_rgba8(clamp_u8(sky.x * 255.0f),
                                                   clamp_u8(sky.y * 255.0f),
                                                   clamp_u8(sky.z * 255.0f));
                    hits[idx] = ph;
                    continue;
                }
                const u32 inst = h.instance < materials.size() ? h.instance : kRoomInstance;
                const Material& m = materials[inst];
                ph.r = m.r;
                ph.g = m.g;
                ph.b = m.b;
                ph.reflectivity = m.reflectivity;
                ph.position = {
                    ray.origin.x + ray.direction.x * h.t,
                    ray.origin.y + ray.direction.y * h.t,
                    ray.origin.z + ray.direction.z * h.t,
                };
                math::Vec3 nrm = math::normalize(h.normal);
                // Make sure the normal faces the viewer (BLAS winding can give
                // either side); used for both shading + reflection.
                if (math::dot(nrm, ray.direction) > 0.0f)
                    nrm = math::mul(nrm, -1.0f);
                ph.normal = nrm;

                // 1-bounce mirror reflection: reflect the view direction about
                // the normal, intersect, and shade that hit. Escaped rays read
                // the sky. Bounded to a single bounce so it stays real-time.
                ph.refl_r = ph.refl_g = ph.refl_b = 0.0f;
                if (m.reflectivity > 0.01f) {
                    const math::Vec3 rdir = math::normalize(reflect(ray.direction, nrm));
                    render::rt::Ray rray{};
                    rray.origin = {ph.position.x + nrm.x * 1e-3f,
                                   ph.position.y + nrm.y * 1e-3f,
                                   ph.position.z + nrm.z * 1e-3f};
                    rray.direction = rdir;
                    rray.t_min = 1e-3f;
                    rray.t_max = 1e3f;
                    const render::rt::Hit rh = tlas.intersect(rray);
                    if (!rh.hit) {
                        const math::Vec3 sky = sample_sky_lin(rdir);
                        ph.refl_r = sky.x;
                        ph.refl_g = sky.y;
                        ph.refl_b = sky.z;
                    } else {
                        const u32 rinst = rh.instance < materials.size() ? rh.instance : kRoomInstance;
                        const math::Vec3 rpos = {
                            rray.origin.x + rray.direction.x * rh.t,
                            rray.origin.y + rray.direction.y * rh.t,
                            rray.origin.z + rray.direction.z * rh.t,
                        };
                        math::Vec3 rn = math::normalize(rh.normal);
                        if (math::dot(rn, rray.direction) > 0.0f)
                            rn = math::mul(rn, -1.0f);
                        const math::Vec3 rc = shade_direct(rpos, rn, materials[rinst], lights);
                        ph.refl_r = rc.x;
                        ph.refl_g = rc.y;
                        ph.refl_b = rc.z;
                    }
                }
                hits[idx] = ph;
            }
        }

        // ── Pass 2: per-pixel direct shadow trace (packets of 8). ───────
        std::vector<f32> accum(static_cast<usize>(kTraceW) * kTraceH * 3, 0.0f);

        for (u32 li = 0; li < kNumLights; ++li) {
            const Light& L = lights[li];
            render::rt::ShadowPacket8 pkt{};
            u32 in_pkt = 0;
            u32 pkt_idx[8] = {0};
            bool pkt_real[8] = {false};

            auto dispatch = [&]() {
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
                    const f32 atten = 1.0f / (1.0f + d * d * 0.05f);
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
                if (math::dot(ph.normal, ld) <= 0.0f)
                    continue;
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

        // ── Combine: direct light + ambient + mirror reflection. ────────
        for (u32 y = 0; y < kTraceH; ++y) {
            for (u32 x = 0; x < kTraceW; ++x) {
                const usize idx = static_cast<usize>(y) * kTraceW + x;
                const PixelHit& ph = hits[idx];
                if (!ph.hit)
                    continue;  // sky already written
                const f32 amb = 0.08f;
                // Diffuse component = ambient + shadowed direct.
                f32 dr = ph.r * amb + accum[idx * 3 + 0];
                f32 dg = ph.g * amb + accum[idx * 3 + 1];
                f32 db = ph.b * amb + accum[idx * 3 + 2];
                // Lerp toward the reflected colour by the surface reflectivity.
                const f32 k = ph.reflectivity;
                const f32 r = dr * (1.0f - k) + ph.refl_r * k;
                const f32 g = dg * (1.0f - k) + ph.refl_g * k;
                const f32 bb = db * (1.0f - k) + ph.refl_b * k;
                // Tone-scale the linear HDR result into 0..255.
                trace_pixels[idx] =
                    pack_rgba8(clamp_u8(r * 255.0f), clamp_u8(g * 255.0f), clamp_u8(bb * 255.0f));
            }
        }

        // ── Pass 3: bilinear upsample low-res into the final FB. ────────
        upsample_bilinear(trace_pixels.data(), kTraceW, kTraceH, final_pixels.data(), kFbW, kFbH);

        // Debug HUD overlay — `r_debug_hud full` enables.
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
            stats.draw_calls = 1;  // single full-screen blit
            stats.triangles = 0;   // raytraced — no rasterized geometry
            stats.active_voices = 0;
            ui::imm::draw_debug_hud(fb, stats);
        }

        ui::console::draw(fb);  // drop-down console (`~`) overlays everything
        window->present(fb);

        if (smoke_frames > 0) {
            const math::Vec3 eye = controller.eye();
            PSY_LOG_INFO("sample_11: frame {} — eye ({:.2f},{:.2f},{:.2f}) traced {}x{}",
                         frame,
                         eye.x,
                         eye.y,
                         eye.z,
                         kTraceW,
                         kTraceH);
        }

        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_11: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             final_pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_11: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_11: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}
