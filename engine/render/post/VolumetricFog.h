// SPDX-License-Identifier: MIT
// Psynder — volumetric / atmospheric fog. Lane 09 / Wave B per DESIGN.md §8.4.
// Sibling header to Post.h (FROZEN).
//
// Implementation shape (per DESIGN §8.4):
//
//   - Low-res 3D froxel grid at the fixed dims 160×90×64. Each froxel stores
//     pre-computed in-scattered radiance + extinction (scalar density).
//   - In-scattering is computed CPU-side per froxel from the active light
//     list. Shadowed contributions are optional: callers supply a callable
//     (`OccluderFn`) that returns true if the light → froxel ray is blocked.
//     The natural binding for that callable is lane 08's `Tlas::occluded`.
//   - The resolve path ray-marches the grid front-to-back, accumulating
//     transmittance × scatter into the HDR framebuffer. Budget: ~1.5 ms at
//     1080p per the design doc.
//
// The grid is exposed so callers can do their own population (e.g. a level
// editor preview that hand-paints fog). The `populate` entry point fills the
// grid from a `FogScene` describing the camera frustum, density, and lights.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"

namespace psynder::render::post {

// Fixed grid dimensions. The numbers come straight from DESIGN.md §8.4 —
// 160×90 is a 16:9 mapping (1080p / 12 in x, 1080p / 12 in y when accounting
// for the aspect) and 64 depth slices is the lowest count that hides slice-
// stepping aliasing on long pans.
inline constexpr u32 kFroxelW = 160;
inline constexpr u32 kFroxelH = 90;
inline constexpr u32 kFroxelD = 64;

inline constexpr usize kFroxelCount =
    static_cast<usize>(kFroxelW) * kFroxelH * kFroxelD;

// One cell of the froxel grid. 32 bytes = half a cache line; two cells fit
// in a line which keeps the ray-march loop's memory pressure low.
struct alignas(32) Froxel {
    // Pre-multiplied in-scattered radiance (R,G,B) for this cell.
    f32 scatter_r = 0.0f;
    f32 scatter_g = 0.0f;
    f32 scatter_b = 0.0f;
    // Extinction coefficient (density × sigma_t). Higher → faster fall-off.
    f32 extinction = 0.0f;
    // Padding; reserved for ambient + phase-function knobs in Wave C.
    f32 _pad[3] = {0.0f, 0.0f, 0.0f};
    f32 _pad2   = 0.0f;
};
static_assert(sizeof(Froxel) == 32, "Froxel layout must be 32 B");

// 3-D grid storage. Contiguous; index_of(x, y, z) below.
struct FroxelGrid {
    Froxel cells[kFroxelCount] = {};

    static constexpr usize index_of(u32 x, u32 y, u32 z) noexcept {
        return (static_cast<usize>(z) * kFroxelH + y) * kFroxelW + x;
    }
};

// One light contribution for fog. Point-only for Wave B; directional and
// spot land in Wave C alongside lane 08's headlight cookie work.
struct FogLight {
    math::Vec3 position;
    math::Vec3 colour;       // pre-exposed (intensity baked in)
    f32        radius = 0.0f;  // metres; 0 means infinite
};

// Lightweight callable that returns true if the segment from `a` to `b` is
// occluded. Caller-supplied so volumetric fog need not link against
// lane 08; a binding for `Tlas::occluded` is a one-liner at the call site.
struct OccluderFn {
    using Fn = bool (*)(void* user, math::Vec3 a, math::Vec3 b);
    Fn    fn   = nullptr;
    void* user = nullptr;
    bool operator()(math::Vec3 a, math::Vec3 b) const noexcept {
        return fn ? fn(user, a, b) : false;
    }
};

// Scene snapshot the fog system reads to populate the grid. Camera + frustum
// + lights + global density. `near_z`/`far_z` describe the *non-linear* slice
// distribution: slices are exponentially spaced between near_z and far_z so
// near-camera resolution is high (where the player notices) and far slices
// cover more world per cell.
struct FogScene {
    math::Vec3 camera_position{0,0,0};
    math::Vec3 camera_forward{0,0,1};
    math::Vec3 camera_right{1,0,0};
    math::Vec3 camera_up{0,1,0};
    f32        fov_y_rad   = 1.04f;     // ~60°
    f32        aspect      = 16.0f / 9.0f;
    f32        near_z      = 0.1f;      // start of slice range
    f32        far_z       = 64.0f;     // end of slice range
    f32        density     = 0.05f;     // base sigma_t per metre
    math::Vec3 ambient{0.04f, 0.05f, 0.06f};  // sky-ambient in-scatter

    const FogLight* lights      = nullptr;
    u32             light_count = 0;

    OccluderFn occluder{};                // optional; nullptr → unshadowed
};

// Populate the grid from the scene. Pure CPU — does not touch the framebuffer.
// Designed to run inside the per-frame job graph in parallel with raster.
void populate_fog_grid(FroxelGrid& grid, const FogScene& scene);

// Resolve the grid into the HDR framebuffer. Reads the framebuffer's depth
// buffer to find the per-pixel back-stop depth; ray-marches the grid from
// the near plane to that depth and blends scatter × (1 − transmittance) into
// the colour.
//
// `depth_linear` is a tight w*h linear-z buffer (caller's responsibility to
// convert from the rasterizer's u24 mantissa to f32 world units). If null,
// the resolve marches to the far plane for every pixel — useful for skybox-
// only scenes and the unit test.
struct VolumetricFogParams {
    bool enabled = true;
    f32  intensity = 1.0f;  // scales the per-pixel scatter contribution
};

void apply_volumetric_fog(Framebuffer& hdr,
                          const FroxelGrid& grid,
                          const FogScene& scene,
                          const f32* depth_linear,
                          const VolumetricFogParams& params);

}  // namespace psynder::render::post
