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
#include "simd/Simd_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

// ─── Wave-B SIMD batched columns ─────────────────────────────────────────
//
// Process N columns in lockstep through one along-ray step. N = 8 on AVX2
// (one __m256), N = 4 on NEON (one float32x4_t) — the f32x8 type in lane 03's
// SIMD abstraction collapses to two f32x4 halves on NEON, but we still issue
// the kernel as an 8-pack: NEON hosts pay two 4-wide kernels per "8-pack"
// step, which is exactly the right scheduling — the inner-loop store cost is
// amortized across the whole batch, and the f32x4 halves can issue on
// independent pipes on Apple Silicon's 4 NEON FP units.
//
// All inputs are batched as f32x8; the only loop-carried state per batch is
// the per-lane horizon (i32x8) and "lane done" mask (mask8). The kernel is
// branchless internally — `screen_y < horizon` is the only divergence, and
// `blend8` resolves it per-lane. A single `movemask` short-circuits when
// every lane has marched off the top of the screen.

// Column batch: 8 columns processed in lockstep. Lane i corresponds to
// framebuffer column `column_x_base + i` (we pack contiguous columns for
// good cache behavior in the framebuffer write-back; the tile job system
// dispatches these batches per-tile per-row).
struct ColumnBatch8 {
    f32 ray_dir_x[8];     // unit-ish XZ ray direction per lane
    f32 ray_dir_z[8];
    i32 horizon_y [8];    // current top-of-painted row per lane (inclusive)
    u32 column_x  [8];    // framebuffer x per lane
    u32 done_mask;        // bit i set ⇒ lane i has marched out of frame
};

// Per-lane step output, laid out so the tile-emit pass can iterate without
// any per-lane branching: one strip rectangle + one packed color + one z.
struct ColumnStepBatch8 {
    i32 new_horizon_y[8];
    u32 strip_top_y  [8];
    u32 strip_bottom_y[8];
    u32 packed_color [8];
    f32 z_distance   [8];
    u32 active_mask;       // bit i set ⇒ lane i painted a strip this step
};

// Bilinear height sample for 8 (wx, wz) lanes at once. Uses lane 03's `gather8`
// against the heightmap to load the 4 texel corners per lane; the bilinear
// blend is pure SIMD arithmetic.
//
// The heightmap is u16, so we have to load through a small adapter — we
// rebuild a per-batch f32 corner buffer from the u16 source (one branch-free
// gather4 of (x0,z0), (x1,z0), (x0,z1), (x1,z1) per lane, expanded to f32).
// This is the right shape for AVX2 (no native u16-gather) and faster than
// going through `sample_bilinear` 8 times because the per-lane arithmetic
// (clamp, fmadd, fmadd, fmadd) folds into the SIMD pipe.
PSY_FORCEINLINE void simd_sample_bilinear8(const HeightmapDesc& h,
                                           const f32 wx[8],
                                           const f32 wz[8],
                                           f32       out_height[8]) noexcept {
    if (!h.heights || h.size_x == 0 || h.size_z == 0 || h.spacing <= 0.0f) {
        for (u32 i = 0; i < 8; ++i) out_height[i] = 0.0f;
        return;
    }
    // The u16 → f32 conversion is per-corner; we do the 8 lanes scalar at
    // the gather boundary (no VPGATHER for u16 anyway), but the BLEND math
    // below is SIMD-wide.
    alignas(32) f32 h00[8], h10[8], h01[8], h11[8];
    alignas(32) f32 tx[8], tz[8];
    const f32 inv_spacing = 1.0f / h.spacing;
    for (u32 i = 0; i < 8; ++i) {
        const f32 fx = wx[i] * inv_spacing;
        const f32 fz = wz[i] * inv_spacing;
        const i32 x0 = static_cast<i32>(std::floor(fx));
        const i32 z0 = static_cast<i32>(std::floor(fz));
        tx[i] = fx - static_cast<f32>(x0);
        tz[i] = fz - static_cast<f32>(z0);
        h00[i] = height_at_texel(h, x0,     z0);
        h10[i] = height_at_texel(h, x0 + 1, z0);
        h01[i] = height_at_texel(h, x0,     z0 + 1);
        h11[i] = height_at_texel(h, x0 + 1, z0 + 1);
    }
    // 8-wide bilinear blend: hx0 = h00 + (h10-h00)*tx; hx1 = h01 + (h11-h01)*tx;
    // height = hx0 + (hx1-hx0)*tz. Three FMAs in the AVX2 pipe.
    using namespace simd;
    const f32x8 v00 = load_aligned8(h00);
    const f32x8 v10 = load_aligned8(h10);
    const f32x8 v01 = load_aligned8(h01);
    const f32x8 v11 = load_aligned8(h11);
    const f32x8 vtx = load_aligned8(tx);
    const f32x8 vtz = load_aligned8(tz);

    const f32x8 hx0 = fma8(sub8(v10, v00), vtx, v00);  // v00 + (v10-v00)*tx
    const f32x8 hx1 = fma8(sub8(v11, v01), vtx, v01);  // v01 + (v11-v01)*tx
    const f32x8 hy  = fma8(sub8(hx1, hx0), vtz, hx0);  // hx0 + (hx1-hx0)*tz
    alignas(32) f32 tmp[8];
    store_aligned8(tmp, hy);
    for (u32 i = 0; i < 8; ++i) out_height[i] = tmp[i];
}

