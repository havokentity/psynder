// SPDX-License-Identifier: MIT
// Psynder — volumetric fog froxel grid + resolve. Lane 09 / Wave B.
//
// CPU-only implementation. Two passes:
//
//   1. populate_fog_grid(): for each of 160*90*64 = 921 600 froxels we walk
//      the active light list, evaluate a point-light radiance term, optionally
//      ask the supplied occluder if the segment is shadowed, and store the
//      pre-scaled in-scatter + extinction in the cell.
//
//   2. apply_volumetric_fog(): ray-march each pixel through the grid using
//      the per-pixel depth as a back-stop, accumulating transmittance ×
//      scatter into the framebuffer.
//
// The grid uses exponential depth slicing so near slices are dense and far
// slices are coarse:
//   z(slice) = near * (far/near) ^ (slice / D)
// This matches the §8.4 budget of ~1.5 ms — most of the work happens close
// to the camera where the player can see banding.

#include "Post.h"
#include "VolumetricFog.h"
#include "Internal.h"

#include "core/console/Console.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psynder::render::post {

namespace {

PSY_CVAR(r_fog_enable,
    "1",
    "Volumetric fog enable (0/1).",
    console::CVAR_ARCHIVE);

PSY_CVAR(r_fog_density,
    "1.0",
    "Volumetric fog density multiplier (0..N).",
    console::CVAR_ARCHIVE);

f32 cvar_density(f32 fallback) noexcept {
    if (const auto* cv = console::Console::Get().FindCVar("r_fog_density")) {
        const f32 v = cv->GetFloat();
        return v >= 0.0f ? v : fallback;
    }
    return fallback;
}

bool cvar_enable() noexcept {
    if (const auto* cv = console::Console::Get().FindCVar("r_fog_enable")) {
        return cv->GetInt() != 0;
    }
    return true;
}

// ─── Slice geometry ───────────────────────────────────────────────────────
// Exponential slicing in [near, far]. Slice `s` covers
// [z(s), z(s+1)) where z(s) = near * (far/near)^(s/D).
PSY_FORCEINLINE f32 slice_to_z(u32 slice, f32 near_z, f32 far_z) noexcept {
    const f32 t = static_cast<f32>(slice) / static_cast<f32>(kFroxelD);
    return near_z * std::pow(far_z / near_z, t);
}

// Inverse — depth in metres → continuous slice index.
PSY_FORCEINLINE f32 z_to_slice(f32 z, f32 near_z, f32 far_z) noexcept {
    if (z <= near_z) return 0.0f;
    if (z >= far_z)  return static_cast<f32>(kFroxelD);
    const f32 ratio = std::log(z / near_z) / std::log(far_z / near_z);
    return ratio * static_cast<f32>(kFroxelD);
}

// World-space centre of a froxel cell.
math::Vec3 froxel_centre(const FogScene& s, u32 fx, u32 fy, u32 fz) noexcept
{
    // Place the cell at the centre of its tile + slice.
    const f32 u = (static_cast<f32>(fx) + 0.5f) / static_cast<f32>(kFroxelW);
    const f32 v = (static_cast<f32>(fy) + 0.5f) / static_cast<f32>(kFroxelH);
    const f32 z_near = slice_to_z(fz,     s.near_z, s.far_z);
    const f32 z_far  = slice_to_z(fz + 1, s.near_z, s.far_z);
    const f32 z      = 0.5f * (z_near + z_far);

    const f32 half_h = std::tan(s.fov_y_rad * 0.5f) * z;
    const f32 half_w = half_h * s.aspect;
    const f32 ndc_x  = u * 2.0f - 1.0f;
    const f32 ndc_y  = 1.0f - v * 2.0f;

    return math::Vec3{
        s.camera_position.x + s.camera_forward.x * z
            + s.camera_right.x * ndc_x * half_w
            + s.camera_up.x    * ndc_y * half_h,
        s.camera_position.y + s.camera_forward.y * z
            + s.camera_right.y * ndc_x * half_w
            + s.camera_up.y    * ndc_y * half_h,
        s.camera_position.z + s.camera_forward.z * z
            + s.camera_right.z * ndc_x * half_w
            + s.camera_up.z    * ndc_y * half_h,
    };
}

// Per-cell radiance accumulator: sum over lights of (colour × atten ×
// visibility). Phase is left isotropic for Wave B (Henyey-Greenstein lands
// when lane 11 ships sky scattering in Wave D).
math::Vec3 in_scatter_at(const math::Vec3& world_pos,
                         const FogScene& s,
                         f32 light_step_size) noexcept
{
    math::Vec3 out = s.ambient;
    for (u32 li = 0; li < s.light_count; ++li) {
        const FogLight& l = s.lights[li];
        const math::Vec3 to_light = math::sub(l.position, world_pos);
        const f32 dist2 = math::dot(to_light, to_light);
        // Distance falloff. For radius>0 we use a smooth Hermite window.
        f32 atten;
        if (l.radius > 0.0f) {
            const f32 r2 = l.radius * l.radius;
            if (dist2 >= r2) continue;
            const f32 t = std::sqrt(dist2) / l.radius;  // 0..1
            const f32 falloff = 1.0f - t;
            atten = falloff * falloff / std::max(dist2, 1e-4f);
        } else {
            atten = 1.0f / std::max(dist2, 1e-4f);
        }

        // Shadow query. Occluder marches scene geometry between froxel
        // and light. For the unshadowed path the occluder is a no-op.
        if (s.occluder.fn) {
            if (s.occluder(world_pos, l.position)) {
                continue;
            }
        }
        out.x += l.colour.x * atten;
        out.y += l.colour.y * atten;
        out.z += l.colour.z * atten;
    }
    (void)light_step_size;
    return out;
}

}  // namespace

