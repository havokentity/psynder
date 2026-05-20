// SPDX-License-Identifier: MIT
// Psynder — BigCoord world driver. Lane 02, Wave B.
//
// Per ADR-005 / DESIGN.md §9.2: maps cap at 16 km × 16 km, world coords are
// float32. To keep the camera-local precision bounded across long sessions
// the renderer re-centers the world origin onto a quantized 1024 m cell
// whenever the camera drifts past a trigger radius from the current origin.
//
// `BigCoordWorld` carries the world-origin state plus its trigger policy.
// `snap_to_camera()` performs the quantized snap when the camera is past
// the trigger radius and returns the world-space offset that got applied
// (zero when no snap fires). The caller subtracts this offset from any
// world-space data they hold (camera, cached entity transforms, etc.).
//
// Wave A landed the primitive (`math/Origin.h::origin_recenter`); this
// header is a thin header-only wrapper that gives the renderer a single
// per-frame call point with the conventional driver API.
//
// Header-only by design: trivial math, no behavior worth a translation
// unit, and lane 07 / lane 12 want to inline the call inside their per-
// frame loops.

#pragma once

#include "math/Math.h"

#include "core/Types.h"

#include <cmath>

namespace psynder::math {

// Driver state for the per-frame origin re-center loop.
//
// `origin`          — current world-origin in world coordinates. The
//                     renderer subtracts this from every world position
//                     before submitting to the rasterizer.
// `trigger_radius`  — how far (metres) the camera may drift from the
//                     current origin before the next snap fires. The cell
//                     size used for the quantized snap is fixed at
//                     2 × trigger_radius so the camera always lands within
//                     one cell of the new origin.
struct BigCoordWorld {
    Vec3 origin{0, 0, 0};
    f32 trigger_radius = 1024.0f;

    // Snap the origin onto the nearest cell containing `camera_world` when
    // the camera is more than `trigger_radius` metres from the current
    // origin. Returns the world-space offset that was added to `origin`
    // (zero when no snap fires). Caller subtracts the returned offset from
    // their own cached world-space data.
    //
    // The snap is quantized to the cell grid (cell size = 2 × radius) on
    // each axis independently — only the axes that drifted past the
    // trigger get nudged. Returning a zero offset is the safe path
    // callers can apply unconditionally.
    inline Vec3 snap_to_camera(Vec3 camera_world) noexcept {
        f32 dx = camera_world.x - origin.x;
        f32 dy = camera_world.y - origin.y;
        f32 dz = camera_world.z - origin.z;
        f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= trigger_radius * trigger_radius) {
            return Vec3{0, 0, 0};
        }
        // Cell size is 2 × radius — that puts the camera within one cell
        // of the new origin regardless of the axis it drifted along.
        f32 cell = 2.0f * trigger_radius;
        // Per-axis quantized snap: round camera onto the nearest cell.
        // We only nudge an axis when it drifted past the trigger; this
        // keeps tests like "drift on +x only" from also moving y/z.
        Vec3 shift{0, 0, 0};
        if (std::fabs(dx) > trigger_radius) {
            f32 target_x = std::round(camera_world.x / cell) * cell;
            shift.x = target_x - origin.x;
        }
        if (std::fabs(dy) > trigger_radius) {
            f32 target_y = std::round(camera_world.y / cell) * cell;
            shift.y = target_y - origin.y;
        }
        if (std::fabs(dz) > trigger_radius) {
            f32 target_z = std::round(camera_world.z / cell) * cell;
            shift.z = target_z - origin.z;
        }
        origin.x += shift.x;
        origin.y += shift.y;
        origin.z += shift.z;
        return shift;
    }
};

}  // namespace psynder::math