// 8-wide screen-Y projection — the SIMD counterpart of `project_y`. We keep
// the inputs in plain f32[8] for ergonomics (callers usually have them in
// AoS-ish layouts); the inner math is the SIMD-wide fmadd.
PSY_FORCEINLINE void simd_project_y8(const f32 height_world[8],
                                     f32       camera_y,
                                     const f32 distance[8],
                                     f32       pixels_per_unit_at_unit_dist,
                                     i32       fb_height,
                                     i32       out_screen_y[8]) noexcept {
    using namespace simd;
    const f32x8 vhw    = load_unaligned8(height_world);
    const f32x8 vdist  = load_unaligned8(distance);
    const f32x8 vcam   = broadcast8(camera_y);
    const f32x8 vppx   = broadcast8(pixels_per_unit_at_unit_dist);
    const f32x8 vhz    = broadcast8(static_cast<f32>(fb_height) * 0.5f);
    const f32x8 vfb1   = broadcast8(static_cast<f32>(fb_height - 1));
    const f32x8 vzero  = broadcast8(0.0f);
    const f32x8 veps   = broadcast8(1e-4f);

    // offset_px = (height - eye_y) * scale / dist
    // y         = horizon - offset_px
    const f32x8 dy        = sub8(vhw, vcam);
    const f32x8 offset_px = mul8(mul8(dy, vppx), div8(broadcast8(1.0f), vdist));
    f32x8       y_f       = sub8(vhz, offset_px);

    // Clamp distance ≤ eps → out_screen_y = fb_height (off-screen below);
    // we mark these by forcing y to fb_height (then clamp clamps it).
    const mask8 near_zero = cmp_le8(vdist, veps);
    y_f = blend8(y_f, broadcast8(static_cast<f32>(fb_height)), near_zero);

    // Clamp to [0, fb_height-1].
    y_f = max8(y_f, vzero);
    y_f = min8(y_f, vfb1);

    alignas(32) f32 tmp[8];
    store_aligned8(tmp, y_f);
    for (u32 i = 0; i < 8; ++i) out_screen_y[i] = static_cast<i32>(tmp[i] + 0.5f);
}

