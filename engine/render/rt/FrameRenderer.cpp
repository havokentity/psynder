// SPDX-License-Identifier: MIT
// Psynder — reusable CPU raytraced frame renderer for RT demos/tools.

#include "render/rt/FrameRenderer.h"

#include "core/console/Console.h"
#include "core/hardware/CpuFeatures.h"
#include "jobs/JobSystem.h"
#include "jobs/JobSystemHetero.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>

namespace psynder::render::rt {
namespace {

console::CVar* g_frame_parallel = nullptr;
console::CVar* g_frame_cores_hint = nullptr;
console::CVar* g_frame_batch_rows = nullptr;
console::CVar* g_frame_min_rows = nullptr;
console::CVar* g_frame_parallel_max_chunks = nullptr;
console::CVar* g_frame_ao = nullptr;
console::CVar* g_frame_ao_samples = nullptr;
console::CVar* g_frame_ao_radius = nullptr;
console::CVar* g_frame_ao_strength = nullptr;
console::CVar* g_frame_ao_lit_strength = nullptr;
console::CVar* g_frame_ao_denoise = nullptr;
console::CVar* g_frame_ao_debug = nullptr;

PSY_FORCEINLINE u32 pack_rgba8(u32 r, u32 g, u32 b) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | (0xFFu << 24);
}

PSY_FORCEINLINE u32 clamp_u8(f32 v) noexcept {
    if (v < 0.0f)
        return 0u;
    if (v > 255.0f)
        return 255u;
    return static_cast<u32>(v);
}

u32 hash_u32(u32 x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

f32 hash_unit_float(u32 x) noexcept {
    return static_cast<f32>(hash_u32(x) >> 8) * (1.0f / 16777216.0f);
}

template <class Fn>
u32 parallel_tiles(u32 width, u32 height, const FrameRenderConfig& config, Fn&& body) {
    const u32 tile = std::max(1u, config.tile_size);
    const u32 tiles_x = (width + tile - 1u) / tile;
    const u32 tiles_y = (height + tile - 1u) / tile;
    const u32 tile_count = tiles_x * tiles_y;
    if (!config.parallel || tile_count <= 1u) {
        body(0u, 0u, width, height);
        return 1u;
    }

    if (jobs::JobSystem::Get().worker_count() == 0u)
        jobs::JobSystem::Get().start(0);

    const u32 active_jobs = frame_renderer_target_jobs(config, tile_count);
    const u32 tile_batch = std::max(1u, config.tile_batch);
    std::atomic<u32> next_tile;
    next_tile.store(0u, std::memory_order_relaxed);
    jobs::JobSystem::Get().parallel_for(0, active_jobs, 1, [&](usize, usize) {
        for (;;) {
            const u32 first_tile = next_tile.fetch_add(tile_batch, std::memory_order_relaxed);
            if (first_tile >= tile_count)
                break;
            const u32 last_tile = std::min(tile_count, first_tile + tile_batch);
            for (u32 ti = first_tile; ti < last_tile; ++ti) {
                const u32 tx = ti % tiles_x;
                const u32 ty = ti / tiles_x;
                const u32 x0 = tx * tile;
                const u32 y0 = ty * tile;
                const u32 x1 = std::min(width, x0 + tile);
                const u32 y1 = std::min(height, y0 + tile);
                body(x0, y0, x1, y1);
            }
        }
    });
    return active_jobs;
}

Ray ao_ray(const math::Vec3& position,
           const math::Vec3& normal,
           u32 pixel_index,
           u32 sample_index,
           f32 radius) {
    const math::Vec3 n = math::normalize(normal);
    const math::Vec3 up =
        std::fabs(n.y) < 0.95f ? math::Vec3{0.0f, 1.0f, 0.0f} : math::Vec3{1.0f, 0.0f, 0.0f};
    const math::Vec3 tangent = math::normalize(math::cross(up, n));
    const math::Vec3 bitangent = math::cross(n, tangent);

    const u32 seed = pixel_index * 0x9e3779b9u + sample_index * 0x85ebca6bu + 0x632be59bu;
    const f32 u1 = hash_unit_float(seed);
    const f32 u2 = hash_unit_float(seed ^ 0xa511e9b3u);
    const f32 z = 0.12f + 0.88f * (u1 * u1);
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const f32 phi = math::kTwoPi * u2;
    const f32 x = r * std::cos(phi);
    const f32 y = r * std::sin(phi);

    math::Vec3 dir{};
    dir.x = tangent.x * x + bitangent.x * y + n.x * z;
    dir.y = tangent.y * x + bitangent.y * y + n.y * z;
    dir.z = tangent.z * x + bitangent.z * y + n.z * z;
    dir = math::normalize(dir);

    Ray ray{};
    ray.origin = {position.x + n.x * 2.0e-3f, position.y + n.y * 2.0e-3f, position.z + n.z * 2.0e-3f};
    ray.direction = dir;
    ray.t_min = 1.0e-4f;
    ray.t_max = radius;
    return ray;
}

u32 sample_sky(math::Vec3 dir) {
    const f32 t = 0.5f * (dir.y + 1.0f);
    const f32 horizon[3] = {7.0f, 11.0f, 30.0f};
    const f32 zenith[3] = {1.0f, 1.0f, 7.0f};
    const f32 r = horizon[0] * (1.0f - t) + zenith[0] * t;
    const f32 g = horizon[1] * (1.0f - t) + zenith[1] * t;
    const f32 b = horizon[2] * (1.0f - t) + zenith[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

math::Vec3 material_rgb(const FrameMaterialTable& materials, const Hit& hit) {
    u32 color = materials.default_rgba8;
    if (materials.instance_rgba8 && hit.instance < materials.instance_count) {
        color = materials.instance_rgba8[hit.instance];
        return {static_cast<f32>(color & 0xFFu) / 255.0f,
                static_cast<f32>((color >> 8) & 0xFFu) / 255.0f,
                static_cast<f32>((color >> 16) & 0xFFu) / 255.0f};
    }
    if (materials.primitive_rgb && hit.primitive < materials.primitive_count)
        return materials.primitive_rgb[hit.primitive];
    return {static_cast<f32>(color & 0xFFu) / 255.0f,
            static_cast<f32>((color >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((color >> 16) & 0xFFu) / 255.0f};
}

math::Vec3 rgba8_to_rgb(u32 color) noexcept {
    return {static_cast<f32>(color & 0xFFu) / 255.0f,
            static_cast<f32>((color >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((color >> 16) & 0xFFu) / 255.0f};
}

struct SceneTraceHit {
    bool hit = false;
    f32 t = 0.0f;
    math::Vec3 normal{};
    math::Vec3 rgb{};
};

bool intersect_analytic_sphere(const scene::AnalyticSphereInstance& sphere,
                               const Ray& ray,
                               f32 max_t,
                               f32& out_t,
                               math::Vec3& out_normal) noexcept {
    if (sphere.world_sphere.radius <= 0.0f)
        return false;
    const math::Vec3 oc = math::sub(ray.origin, sphere.world_sphere.center);
    const f32 a = math::dot(ray.direction, ray.direction);
    const f32 half_b = math::dot(oc, ray.direction);
    const f32 c = math::dot(oc, oc) - sphere.world_sphere.radius * sphere.world_sphere.radius;
    const f32 disc = half_b * half_b - a * c;
    if (disc < 0.0f || a <= 0.0f)
        return false;

    const f32 root = std::sqrt(disc);
    f32 t = (-half_b - root) / a;
    if (t < ray.t_min || t > max_t) {
        t = (-half_b + root) / a;
        if (t < ray.t_min || t > max_t)
            return false;
    }

    const math::Vec3 p = math::add(ray.origin, math::mul(ray.direction, t));
    out_t = t;
    out_normal = math::normalize(math::sub(p, sphere.world_sphere.center));
    return true;
}

SceneTraceHit trace_scene(const FrameRenderInput& input,
                          std::span<const scene::AnalyticSphereInstance> spheres,
                          const Ray& ray) {
    SceneTraceHit best{};
    f32 best_t = ray.t_max;
    if (input.tlas) {
        const Hit h = input.tlas->intersect(ray);
        if (h.hit && h.t >= ray.t_min && h.t <= best_t) {
            best.hit = true;
            best.t = h.t;
            best.normal = h.normal;
            best.rgb = material_rgb(input.materials, h);
            best_t = h.t;
        }
    }

    for (const scene::AnalyticSphereInstance& sphere : spheres) {
        f32 t = 0.0f;
        math::Vec3 normal{};
        if (!intersect_analytic_sphere(sphere, ray, best_t, t, normal))
            continue;
        best.hit = true;
        best.t = t;
        best.normal = normal;
        best.rgb = rgba8_to_rgb(sphere.material_rgba8);
        best_t = t;
    }

    return best;
}

bool analytic_spheres_occluded(std::span<const scene::AnalyticSphereInstance> spheres,
                               const Ray& ray) noexcept {
    for (const scene::AnalyticSphereInstance& sphere : spheres) {
        f32 t = 0.0f;
        math::Vec3 normal{};
        if (intersect_analytic_sphere(sphere, ray, ray.t_max, t, normal))
            return true;
    }
    return false;
}

void trace_scene_shadow_packet(const FrameRenderInput& input,
                               std::span<const scene::AnalyticSphereInstance> spheres,
                               ShadowPacket8& pkt) {
    if (input.tlas) {
        trace_shadow_packet(*input.tlas, pkt);
    } else {
        for (bool& occ : pkt.occluded)
            occ = false;
    }
    for (u32 i = 0; i < 8u; ++i) {
        if (!pkt.occluded[i] && analytic_spheres_occluded(spheres, pkt.rays[i]))
            pkt.occluded[i] = true;
    }
}

void fill_dummy_ray(Ray& ray) noexcept {
    ray.origin = {0.0f, 1.0e6f, 0.0f};
    ray.direction = {0.0f, 1.0f, 0.0f};
    ray.t_min = 1.0e-4f;
    ray.t_max = 1.0f;
}

void upsample_bilinear(const u32* src, u32 sw, u32 sh, u32* dst, u32 dw, u32 dh) {
    const f32 fx_scale = static_cast<f32>(sw) / static_cast<f32>(dw);
    const f32 fy_scale = static_cast<f32>(sh) / static_cast<f32>(dh);
    for (u32 y = 0; y < dh; ++y) {
        const f32 sy = (static_cast<f32>(y) + 0.5f) * fy_scale - 0.5f;
        const i32 y0 = std::max(0, static_cast<i32>(std::floor(sy)));
        const i32 y1 = std::min(static_cast<i32>(sh) - 1, y0 + 1);
        const f32 fy = sy - static_cast<f32>(y0);
        for (u32 x = 0; x < dw; ++x) {
            const f32 sx = (static_cast<f32>(x) + 0.5f) * fx_scale - 0.5f;
            const i32 x0 = std::max(0, static_cast<i32>(std::floor(sx)));
            const i32 x1 = std::min(static_cast<i32>(sw) - 1, x0 + 1);
            const f32 fx = sx - static_cast<f32>(x0);

            const u32 p00 = src[static_cast<usize>(y0) * sw + static_cast<usize>(x0)];
            const u32 p10 = src[static_cast<usize>(y0) * sw + static_cast<usize>(x1)];
            const u32 p01 = src[static_cast<usize>(y1) * sw + static_cast<usize>(x0)];
            const u32 p11 = src[static_cast<usize>(y1) * sw + static_cast<usize>(x1)];

            auto unpack = [](u32 p, f32& r, f32& g, f32& b) {
                r = static_cast<f32>(p & 0xFFu);
                g = static_cast<f32>((p >> 8) & 0xFFu);
                b = static_cast<f32>((p >> 16) & 0xFFu);
            };
            f32 r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            unpack(p00, r00, g00, b00);
            unpack(p10, r10, g10, b10);
            unpack(p01, r01, g01, b01);
            unpack(p11, r11, g11, b11);
            const f32 w00 = (1.0f - fx) * (1.0f - fy);
            const f32 w10 = fx * (1.0f - fy);
            const f32 w01 = (1.0f - fx) * fy;
            const f32 w11 = fx * fy;
            dst[static_cast<usize>(y) * dw + x] =
                pack_rgba8(clamp_u8(r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11),
                           clamp_u8(g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11),
                           clamp_u8(b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11));
        }
    }
}

f32 cvar_f32_clamped(const console::CVar* cvar, f32 fallback, f32 lo, f32 hi) noexcept {
    const f32 value = cvar ? cvar->GetFloat() : fallback;
    return std::clamp(value, lo, hi);
}

u32 ao_samples_from_cvar() noexcept {
    const int value = g_frame_ao_samples ? g_frame_ao_samples->GetInt() : 4;
    return std::clamp(static_cast<u32>(value > 0 ? value : 4), 1u, 8u);
}

void register_frame_renderer_dump_command(std::string name, std::string description) {
    console::Console::Get().RegisterCommand(
        std::move(name),
        std::move(description),
        [](std::span<const std::string_view>, console::Output& out) {
            FrameRenderConfig config = frame_render_config_from_console(1024u, 512u, 512u, 256u, 16u);
            const u32 tiles = ((config.trace_width + config.tile_size - 1u) / config.tile_size) *
                              ((config.trace_height + config.tile_size - 1u) / config.tile_size);
            const auto hc = jobs::hetero_detected_counts();
            out.FormatLine(
                "rt_frame_renderer: parallel={}, hint_cores={}, max_auto_chunks={}, tile_batch={}, "
                "effective_jobs={}, tiles={} | ao={}, ao_samples={}, ao_radius={}, ao_strength={}, "
                "ao_lit_strength={}, ao_denoise={}, ao_debug={} | workers={} hetero={} (p={}, "
                "e={})",
                config.parallel ? 1 : 0,
                config.cores_hint,
                config.max_auto_chunks,
                config.tile_batch,
                frame_renderer_target_jobs(config, tiles),
                tiles,
                config.ambient_occlusion ? 1 : 0,
                config.ao_samples,
                config.ao_radius,
                config.ao_strength,
                config.ao_lit_strength,
                config.ao_denoise ? 1 : 0,
                config.ao_debug ? 1 : 0,
                jobs::JobSystem::Get().worker_count(),
                jobs::hetero_is_active() ? 1 : 0,
                hc.p_cores,
                hc.e_cores);
        });
}

}  // namespace

FrameCamera make_frame_camera(
    math::Vec3 eye, math::Vec3 forward, f32 aspect, f32 fov_y_rad, math::Vec3 world_up) {
    forward = math::normalize(forward);
    math::Vec3 right = math::cross(forward, world_up);
    if (math::dot(right, right) < 1.0e-6f)
        right = {1.0f, 0.0f, 0.0f};
    right = math::normalize(right);
    const math::Vec3 up = math::cross(right, forward);

    FrameCamera camera{};
    camera.origin = eye;
    camera.forward = forward;
    camera.right = right;
    camera.up = up;
    camera.fov_tan = std::tan(fov_y_rad * 0.5f);
    camera.aspect = aspect;
    return camera;
}

Ray primary_ray(const FrameCamera& cam, f32 nx, f32 ny, f32 t_min, f32 t_max) {
    const f32 sx = (2.0f * nx - 1.0f) * cam.aspect * cam.fov_tan;
    const f32 sy = (1.0f - 2.0f * ny) * cam.fov_tan;
    math::Vec3 dir{};
    dir.x = cam.forward.x + cam.right.x * sx + cam.up.x * sy;
    dir.y = cam.forward.y + cam.right.y * sx + cam.up.y * sy;
    dir.z = cam.forward.z + cam.right.z * sx + cam.up.z * sy;
    dir = math::normalize(dir);

    Ray ray{};
    ray.origin = cam.origin;
    ray.direction = dir;
    ray.t_min = t_min;
    ray.t_max = t_max;
    return ray;
}

math::Vec3 reflect(math::Vec3 incident, math::Vec3 unit_normal) noexcept {
    const f32 k = 2.0f * math::dot(incident, unit_normal);
    return {incident.x - k * unit_normal.x,
            incident.y - k * unit_normal.y,
            incident.z - k * unit_normal.z};
}

void upsample_bilinear_rgba8(
    const u32* src, u32 src_width, u32 src_height, u32* dst, u32 dst_width, u32 dst_height) {
    upsample_bilinear(src, src_width, src_height, dst, dst_width, dst_height);
}

void ensure_frame_scheduler_console_registered() {
    static bool once = false;
    if (once)
        return;
    once = true;

    auto& con = console::Console::Get();
    g_frame_parallel = con.RegisterCVar("r_rt_sample_parallel",
                                        "1",
                                        "Enable engine RT row/frame parallel scheduling (0/1)",
                                        console::CVAR_ARCHIVE);
    g_frame_cores_hint = con.RegisterCVar("r_rt_sample_cpu_cores_hint",
                                          "0",
                                          "Preferred engine RT worker cores (0=auto perf-cores-2)",
                                          console::CVAR_ARCHIVE);
    g_frame_batch_rows = con.RegisterCVar("r_rt_sample_batch_rows",
                                          "0",
                                          "Rows per engine RT row-scheduler chunk (0=auto)",
                                          console::CVAR_ARCHIVE);
    g_frame_min_rows =
        con.RegisterCVar("r_rt_sample_min_rows_per_core",
                         "8",
                         "Minimum engine RT row-scheduler rows per worker chunk in auto mode",
                         console::CVAR_ARCHIVE);

    con.RegisterCommand(
        "r_rt_sample_sched_dump",
        "Print engine RT sample row scheduler settings",
        [](std::span<const std::string_view>, console::Output& out) {
            const FrameRowScheduleConfig config = frame_row_schedule_config_from_console();
            const auto hc = jobs::hetero_detected_counts();
            constexpr u32 kExampleRows = 256u;
            out.FormatLine(
                "rt_frame_rows: parallel={}, hint_cores={}, batch_rows={}, min_rows={}, "
                "target_cores={}, grain_rows={} (for {} rows) | workers={} hetero={} (p={}, e={})",
                config.parallel ? 1 : 0,
                config.cores_hint,
                config.batch_rows,
                config.min_rows_per_core,
                frame_scheduler_target_cores(config.cores_hint, config.max_auto_cores, kExampleRows),
                frame_row_schedule_grain(kExampleRows, config),
                kExampleRows,
                jobs::JobSystem::Get().worker_count(),
                jobs::hetero_is_active() ? 1 : 0,
                hc.p_cores,
                hc.e_cores);
        });
}

FrameRowScheduleConfig frame_row_schedule_config_from_console() {
    ensure_frame_scheduler_console_registered();

    FrameRowScheduleConfig config{};
    config.parallel = g_frame_parallel ? g_frame_parallel->GetBool() : true;
    config.cores_hint =
        g_frame_cores_hint ? static_cast<u32>(std::max(0, g_frame_cores_hint->GetInt())) : 0u;
    config.batch_rows =
        g_frame_batch_rows ? static_cast<u32>(std::max(0, g_frame_batch_rows->GetInt())) : 0u;
    const int min_rows = g_frame_min_rows ? g_frame_min_rows->GetInt() : 8;
    config.min_rows_per_core = static_cast<u32>(min_rows > 0 ? min_rows : 8);
    return config;
}

u32 frame_scheduler_target_cores(u32 cores_hint, u32 max_auto_cores, u32 work_items) {
    if (work_items == 0u)
        return 0u;

    const u32 workers = jobs::JobSystem::Get().worker_count();
    const u32 runtime_max = workers > 0u ? (workers + 1u) : 1u;
    u32 target = 1u;

    if (cores_hint > 0u) {
        target = std::clamp(cores_hint, 1u, runtime_max);
    } else {
        const auto hc = jobs::hetero_detected_counts();
        u32 perf_like = hc.p_cores > 0u ? hc.p_cores : hardware::detect().cores_physical;
        if (perf_like == 0u)
            perf_like = 1u;
        target = std::clamp(perf_like > 2u ? (perf_like - 2u) : perf_like, 1u, runtime_max);
        if (max_auto_cores > 0u)
            target = std::min(target, max_auto_cores);
    }

    return std::clamp(target, 1u, work_items);
}

u32 frame_row_schedule_grain(u32 rows, const FrameRowScheduleConfig& config) {
    if (rows == 0u)
        return 0u;
    if (config.batch_rows > 0u)
        return std::clamp(config.batch_rows, 1u, rows);

    const u32 min_rows =
        std::clamp(config.min_rows_per_core > 0u ? config.min_rows_per_core : 8u, 1u, rows);
    const u32 target =
        std::max(1u, frame_scheduler_target_cores(config.cores_hint, config.max_auto_cores, rows));
    const u32 rows_from_cores = std::max(1u, (rows + target - 1u) / target);
    return std::clamp(std::max(min_rows, rows_from_cores), 1u, rows);
}

FrameRowScheduleStats parallel_frame_rows(u32 rows,
                                          const FrameRowScheduleConfig& config,
                                          FrameRowRangeCallback callback,
                                          void* user) {
    FrameRowScheduleStats stats{};
    stats.rows = rows;
    if (rows == 0u || !callback)
        return stats;

    if (!config.parallel) {
        stats.target_cores =
            frame_scheduler_target_cores(config.cores_hint, config.max_auto_cores, rows);
        stats.grain_rows = frame_row_schedule_grain(rows, config);
        callback(0u, rows, user);
        stats.scheduled_jobs = 1u;
        return stats;
    }

    if (jobs::JobSystem::Get().worker_count() == 0u)
        jobs::JobSystem::Get().start(0);

    stats.target_cores = frame_scheduler_target_cores(config.cores_hint, config.max_auto_cores, rows);
    stats.grain_rows = frame_row_schedule_grain(rows, config);
    if (rows <= stats.grain_rows) {
        callback(0u, rows, user);
        stats.scheduled_jobs = 1u;
        return stats;
    }

    stats.scheduled_jobs = (rows + stats.grain_rows - 1u) / stats.grain_rows;
    jobs::JobSystem::Get().parallel_for(0, rows, stats.grain_rows, [&](usize y0, usize y1) {
        callback(static_cast<u32>(y0), static_cast<u32>(y1), user);
    });
    return stats;
}

void ensure_frame_renderer_console_registered() {
    ensure_frame_scheduler_console_registered();

    static bool once = false;
    if (once)
        return;
    once = true;

    auto& con = console::Console::Get();
    g_frame_parallel_max_chunks =
        con.RegisterCVar("r_rt_sample_parallel_max_chunks",
                         "4",
                         "Maximum auto-mode engine RT jobs (explicit core hint overrides this)",
                         console::CVAR_ARCHIVE);
    g_frame_ao = con.RegisterCVar("r_rt_sample_ao",
                                  "0",
                                  "Enable engine RT ambient occlusion (0/1)",
                                  console::CVAR_ARCHIVE);
    g_frame_ao_samples = con.RegisterCVar("r_rt_sample_ao_samples",
                                          "4",
                                          "Engine RT AO rays per low-res hit pixel (1..8)",
                                          console::CVAR_ARCHIVE);
    g_frame_ao_radius = con.RegisterCVar("r_rt_sample_ao_radius",
                                         "3.0",
                                         "Engine RT AO ray max distance in world units",
                                         console::CVAR_ARCHIVE);
    g_frame_ao_strength = con.RegisterCVar("r_rt_sample_ao_strength",
                                           "1.00",
                                           "Engine RT AO ambient darkening strength (0..1)",
                                           console::CVAR_ARCHIVE);
    g_frame_ao_lit_strength = con.RegisterCVar(
        "r_rt_sample_ao_lit_strength",
        "0.75",
        "Extra engine RT AO multiplier applied to direct lighting for visible contact AO (0..1)",
        console::CVAR_ARCHIVE);
    g_frame_ao_denoise = con.RegisterCVar("r_rt_sample_ao_denoise",
                                          "1",
                                          "Edge-aware denoise engine RT AO visibility (0/1)",
                                          console::CVAR_ARCHIVE);
    g_frame_ao_debug =
        con.RegisterCVar("r_rt_sample_ao_debug",
                         "0",
                         "Show engine RT AO visibility as grayscale when AO is enabled (0/1)",
                         console::CVAR_ARCHIVE);

    register_frame_renderer_dump_command("r_rt_frame_sched_dump",
                                         "Print engine RT frame renderer scheduler settings");
}

void apply_frame_renderer_console_overrides(const FrameRendererConsoleOverrides& overrides) {
    ensure_frame_renderer_console_registered();
    auto& con = console::Console::Get();
    if (!overrides.cores_hint.empty())
        con.SetCVarOverride("r_rt_sample_cpu_cores_hint", overrides.cores_hint);
    if (overrides.ambient_occlusion >= 0)
        con.SetCVarOverride("r_rt_sample_ao", overrides.ambient_occlusion != 0 ? "1" : "0");
    if (overrides.ao_debug >= 0)
        con.SetCVarOverride("r_rt_sample_ao_debug", overrides.ao_debug != 0 ? "1" : "0");
    if (!overrides.ao_samples.empty())
        con.SetCVarOverride("r_rt_sample_ao_samples", overrides.ao_samples);
    if (!overrides.ao_radius.empty())
        con.SetCVarOverride("r_rt_sample_ao_radius", overrides.ao_radius);
    if (!overrides.ao_strength.empty())
        con.SetCVarOverride("r_rt_sample_ao_strength", overrides.ao_strength);
    if (!overrides.ao_lit_strength.empty())
        con.SetCVarOverride("r_rt_sample_ao_lit_strength", overrides.ao_lit_strength);
}

FrameRenderConfig frame_render_config_from_console(
    u32 output_width, u32 output_height, u32 trace_width, u32 trace_height, u32 tile_size) {
    ensure_frame_renderer_console_registered();

    FrameRenderConfig config{};
    config.output_width = output_width;
    config.output_height = output_height;
    config.trace_width = trace_width;
    config.trace_height = trace_height;
    config.tile_size = std::max(1u, tile_size);
    config.parallel = g_frame_parallel ? g_frame_parallel->GetBool() : true;
    config.cores_hint =
        g_frame_cores_hint ? static_cast<u32>(std::max(0, g_frame_cores_hint->GetInt())) : 0u;
    config.max_auto_chunks = g_frame_parallel_max_chunks
                                 ? static_cast<u32>(std::max(0, g_frame_parallel_max_chunks->GetInt()))
                                 : 4u;
    config.tile_batch = 2u;
    config.ambient_occlusion = g_frame_ao && g_frame_ao->GetBool();
    config.ao_debug = config.ambient_occlusion && g_frame_ao_debug && g_frame_ao_debug->GetBool();
    config.ao_denoise = g_frame_ao_denoise ? g_frame_ao_denoise->GetBool() : true;
    config.ao_samples = ao_samples_from_cvar();
    config.ao_radius = cvar_f32_clamped(g_frame_ao_radius, 3.0f, 0.05f, 20.0f);
    config.ao_strength = cvar_f32_clamped(g_frame_ao_strength, 1.0f, 0.0f, 1.0f);
    config.ao_lit_strength = cvar_f32_clamped(g_frame_ao_lit_strength, 0.75f, 0.0f, 1.0f);
    return config;
}

u32 frame_renderer_target_jobs(const FrameRenderConfig& config, u32 work_items) {
    return frame_scheduler_target_cores(config.cores_hint, config.max_auto_chunks, work_items);
}

void FrameRenderer::render(const FrameRenderInput& input,
                           const FrameRenderConfig& config,
                           u32* output_rgba8,
                           FrameRenderStats* stats) {
    if (stats)
        *stats = {};
    if ((!input.tlas && !input.scene_graph) || !output_rgba8 || config.output_width == 0u ||
        config.output_height == 0u)
        return;

    const u32 trace_w = config.trace_width > 0u ? config.trace_width : config.output_width;
    const u32 trace_h = config.trace_height > 0u ? config.trace_height : config.output_height;
    const u32 light_count = input.lights ? input.light_count : 0u;
    if (input.scene_graph)
        input.scene_graph->gather_analytic_spheres(analytic_spheres_);
    else
        analytic_spheres_.clear();
    const usize pixels = static_cast<usize>(trace_w) * trace_h;
    hits_.assign(pixels, PixelHit{});
    accum_.assign(pixels * 3u, 0.0f);
    trace_pixels_.assign(pixels, 0u);

    if (config.ambient_occlusion) {
        ao_raw_.assign(pixels, 1.0f);
        ao_filtered_.assign(pixels, 1.0f);
        hit_depth_.assign(pixels, 1.0e6f);
        hit_normals_.assign(pixels * 3u, 0.0f);
    }

    const u32 tile = std::max(1u, config.tile_size);
    const u32 tile_count = ((trace_w + tile - 1u) / tile) * ((trace_h + tile - 1u) / tile);
    const u32 scheduled_jobs =
        parallel_tiles(trace_w, trace_h, config, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = x0; x < x1; ++x) {
                    const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(trace_w);
                    const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(trace_h);
                    const Ray ray = primary_ray(input.camera, nx, ny);
                    const SceneTraceHit h = trace_scene(input, analytic_spheres_, ray);

                    const usize idx = static_cast<usize>(y) * trace_w + x;
                    PixelHit ph{};
                    ph.hit = h.hit;
                    if (!h.hit) {
                        trace_pixels_[idx] = sample_sky(ray.direction);
                        if (config.ambient_occlusion) {
                            hit_depth_[idx] = 1.0e6f;
                            hit_normals_[idx * 3u + 1u] = 1.0f;
                        }
                    } else {
                        ph.r = h.rgb.x;
                        ph.g = h.rgb.y;
                        ph.b = h.rgb.z;
                        ph.position = {ray.origin.x + ray.direction.x * h.t,
                                       ray.origin.y + ray.direction.y * h.t,
                                       ray.origin.z + ray.direction.z * h.t};
                        ph.normal = math::normalize(h.normal);
                        if (config.ambient_occlusion) {
                            hit_depth_[idx] = h.t;
                            hit_normals_[idx * 3u + 0u] = ph.normal.x;
                            hit_normals_[idx * 3u + 1u] = ph.normal.y;
                            hit_normals_[idx * 3u + 2u] = ph.normal.z;
                        }
                    }
                    hits_[idx] = ph;
                }
            }
        });

    if (config.ambient_occlusion) {
        const u32 ao_samples = std::clamp(config.ao_samples, 1u, 8u);
        const f32 ao_radius = std::max(0.01f, config.ao_radius);
        parallel_tiles(trace_w, trace_h, config, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            ShadowPacket8 pkt{};
            u32 in_pkt = 0;
            u32 pkt_idx[8]{};
            bool pkt_real[8]{};

            auto dispatch_ao = [&]() {
                for (u32 i = in_pkt; i < 8u; ++i) {
                    fill_dummy_ray(pkt.rays[i]);
                    pkt_real[i] = false;
                }
                trace_scene_shadow_packet(input, analytic_spheres_, pkt);
                const f32 weight = 1.0f / static_cast<f32>(ao_samples);
                for (u32 i = 0; i < 8u; ++i) {
                    if (pkt_real[i] && !pkt.occluded[i])
                        ao_raw_[pkt_idx[i]] += weight;
                }
                in_pkt = 0;
            };

            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = x0; x < x1; ++x) {
                    const usize px = static_cast<usize>(y) * trace_w + x;
                    const PixelHit& ph = hits_[px];
                    if (!ph.hit)
                        continue;
                    ao_raw_[px] = 0.0f;
                    for (u32 si = 0; si < ao_samples; ++si) {
                        u32& idx = in_pkt;
                        pkt.rays[idx] =
                            ao_ray(ph.position, ph.normal, static_cast<u32>(px), si, ao_radius);
                        pkt_idx[idx] = static_cast<u32>(px);
                        pkt_real[idx] = true;
                        ++idx;
                        if (idx == 8u)
                            dispatch_ao();
                    }
                }
            }
            if (in_pkt > 0u)
                dispatch_ao();
        });

        if (config.ao_denoise) {
            DenoiseInput ao_in{};
            ao_in.shadow_visibility = ao_raw_.data();
            ao_in.depth = hit_depth_.data();
            ao_in.normals = hit_normals_.data();
            ao_in.width = trace_w;
            ao_in.height = trace_h;
            denoise_shadows(ao_in, ao_filtered_.data());
        } else {
            ao_filtered_ = ao_raw_;
        }
    }

    parallel_tiles(trace_w, trace_h, config, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
        std::vector<ShadowPacket8> pkts(light_count);
        std::vector<u32> in_pkt(light_count, 0u);
        std::vector<u32> pkt_idx(static_cast<usize>(light_count) * 8u, 0u);
        std::vector<u8> pkt_real(static_cast<usize>(light_count) * 8u, 0u);

        auto dispatch = [&](u32 li) {
            ShadowPacket8& pkt = pkts[li];
            const FrameLight& light = input.lights[li];
            for (u32 i = in_pkt[li]; i < 8u; ++i) {
                fill_dummy_ray(pkt.rays[i]);
                pkt_real[static_cast<usize>(li) * 8u + i] = 0u;
            }
            trace_scene_shadow_packet(input, analytic_spheres_, pkt);
            for (u32 i = 0; i < 8u; ++i) {
                if (pkt_real[static_cast<usize>(li) * 8u + i] == 0u || pkt.occluded[i])
                    continue;
                const usize px = pkt_idx[static_cast<usize>(li) * 8u + i];
                const PixelHit& ph = hits_[px];
                math::Vec3 to_light = math::sub(light.position, ph.position);
                const f32 d2 = math::dot(to_light, to_light);
                const f32 d = std::sqrt(d2);
                if (d > light.range || d < 1.0e-4f)
                    continue;
                const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                const f32 ndotl = std::max(0.0f, math::dot(ph.normal, ld));
                if (ndotl <= 0.0f)
                    continue;
                const f32 atten = 1.0f / (1.0f + d * d * config.attenuation_quadratic);
                const f32 k = ndotl * atten * light.intensity;
                accum_[px * 3u + 0u] += ph.r * light.r * k;
                accum_[px * 3u + 1u] += ph.g * light.g * k;
                accum_[px * 3u + 2u] += ph.b * light.b * k;
            }
            in_pkt[li] = 0u;
        };

        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                const usize px = static_cast<usize>(y) * trace_w + x;
                const PixelHit& ph = hits_[px];
                if (!ph.hit)
                    continue;

                for (u32 li = 0; li < light_count; ++li) {
                    const FrameLight& light = input.lights[li];
                    math::Vec3 to_light = math::sub(light.position, ph.position);
                    const f32 d2 = math::dot(to_light, to_light);
                    const f32 d = std::sqrt(d2);
                    if (d > light.range || d < 1.0e-4f)
                        continue;
                    const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                    if (math::dot(ph.normal, ld) <= 0.0f)
                        continue;

                    const math::Vec3 origin = {ph.position.x + ph.normal.x * 1.0e-3f,
                                               ph.position.y + ph.normal.y * 1.0e-3f,
                                               ph.position.z + ph.normal.z * 1.0e-3f};
                    u32& idx = in_pkt[li];
                    pkts[li].rays[idx].origin = origin;
                    pkts[li].rays[idx].direction = ld;
                    pkts[li].rays[idx].t_min = 1.0e-4f;
                    pkts[li].rays[idx].t_max = d - 1.0e-3f;
                    pkt_idx[static_cast<usize>(li) * 8u + idx] = static_cast<u32>(px);
                    pkt_real[static_cast<usize>(li) * 8u + idx] = 1u;
                    ++idx;
                    if (idx == 8u)
                        dispatch(li);
                }
            }
        }

