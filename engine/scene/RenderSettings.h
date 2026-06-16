// SPDX-License-Identifier: MIT
// Psynder -- scene-level render settings (render mode, global sun + ambient
// lighting, shadow controls, raytracer quality). Pure POD, trivially copyable,
// serialized in the scene file next to EnvironmentSettings and applied by the
// host each frame. Like EnvironmentSettings this lives in engine/scene with no
// dependency on physics, editor, or render internals.
//
// Colours are packed RGBA8 in the engine's 0xAABBGGRR layout (R in the low
// byte), matching EnvironmentSettings / MaterialDesc / the framebuffer.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace psynder::scene {

// Which viewport renderer the host should drive for the scene. Raster uses the
// software rasterizer (App::render_raster); Raytraced uses the software
// raytracer (editor::render::render_scene_rt). Hybrid is a Phase-A placeholder
// that currently maps to the raytraced path -- true raster+traced-shadow
// compositing is a separate task.
enum class RenderMode : u8 {
    Raster = 0,
    Raytraced = 1,
    Hybrid = 2,
};

struct RenderSettings {
    RenderMode render_mode = RenderMode::Raster;
    u8 _pad_mode[3] = {};

    // Global directional sun. Defaults to disabled (sun_enabled == 0) so that
    // loading an older scene -- or a scene authored before this struct existed
    // -- shades exactly as before: no extra directional light is synthesized,
    // and the golden raster/RT references are unaffected. Enable it from the
    // Render Settings panel to add a controllable key light.
    u8 sun_enabled = 0u;
    u8 _pad0[3] = {};
    math::Vec3 sun_direction{-0.4f, -0.8f, -0.45f};
    u32 sun_color_rgba8 = 0xFFFFFFFFu;
    f32 sun_intensity = 1.0f;

    // Ambient fill. ambient_intensity scales the ambient_color contribution.
    u32 ambient_color_rgba8 = 0xFF404040u;
    f32 ambient_intensity = 1.0f;

    // Shadow controls. shadows_enabled gates softness/opacity.
    u8 shadows_enabled = 1u;
    u8 _pad1[3] = {};
    f32 shadow_softness = 0.5f;
    f32 shadow_opacity = 0.7f;

    // Raytracer quality knobs. trace_downscale feeds SceneRtOptions; ao /
    // reflection_bounces / samples feed the RT FrameRenderConfig.
    u32 rt_trace_downscale = 2u;
    u32 rt_ao = 1u;
    u32 rt_reflection_bounces = 1u;
    u32 rt_samples = 1u;
};

static_assert(std::is_trivially_copyable_v<RenderSettings>,
              "RenderSettings must stay trivially copyable (POD scene state)");

[[nodiscard]] inline math::Vec3 sanitize_render_sun_direction(math::Vec3 dir) noexcept {
    const bool finite =
        std::isfinite(dir.x) && std::isfinite(dir.y) && std::isfinite(dir.z);
    if (!finite)
        return {-0.4f, -0.8f, -0.45f};
    const f32 len2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (len2 <= 1.0e-12f)
        return {-0.4f, -0.8f, -0.45f};
    return dir;
}

[[nodiscard]] inline RenderSettings sanitize_render_settings(RenderSettings s) noexcept {
    if (s.render_mode != RenderMode::Raster && s.render_mode != RenderMode::Raytraced &&
        s.render_mode != RenderMode::Hybrid) {
        s.render_mode = RenderMode::Raster;
    }
    s.sun_enabled = s.sun_enabled ? 1u : 0u;
    s.sun_direction = sanitize_render_sun_direction(s.sun_direction);
    s.sun_intensity = std::isfinite(s.sun_intensity) ? std::max(0.0f, s.sun_intensity) : 1.0f;
    s.ambient_intensity =
        std::isfinite(s.ambient_intensity) ? std::max(0.0f, s.ambient_intensity) : 1.0f;
    s.shadows_enabled = s.shadows_enabled ? 1u : 0u;
    s.shadow_softness =
        std::isfinite(s.shadow_softness) ? std::clamp(s.shadow_softness, 0.0f, 1.0f) : 0.5f;
    s.shadow_opacity =
        std::isfinite(s.shadow_opacity) ? std::clamp(s.shadow_opacity, 0.0f, 1.0f) : 0.7f;
    s.rt_trace_downscale = std::clamp(s.rt_trace_downscale, 1u, 16u);
    s.rt_ao = s.rt_ao ? 1u : 0u;
    s.rt_reflection_bounces = std::min(s.rt_reflection_bounces, 8u);
    s.rt_samples = std::clamp(s.rt_samples, 1u, 64u);
    return s;
}

}  // namespace psynder::scene