// ─── Public: populate ─────────────────────────────────────────────────────

void populate_fog_grid(FroxelGrid& grid, const FogScene& scene)
{
    // Honour the global cvar — disabled fog clears the grid so the resolve
    // path becomes a fast no-op.
    if (!cvar_enable()) {
        std::memset(grid.cells, 0, sizeof(grid.cells));
        return;
    }
    const f32 density_mul = cvar_density(1.0f);
    const f32 sigma_t = std::max(0.0f, scene.density) * density_mul;

    // Step size between adjacent slices at the centre of the frustum (used
    // by the in-scatter accumulator for distance attenuation).
    const f32 mid_step =
        (scene.far_z - scene.near_z) / static_cast<f32>(kFroxelD);

    for (u32 z = 0; z < kFroxelD; ++z) {
        for (u32 y = 0; y < kFroxelH; ++y) {
            for (u32 x = 0; x < kFroxelW; ++x) {
                const math::Vec3 cw = froxel_centre(scene, x, y, z);
                const math::Vec3 scatter = in_scatter_at(cw, scene, mid_step);
                const usize idx = FroxelGrid::index_of(x, y, z);
                Froxel& cell = grid.cells[idx];
                cell.scatter_r  = scatter.x;
                cell.scatter_g  = scatter.y;
                cell.scatter_b  = scatter.z;
                cell.extinction = sigma_t;
            }
        }
    }
}

// ─── Public: resolve / ray-march ──────────────────────────────────────────

void apply_volumetric_fog(Framebuffer& hdr,
                          const FroxelGrid& grid,
                          const FogScene& scene,
                          const f32* depth_linear,
                          const VolumetricFogParams& params)
{
    if (!params.enabled) return;
    if (!cvar_enable()) return;
    if (!hdr.pixels || hdr.width == 0 || hdr.height == 0) return;

    auto* hdr_pix = reinterpret_cast<detail::HdrPixel*>(hdr.pixels);
    const usize pitch_pix =
        hdr.pitch ? (hdr.pitch / sizeof(detail::HdrPixel)) : hdr.width;

    const u32 w = hdr.width;
    const u32 h = hdr.height;

    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            // Determine the back-stop depth in metres.
            f32 z_back = scene.far_z;
            if (depth_linear) {
                z_back = depth_linear[static_cast<usize>(y) * w + x];
                if (z_back <= 0.0f || z_back > scene.far_z) z_back = scene.far_z;
            }
            if (z_back <= scene.near_z) {
                // Entirely in front of the slice range — no fog contribution.
                continue;
            }

            // Compute the (continuous) start/end slice indices.
            const f32 s_start = 0.0f;
            const f32 s_end   = z_to_slice(z_back, scene.near_z, scene.far_z);
            if (s_end <= s_start) continue;

            // Slice the integration into the integer slice cells, marching
            // front-to-back. Accumulate in_scatter × transmittance.
            const u32 ix = std::min<u32>(
                static_cast<u32>(
                    (static_cast<f32>(x) + 0.5f)
                    / static_cast<f32>(w) * static_cast<f32>(kFroxelW)),
                kFroxelW - 1u);
            const u32 iy = std::min<u32>(
                static_cast<u32>(
                    (static_cast<f32>(y) + 0.5f)
                    / static_cast<f32>(h) * static_cast<f32>(kFroxelH)),
                kFroxelH - 1u);

            const u32 iz_end_capped = std::min<u32>(
                kFroxelD - 1u,
                static_cast<u32>(std::ceil(s_end)));

            f32 trans = 1.0f;
            math::Vec3 acc{0,0,0};

            for (u32 sz = 0; sz <= iz_end_capped; ++sz) {
                // Slice geometry in metres.
                const f32 z0 = slice_to_z(sz,     scene.near_z, scene.far_z);
                const f32 z1 = slice_to_z(sz + 1, scene.near_z, scene.far_z);
                // Effective slab thickness, clipped by the back-stop.
                const f32 thick_far  = std::min(z1, z_back);
                const f32 thick_near = std::max(z0, scene.near_z);
                if (thick_far <= thick_near) continue;
                const f32 dz = thick_far - thick_near;

                const Froxel& cell =
                    grid.cells[FroxelGrid::index_of(ix, iy, sz)];

                // Beer-Lambert: trans through this slab.
                const f32 tau = cell.extinction * dz;
                const f32 transmittance_slab = std::exp(-tau);
                const f32 absorbed = 1.0f - transmittance_slab;

                // In-scatter contribution from this slab is the cell's
                // pre-computed scatter * (1 - trans) * running trans.
                acc.x += cell.scatter_r * absorbed * trans;
                acc.y += cell.scatter_g * absorbed * trans;
                acc.z += cell.scatter_b * absorbed * trans;

                trans *= transmittance_slab;
                if (trans < 1e-3f) break;   // visually opaque, early-out
            }

            // Composite: out_colour = fb * trans + scatter * intensity.
            detail::HdrPixel& p =
                hdr_pix[static_cast<usize>(y) * pitch_pix + x];
            p.r = p.r * trans + acc.x * params.intensity;
            p.g = p.g * trans + acc.y * params.intensity;
            p.b = p.b * trans + acc.z * params.intensity;
            // alpha untouched
        }
    }
}

}  // namespace psynder::render::post
