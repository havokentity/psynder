// SPDX-License-Identifier: MIT
// Psynder — internal header. Heightmap sculpt brushes for the editor
// (raise / lower / smooth / flatten + 4-weight splat paint). Works for
// both terrain backends (mesh CDLOD + heightmap raymarch) — DESIGN.md §10.8.
//
// Header-only: the algorithms are pure functions on a heightfield + a
// vertex-weight grid. The Editor.h facade calls into these; tests can
// drive them directly without linking psynder_editor_core.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace psynder::editor::sculpt {

// ─── Heightfield model ───────────────────────────────────────────────────
// Editor-side heightfield. The runtime side (lane 11) stores a u16
// heightmap; the editor maintains a f32 mirror for precision during
// sculpt, then quantises on save.
struct Heightfield {
    u32 size_x = 0;
    u32 size_z = 0;
    f32 spacing = 1.0f;          // metres per texel
    math::Vec3 origin{0, 0, 0};  // world coords of texel (0,0)
    std::vector<f32> heights;    // size_x * size_z

    void allocate(u32 sx, u32 sz, f32 spc) {
        size_x = sx;
        size_z = sz;
        spacing = spc;
        heights.assign(static_cast<usize>(sx) * static_cast<usize>(sz), 0.0f);
    }

    PSY_FORCEINLINE bool inbounds(u32 ix, u32 iz) const noexcept {
        return ix < size_x && iz < size_z;
    }
    PSY_FORCEINLINE usize index(u32 ix, u32 iz) const noexcept {
        return static_cast<usize>(iz) * static_cast<usize>(size_x) + static_cast<usize>(ix);
    }
    PSY_FORCEINLINE f32 sample(u32 ix, u32 iz) const noexcept { return heights[index(ix, iz)]; }
    PSY_FORCEINLINE void store(u32 ix, u32 iz, f32 v) noexcept { heights[index(ix, iz)] = v; }
};

// ─── Splat weights (4 channels) ───────────────────────────────────────────
// Each vertex has 4 weights summing to 1.0 (normalised after every paint
// stroke). This matches DESIGN.md §10.8 "4-weight splat paint".
struct SplatGrid {
    u32 size_x = 0;
    u32 size_z = 0;
    std::vector<std::array<f32, 4>> weights;

    void allocate(u32 sx, u32 sz) {
        size_x = sx;
        size_z = sz;
        weights.assign(static_cast<usize>(sx) * static_cast<usize>(sz),
                       std::array<f32, 4>{1.0f, 0.0f, 0.0f, 0.0f});
    }
    PSY_FORCEINLINE usize index(u32 ix, u32 iz) const noexcept {
        return static_cast<usize>(iz) * static_cast<usize>(size_x) + static_cast<usize>(ix);
    }
    PSY_FORCEINLINE std::array<f32, 4>& at(u32 ix, u32 iz) noexcept {
        return weights[index(ix, iz)];
    }
};

// ─── Falloff (smoothstep, normalised over the brush radius) ──────────────
PSY_FORCEINLINE f32 falloff(f32 d, f32 radius) noexcept {
    if (radius <= 0.0f)
        return 0.0f;
    f32 t = std::clamp(1.0f - d / radius, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);  // smoothstep
}

// ─── Walk the brush footprint, calling `f(ix, iz, weight)` per texel ────
template <class F>
inline void for_each_in_brush(const Heightfield& hf, math::Vec3 wp, f32 radius, F&& f) {
    if (radius <= 0.0f || hf.spacing <= 0.0f || hf.size_x == 0 || hf.size_z == 0)
        return;
    const f32 fx = (wp.x - hf.origin.x) / hf.spacing;
    const f32 fz = (wp.z - hf.origin.z) / hf.spacing;
    const f32 rt = radius / hf.spacing;
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(fx - rt)));
    const i32 z0 = std::max(0, static_cast<i32>(std::floor(fz - rt)));
    const i32 x1 = std::min(static_cast<i32>(hf.size_x) - 1, static_cast<i32>(std::ceil(fx + rt)));
    const i32 z1 = std::min(static_cast<i32>(hf.size_z) - 1, static_cast<i32>(std::ceil(fz + rt)));
    for (i32 iz = z0; iz <= z1; ++iz) {
        for (i32 ix = x0; ix <= x1; ++ix) {
            const f32 dx = static_cast<f32>(ix) - fx;
            const f32 dz = static_cast<f32>(iz) - fz;
            const f32 d = std::sqrt(dx * dx + dz * dz) * hf.spacing;
            const f32 w = falloff(d, radius);
            if (w <= 0.0f)
                continue;
            f(static_cast<u32>(ix), static_cast<u32>(iz), w);
        }
    }
}