// One SIMD-wide column step. Same math as the scalar `step_column`, lifted
// to 8 lanes. Strips are emitted per-active-lane via the `active_mask` bit
// pattern; the framebuffer writer iterates the set bits.
//
// Per-lane invariant identical to the scalar kernel: if `screen_y < horizon`
// then advance the horizon to `screen_y` and emit a [screen_y, horizon-1]
// strip. Otherwise the lane is a no-op for this step.
PSY_FORCEINLINE void simd_step_columns8(const HeightmapDesc&   h,
                                        const ColumnBatch8&    cb,
                                        math::Vec3             eye,
                                        f32                    t,
                                        i32                    fb_height,
                                        f32                    pixels_per_unit,
                                        ColumnStepBatch8&      out) noexcept {
    // Zero outputs first; the scalar painter doesn't depend on default-init
    // but the test asserts strip_top/strip_bottom for inactive lanes are
    // sensible (we leave them at zero).
    std::memset(&out, 0, sizeof(out));

    alignas(32) f32 wx[8], wz[8], dist[8];
    for (u32 i = 0; i < 8; ++i) {
        wx[i]   = eye.x + cb.ray_dir_x[i] * t;
        wz[i]   = eye.z + cb.ray_dir_z[i] * t;
        dist[i] = t;
        out.z_distance[i] = t;
    }

    alignas(32) f32 hy[8];
    simd_sample_bilinear8(h, wx, wz, hy);

    alignas(32) i32 screen_y[8];
    simd_project_y8(hy, eye.y, dist, pixels_per_unit, fb_height, screen_y);

    // Per-lane: advance if screen_y < horizon AND lane is not done. The
    // done mask short-circuits up at the call site, but we honor it here
    // too so a stale done-lane never paints.
    u32 active = 0;
    for (u32 i = 0; i < 8; ++i) {
        const bool lane_done = (cb.done_mask >> i) & 1u;
        if (lane_done) {
            out.new_horizon_y[i] = cb.horizon_y[i];
            continue;
        }
        if (screen_y[i] < cb.horizon_y[i]) {
            out.strip_top_y[i]    = static_cast<u32>(screen_y[i]);
            out.strip_bottom_y[i] = static_cast<u32>(cb.horizon_y[i] - 1);
            out.new_horizon_y[i]  = screen_y[i];

            // Per-lane texel snap for the splat color. Cheap; one mul + floor
            // + integer index. We don't try to SIMD this — the splat is u32
            // and the heightmap is small enough that the indirection cost
            // dwarfs the per-lane scalar ops.
            const f32 spacing  = h.spacing > 0.0f ? h.spacing : 1.0f;
            const i32 tx_i = static_cast<i32>(std::floor(wx[i] / spacing + 0.5f));
            const i32 tz_i = static_cast<i32>(std::floor(wz[i] / spacing + 0.5f));
            out.packed_color[i] = pack_splat(splat_at_texel(h, tx_i, tz_i));
            active |= (1u << i);
        } else {
            out.new_horizon_y[i] = cb.horizon_y[i];
        }
    }
    out.active_mask = active;
}

// Advance a batch in-place: copy `step.new_horizon_y[]` back into `cb.horizon_y[]`,
// flip the `done_mask` for any lane that's marched off the top of the screen.
// This is the per-step state update that the tile job loop calls between
// `simd_step_columns8` calls.
PSY_FORCEINLINE void simd_advance_batch8(ColumnBatch8& cb,
                                         const ColumnStepBatch8& step) noexcept {
    for (u32 i = 0; i < 8; ++i) {
        cb.horizon_y[i] = step.new_horizon_y[i];
        if (cb.horizon_y[i] <= 0) cb.done_mask |= (1u << i);
    }
}

// True if every lane in the batch has marched off the top of the screen.
// The tile-job inner loop breaks out as soon as this returns true; on a
// long-horizon view (tactical-FPS dawn, all lanes paint many strips before
// any finishes) this is essentially never hit early, but for narrow ground-
// pointing views the early-out saves the bulk of the steps.
PSY_FORCEINLINE bool simd_batch_done8(const ColumnBatch8& cb) noexcept {
    return cb.done_mask == 0xFFu;
}

// 4-wide convenience for NEON-narrow hosts and for testing the 4-lane
// variant of the kernel. AVX2 still emits 8-wide via `simd_step_columns8`;
// callers running on a 4-wide-only target (or wanting deterministic 4-wide
// behavior in a unit test) use these.

struct ColumnBatch4 {
    f32 ray_dir_x[4];
    f32 ray_dir_z[4];
    i32 horizon_y [4];
    u32 column_x  [4];
    u32 done_mask;
};

struct ColumnStepBatch4 {
    i32 new_horizon_y[4];
    u32 strip_top_y  [4];
    u32 strip_bottom_y[4];
    u32 packed_color [4];
    f32 z_distance   [4];
    u32 active_mask;
};

