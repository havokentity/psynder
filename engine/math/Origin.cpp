// SPDX-License-Identifier: MIT
// Psynder — render-relative origin re-centering impl. Lane 02.

#include "Origin.h"

#include <cmath>

namespace psynder::math {

OriginRecenterResult origin_recenter(Vec3& world_origin,
                                     Vec3 camera,
                                     const OriginRecenterConfig& cfg) noexcept {
    OriginRecenterResult result{};

    if (!origin_would_recenter(world_origin, camera, cfg)) {
        return result;
    }

    // Snap each axis independently to the nearest multiple of cell_size.
    // We round the *camera* (in world space) and use that as the new origin
    // — this guarantees that the post-shift camera coords always sit
    // inside a single cell, so subsequent precision behavior is bounded.
    f32 inv_cell = 1.0f / cfg.cell_size;
    f32 sx = std::floor(camera.x * inv_cell + 0.5f) * cfg.cell_size;
    f32 sy = std::floor(camera.y * inv_cell + 0.5f) * cfg.cell_size;
    f32 sz = std::floor(camera.z * inv_cell + 0.5f) * cfg.cell_size;

    result.shift = Vec3{
        sx - world_origin.x,
        sy - world_origin.y,
        sz - world_origin.z,
    };
    result.changed = result.shift.x != 0.0f
                  || result.shift.y != 0.0f
                  || result.shift.z != 0.0f;

    world_origin.x = sx;
    world_origin.y = sy;
    world_origin.z = sz;
    return result;
}

}  // namespace psynder::math
