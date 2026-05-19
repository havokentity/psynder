// SPDX-License-Identifier: MIT
// Psynder — heightmap shadow raymarcher impl. Lane 08.
//
// See HeightmapShadow.h for the contract. The march combines:
//   1. A slab clip against the heightmap's overall Y range so we skip
//      empty altitude bands instantly.
//   2. A 2D DDA on the XZ grid that yields the next cell crossing.
//   3. A per-cell test: at each step we evaluate the bilinear surface
//      height at the ray's current XZ position; if the ray's Y dipped
//      below the surface, we report occlusion.
//
// Step size policy: we advance to the next XZ cell boundary, with the
// step capped at a log-growing maximum (8 + t/4 cells), so distant
// columns spend fewer samples. This matches DESIGN.md §9.2 — "logarithmic-
// distance steps".
//
// No allocations. No vtables. Pure scalar code; SIMD widening lives in
// lane 11's outdoor raymarcher render path. This routine is the
// **shadow** path only — a single ray, hit-or-miss.

#include "HeightmapShadow.h"

#include <algorithm>
#include <cmath>

namespace psynder::render::rt {

namespace {

PSY_FORCEINLINE
f32 sample_bilinear(const Heightmap& hm, f32 fx, f32 fz) noexcept {
    // fx, fz are in cell space ([0, width-1], [0, height-1]).
    if (fx < 0.0f) fx = 0.0f;
    if (fz < 0.0f) fz = 0.0f;
    const f32 wmax = static_cast<f32>(hm.width  - 1);
    const f32 hmax = static_cast<f32>(hm.height - 1);
    if (fx > wmax) fx = wmax;
    if (fz > hmax) fz = hmax;

    const u32 i0 = static_cast<u32>(fx);
    const u32 j0 = static_cast<u32>(fz);
    const u32 i1 = (i0 + 1 < hm.width)  ? (i0 + 1) : i0;
    const u32 j1 = (j0 + 1 < hm.height) ? (j0 + 1) : j0;
    const f32 tu = fx - static_cast<f32>(i0);
    const f32 tv = fz - static_cast<f32>(j0);

    const f32 y00 = hm.y_data[j0 * hm.width + i0];
    const f32 y10 = hm.y_data[j0 * hm.width + i1];
    const f32 y01 = hm.y_data[j1 * hm.width + i0];
    const f32 y11 = hm.y_data[j1 * hm.width + i1];

    const f32 yx0 = y00 + (y10 - y00) * tu;
    const f32 yx1 = y01 + (y11 - y01) * tu;
    return yx0 + (yx1 - yx0) * tv;
}

// Tighten [t_in, t_out] toward the part of the ray that could possibly
// touch the heightmap's Y band. We can NEVER cull a ray segment whose Y
// is *below* y_min — the ray is under the surface there → definitely
// occluded. So we only clip the high side: if the ray is entirely above
// y_max throughout [t_in, t_out], it cannot intersect the field; if it
// dives down through y_max, that's the earliest possible crossing.
//
// Behaviour:
//   * Ray segment fully above y_max  → no possible intersection (miss).
//   * Otherwise → leave [t_in, t_out] unchanged (the march will
//     determine the actual hit; clipping the *lower* side would
//     incorrectly skip "starts-below-surface" cases).
PSY_FORCEINLINE
bool clip_y_slab(f32 oy, f32 dy, f32 y_min, f32 y_max,
                 f32 t_in, f32 t_out) noexcept
{
    (void)y_min;
    // Earliest Y on the segment.
    f32 y_lo, y_hi;
    if (std::fabs(dy) < 1e-20f) {
        y_lo = y_hi = oy;
    } else {
        const f32 y_a = oy + dy * t_in;
        const f32 y_b = oy + dy * t_out;
        y_lo = std::fmin(y_a, y_b);
        y_hi = std::fmax(y_a, y_b);
    }
    // If both endpoints are well above y_max, no intersection possible.
    if (y_lo > y_max && y_hi > y_max) return false;
    return true;
}

}  // anonymous namespace

bool trace_heightmap_shadow(const Heightmap& hm, const Ray& ray,
                            u32 max_steps) noexcept
{
    if (!hm.y_data || hm.width < 2 || hm.height < 2 || hm.cell_size <= 0.0f) {
        return false;
    }
    if (ray.t_max <= ray.t_min) return false;

    // Map world XZ to cell space.
    auto world_to_cell_x = [&](f32 wx) noexcept {
        return (wx - hm.origin_xz.x) / hm.cell_size;
    };
    auto world_to_cell_z = [&](f32 wz) noexcept {
        return (wz - hm.origin_xz.y) / hm.cell_size;
    };

    // ─── Initial t window ────────────────────────────────────────────────
    f32 t_in  = ray.t_min;
    f32 t_out = ray.t_max;
    if (!clip_y_slab(ray.origin.y, ray.direction.y,
                     hm.y_min, hm.y_max, t_in, t_out)) {
        return false;
    }
    if (t_out < t_in) return false;

    // ─── March in XZ via DDA. Step until t_out or a hit. ─────────────────
    const f32 dx = ray.direction.x;
    const f32 dz = ray.direction.z;
    const f32 dy = ray.direction.y;

    // If the ray has no horizontal motion, we degenerate to a vertical
    // check at the origin column.
    if (std::fabs(dx) < 1e-20f && std::fabs(dz) < 1e-20f) {
        const f32 cx = world_to_cell_x(ray.origin.x);
        const f32 cz = world_to_cell_z(ray.origin.z);
        const f32 ys = sample_bilinear(hm, cx, cz);
        // Vertical ray: occluded iff the surface intersects (origin.y + t*dy)
        // somewhere in [t_in, t_out]. dy may be 0 — already handled by
        // clip_y_slab returning false above for an empty slab.
        if (std::fabs(dy) < 1e-20f) {
            return ray.origin.y <= ys;
        }
        const f32 t_hit = (ys - ray.origin.y) / dy;
        return (t_hit >= t_in && t_hit <= t_out);
    }

    // Conservative step cap in cell-units: at very low grazing the march
    // crosses a cell every `cell_size / max(|dx|, |dz|)` world units.
    const f32 horiz_mag = std::sqrt(dx*dx + dz*dz);
    const f32 step_world = hm.cell_size * 0.5f / std::max(horiz_mag, 1e-6f);

    f32 t = t_in;
    f32 prev_dy_above = 0.0f;
    bool first = true;
    for (u32 s = 0; s < max_steps && t <= t_out; ++s) {
        // Log-growing step: small near the origin, larger far away.
        const f32 grow = 1.0f + 0.25f * t;
        const f32 step = step_world * grow;

        const f32 wx = ray.origin.x + dx * t;
        const f32 wy = ray.origin.y + dy * t;
        const f32 wz = ray.origin.z + dz * t;

        const f32 cx = world_to_cell_x(wx);
        const f32 cz = world_to_cell_z(wz);

        // If we wandered off the heightmap, terrain doesn't occlude here.
        if (cx < 0.0f || cz < 0.0f ||
            cx > static_cast<f32>(hm.width  - 1) ||
            cz > static_cast<f32>(hm.height - 1)) {
            // No coverage — keep marching but skip the surface test, since
            // an out-of-bounds column has no defined height.
            t += step;
            continue;
        }
        const f32 ys = sample_bilinear(hm, cx, cz);
        const f32 dy_above = wy - ys;
        if (!first) {
            // Crossing detection: ray dipped from above the surface to
            // at-or-below the surface → occluded.
            if (prev_dy_above > 0.0f && dy_above <= 0.0f) {
                return true;
            }
        } else {
            // Starting underground counts as occluded.
            if (dy_above <= 0.0f) return true;
            first = false;
        }
        prev_dy_above = dy_above;

        t += step;
    }

    return false;
}

}  // namespace psynder::render::rt
