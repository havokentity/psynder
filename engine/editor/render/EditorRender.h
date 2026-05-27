// SPDX-License-Identifier: MIT
// Psynder -- editor render glue. Frozen entry points the editor host calls to
// (a) render the active scene through the software raytracer and (b) bake
// lightmaps for the scene. These mirror how the RT samples assemble TLAS +
// lights + FrameRenderInput, and how tools/lm_bake drives the offline bake.
//
// The host owns the scene (scene::Scene), the rendering system
// (render::RenderingSystem, which exposes render_rt + a MeshLibrary), the
// camera view, and the target framebuffer. This module owns none of that; it
// gathers the renderables/geometry into the RT/bake inputs, GATHERS the lights
// from the scene ECS (scene::LightComponent), and calls the existing engine
// kernels. New files live only under engine/editor/render/.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"
#include "render/RenderingSystem.h"
#include "render/rt/Bvh.h"
#include "render/rt/FrameRenderer.h"
#include "scene/SceneEcs.h"

#include "Bake.h"  // tools/lm_bake -- BakeScene, bake(), write_lmlight()

#include <span>
#include <vector>

namespace psynder::editor::render {

// --- RT render options -------------------------------------------------------
struct SceneRtOptions {
    // Trace-resolution divisor relative to the target framebuffer. 1 = full
    // res, 2 = quarter-res lit pass bilinear-upsampled into the target (the
    // sample default -- RT per pixel is expensive). Clamped to >= 1.
    u32 trace_downscale = 2;
    u32 tile_size = 16;
    // When true, pull AO / cores / reflection overrides from the console
    // (frame_render_config_from_console). When false, defaults are used.
    bool use_console_config = true;
};

struct SceneRtStats {
    u32 instance_count = 0;  // TLAS instances built (one per visible mesh renderable)
    u32 blas_count = 0;      // unique meshes turned into a BLAS
    u32 light_count = 0;     // scene lights gathered from the ECS
    bool rendered = false;   // false when nothing renderable was found / inputs invalid
    ::psynder::render::rt::FrameRenderStats frame{};
};

// Gather the scene's RT-visible mesh renderables into a render::rt::Tlas (one
// BLAS per unique mesh, one instance per entity with its world transform),
// GATHER the scene's lights from the ECS (scene::LightComponent), build the
// rt::FrameRenderInput, and call renderer.render_rt(...), writing (and
// upsampling, when trace_downscale > 1) into `target`. The camera basis is
// derived from `view` (a scene::SceneCameraView -- its `view`/`projection`
// matrices). Returns stats; on an empty/invalid scene the target is left
// untouched and rendered == false.
SceneRtStats render_scene_rt(const ::psynder::scene::Scene& scene,
                             const ::psynder::scene::SceneCameraView& view,
                             ::psynder::render::RenderingSystem& renderer,
                             ::psynder::render::Framebuffer& target,
                             const SceneRtOptions& options = {});

// Host-span overload: render with a caller-supplied light list instead of
// gathering from the ECS. Kept for tooling that drives lights directly; the
// ECS-gather path above is the primary one.
SceneRtStats render_scene_rt(const ::psynder::scene::Scene& scene,
                             const ::psynder::scene::SceneCameraView& view,
                             ::psynder::render::RenderingSystem& renderer,
                             std::span<const ::psynder::render::rt::FrameLight> lights,
                             ::psynder::render::Framebuffer& target,
                             const SceneRtOptions& options = {});

// --- Lightmap bake ---------------------------------------------------------
struct BakeOptions {
    u32 lightmap_resolution = 8;   // texels per triangle edge (lm_bake grid)
    u32 max_indirect_bounces = 0;  // 0 = direct only; 2-4 for path-traced GI
    u32 indirect_samples_per_bounce = 16;
    // Default diffuse albedo applied to baked triangles (lm_bake expects an
    // albedo per triangle; the engine MaterialDesc albedo is RGBA8 and is
    // decoded per-renderable when present).
    math::Vec3 default_albedo{0.6f, 0.6f, 0.6f};
};

struct BakeResult {
    bool ok = false;                            // false when nothing bakeable was found
    u32 triangle_count = 0;                     // bake triangles assembled from the scene
    u32 light_count = 0;                        // scene lights gathered from the ECS
    u32 surface_count = 0;                      // baked surfaces (one per triangle)
    f32 max_luminance = 0.0f;                   // brightest baked texel (sanity / plausibility)
    ::psynder::tools::bake::BakedAtlas atlas{};  // the baked lightmap the editor stores/toggles
    std::vector<u8> lmlight{};                   // .lmlight blob (write_lmlight) for persistence
};

// Drive tools/lm_bake over the scene's bake-eligible static mesh geometry plus
// the scene's lights (gathered from the ECS) to produce a lightmap atlas the
// editor can store and toggle (baked vs flat). Triangles come from each
// renderable's mesh in world space; only Static renderables whose material
// opts into baked lighting participate (mirrors the bake_static queue policy).
// Returns the atlas + a .lmlight blob.
BakeResult bake_lightmaps(::psynder::scene::Scene& scene,
                          ::psynder::render::RenderingSystem& renderer,
                          const BakeOptions& options = {});

// Host-span overload: bake with a caller-supplied light list.
BakeResult bake_lightmaps(::psynder::scene::Scene& scene,
                          ::psynder::render::RenderingSystem& renderer,
                          std::span<const ::psynder::render::rt::FrameLight> lights,
                          const BakeOptions& options = {});

}  // namespace psynder::editor::render
