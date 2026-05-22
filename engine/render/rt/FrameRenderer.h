// SPDX-License-Identifier: MIT
// Psynder — reusable CPU raytraced frame renderer for RT demos/tools.
// Lane 08 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "render/Material.h"
#include "render/rt/Bvh.h"
#include "scene/SceneGraph.h"

#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psynder::render::rt {

struct FrameCamera {
    math::Vec3 origin;
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up;
    f32 fov_tan = 1.0f;
    f32 aspect = 1.0f;
};

struct FrameLight {
    math::Vec3 position;
    f32 intensity = 1.0f;
    f32 r = 1.0f;
    f32 g = 1.0f;
    f32 b = 1.0f;
    f32 range = 100.0f;
};

FrameCamera make_frame_camera(math::Vec3 eye,
                              math::Vec3 forward,
                              f32 aspect,
                              f32 fov_y_rad,
                              math::Vec3 world_up = {0.0f, 1.0f, 0.0f});
Ray primary_ray(const FrameCamera& camera, f32 nx, f32 ny, f32 t_min = 1.0e-3f, f32 t_max = 1.0e3f);
math::Vec3 reflect(math::Vec3 incident, math::Vec3 unit_normal) noexcept;
void upsample_bilinear_rgba8(
    const u32* src, u32 src_width, u32 src_height, u32* dst, u32 dst_width, u32 dst_height);

struct FrameRowScheduleConfig {
    bool parallel = true;
    u32 cores_hint = 0;         // 0 = auto perf-cores-2.
    u32 max_auto_cores = 0;     // 0 = no cap; explicit cores_hint overrides this.
    u32 batch_rows = 0;         // 0 = derive from rows / target cores.
    u32 min_rows_per_core = 8;  // Lower bound for auto-derived row chunks.
};

struct FrameRowScheduleStats {
    u32 rows = 0;
    u32 grain_rows = 0;
    u32 target_cores = 0;
    u32 scheduled_jobs = 0;
};

using FrameRowRangeCallback = void (*)(u32 y0, u32 y1, void* user);

void ensure_frame_scheduler_console_registered();
FrameRowScheduleConfig frame_row_schedule_config_from_console();
u32 frame_scheduler_target_cores(u32 cores_hint, u32 max_auto_cores, u32 work_items);
u32 frame_row_schedule_grain(u32 rows, const FrameRowScheduleConfig& config);
FrameRowScheduleStats parallel_frame_rows(u32 rows,
                                          const FrameRowScheduleConfig& config,
                                          FrameRowRangeCallback callback,
                                          void* user);

template <class Fn>
FrameRowScheduleStats parallel_frame_rows(u32 rows, const FrameRowScheduleConfig& config, Fn&& body) {
    using Body = std::remove_reference_t<Fn>;
    Body local_body(std::forward<Fn>(body));
    return parallel_frame_rows(
        rows,
        config,
        [](u32 y0, u32 y1, void* user) { (*static_cast<Body*>(user))(y0, y1); },
        &local_body);
}

struct FrameMaterialTable {
    const ::psynder::render::MaterialLibrary* library = nullptr;

    // Shared material IDs. Preferred path for engine-owned scene submissions.
    const ::psynder::render::MaterialId* instance_materials = nullptr;
    u32 instance_material_count = 0;
    const ::psynder::render::MaterialId* primitive_materials = nullptr;
    u32 primitive_material_count = 0;

    // RGBA8, one color per TLAS instance. Used first when present.
    const u32* instance_rgba8 = nullptr;
    u32 instance_count = 0;
    const f32* instance_reflectivity = nullptr;
    u32 instance_reflectivity_count = 0;

    // RGB in 0..1, one color per primitive. Used when no instance color matches.
    const math::Vec3* primitive_rgb = nullptr;
    u32 primitive_count = 0;
    const f32* primitive_reflectivity = nullptr;
    u32 primitive_reflectivity_count = 0;

    u32 default_rgba8 = 0xFF413737u;
    f32 default_reflectivity = 0.0f;
};

struct FrameRenderConfig {
    u32 output_width = 0;
    u32 output_height = 0;
    u32 trace_width = 0;
    u32 trace_height = 0;
    u32 tile_size = 16;

    bool parallel = true;
    u32 cores_hint = 0;
    u32 max_auto_chunks = 4;
    u32 tile_batch = 2;

    bool ambient_occlusion = false;
    bool ao_debug = false;
    bool ao_denoise = true;
    u32 ao_samples = 4;
    f32 ao_radius = 1.75f;
    f32 ao_strength = 0.75f;
    f32 ao_lit_strength = 0.75f;

    f32 ambient_scale = 3.0f;
    f32 direct_scale = 18.0f;
    f32 attenuation_quadratic = 0.06f;
    u32 reflection_bounces = 0;  // Currently 0 or 1.
    f32 reflection_scale = 1.0f;
};

struct FrameRenderInput {
    const Tlas* tlas = nullptr;
    const scene::SceneGraph* scene_graph = nullptr;
    FrameCamera camera{};
    const FrameLight* lights = nullptr;
    u32 light_count = 0;
    FrameMaterialTable materials{};
};

struct FrameRenderStats {
    u32 tile_count = 0;
    u32 scheduled_jobs = 0;
    usize hit_pixels = 0;
    f32 ao_min = 1.0f;
    f32 ao_avg = 1.0f;
};

struct FrameRendererConsoleOverrides {
    std::string_view cores_hint;
    int ambient_occlusion = -1;
    int ao_debug = -1;
    std::string_view ao_samples;
    std::string_view ao_radius;
    std::string_view ao_strength;
    std::string_view ao_lit_strength;
    std::string_view reflection_bounces;
};

void ensure_frame_renderer_console_registered();
void apply_frame_renderer_console_overrides(const FrameRendererConsoleOverrides& overrides);
FrameRenderConfig frame_render_config_from_console(
    u32 output_width, u32 output_height, u32 trace_width, u32 trace_height, u32 tile_size);
u32 frame_renderer_target_jobs(const FrameRenderConfig& config, u32 work_items);

class FrameRenderer {
   public:
    void render(const FrameRenderInput& input,
                const FrameRenderConfig& config,
                u32* output_rgba8,
                FrameRenderStats* stats = nullptr);

   private:
    struct PixelHit {
        bool hit = false;
        math::Vec3 position{};
        math::Vec3 normal{};
        f32 r = 0.0f;
        f32 g = 0.0f;
        f32 b = 0.0f;
        f32 reflectivity = 0.0f;
        f32 refl_r = 0.0f;
        f32 refl_g = 0.0f;
        f32 refl_b = 0.0f;
    };

    std::vector<PixelHit> hits_;
    std::vector<f32> accum_;
    std::vector<u32> trace_pixels_;
    std::vector<f32> ao_raw_;
    std::vector<f32> ao_filtered_;
    std::vector<f32> hit_depth_;
    std::vector<f32> hit_normals_;
    std::vector<scene::AnalyticSphereInstance> analytic_spheres_;
};

}  // namespace psynder::render::rt
