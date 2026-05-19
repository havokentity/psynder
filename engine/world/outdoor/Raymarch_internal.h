// SPDX-License-Identifier: MIT
// Psynder — internal heightmap raymarcher (Backend B, DESIGN.md §9.2).
//
// "Voxel Space"-style per-column ray-march through a 2D heightfield. For
// each pixel column of the framebuffer we cast a ray on the XZ plane,
// step forward through (x, z) texels, sample the height, and update a
// running horizon — every height that pokes above the horizon paints
// a vertical strip of color.
//
// Wave A ships the SCALAR REFERENCE implementation. The SIMD-real (8 cols
// at once on AVX2, 4 on NEON) variant is sketched at the bottom for Wave B —
// the scalar reference is structured to make that SIMD lift mechanical
// (the inner loop is loop-trivial; the only divergence is "did this column
// already advance its horizon past screen-top"). See §9.2 third bullet.
//
// All ops are header-only so the unit test can verify the surface-hit
// invariant without depending on the lane's static lib (tests/unit
// CMakeLists.txt link set is fixed by the build-system maintainer).

#pragma once

#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <cmath>

namespace psynder::world::outdoor::detail {

// ─── Single-ray heightfield hit ──────────────────────────────────────────
// The fundamental kernel: trace a ray (origin, dir) along XZ through the
// heightfield, stepping `step_metres` at a time, and report the first hit
// where ray-Y dips below terrain-Y. `t_hit_out` is the parameter along the
// ray. Returns false if no hit within `max_t`.
//
// This is the kernel the raymarcher's per-column step uses for color and
// the §8.2 hybrid shadow path uses for occlusion against raymarched maps.
struct RayHit {
    f32        t     = 0.0f;       // along-ray parameter
    math::Vec3 pos   = {0, 0, 0};  // world hit position
    f32        height = 0.0f;      // terrain height at hit
};

PSY_FORCEINLINE bool march_ray(const HeightmapDesc& h,
                               math::Vec3           origin,
                               math::Vec3           dir,
                               f32                  step_metres,
                               f32                  max_t,
                               RayHit&              hit) noexcept {
    if (!h.heights || h.size_x == 0 || h.size_z == 0) return false;
    if (step_metres <= 0.0f) step_metres = h.spacing > 0.0f ? h.spacing : 1.0f;

    // Walk forward, comparing ray-Y vs terrain-Y. When ray-Y crosses
    // terrain-Y (going from above to below), refine with a bisection
    // between the last "above" t and the current "below" t.
    f32 prev_t    = 0.0f;
    f32 prev_ry   = origin.y;
    f32 prev_th   = sample_bilinear(h, origin.x, origin.z);

    if (prev_ry <= prev_th) {
        // Already underground at the origin — that's a hit at t=0.
        hit.t      = 0.0f;
        hit.pos    = origin;
        hit.height = prev_th;
        return true;
    }

    f32 t = step_metres;
    while (t <= max_t) {
        const f32 wx = origin.x + dir.x * t;
        const f32 wy = origin.y + dir.y * t;
        const f32 wz = origin.z + dir.z * t;
        const f32 th = sample_bilinear(h, wx, wz);

        if (wy <= th) {
            // Bisect once for sub-step accuracy. The terrain is bilinear-
            // interpolated, so a single linear refinement reproduces the
            // intersection to within a fraction of a texel — good enough
            // for color shading; lane 13 physics will use a tighter solve.
            const f32 dy_a = prev_ry - prev_th;     // > 0 (above)
            const f32 dy_b = wy     - th;            // <= 0 (below)
            const f32 denom = dy_a - dy_b;
            f32 frac = denom > 0.0f ? (dy_a / denom) : 0.0f;
            frac = clamp_f32(frac, 0.0f, 1.0f);
            const f32 t_hit = prev_t + (t - prev_t) * frac;

            hit.t   = t_hit;
            hit.pos = math::Vec3{
                origin.x + dir.x * t_hit,
                origin.y + dir.y * t_hit,
                origin.z + dir.z * t_hit,
            };
            hit.height = sample_bilinear(h, hit.pos.x, hit.pos.z);
            return true;
        }

        prev_t  = t;
        prev_ry = wy;
        prev_th = th;
        t      += step_metres;
    }
    return false;
}

// ─── Per-column raymarch state (scalar reference) ────────────────────────
// A column's state during the march: running screen-Y horizon, current
// step distance. The Wave B SIMD lift bundles 8 of these into AVX2 lanes.
struct ColumnState {
    i32 horizon_y;        // current top-of-painted row (inclusive)
    u32 column_x;         // framebuffer x
    f32 ray_dir_x;        // unit-ish XZ ray direction (we ignore Y)
    f32 ray_dir_z;
};

// Project a world-space height to screen Y given a camera Y, a vertical
// half-FOV pixel scale, and the framebuffer height. Returns a CLAMPED Y in
// pixel space. Used by the per-column renderer to decide which rows to
// paint each step.
PSY_FORCEINLINE i32 project_y(f32  height_world,
                              f32  camera_y,
                              f32  distance,
                              f32  pixels_per_unit_at_unit_dist,
                              i32  fb_height) noexcept {
    if (distance <= 1e-4f) return fb_height;        // off-screen below
    // Standard pinhole: screen_offset_px = (height - eye_y) * scale / dist.
    const f32 offset_px = (height_world - camera_y) *
                          pixels_per_unit_at_unit_dist / distance;
    // Screen Y goes down; horizon (y == half-height) corresponds to
    // "at camera altitude infinitely far". A higher terrain produces a
    // smaller screen Y (further up the screen).
    const f32 horizon = static_cast<f32>(fb_height) * 0.5f;
    const f32 y       = horizon - offset_px;
    if (y < 0.0f)                                  return 0;
    if (y >= static_cast<f32>(fb_height))          return fb_height - 1;
    return static_cast<i32>(y + 0.5f);
}

// Per-step result: how far the column's horizon moved up, and the color
// painted for that strip. Wave A returns the packed splat color; the
// rasterizer / post pass resolves it through the material atlas later.
struct ColumnStep {
    i32 new_horizon_y;
    u32 strip_top_y;
    u32 strip_bottom_y;
    u32 packed_color;       // splat weights as RGBA8 (re-used as a material id)
    f32 z_distance;         // distance for Z-buffer fill
};

// Advance a single column one logarithmic raymarch step. `t` is the current
// along-ray distance (metres); `dt` is the next step length. This is the
// inner loop of `render_columns` extracted so unit tests can pin individual
// steps and Wave B's SIMD variant can call it 8-wide with identical math.
PSY_FORCEINLINE ColumnStep step_column(const HeightmapDesc& h,
                                       const ColumnState&   col,
                                       math::Vec3           eye,
                                       f32                  t,
                                       i32                  fb_height,
                                       f32                  pixels_per_unit) noexcept {
    ColumnStep out{};
    out.new_horizon_y  = col.horizon_y;
    out.strip_top_y    = 0;
    out.strip_bottom_y = 0;
    out.packed_color   = 0;
    out.z_distance     = t;

    const f32 wx = eye.x + col.ray_dir_x * t;
    const f32 wz = eye.z + col.ray_dir_z * t;
    const f32 hy = sample_bilinear(h, wx, wz);

    const i32 screen_y = project_y(hy, eye.y, t, pixels_per_unit, fb_height);

    if (screen_y < col.horizon_y) {
        out.strip_top_y    = static_cast<u32>(screen_y);
        out.strip_bottom_y = static_cast<u32>(col.horizon_y - 1);
        out.new_horizon_y  = screen_y;

        // Splat-derived color, sampled at the texel under the ray.
        // We snap to the nearest texel for the color (the bilinear pos
        // we already used for height is fine for the silhouette).
        const i32 tx = static_cast<i32>(std::floor(wx / (h.spacing > 0.0f ? h.spacing : 1.0f) + 0.5f));
        const i32 tz = static_cast<i32>(std::floor(wz / (h.spacing > 0.0f ? h.spacing : 1.0f) + 0.5f));
        out.packed_color = pack_splat(splat_at_texel(h, tx, tz));
    }
    return out;
}

// Pick a "good enough" step size that grows with distance — the canonical
// Voxel Space trick that buys infinite view distance for ~free. Each step
// is `base * (1 + dist/falloff)`, so we ramp from sub-texel near the eye
// to many-texel at the horizon.
PSY_FORCEINLINE f32 logstep_size(const HeightmapDesc& h,
                                 f32                  current_t,
                                 f32                  near_step,
                                 f32                  falloff) noexcept {
    const f32 spacing = h.spacing > 0.0f ? h.spacing : 1.0f;
    if (near_step <= 0.0f) near_step = spacing * 0.5f;
    if (falloff   <= 0.0f) falloff   = 64.0f * spacing;
    return near_step * (1.0f + current_t / falloff);
}

// ─── Wave-B SIMD note ────────────────────────────────────────────────────
// The SIMD lift packs 8 ColumnStates into one AVX2 batch:
//   __m256  ray_dir_x_8, ray_dir_z_8;
//   __m256  t_8, dt_8;
//   __m256  hy_8 = simd_sample_bilinear(...);  // gather lanes via VGATHERDPS
//   __m256  screen_y_8 = simd_project_y(...);
//   __m256i horizon_8;
//   __m256  mask_advance = _mm256_cmp_ps(screen_y_8, horizon_8, _CMP_LT_OQ);
//   <conditional strip emit per-lane>
// All math above is branchless / lane-pure; the only divergence is "did
// this lane's column finish (horizon hit top of screen)?", and a single
// movemask suffices to early-out the batch when all 8 are done.

}  // namespace psynder::world::outdoor::detail
