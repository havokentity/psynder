// SPDX-License-Identifier: MIT
// Psynder -- M-HYB hybrid-shadow occluder builder (DESIGN.md §8).
//
// Builds (and caches across frames) a render::rt::Tlas from the scene's
// RT-visible mesh renderables, then hands the raster fragment stage a borrowed,
// opaque ShadowOccluder it can fire shadow rays through. This is the bridge that
// makes "raster primary visibility + software-raytraced shadow rays in the same
// frame" work: the host calls build_shadow_scene() in Hybrid mode and sets the
// returned occluder on the raster ViewState.
//
// The TLAS-build mirrors editor/render/EditorRender.cpp's RT cache approach:
//   - one address-stable BLAS (render::rt::Bvh8) per unique mesh, kept in a
//     std::deque (never popped) so its registry state stays valid;
//   - a fingerprint over (vertex/index pointers + counts) so a BLAS is only
//     rebuilt when its mesh geometry actually changed;
//   - a single persistent Tlas rebuilt each frame from the per-frame instance
//     transforms (cheap; the expensive BLAS are cached).
// In a steady scene nothing is allocated after the first frame.
//
// Lives under engine/render/ (raster lane owns the raster->rt edge here); the
// raster core itself does NOT link rt -- the ShadowOccluder it consumes is
// opaque (void* + trampoline). This module links both.

#pragma once

#include "core/Types.h"
#include "render/RenderingSystem.h"
#include "render/raster/RasterLighting.h"
#include "scene/SceneEcs.h"

namespace psynder::render::hybrid {

struct ShadowSceneStats {
    u32 instance_count = 0;  // TLAS instances built this frame
    u32 blas_count = 0;      // unique meshes referenced this frame
    u32 blas_built = 0;      // BLAS actually (re)built this frame (0 = cache hit)
    bool built = false;      // false when nothing traceable was found
};

// Refresh the module-owned shadow TLAS from the scene's RT-visible mesh
// geometry. `scene` is mutated (transform refresh + queue gather, like the RT
// path). On success the returned ShadowOccluder is active and borrows the
// module-owned TLAS (stable for the lifetime of the process / until the next
// build_shadow_scene call). Fill `out_occluder.opacity/softness/samples` from
// scene::RenderSettings before handing it to the raster ViewState.
//
// When the scene has no traceable geometry the occluder is left inactive
// (null) and stats.built == false; the caller then renders plain raster.
ShadowSceneStats build_shadow_scene(::psynder::scene::Scene& scene,
                                    ::psynder::render::RenderingSystem& renderer,
                                    ::psynder::render::raster::ShadowOccluder& out_occluder) noexcept;

}  // namespace psynder::render::hybrid