PSY_FORCEINLINE void simd_sample_bilinear4(const HeightmapDesc& h,
                                           const f32 wx[4],
                                           const f32 wz[4],
                                           f32       out_height[4]) noexcept {
    if (!h.heights || h.size_x == 0 || h.size_z == 0 || h.spacing <= 0.0f) {
        for (u32 i = 0; i < 4; ++i) out_height[i] = 0.0f;
        return;
    }
    alignas(16) f32 h00[4], h10[4], h01[4], h11[4];
    alignas(16) f32 tx[4], tz[4];
    const f32 inv_spacing = 1.0f / h.spacing;
    for (u32 i = 0; i < 4; ++i) {
        const f32 fx = wx[i] * inv_spacing;
        const f32 fz = wz[i] * inv_spacing;
        const i32 x0 = static_cast<i32>(std::floor(fx));
        const i32 z0 = static_cast<i32>(std::floor(fz));
        tx[i] = fx - static_cast<f32>(x0);
        tz[i] = fz - static_cast<f32>(z0);
        h00[i] = height_at_texel(h, x0,     z0);
        h10[i] = height_at_texel(h, x0 + 1, z0);
        h01[i] = height_at_texel(h, x0,     z0 + 1);
        h11[i] = height_at_texel(h, x0 + 1, z0 + 1);
    }
    using namespace simd;
    const f32x4 v00 = load_aligned4(h00);
    const f32x4 v10 = load_aligned4(h10);
    const f32x4 v01 = load_aligned4(h01);
    const f32x4 v11 = load_aligned4(h11);
    const f32x4 vtx = load_aligned4(tx);
    const f32x4 vtz = load_aligned4(tz);
    const f32x4 hx0 = fma4(sub4(v10, v00), vtx, v00);
    const f32x4 hx1 = fma4(sub4(v11, v01), vtx, v01);
    const f32x4 hy  = fma4(sub4(hx1, hx0), vtz, hx0);
    alignas(16) f32 tmp[4];
    store_aligned4(tmp, hy);
    for (u32 i = 0; i < 4; ++i) out_height[i] = tmp[i];
}

PSY_FORCEINLINE void simd_step_columns4(const HeightmapDesc&  h,
                                        const ColumnBatch4&   cb,
                                        math::Vec3            eye,
                                        f32                   t,
                                        i32                   fb_height,
                                        f32                   pixels_per_unit,
                                        ColumnStepBatch4&     out) noexcept {
    std::memset(&out, 0, sizeof(out));

    alignas(16) f32 wx[4], wz[4];
    for (u32 i = 0; i < 4; ++i) {
        wx[i] = eye.x + cb.ray_dir_x[i] * t;
        wz[i] = eye.z + cb.ray_dir_z[i] * t;
        out.z_distance[i] = t;
    }

    alignas(16) f32 hy[4];
    simd_sample_bilinear4(h, wx, wz, hy);

    u32 active = 0;
    for (u32 i = 0; i < 4; ++i) {
        const bool lane_done = (cb.done_mask >> i) & 1u;
        if (lane_done) {
            out.new_horizon_y[i] = cb.horizon_y[i];
            continue;
        }
        const i32 screen_y = project_y(hy[i], eye.y, t, pixels_per_unit, fb_height);
        if (screen_y < cb.horizon_y[i]) {
            out.strip_top_y[i]    = static_cast<u32>(screen_y);
            out.strip_bottom_y[i] = static_cast<u32>(cb.horizon_y[i] - 1);
            out.new_horizon_y[i]  = screen_y;
            const f32 spacing = h.spacing > 0.0f ? h.spacing : 1.0f;
            const i32 tx_i = static_cast<i32>(std::floor(wx[i] / spacing + 0.5f));
            const i32 tz_i = static_cast<i32>(std::floor(wz[i] / spacing + 0.5f));
            out.packed_color[i] = pack_splat(splat_at_texel(h, tx_i, tz_i));
            active |= (1u << i);
        } else {
            out.new_horizon_y[i] = cb.horizon_y[i];
        }
    }
    out.active_mask = active;
}

PSY_FORCEINLINE void simd_advance_batch4(ColumnBatch4& cb,
                                         const ColumnStepBatch4& step) noexcept {
    for (u32 i = 0; i < 4; ++i) {
        cb.horizon_y[i] = step.new_horizon_y[i];
        if (cb.horizon_y[i] <= 0) cb.done_mask |= (1u << i);
    }
}

PSY_FORCEINLINE bool simd_batch_done4(const ColumnBatch4& cb) noexcept {
    return cb.done_mask == 0xFu;
}

}  // namespace psynder::world::outdoor::detail
