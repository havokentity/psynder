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

// ─── Hybrid shadow occluder (M-HYB; DESIGN.md §8) ────────────────────────
// A borrowed, opaque view of the scene's ray-traceable geometry that the
// fragment stage can fire shadow rays through. The raster lane does NOT link
// the raytracer; instead the host (which links both) supplies an occluder
// pointer plus a trampoline that casts it back to the concrete acceleration
// structure (render::rt::Tlas) and answers an occlusion query. This keeps the
// raster->rt dependency out of the raster core while letting Hybrid mode trace
// genuine shadow rays per pixel per light.
//
// `occluded` returns true when the segment from `origin` along `dir` (a unit
// vector) within [t_min, t_max] hits any occluder. When `occluder == nullptr`
// or `occluded == nullptr` the shadow term is skipped entirely, so the default
// (Raster mode / no occluder) path is byte-identical to pre-M-HYB behaviour.
struct ShadowOccluder {
    using OccludedFn = bool (*)(const void* occluder,
                               math::Vec3 origin,
                               math::Vec3 dir,
                               f32 t_min,
                               f32 t_max) noexcept;

    const void* occluder = nullptr;  // borrowed render::rt::Tlas* (opaque here)
    OccludedFn occluded = nullptr;   // host-supplied trampoline

    // Shadow controls mirrored from scene::RenderSettings.
    f32 opacity = 0.7f;   // 1 => fully black shadow; 0 => no darkening
    f32 softness = 0.5f;  // penumbra width knob (reserved; see samples below)
    u32 samples = 1u;     // shadow rays per light (1 = hard shadow). Quality knob.

    [[nodiscard]] PSY_FORCEINLINE bool active() const noexcept {
        return occluder != nullptr && occluded != nullptr;
    }
};

struct RasterLightPacket {
    static constexpr u32 kMaxLights = 16;

    math::Vec3 ambient_linear{1.0f, 1.0f, 1.0f};
    const RasterLight* lights = nullptr;
    u32 light_count = 0;

