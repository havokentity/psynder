// SPDX-License-Identifier: MIT
// Psynder — render-relative origin re-centering. Lane 02.
//
// Per ADR-005 / DESIGN.md §9.2: maps cap at 16 km × 16 km, so float32 world
// coordinates have enough precision for everything inside a map (a single
// metre at 16 km × 16 km is ~14 mantissa bits — plenty for sub-mm work in
// the worst case). What float32 still can't do is hold *cumulative* world
// position across hours of play without drift creeping into the bottom
// bits. The fix is per-frame render-relative re-centering: shift the world
// origin to a region near the camera before submitting verts to the
// rasterizer, and translate the camera by the inverse amount.
//
// `origin_recenter` rounds the origin shift to a fixed grid so the change
// is *quantized* — actors snap to the new origin all at once rather than
// jittering every frame. Choose a grid size that's much larger than your
// fastest moving object (default 1024 m).

#pragma once

#include "Math.h"

#include <cmath>

namespace psynder::math {

// Origin recentering policy.
//
// `cell_size` is the grid quantum in metres. A pragmatic value is 1024 m —
// large enough that re-centering doesn't happen every frame, small enough
// that camera coords stay well inside float32's high-precision range
// (camera coords post-recenter are bounded by `±cell_size/2 + speed*dt`).
//
// `trigger_distance` is the radius (also metres) the camera can drift from
// the current origin before the next re-center fires. Defaults to half the
// cell size, so the camera always sits inside the central cell. Larger
// values reduce recenter frequency at the cost of holding lower-precision
// coordinates a little longer.
struct OriginRecenterConfig {
    f32 cell_size        = 1024.0f;
    f32 trigger_distance = 512.0f;
};

inline constexpr OriginRecenterConfig kDefaultOriginRecenter{};

// Result reported to the caller. `shift` is the *world-space* vector that
// got subtracted from every world-space position when re-centering — the
// caller applies the inverse offset to scene entities and the camera.
struct OriginRecenterResult {
    Vec3 shift{0, 0, 0};   // world_origin += shift
    bool changed = false;  // true ⇔ shift != zero
};

// Inspect-only test: would the given camera trip the trigger?
inline bool origin_would_recenter(const Vec3& world_origin,
                                  const Vec3& camera,
                                  const OriginRecenterConfig& cfg = kDefaultOriginRecenter) noexcept {
    f32 dx = camera.x - world_origin.x;
    f32 dy = camera.y - world_origin.y;
    f32 dz = camera.z - world_origin.z;
    f32 d2 = dx*dx + dy*dy + dz*dz;
    return d2 > cfg.trigger_distance * cfg.trigger_distance;
}

// Re-center the world origin onto the cell that contains the camera.
//
// Mutates `world_origin` in place and returns the applied shift so the
// caller can offset entity positions, camera state, and any cached
// vectors. Returns `changed=false` (and a zero shift) when the camera is
// still inside the trigger radius — caller-side updates can be skipped.
//
// Snap is per-axis: only the axes that drifted past the trigger get
// nudged. (Practical for FPS / racing where horizontal motion dominates.)
OriginRecenterResult origin_recenter(Vec3& world_origin,
                                     Vec3 camera,
                                     const OriginRecenterConfig& cfg = kDefaultOriginRecenter) noexcept;

}  // namespace psynder::math
