// SPDX-License-Identifier: MIT
// Psynder — lane-07 internal dynamic-lighting packet and CPU shader helpers.
//
// This is intentionally not part of render/Raster.h's public draw contract.
// Until the scene/render submission lanes expose lights to raster, callers
// cannot fill these packets directly; DrawCmd carries them once raster owns a
// per-frame internal light list.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>

namespace psynder::render::raster {

enum class RasterLightKind : u8 {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

struct RasterLight {
    RasterLightKind kind = RasterLightKind::Directional;
    math::Vec3 position_world{0.0f, 0.0f, 0.0f};
    // Direction the light points toward, in world space. Directional lights
    // illuminate along -direction_world at the surface.
    math::Vec3 direction_world{0.0f, -1.0f, 0.0f};
    math::Vec3 color_linear{1.0f, 1.0f, 1.0f};
    f32 intensity = 1.0f;
    f32 range = 8.0f;
    f32 spot_inner_cos = 0.90f;
    f32 spot_outer_cos = 0.75f;
};

struct RasterLightPacket {
    static constexpr u32 kMaxLights = 16;

    math::Vec3 ambient_linear{1.0f, 1.0f, 1.0f};
    const RasterLight* lights = nullptr;
    u32 light_count = 0;
};

struct RasterMaterialInputs {
    f32 diffuse = 1.0f;
    f32 emissive = 0.0f;
    f32 roughness = 1.0f;
    f32 reflectivity = 0.0f;
    bool receives_dynamic_lights = false;
};

PSY_FORCEINLINE f32 saturate(f32 v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

PSY_FORCEINLINE math::Vec3 add3(math::Vec3 a, math::Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

PSY_FORCEINLINE math::Vec3 mul3(math::Vec3 v, f32 s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

PSY_FORCEINLINE math::Vec3 normalize_or(math::Vec3 v, math::Vec3 fallback) noexcept {
    const f32 len2 = math::dot(v, v);
    if (len2 <= 1.0e-12f)
        return fallback;
    return math::mul(v, 1.0f / std::sqrt(len2));
}

PSY_FORCEINLINE f32 smooth_spot(f32 cd, f32 inner, f32 outer) noexcept {
    if (inner <= outer)
        return cd >= inner ? 1.0f : 0.0f;
    return saturate((cd - outer) / (inner - outer));
}

PSY_FORCEINLINE math::Vec3 evaluate_raster_lighting(const RasterLightPacket& packet,
                                                    const RasterMaterialInputs& material,
                                                    math::Vec3 position_world,
                                                    math::Vec3 normal_world) noexcept {
    if (!material.receives_dynamic_lights || !packet.lights || packet.light_count == 0) {
        const f32 base = material.diffuse + material.emissive;
        return {base, base, base};
    }

    const math::Vec3 n = normalize_or(normal_world, {0.0f, 1.0f, 0.0f});
    math::Vec3 lit = mul3(packet.ambient_linear, material.diffuse);

    const u32 count = std::min(packet.light_count, RasterLightPacket::kMaxLights);
    for (u32 i = 0; i < count; ++i) {
        const RasterLight& light = packet.lights[i];
        math::Vec3 light_dir{0.0f, 1.0f, 0.0f};
        f32 attenuation = 1.0f;

        if (light.kind == RasterLightKind::Directional) {
            light_dir = normalize_or(math::mul(light.direction_world, -1.0f), {0.0f, 1.0f, 0.0f});
        } else {
            const math::Vec3 to_light = math::sub(light.position_world, position_world);
            const f32 dist2 = math::dot(to_light, to_light);
            if (dist2 <= 1.0e-8f)
                continue;
            const f32 dist = std::sqrt(dist2);
            light_dir = math::mul(to_light, 1.0f / dist);
            const f32 range = std::max(light.range, 1.0e-3f);
            const f32 falloff = saturate(1.0f - (dist / range));
            attenuation = falloff * falloff;

            if (light.kind == RasterLightKind::Spot) {
                const math::Vec3 spot_dir = normalize_or(light.direction_world, {0.0f, -1.0f, 0.0f});
                const f32 cd = math::dot(math::mul(light_dir, -1.0f), spot_dir);
                attenuation *= smooth_spot(cd, light.spot_inner_cos, light.spot_outer_cos);
            }
        }

        const f32 ndotl = saturate(math::dot(n, light_dir));
        const f32 strength = ndotl * attenuation * light.intensity * material.diffuse;
        lit = add3(lit,
                   {light.color_linear.x * strength,
                    light.color_linear.y * strength,
                    light.color_linear.z * strength});
    }

    if (material.emissive > 0.0f) {
        lit = add3(lit, {material.emissive, material.emissive, material.emissive});
    }
    return lit;
}

}  // namespace psynder::render::raster