    // Optional traced-shadow occluder. Null/inactive in Raster mode; the host
    // fills it only in Hybrid mode so the shadow term never perturbs goldens.
    ShadowOccluder shadow{};
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

// ─── Hybrid shadow term (M-HYB; DESIGN.md §8) ────────────────────────────
// Per-light visibility traced through the borrowed occluder. Returns a scalar
// in [1 - opacity, 1]: 1.0 when the light is fully visible, (1 - opacity) when
// fully occluded. Cost: this is per-pixel-per-light occlusion tracing and runs
// ONLY in Hybrid mode (occluder active) — never on the Raster default path.
//
// Bias: the ray origin is pushed off the surface along the geometric normal by
// `bias` world units so a receiver does not shadow itself (self-intersection
// acne). Directional lights trace toward -direction (clamped t_max large);
// point/spot trace toward the light position with t_max = distance so geometry
// behind the light never occludes.
//
// kSoftDiscHalfAngleRad is the maximum half-angle (radians) of the cone the
// jittered soft-shadow rays spread over at softness == 1. ~5.7 deg gives a
// visibly soft penumbra without flooding the receiver with false self-shadowing.
inline constexpr f32 kShadowNormalBias = 1.0e-3f;
inline constexpr f32 kSoftDiscHalfAngleRad = 0.10f;
inline constexpr u32 kMaxShadowSamples = 64u;

// Common ray setup shared by the hard and soft paths: bias the origin off the
// surface and compute the per-light t_max (so geometry behind a point/spot
// light never occludes). Returns false when the light is effectively at the
// surface (no span to trace). `out_n` is the renormalized surface normal used
// to build the soft-disc basis.
PSY_FORCEINLINE bool setup_shadow_ray(const RasterLight& light,
                                      math::Vec3 position_world,
                                      math::Vec3 normal_world,
                                      math::Vec3 light_dir,
                                      math::Vec3& out_origin,
                                      math::Vec3& out_n,
                                      f32& out_t_max) noexcept {
    const f32 nlen2 = math::dot(normal_world, normal_world);
    const math::Vec3 n = nlen2 > 1.0e-12f ? math::mul(normal_world, 1.0f / std::sqrt(nlen2))
                                           : light_dir;
    out_n = n;
    out_origin = add3(position_world, mul3(n, kShadowNormalBias));

    f32 t_max = 1.0e30f;
    if (light.kind != RasterLightKind::Directional) {
        const math::Vec3 to_light = math::sub(light.position_world, out_origin);
        const f32 dist = std::sqrt(std::max(math::dot(to_light, to_light), 0.0f));
        // Stop just short of the light so a mesh AT the light position does not
        // self-occlude; clamp to the light's range too.
        t_max = std::min(dist, std::max(light.range, 1.0e-3f)) - kShadowNormalBias;
        if (t_max <= kShadowNormalBias)
            return false;  // light is effectively at the surface: no span
    }
    out_t_max = t_max;
    return true;
}

// Deterministic low-discrepancy disc sample (concentric-mapped R2 / radical-
// inverse pair). Indexed ONLY by the sample number -- NO RNG, NO time, no
// per-pixel mutable state -- so the result is bit-stable under -fno-fast-math
// and identical across tiles/threads (the tile loop is already parallel and
// this is a pure function of `s`). Returns a point in the unit disc.
PSY_FORCEINLINE void soft_disc_sample(u32 s, f32& out_u, f32& out_v) noexcept {
    // R2 low-discrepancy sequence (Roberts 2018): additive recurrence with the
    // plastic-constant reciprocals. Fully deterministic, no fmod-of-time.
    constexpr f32 kA1 = 0.7548776662466927f;  // 1 / plastic
    constexpr f32 kA2 = 0.5698402909980532f;  // 1 / plastic^2
    const f32 fs = static_cast<f32>(s) + 0.5f;
    f32 x = kA1 * fs;
    f32 y = kA2 * fs;
    x -= std::floor(x);  // fractional part in [0,1)
    y -= std::floor(y);
    // Concentric (equal-area) map of the unit square to the unit disc so the
    // samples cover the disc evenly rather than clustering at the centre.
    const f32 sx = 2.0f * x - 1.0f;
    const f32 sy = 2.0f * y - 1.0f;
    if (sx == 0.0f && sy == 0.0f) {
        out_u = 0.0f;
        out_v = 0.0f;
        return;
    }
    f32 r, theta;
    if (std::fabs(sx) > std::fabs(sy)) {
        r = sx;
        theta = (math::kPi * 0.25f) * (sy / sx);
    } else {
        r = sy;
        theta = (math::kPi * 0.5f) - (math::kPi * 0.25f) * (sx / sy);
    }
    out_u = r * std::cos(theta);
    out_v = r * std::sin(theta);
}

// Hard (single-ray) visibility. Bit-identical to the pre-soft path: one shadow
// ray straight along light_dir. This is the default (samples == 1) and the
// Raster goldens depend on it being byte-for-byte unchanged.
PSY_FORCEINLINE f32 trace_light_visibility_hard(const ShadowOccluder& shadow,
                                                const RasterLight& light,
                                                math::Vec3 position_world,
                                                math::Vec3 normal_world,
                                                math::Vec3 light_dir) noexcept {
    math::Vec3 origin{}, n{};
    f32 t_max = 1.0e30f;
    if (!setup_shadow_ray(light, position_world, normal_world, light_dir, origin, n, t_max))
        return 1.0f;
    const bool blocked =
        shadow.occluded(shadow.occluder, origin, light_dir, kShadowNormalBias, t_max);
    if (!blocked)
        return 1.0f;
    return saturate(1.0f - shadow.opacity);
}

// Soft (multi-ray) visibility. Traces `shadow.samples` jittered rays toward a
// small disc around light_dir (disc half-angle = softness * kSoftDiscHalfAngle)
// and averages the per-ray visibility, producing a smooth penumbra: a fully-
// lit receiver pixel has every ray clear (-> 1.0), a fully-occluded pixel has
// every ray blocked (-> 1-opacity), and a partially-occluded pixel lands
// strictly between (the penumbra gradient). Deterministic jitter (see
// soft_disc_sample) keeps it golden-stable; the loop is a fixed stack walk
// (no heap, no per-pixel allocation), so it stays parallel-safe.
PSY_FORCEINLINE f32 trace_light_visibility_soft(const ShadowOccluder& shadow,
                                                const RasterLight& light,
                                                math::Vec3 position_world,
                                                math::Vec3 normal_world,
                                                math::Vec3 light_dir) noexcept {
    math::Vec3 origin{}, n{};
    f32 t_max = 1.0e30f;
    if (!setup_shadow_ray(light, position_world, normal_world, light_dir, origin, n, t_max))
        return 1.0f;

    // Orthonormal basis (tangent, bitangent) perpendicular to light_dir so the
    // jitter spreads across the disc facing the light.
    const math::Vec3 up =
        std::fabs(light_dir.y) < 0.999f ? math::Vec3{0.0f, 1.0f, 0.0f} : math::Vec3{1.0f, 0.0f, 0.0f};
    const math::Vec3 tangent = normalize_or(math::cross(up, light_dir), {1.0f, 0.0f, 0.0f});
    const math::Vec3 bitangent = math::cross(light_dir, tangent);

    const f32 radius = saturate(shadow.softness) * kSoftDiscHalfAngleRad;
    const u32 samples = std::clamp(shadow.samples, 1u, kMaxShadowSamples);
    const f32 occluded_value = saturate(1.0f - shadow.opacity);

    f32 visible_sum = 0.0f;
    for (u32 s = 0; s < samples; ++s) {
        f32 du = 0.0f, dv = 0.0f;
        soft_disc_sample(s, du, dv);
        // Perturb light_dir within the cone and renormalize. radius scales the
        // disc offset; small-angle so the dir stays close to the light.
        math::Vec3 dir = add3(light_dir, add3(mul3(tangent, du * radius), mul3(bitangent, dv * radius)));
        dir = normalize_or(dir, light_dir);
        const bool blocked = shadow.occluded(shadow.occluder, origin, dir, kShadowNormalBias, t_max);
        visible_sum += blocked ? occluded_value : 1.0f;
    }
    return visible_sum / static_cast<f32>(samples);
}

// Dispatcher kept for callers that want a single entry point. Picks hard vs
// soft on a const bool so a hot loop can hoist the choice out per draw (see
// evaluate_raster_lighting_hybrid, which does exactly that).
PSY_FORCEINLINE f32 trace_light_visibility(const ShadowOccluder& shadow,
                                           const RasterLight& light,
                                           math::Vec3 position_world,
                                           math::Vec3 normal_world,
                                           math::Vec3 light_dir) noexcept {
    const bool soft = shadow.softness > 0.0f && shadow.samples > 1u;
    return soft ? trace_light_visibility_soft(shadow, light, position_world, normal_world, light_dir)
                : trace_light_visibility_hard(shadow, light, position_world, normal_world, light_dir);
}

// Hybrid lighting: identical to evaluate_raster_lighting EXCEPT each light's
// diffuse contribution is multiplied by a traced visibility term. Ambient and
// emissive are unshadowed. When the occluder is inactive this delegates to the
// plain path so callers can always use it (and Raster mode stays identical).
PSY_FORCEINLINE math::Vec3 evaluate_raster_lighting_hybrid(const RasterLightPacket& packet,
                                                           const RasterMaterialInputs& material,
                                                           math::Vec3 position_world,
                                                           math::Vec3 normal_world) noexcept {
    if (!packet.shadow.active())
        return evaluate_raster_lighting(packet, material, position_world, normal_world);
    if (!material.receives_dynamic_lights || !packet.lights || packet.light_count == 0) {
        const f32 base = material.diffuse + material.emissive;
        return {base, base, base};
    }

    const math::Vec3 n = normalize_or(normal_world, {0.0f, 1.0f, 0.0f});
    math::Vec3 lit = mul3(packet.ambient_linear, material.diffuse);

    // M-HYB soft shadows: hard vs soft is a property of the packet's shadow
    // knobs (softness/samples), constant across every pixel of this draw, so we
    // pick the tracer ONCE here on a const bool. The per-pixel-per-light loop
    // then dispatches without re-evaluating the knobs. samples == 1 (or
    // softness == 0) keeps the single-ray hard path, byte-identical to before.
    const bool soft_shadows = packet.shadow.softness > 0.0f && packet.shadow.samples > 1u;

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
        f32 strength = ndotl * attenuation * light.intensity * material.diffuse;
        // Only pay for shadow rays when the light actually contributes here.
        if (strength > 0.0f) {
            const f32 visibility =
                soft_shadows
                    ? trace_light_visibility_soft(packet.shadow, light, position_world, n, light_dir)
                    : trace_light_visibility_hard(packet.shadow, light, position_world, n, light_dir);
            strength *= visibility;
        }
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
