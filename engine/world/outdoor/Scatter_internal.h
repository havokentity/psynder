// SPDX-License-Identifier: MIT
// Psynder — internal scatter system (vegetation / props / impostors).
//
// Deterministic instanced billboards (and meshes, in Wave B) seeded from a
// density map. Determinism is non-negotiable: lockstep multiplayer
// (DESIGN.md §9.2) sees the same trees in the same places on every host.
// We use a splitmix64-derived hash keyed on (tile_x, tile_z, slot) so any
// host can reproduce any tile's instances without sharing state.
//
// Wave A produces an array of `ScatterInstance`s; lane 07 draws them as
// billboards (a 2-tri quad each) facing the camera. Wave B adds LOD bands
// (mesh ↔ impostor switch).

#pragma once

#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Terrain.h"

#include "core/Types.h"
#include "math/Math.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace psynder::world::outdoor::detail {

// One scatter instance — a billboard or mesh placed on the terrain.
struct ScatterInstance {
    math::Vec3 position;  // world-space placement (on the terrain surface)
    f32 scale = 1.0f;
    f32 yaw = 0.0f;
    u32 kind = 0;  // index into the scatter asset table
};

// SplitMix64 finalizer. Deterministic, bit-stable across platforms when
// fed the same u64 seed (we don't touch FP in the seed path).
PSY_FORCEINLINE u64 splitmix64(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    u64 z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Hash a (tile, slot) triple into a deterministic u64.
PSY_FORCEINLINE u64 scatter_hash(u32 tile_x, u32 tile_z, u32 slot, u64 seed) noexcept {
    u64 h = seed;
    h = splitmix64(h ^ static_cast<u64>(tile_x));
    h = splitmix64(h ^ static_cast<u64>(tile_z));
    h = splitmix64(h ^ static_cast<u64>(slot));
    return h;
}

// Convert a u64 hash byte to a f32 in [0,1). We avoid FP division by
// flooring 24 bits of the hash and dividing by 2^24 — bit-exact across
// hosts when both compile with IEEE-default rounding (no -ffast-math).
PSY_FORCEINLINE f32 hash_to_unit(u64 h) noexcept {
    const u32 bits = static_cast<u32>(h >> 40) & 0x00FFFFFFu;
    return static_cast<f32>(bits) * (1.0f / 16777216.0f);
}

// Density map: a tile-aligned 8-bit grid giving the placement probability
// per scatter cell. (0 = empty, 255 = full density.) The map is sampled at
// the same texel resolution as the heightmap by default; both backends
// place identical scatter for a given (density, heightmap, seed) triple.
struct DensityMap {
    u32 size_x = 0;
    u32 size_z = 0;
    f32 spacing = 1.0f;  // metres per density cell
    const u8* data = nullptr;
};

PSY_FORCEINLINE u8 sample_density(const DensityMap& d, i32 x, i32 z) noexcept {
    if (!d.data || d.size_x == 0 || d.size_z == 0)
        return 0;
    if (x < 0 || z < 0)
        return 0;
    const u32 ux = static_cast<u32>(x);
    const u32 uz = static_cast<u32>(z);
    if (ux >= d.size_x || uz >= d.size_z)
        return 0;
    return d.data[static_cast<usize>(uz) * d.size_x + ux];
}

// Place instances across a rectangular world-XZ region. Inside each density
// cell, we attempt up to `attempts_per_cell` placements; each placement is
// rolled deterministically and accepted iff a hash byte < density_value.
//
// Output is appended to `out`. Caller-supplied `seed` makes the map's
// scatter a deterministic function of the map asset hash.
inline u32 place_scatter(const HeightmapDesc& h,
                         const DensityMap& d,
                         u32 kind,
                         u32 attempts_per_cell,
                         u64 seed,
                         std::vector<ScatterInstance>& out) {
    if (h.size_x == 0 || h.size_z == 0)
        return 0;
    if (d.size_x == 0 || d.size_z == 0)
        return 0;
    if (attempts_per_cell == 0)
        attempts_per_cell = 1;

    const u32 before = static_cast<u32>(out.size());
    for (u32 z = 0; z < d.size_z; ++z) {
        for (u32 x = 0; x < d.size_x; ++x) {
            const u8 dv = sample_density(d, static_cast<i32>(x), static_cast<i32>(z));
            if (dv == 0)
                continue;
            for (u32 slot = 0; slot < attempts_per_cell; ++slot) {
                const u64 hbits = scatter_hash(x, z, slot, seed);
                // Accept iff the low byte is under the density value
                // (so density=128 → ~50% acceptance per attempt).
                const u8 roll = static_cast<u8>(hbits & 0xFFu);
                if (roll >= dv)
                    continue;

                // Two more hashes for in-cell offset, scale, yaw.
                const f32 ox = hash_to_unit(splitmix64(hbits ^ 0xA1ULL));
                const f32 oz = hash_to_unit(splitmix64(hbits ^ 0xB2ULL));
                const f32 sc = 0.75f + 0.5f * hash_to_unit(splitmix64(hbits ^ 0xC3ULL));
                const f32 yw = (math::kTwoPi)*hash_to_unit(splitmix64(hbits ^ 0xD4ULL));

                const f32 cell_world_x = static_cast<f32>(x) * d.spacing;
                const f32 cell_world_z = static_cast<f32>(z) * d.spacing;
                const f32 wx = cell_world_x + ox * d.spacing;
                const f32 wz = cell_world_z + oz * d.spacing;
                const f32 wy = sample_bilinear(h, wx, wz);

                ScatterInstance inst{};
                inst.position = math::Vec3{wx, wy, wz};
                inst.scale = sc;
                inst.yaw = yw;
                inst.kind = kind;
                out.push_back(inst);
            }
        }
    }
    return static_cast<u32>(out.size()) - before;
}

}  // namespace psynder::world::outdoor::detail