        for (u32 li = 0; li < light_count; ++li) {
            if (in_pkt[li] > 0u)
                dispatch(li);
        }
    });

    parallel_tiles(trace_w, trace_h, config, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                const usize idx = static_cast<usize>(y) * trace_w + x;
                if (config.ambient_occlusion && config.ao_debug) {
                    const f32 ao_visibility = std::clamp(ao_filtered_[idx], 0.0f, 1.0f);
                    const u32 v = clamp_u8(ao_visibility * 255.0f);
                    trace_pixels_[idx] = pack_rgba8(v, v, v);
                    continue;
                }
                const PixelHit& ph = hits_[idx];
                if (!ph.hit)
                    continue;

                f32 ambient_scale = config.ambient_scale;
                f32 direct_scale = 1.0f;
                if (config.ambient_occlusion) {
                    const f32 ao_visibility = std::clamp(ao_filtered_[idx], 0.0f, 1.0f);
                    ambient_scale *=
                        1.0f - std::clamp(config.ao_strength, 0.0f, 1.0f) * (1.0f - ao_visibility);
                    direct_scale = 1.0f - std::clamp(config.ao_lit_strength, 0.0f, 1.0f) *
                                              (1.0f - ao_visibility);
                }

                const f32 r = ph.r * ambient_scale +
                              accum_[idx * 3u + 0u] * config.direct_scale * direct_scale;
                const f32 g = ph.g * ambient_scale +
                              accum_[idx * 3u + 1u] * config.direct_scale * direct_scale;
                const f32 b = ph.b * ambient_scale +
                              accum_[idx * 3u + 2u] * config.direct_scale * direct_scale;
                trace_pixels_[idx] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
            }
        }
    });

    if (trace_w == config.output_width && trace_h == config.output_height) {
        std::copy(trace_pixels_.begin(), trace_pixels_.end(), output_rgba8);
    } else {
        upsample_bilinear(trace_pixels_.data(),
                          trace_w,
                          trace_h,
                          output_rgba8,
                          config.output_width,
                          config.output_height);
    }

    if (stats) {
        stats->tile_count = tile_count;
        stats->scheduled_jobs = scheduled_jobs;
        stats->hit_pixels = 0;
        stats->ao_min = 1.0f;
        f32 ao_sum = 0.0f;
        for (usize i = 0; i < hits_.size(); ++i) {
            if (!hits_[i].hit)
                continue;
            ++stats->hit_pixels;
            if (config.ambient_occlusion) {
                const f32 v = std::clamp(ao_filtered_[i], 0.0f, 1.0f);
                stats->ao_min = std::min(stats->ao_min, v);
                ao_sum += v;
            }
        }
        stats->ao_avg = config.ambient_occlusion && stats->hit_pixels > 0
                            ? ao_sum / static_cast<f32>(stats->hit_pixels)
                            : 1.0f;
    }
}

}  // namespace psynder::render::rt
