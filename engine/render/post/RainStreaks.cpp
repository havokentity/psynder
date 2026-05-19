// SPDX-License-Identifier: MIT
// Psynder — rain streaks. Lane 09 / Wave B per DESIGN.md §8.4.
//
// The hot path is render() — it must run after fog (so streaks pick up the
// fog-attenuated framebuffer behind them) and before resolve() (so the
// alpha blend happens in HDR space).
//
// The streak quad is a 1-pixel-wide segment in screen space; we draw it by
// stepping along the projected length and writing alpha-blended HDR pixels.
// This is cheap and produces the desired "vertical hyphen" look. A proper
// thick quad with anti-aliased edges is a Wave-C nice-to-have.

#include "Post.h"
#include "RainStreaks.h"
#include "Internal.h"
#include "VolumetricFog.h"   // FogLight

#include "core/console/Console.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

namespace psynder::render::post {

namespace {

PSY_CVAR(r_rain_enable,
    "1",
    "Rain streaks enable (0/1).",
    console::CVAR_ARCHIVE);

PSY_CVAR(r_rain_intensity,
    "1.0",
    "Rain streak HDR intensity multiplier.",
    console::CVAR_ARCHIVE);

bool cvar_enable() noexcept {
    if (const auto* cv = console::Console::Get().FindCVar("r_rain_enable")) {
        return cv->GetInt() != 0;
    }
    return true;
}

f32 cvar_intensity(f32 fb) noexcept {
    if (const auto* cv = console::Console::Get().FindCVar("r_rain_intensity")) {
        const f32 v = cv->GetFloat();
        return v >= 0.0f ? v : fb;
    }
    return fb;
}

// Compute the lit HDR colour for a single streak point. We sum lights with
// inverse-square attenuation (same shape as fog so the look is coherent).
math::Vec3 light_streak(const math::Vec3& p,
                        const FogScene& scene) noexcept
{
    // Start with a baseline silvery tint reflecting the night-rain feel +
    // the scene ambient (so unlit drops still read).
    math::Vec3 c{
        0.05f + scene.ambient.x,
        0.06f + scene.ambient.y,
        0.10f + scene.ambient.z
    };
    for (u32 i = 0; i < scene.light_count; ++i) {
        const FogLight& l = scene.lights[i];
        const math::Vec3 d = math::sub(l.position, p);
        const f32 d2 = math::dot(d, d);
        f32 atten;
        if (l.radius > 0.0f) {
            const f32 r2 = l.radius * l.radius;
            if (d2 >= r2) continue;
            const f32 t = std::sqrt(d2) / l.radius;
            const f32 falloff = 1.0f - t;
            atten = falloff * falloff / std::max(d2, 1e-4f);
        } else {
            atten = 1.0f / std::max(d2, 1e-4f);
        }
        c.x += l.colour.x * atten;
        c.y += l.colour.y * atten;
        c.z += l.colour.z * atten;
    }
    return c;
}

// Project a world position through a FogScene-style camera into screen space.
// Returns true when the projected point is in front of the near plane and
// inside the viewport; writes pixel coords + linear z.
bool project_to_screen(const math::Vec3& world_p,
                       const FogScene& scene,
                       u32 fb_w, u32 fb_h,
                       f32& out_sx, f32& out_sy, f32& out_z) noexcept
{
    const math::Vec3 to_p = math::sub(world_p, scene.camera_position);
    const f32 z = math::dot(to_p, scene.camera_forward);
    if (z <= scene.near_z) return false;
    if (z >= scene.far_z)  return false;
    const f32 half_h = std::tan(scene.fov_y_rad * 0.5f) * z;
    const f32 half_w = half_h * scene.aspect;
    const f32 rx = math::dot(to_p, scene.camera_right) / half_w;
    const f32 ry = math::dot(to_p, scene.camera_up)    / half_h;
    if (rx < -1.0f || rx > 1.0f) return false;
    if (ry < -1.0f || ry > 1.0f) return false;
    out_sx = (rx * 0.5f + 0.5f) * static_cast<f32>(fb_w);
    out_sy = (0.5f - ry * 0.5f) * static_cast<f32>(fb_h);
    out_z  = z;
    return true;
}

// Bresenham-like alpha-blended HDR line draw. Endpoints are float pixel
// coordinates; the line is sampled along its longer axis, snapping to integer
// pixels. We don't AA the edges — Wave C's job.
void draw_streak_line(detail::HdrPixel* px, usize pitch_pix,
                      u32 w, u32 h,
                      f32 x0, f32 y0, f32 x1, f32 y1,
                      math::Vec3 colour, f32 alpha) noexcept
{
    const f32 dx = x1 - x0;
    const f32 dy = y1 - y0;
    const f32 len = std::max(std::abs(dx), std::abs(dy));
    if (len <= 0.0f) {
        // Degenerate streak — single-pixel splat at (x0,y0) if in bounds.
        const i32 ix = static_cast<i32>(x0);
        const i32 iy = static_cast<i32>(y0);
        if (ix < 0 || iy < 0
            || ix >= static_cast<i32>(w) || iy >= static_cast<i32>(h)) return;
        auto& p = px[static_cast<usize>(iy) * pitch_pix + static_cast<usize>(ix)];
        const f32 a = std::clamp(alpha, 0.0f, 1.0f);
        p.r = p.r * (1.0f - a) + colour.x * a;
        p.g = p.g * (1.0f - a) + colour.y * a;
        p.b = p.b * (1.0f - a) + colour.z * a;
        return;
    }
    const i32 steps = static_cast<i32>(std::ceil(len));
    const f32 step_x = dx / static_cast<f32>(steps);
    const f32 step_y = dy / static_cast<f32>(steps);
    for (i32 i = 0; i <= steps; ++i) {
        const f32 fx = x0 + step_x * static_cast<f32>(i);
        const f32 fy = y0 + step_y * static_cast<f32>(i);
        const i32 ix = static_cast<i32>(fx);
        const i32 iy = static_cast<i32>(fy);
        if (ix < 0 || iy < 0
            || ix >= static_cast<i32>(w) || iy >= static_cast<i32>(h)) continue;
        auto& p = px[static_cast<usize>(iy) * pitch_pix + static_cast<usize>(ix)];
        const f32 a = std::clamp(alpha, 0.0f, 1.0f);
        p.r = p.r * (1.0f - a) + colour.x * a;
        p.g = p.g * (1.0f - a) + colour.y * a;
        p.b = p.b * (1.0f - a) + colour.z * a;
    }
}

}  // namespace

// ─── StreakSystem internals ──────────────────────────────────────────────

inline u32 StreakSystem::next_u32() noexcept {
    // xorshift32
    u32 x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

inline f32 StreakSystem::unit() noexcept {
    return static_cast<f32>(next_u32() & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

inline f32 StreakSystem::signed_unit() noexcept {
    return unit() * 2.0f - 1.0f;
}

void StreakSystem::spawn_one(Streak& s, const RainParams& params) noexcept {
    s.position = math::Vec3{
        params.spawn_centre.x + signed_unit() * params.spawn_extent.x,
        params.spawn_centre.y,
        params.spawn_centre.z + signed_unit() * params.spawn_extent.z,
    };
    s.velocity = math::Vec3{
        params.base_velocity.x + signed_unit() * params.jitter_velocity.x,
        params.base_velocity.y + unit() * params.jitter_velocity.y,
        params.base_velocity.z + signed_unit() * params.jitter_velocity.z,
    };
    s.length = params.streak_length;
    s.alpha  = params.streak_alpha;
}

// ─── Public: seed / step / render ────────────────────────────────────────

void StreakSystem::seed(u32 count, const RainParams& params) {
    rng_   = params.rng_seed;
    count_ = std::min(count, kMaxStreaks);
    for (u32 i = 0; i < count_; ++i) {
        spawn_one(pool_[i], params);
        // Distribute initial y across the column so the first frame looks
        // continuous instead of a wall of drops at the spawn plane.
        const f32 drop_t = unit();   // 0..1
        const f32 y_top  = params.spawn_centre.y;
        const f32 y_bot  = params.kill_height_y;
        pool_[i].position.y = y_top * (1.0f - drop_t) + y_bot * drop_t;
    }
}

void StreakSystem::step(f32 dt, const RainParams& params) {
    if (!params.enabled) return;
    for (u32 i = 0; i < count_; ++i) {
        Streak& s = pool_[i];
        s.position.x += s.velocity.x * dt;
        s.position.y += s.velocity.y * dt;
        s.position.z += s.velocity.z * dt;
        if (s.position.y < params.kill_height_y) {
            spawn_one(s, params);
        }
    }
}

void StreakSystem::render(Framebuffer& hdr,
                          const FogScene& camera,
                          const RainParams& params,
                          const f32* depth_linear) const
{
    if (!params.enabled) return;
    if (!cvar_enable()) return;
    if (!hdr.pixels || hdr.width == 0 || hdr.height == 0) return;
    auto* hdr_pix = reinterpret_cast<detail::HdrPixel*>(hdr.pixels);
    const usize pitch_pix =
        hdr.pitch ? (hdr.pitch / sizeof(detail::HdrPixel)) : hdr.width;
    const u32 w = hdr.width;
    const u32 h = hdr.height;
    const f32 intensity = params.intensity * cvar_intensity(1.0f);
    if (intensity <= 0.0f) return;

    for (u32 i = 0; i < count_; ++i) {
        const Streak& s = pool_[i];

        // Head of streak (current position). Tail is one streak-length
        // back along its current velocity vector.
        const math::Vec3 head = s.position;
        const math::Vec3 vel  = s.velocity;
        const f32 vmag = std::sqrt(math::dot(vel, vel));
        if (vmag <= 1e-4f) continue;
        const math::Vec3 tail{
            head.x - vel.x / vmag * s.length,
            head.y - vel.y / vmag * s.length,
            head.z - vel.z / vmag * s.length,
        };

        f32 hsx, hsy, hsz;
        f32 tsx, tsy, tsz;
        const bool h_ok = project_to_screen(head, camera, w, h, hsx, hsy, hsz);
        const bool t_ok = project_to_screen(tail, camera, w, h, tsx, tsy, tsz);
        if (!h_ok && !t_ok) continue;
        // Use a single endpoint if the other is clipped.
        if (!h_ok) { hsx = tsx; hsy = tsy; hsz = tsz; }
        if (!t_ok) { tsx = hsx; tsy = hsy; tsz = hsz; }

        // Depth test against the rasterizer's depth buffer at the streak
        // head — if there's geometry in front of the head, skip the streak.
        if (depth_linear) {
            const i32 ix = static_cast<i32>(hsx);
            const i32 iy = static_cast<i32>(hsy);
            if (ix >= 0 && iy >= 0
                && ix < static_cast<i32>(w) && iy < static_cast<i32>(h))
            {
                const f32 gz = depth_linear[
                    static_cast<usize>(iy) * w + static_cast<usize>(ix)];
                if (gz > 0.0f && gz < hsz) continue;
            }
        }

        // Compute lit colour at the streak's midpoint so distant lights
        // contribute the same to both ends without two separate lookups.
        const math::Vec3 mid{
            (head.x + tail.x) * 0.5f,
            (head.y + tail.y) * 0.5f,
            (head.z + tail.z) * 0.5f,
        };
        math::Vec3 c = light_streak(mid, camera);
        c.x *= intensity;
        c.y *= intensity;
        c.z *= intensity;

        draw_streak_line(hdr_pix, pitch_pix, w, h,
                         hsx, hsy, tsx, tsy,
                         c, s.alpha);
    }
}

}  // namespace psynder::render::post