// ─── Brushes ──────────────────────────────────────────────────────────────
inline void raise(Heightfield& hf, math::Vec3 wp, f32 radius, f32 strength) {
    for_each_in_brush(hf, wp, radius, [&](u32 ix, u32 iz, f32 w) {
        hf.store(ix, iz, hf.sample(ix, iz) + w * strength);
    });
}

inline void lower(Heightfield& hf, math::Vec3 wp, f32 radius, f32 strength) {
    raise(hf, wp, radius, -strength);
}

inline void flatten(Heightfield& hf, math::Vec3 wp, f32 radius, f32 strength) {
    // Strength = how strongly we pull each in-brush vertex toward the
    // brush-centre height; strength=1 is "fully flat".
    if (hf.size_x == 0 || hf.size_z == 0)
        return;
    const f32 fx = (wp.x - hf.origin.x) / hf.spacing;
    const f32 fz = (wp.z - hf.origin.z) / hf.spacing;
    const i32 cx = std::clamp(static_cast<i32>(std::round(fx)), 0, static_cast<i32>(hf.size_x) - 1);
    const i32 cz = std::clamp(static_cast<i32>(std::round(fz)), 0, static_cast<i32>(hf.size_z) - 1);
    const f32 target = hf.sample(static_cast<u32>(cx), static_cast<u32>(cz));
    for_each_in_brush(hf, wp, radius, [&](u32 ix, u32 iz, f32 w) {
        const f32 h = hf.sample(ix, iz);
        hf.store(ix, iz, h + (target - h) * w * strength);
    });
}

inline void smooth(Heightfield& hf, math::Vec3 wp, f32 radius, f32 strength) {
    // 3x3 box blur weighted by brush falloff.
    if (hf.size_x < 3 || hf.size_z < 3)
        return;
    std::vector<std::pair<usize, f32>> writes;
    writes.reserve(64);
    for_each_in_brush(hf, wp, radius, [&](u32 ix, u32 iz, f32 w) {
        if (ix == 0 || iz == 0 || ix + 1 >= hf.size_x || iz + 1 >= hf.size_z)
            return;
        const f32 sum = hf.sample(ix - 1, iz - 1) + hf.sample(ix, iz - 1) +
                        hf.sample(ix + 1, iz - 1) + hf.sample(ix - 1, iz) + hf.sample(ix, iz) +
                        hf.sample(ix + 1, iz) + hf.sample(ix - 1, iz + 1) + hf.sample(ix, iz + 1) +
                        hf.sample(ix + 1, iz + 1);
        const f32 avg = sum / 9.0f;
        const f32 h = hf.sample(ix, iz);
        writes.emplace_back(hf.index(ix, iz), h + (avg - h) * w * strength);
    });
    for (const auto& [idx, v] : writes)
        hf.heights[idx] = v;
}

// ─── Splat paint ──────────────────────────────────────────────────────────
inline void paint(SplatGrid& g,
                  math::Vec3 wp,
                  f32 radius,
                  const Heightfield& sample_grid,
                  u8 material_index,
                  f32 weight) {
    if (g.size_x == 0 || g.size_z == 0)
        return;
    if (material_index >= 4)
        return;
    for_each_in_brush(sample_grid, wp, radius, [&](u32 ix, u32 iz, f32 w) {
        auto& c = g.at(ix, iz);
        c[material_index] = std::clamp(c[material_index] + w * weight, 0.0f, 1.0f);
        // Renormalise so weights sum to 1.0 (preserves visual consistency
        // across the four blend layers).
        const f32 s = c[0] + c[1] + c[2] + c[3];
        if (s > 0.0f) {
            const f32 inv = 1.0f / s;
            c[0] *= inv;
            c[1] *= inv;
            c[2] *= inv;
            c[3] *= inv;
        } else {
            c = {1.0f, 0.0f, 0.0f, 0.0f};
        }
    });
}

}  // namespace psynder::editor::sculpt
