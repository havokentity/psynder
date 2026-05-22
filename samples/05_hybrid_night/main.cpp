// SPDX-License-Identifier: MIT
// Psynder — Sample 05 / M5 demo. Hybrid night scene with raytraced shadows.
//
// The scene: a dark ground plane and five colored cubes scattered around it,
// lit by three orbiting point lights (red / green / blue). Per pixel we
// rasterize the cube's ambient color, then trace shadow rays toward each
// light through a TLAS containing all five cubes + ground. Each shadow ray
// is gathered into an 8-wide ShadowPacket8 and dispatched via
// `psynder::render::rt::trace_shadow_packet`. The skybox is a vertical
// gradient.
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

// ─── Skybox / background ─────────────────────────────────────────────────
// Vertical gradient — deep navy at the horizon, fading to nearly black
// overhead. Sample by ray direction Y.
u32 sample_sky(math::Vec3 dir) {
    const f32 t = 0.5f * (dir.y + 1.0f);  // 0..1
    // Horizon (t small): slightly brighter navy. Zenith (t large): black.
    const f32 horizon[3] = {6.0f, 10.0f, 28.0f};
    const f32 zenith[3] = {1.0f, 1.0f, 6.0f};
    const f32 r = horizon[0] * (1.0f - t) + zenith[0] * t;
    const f32 g = horizon[1] * (1.0f - t) + zenith[1] * t;
    const f32 b = horizon[2] * (1.0f - t) + zenith[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

}  // namespace

int main(int argc, char** argv) {
    const app::AppArgs args = app::parse_common_args(argc, argv).args;
    const u32 smoke_frames = args.smoke_frames;
    render::rt::ensure_frame_scheduler_console_registered();

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 05 (hybrid night, RT shadows)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;

    app::WindowApp app_host{args, desc};
    if (!app_host) {
        PSY_LOG_ERROR("sample_05: failed to create window");
        return EXIT_FAILURE;
    }
    auto* window = &app_host.window();

    // ── Build the static scene geometry. ────────────────────────────────
    // One Bvh8 per cube + one Bvh8 for the ground. Each cube BLAS holds
    // the 12 unit-cube triangles; the per-cube transform places + scales
    // it via the TLAS InstanceDesc.
    std::array<CubeInstance, kNumCubes> cube_instances = make_cube_instances();

    std::vector<render::rt::Triangle> cube_tris;
    emit_unit_cube(cube_tris);

    std::vector<render::rt::Triangle> ground_tris;
    emit_ground(ground_tris, /*half=*/8.0f);

    render::rt::Bvh8 cube_blas;
    cube_blas.build(cube_tris.data(), static_cast<u32>(cube_tris.size()));
    render::rt::Bvh8 ground_blas;
    ground_blas.build(ground_tris.data(), static_cast<u32>(ground_tris.size()));

    // TLAS: 5 cubes + 1 ground.
    std::array<render::rt::Tlas::InstanceDesc, kNumCubes + 1> insts{};
    for (u32 i = 0; i < kNumCubes; ++i) {
        insts[i].blas = &cube_blas;
        insts[i].transform = mat4_trs(cube_instances[i].center, cube_instances[i].size);
    }
    insts[kNumCubes].blas = &ground_blas;
    insts[kNumCubes].transform = math::identity4();

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    std::vector<u32>& final_pixels = app_host.pixels();
    render::Framebuffer& fb = app_host.framebuffer();
    std::vector<u32> shadow_pixels(static_cast<usize>(kShadowW) * kShadowH, 0u);

    PSY_LOG_INFO("Psynder sample 05 running{}",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{});

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
    // `u32`-sized to keep `min(frame+1, kFrameHistory)` in a single
    // domain (mirrors the pattern from PR #115's self-review).
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

        // ESC quits — unless the console is open, where Esc closes it instead.
        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
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
        const render::rt::FrameRowScheduleConfig row_schedule =
            render::rt::frame_row_schedule_config_from_console();

        // ── Pass 1: primary visibility @ low-res (256×144). ─────────────
        // Cast one primary ray per low-res pixel against the TLAS and
        // stash the hit. Pixels that miss draw the sky immediately.
        render::rt::parallel_frame_rows(kShadowH, row_schedule, [&](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = 0; x < kShadowW; ++x) {
                    const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kShadowW);
                    const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kShadowH);
                    const render::rt::Ray ray = render::rt::primary_ray(cam, nx, ny);
                    const render::rt::Hit h = tlas.intersect(ray);

                    const usize idx = static_cast<usize>(y) * kShadowW + x;
                    PixelHit ph{};
                    ph.hit = h.hit;
                    if (!h.hit) {
                        shadow_pixels[idx] = sample_sky(ray.direction);
                    } else {
                        // Surface color: cube[idx] tint, or dim grey for ground.
                        // We don't know which instance was hit without
                        // h.instance, so we use it directly.
                        u32 color = pack_rgba8(60, 60, 70);  // ground default
                        if (h.instance < kNumCubes) {
                            color = cube_instances[h.instance].color;
                        }
                        ph.r = static_cast<f32>(color & 0xFFu) / 255.0f;
                        ph.g = static_cast<f32>((color >> 8) & 0xFFu) / 255.0f;
                        ph.b = static_cast<f32>((color >> 16) & 0xFFu) / 255.0f;
                        // Hit position with a small ray-direction back-off so
                        // shadow-ray origins don't self-intersect.
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
        });

        // ── Pass 2: per-pixel shadow trace (packets of 8). ──────────────
        // For each light, fill ShadowPacket8 with 8 successive hit-pixels'
        // shadow rays, dispatch via trace_shadow_packet, accumulate the
        // lit contribution per pixel.
        std::vector<f32> accum(static_cast<usize>(kShadowW) * kShadowH * 3, 0.0f);

        for (u32 li = 0; li < kNumLights; ++li) {
            const Light& L = lights[li];
            render::rt::parallel_frame_rows(kShadowH, row_schedule, [&](u32 y0, u32 y1) {
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
                        const f32 atten = 1.0f / (1.0f + d * d * 0.08f);
                        const f32 k = ndotl * atten * L.intensity;
                        accum[px * 3 + 0] += ph.r * L.r * k;
                        accum[px * 3 + 1] += ph.g * L.g * k;
                        accum[px * 3 + 2] += ph.b * L.b * k;
                    }
                    in_pkt = 0;
                };

                const usize first_px = static_cast<usize>(y0) * kShadowW;
                const usize last_px = static_cast<usize>(y1) * kShadowW;
                for (usize px = first_px; px < last_px; ++px) {
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
            });
        }

        // ── Combine: write final low-res lit color. ─────────────────────
        render::rt::parallel_frame_rows(kShadowH, row_schedule, [&](u32 y0, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
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
        });

        // ── Pass 3: bilinear upsample low-res into the final FB. ────────
        render::rt::upsample_bilinear_rgba8(shadow_pixels.data(),
                                            kShadowW,
                                            kShadowH,
                                            final_pixels.data(),
                                            kFbW,
                                            kFbH);

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

        ui::console::draw(fb);  // drop-down console (`~`) overlays everything
        window->present(fb);

        // Bump unconditionally each iteration so the frame_ms_ring populates
        // in interactive runs (smoke_frames == 0) too — otherwise the ring
        // sits at index 0 forever and avg_frame_ms collapses to frame_ms.
        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_05: smoke target reached ({}); exiting", smoke_frames);
            break;
        }
    }

    const bool capture_ok = app_host.write_capture_if_requested("sample_05");

    return capture_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
