// SPDX-License-Identifier: MIT
// Psynder — outdoor terrain public-API glue (DESIGN.md §9.2, ADR-008).
//
// The heavy math lives in the internal headers so unit tests can use it
// directly (tests/unit/CMakeLists.txt is owned by the build-system
// maintainer and links a fixed lane set). This TU wires up the public
// `TerrainMesh` / `TerrainRaymarch` / `load_spline_track` symbols.

#include "world/outdoor/Terrain.h"
#include "world/outdoor/TerrainTarget.h"

#include "world/outdoor/CdlodMesh_internal.h"
#include "world/outdoor/Heightmap_internal.h"
#include "world/outdoor/Raymarch_internal.h"
#include "world/outdoor/Scatter_internal.h"
#include "world/outdoor/Spline_internal.h"

#include "asset/Vfs.h"
#include "core/console/Console.h"
#include "core/hardware/CpuFeatures.h"
#include "core/Log.h"
#include "jobs/JobSystem.h"
#include "jobs/JobSystemHetero.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace psynder::world::outdoor {

namespace {

// Cached per-instance state. The class layouts in the public header are
// (intentionally) opaque — we keep state in this TU-local map keyed by
// `this` so we don't have to widen the public header (frozen Wave A).
//
// Wave A only stores the heightmap pointer + the precomputed chunk set;
// Wave B will plug in inner-frame CDLOD selection and a real DrawItem
// emit path against lane 07's queue.
struct MeshState {
    HeightmapDesc desc{};
    std::vector<detail::CdlodChunk> chunks;
    bool built = false;
};

struct RaymarchState {
    HeightmapDesc desc{};
    render::Framebuffer* target = nullptr;  // borrowed; lifetime = caller
};

// We don't have a global ECS handle for the terrain object yet, so use
// `this`-keyed slot maps. They're touched once at load and once at draw —
// not on a hot path.
//
// NOTE: For Wave A we keep these as simple TU-local statics. Wave B will
// migrate to a properly-allocated arena slot tied to the scene root entity.
struct Slot {
    const void* key = nullptr;
    MeshState mesh;
};
struct RaySlot {
    const void* key = nullptr;
    RaymarchState ray;
};

constexpr usize kMaxTerrains = 16;
Slot g_mesh_slots[kMaxTerrains];
RaySlot g_ray_slots[kMaxTerrains];

PSY_CVAR(r_terrain_cpu_cores_hint,
         "0",
         "Preferred terrain CPU cores (0=auto; auto uses perf-cores-2 when possible)",
         console::CVAR_ARCHIVE);
PSY_CVAR(r_terrain_batch_rows,
         "0",
         "Rows per terrain job batch (0=auto from core hint)",
         console::CVAR_ARCHIVE);
PSY_CVAR(r_terrain_min_rows_per_core,
         "8",
         "Minimum rows per terrain worker chunk in auto mode",
         console::CVAR_ARCHIVE);

struct TerrainSchedSnapshot {
    bool valid = false;
    u32 rows = 0;
    i32 core_hint = 0;
    u32 target_cores = 0;
    i32 min_rows = 0;
    u32 batch_rows = 0;
    u32 chunks = 0;
    bool hetero = false;
    u32 p_cores = 0;
    u32 e_cores = 0;
    u32 workers = 0;
    u32 latency_workers = 0;
    u32 throughput_workers = 0;
};

TerrainSchedSnapshot g_terrain_sched_snapshot{};

PSY_COMMAND(
    r_terrain_sched_dump,
    "Print terrain scheduler core/batch snapshot from the latest frame",
    [](std::span<const std::string_view> /*args*/, console::Output& out) {
        if (!g_terrain_sched_snapshot.valid) {
            out.PrintLine("terrain_sched: no frame rendered yet");
            return;
        }
        const auto& s = g_terrain_sched_snapshot;
        out.FormatLine(
            "terrain_sched: rows={}, hint_cores={}, target_cores={}, min_rows={}, batch_rows={}, "
            "chunks={}, hetero={} (p={}, e={}), workers={} (latency={}, throughput={})",
            s.rows,
            s.core_hint,
            s.target_cores,
            s.min_rows,
            s.batch_rows,
            s.chunks,
            s.hetero ? 1 : 0,
            s.p_cores,
            s.e_cores,
            s.workers,
            s.latency_workers,
            s.throughput_workers);
    });

u32 clamp_core_hint_to_runtime(u32 requested) noexcept {
    const u32 workers = jobs::JobSystem::Get().worker_count();
    const u32 runtime_max = workers > 0u ? (workers + 1u) : 1u;
    return std::clamp(requested, 1u, runtime_max);
}

u32 terrain_target_cores() noexcept {
    const int hint = r_terrain_cpu_cores_hint ? r_terrain_cpu_cores_hint->GetInt() : 0;
    if (hint > 0) {
        return clamp_core_hint_to_runtime(static_cast<u32>(hint));
    }

    // Auto: prefer perf cores where available (P-cores on hetero hosts),
    // then leave two cores free for the rest of the frame if we can.
    u32 perf_like_cores = 0;
    const jobs::HeteroCounts hetero = jobs::hetero_detected_counts();
    if (hetero.p_cores > 0u) {
        perf_like_cores = hetero.p_cores;
    } else {
        perf_like_cores = hardware::detect().cores_physical;
    }
    if (perf_like_cores == 0u) {
        perf_like_cores = 1u;
    }
    const u32 auto_target = perf_like_cores > 2u ? (perf_like_cores - 2u) : perf_like_cores;
    return clamp_core_hint_to_runtime(auto_target);
}

u32 terrain_batch_rows(u32 total_rows, u32 target_cores) noexcept {
    const int batch_hint = r_terrain_batch_rows ? r_terrain_batch_rows->GetInt() : 0;
    if (batch_hint > 0) {
        return std::clamp(static_cast<u32>(batch_hint), 1u, std::max(1u, total_rows));
    }

    const int min_rows_hint = r_terrain_min_rows_per_core ? r_terrain_min_rows_per_core->GetInt() : 8;
    const u32 min_rows = std::clamp(static_cast<u32>(min_rows_hint > 0 ? min_rows_hint : 8),
                                    1u,
                                    std::max(1u, total_rows));

    const u32 jobs_target = std::clamp(target_cores, 1u, std::max(1u, total_rows));
    const u32 rows_from_cores = std::max(1u, (total_rows + jobs_target - 1u) / jobs_target);
    return std::clamp(std::max(min_rows, rows_from_cores), 1u, std::max(1u, total_rows));
}

template <class Fn>
void run_rows_latency_pref(u32 total_rows, u32 batch_rows, Fn&& fn) {
    if (total_rows == 0u) {
        return;
    }
    const u32 grain = std::max(1u, batch_rows);
    const u32 chunks = (total_rows + grain - 1u) / grain;
    if (chunks <= 1u) {
        fn(0u, total_rows);
        return;
    }

    if (!jobs::hetero_is_active()) {
        jobs::JobSystem::Get().parallel_for(0, total_rows, grain, [&](usize y_begin, usize y_end) {
            fn(static_cast<u32>(y_begin), static_cast<u32>(y_end));
        });
        return;
    }

    struct RowTask {
        u32 y0 = 0;
        u32 y1 = 0;
        void* fn_ptr = nullptr;
    };

    auto run = +[](void* user) noexcept {
        auto* t = static_cast<RowTask*>(user);
        auto* body = static_cast<Fn*>(t->fn_ptr);
        (*body)(t->y0, t->y1);
    };

    std::vector<RowTask> tasks(chunks);
    std::vector<jobs::JobHandle> handles(chunks);

    for (u32 i = 0; i < chunks; ++i) {
        const u32 y0 = i * grain;
        const u32 y1 = std::min(total_rows, y0 + grain);
        tasks[i].y0 = y0;
        tasks[i].y1 = y1;
        tasks[i].fn_ptr = &fn;

        jobs::JobDesc d{};
        d.fn = run;
        d.user = &tasks[i];
        d.name = "terrain_rows_latency";
        handles[i] = jobs::submit_latency(d);
    }

    auto& js = jobs::JobSystem::Get();
    for (const jobs::JobHandle h : handles) {
        js.wait(h);
    }
}

void capture_terrain_sched(u32 rows, u32 target_cores, u32 batch_rows) {
    const jobs::HeteroCounts hetero = jobs::hetero_detected_counts();
    const u32 chunks = batch_rows > 0u ? ((rows + batch_rows - 1u) / batch_rows) : rows;
    const int core_hint = r_terrain_cpu_cores_hint ? r_terrain_cpu_cores_hint->GetInt() : 0;
    const int min_rows_hint = r_terrain_min_rows_per_core ? r_terrain_min_rows_per_core->GetInt() : 8;
    g_terrain_sched_snapshot.valid = true;
    g_terrain_sched_snapshot.rows = rows;
    g_terrain_sched_snapshot.core_hint = core_hint;
    g_terrain_sched_snapshot.target_cores = target_cores;
    g_terrain_sched_snapshot.min_rows = min_rows_hint;
    g_terrain_sched_snapshot.batch_rows = batch_rows;
    g_terrain_sched_snapshot.chunks = chunks;
    g_terrain_sched_snapshot.hetero = jobs::hetero_is_active();
    g_terrain_sched_snapshot.p_cores = hetero.p_cores;
    g_terrain_sched_snapshot.e_cores = hetero.e_cores;
    g_terrain_sched_snapshot.workers = jobs::JobSystem::Get().worker_count();
    g_terrain_sched_snapshot.latency_workers = jobs::hetero_latency_workers();
    g_terrain_sched_snapshot.throughput_workers = jobs::hetero_throughput_workers();
}

MeshState* mesh_state_for(const TerrainMesh* tm) noexcept {
    for (auto& s : g_mesh_slots) {
        if (s.key == tm)
            return &s.mesh;
    }
    for (auto& s : g_mesh_slots) {
        if (s.key == nullptr) {
            s.key = tm;
            s.mesh = MeshState{};
            return &s.mesh;
        }
    }
    return nullptr;
}

RaymarchState* ray_state_for(const TerrainRaymarch* tr) noexcept {
    for (auto& s : g_ray_slots) {
        if (s.key == tr)
            return &s.ray;
    }
    for (auto& s : g_ray_slots) {
        if (s.key == nullptr) {
            s.key = tr;
            s.ray = RaymarchState{};
            return &s.ray;
        }
    }
    return nullptr;
}

}  // namespace

// ─── TerrainMesh ─────────────────────────────────────────────────────────
void TerrainMesh::build(const HeightmapDesc& desc) {
    MeshState* st = mesh_state_for(this);
    if (!st) {
        PSY_LOG_WARN("world_outdoor: TerrainMesh slot table full");
        return;
    }
    st->desc = desc;
    st->chunks = detail::build_all_chunks(desc);
    st->built = true;
}

void TerrainMesh::render_cdlod(const math::Mat4& /*view*/, const math::Mat4& /*proj*/) const {
    const MeshState* st = nullptr;
    for (auto& s : g_mesh_slots) {
        if (s.key == this) {
            st = &s.mesh;
            break;
        }
    }
    if (!st || !st->built)
        return;

    // Emit each leaf chunk into lane 07's submit queue. Wave A's chunk
    // morph is identity (no inter-level slope yet); the watertight
    // invariant comes from sharing integer texel positions between chunks.
    auto& rast = render::raster::Rasterizer::Get();
    for (const auto& c : st->chunks) {
        if (c.vertices.empty() || c.indices.empty())
            continue;
        render::raster::DrawItem item{};
        item.vertices = c.vertices.data();
        item.vertex_count = static_cast<u32>(c.vertices.size());
        item.indices = c.indices.data();
        item.index_count = static_cast<u32>(c.indices.size());
        item.model = math::identity4();
        item.flags = 0;
        rast.submit(item);
    }
}

// ─── TerrainRaymarch ─────────────────────────────────────────────────────
void TerrainRaymarch::set_heightmap(const HeightmapDesc& desc) {
    RaymarchState* st = ray_state_for(this);
    if (!st) {
        PSY_LOG_WARN("world_outdoor: TerrainRaymarch slot table full");
        return;
    }
    st->desc = desc;
}

// ─── Wave-E render() — paint into the bound framebuffer ─────────────────
//
// The per-column march algorithm originally lived inlined in sample_06
// (`samples/06_tactical_map/main.cpp::render_terrain`), reaching into
// `detail::march_ray` / `splat_at_texel` / `normal_at_texel` directly to
// paint the framebuffer because Wave A's `TerrainRaymarch::render` was a
// no-op stub. Wave E pulls that algorithm here so callers can use the
// public API.
//
// Camera derivation: the engine has no `Mat4::inverse()`, but the view
// matrix is an orthonormal rotation + translation produced by
// `math::look_at_rh`. Column-major layout (cf. `math/Math.cpp`):
//
//     m[0]  m[4]  m[8]   m[12]      right.x  right.y  right.z   -dot(right,eye)
//     m[1]  m[5]  m[9]   m[13]  =   up.x     up.y     up.z      -dot(up,eye)
//     m[2]  m[6]  m[10]  m[14]      -fwd.x   -fwd.y   -fwd.z     dot(fwd,eye)
//     m[3]  m[7]  m[11]  m[15]      0        0        0         1
//
// so the basis vectors fall out by reading the rotation block, and
// `eye = -m[12]*right - m[13]*up + m[14]*fwd` recovers the camera origin.
// The vertical-FOV tan and aspect ratio drop out of the perspective
// matrix: `m[5] = 1/tan(fov_y/2)`, `m[0] = m[5] / aspect`.
namespace {

struct DerivedCamera {
    math::Vec3 eye{};
    math::Vec3 fwd{};
    math::Vec3 right{};
    math::Vec3 up{};
    f32 fov_tan = 0.0f;
    f32 aspect = 1.0f;
};

DerivedCamera derive_camera(const math::Mat4& view, const math::Mat4& proj) noexcept {
    DerivedCamera c{};
    c.right = math::Vec3{view.m[0], view.m[4], view.m[8]};
    c.up = math::Vec3{view.m[1], view.m[5], view.m[9]};
    c.fwd = math::Vec3{-view.m[2], -view.m[6], -view.m[10]};
    // Renormalize defensively — the produced view matrix is orthonormal
    // but rounding can creep in once the caller composes transforms.
    c.right = math::normalize(c.right);
    c.up = math::normalize(c.up);
    c.fwd = math::normalize(c.fwd);

    // eye in world-space from the inverse of (R * T(-eye)).
    const f32 tx = view.m[12];
    const f32 ty = view.m[13];
    const f32 tz = view.m[14];
    c.eye = math::Vec3{
        -tx * c.right.x - ty * c.up.x + tz * c.fwd.x,
        -tx * c.right.y - ty * c.up.y + tz * c.fwd.y,
        -tx * c.right.z - ty * c.up.z + tz * c.fwd.z,
    };

    // perspective_rh: m[5] = 1/tan(fov_y/2); m[0] = m[5] / aspect.
    c.fov_tan = proj.m[5] > 1e-6f ? 1.0f / proj.m[5] : 1.0f;
    c.aspect = proj.m[0] > 1e-6f ? proj.m[5] / proj.m[0] : 1.0f;
    return c;
}

// Pack a [0,1] NDC z into the 24-bit-float + 8-bit-stencil layout the
// rasterizer's per-pixel `pack_depth` writes (cf. `TileRaster.cpp`). Inlined
// here so we don't have to add a new symbol to the render lane.
PSY_FORCEINLINE u32 pack_depth_u24(f32 z) noexcept {
    if (z < 0.0f)
        z = 0.0f;
    if (z > 1.0f)
        z = 1.0f;
    u32 raw;
    std::memcpy(&raw, &z, sizeof(raw));
    return raw & 0xFFFFFF00u;
}

// Build a primary ray direction for the (px+0.5, py+0.5) pixel sample
// using the derived camera. Identical math to sample_06's
// `primary_ray_dir`.
PSY_FORCEINLINE math::Vec3 primary_ray_dir(
    const DerivedCamera& cam, u32 px, u32 py, u32 fb_w, u32 fb_h) noexcept {
    const f32 nx = (static_cast<f32>(px) + 0.5f) / static_cast<f32>(fb_w);
    const f32 ny = (static_cast<f32>(py) + 0.5f) / static_cast<f32>(fb_h);
    const f32 sx = (2.0f * nx - 1.0f) * cam.aspect * cam.fov_tan;
    const f32 sy = (1.0f - 2.0f * ny) * cam.fov_tan;
    math::Vec3 d{
        cam.fwd.x + cam.right.x * sx + cam.up.x * sy,
        cam.fwd.y + cam.right.y * sx + cam.up.y * sy,
        cam.fwd.z + cam.right.z * sx + cam.up.z * sy,
    };
    return math::normalize(d);
}

// NDC z (in [0, 1]) for a world-space hit using the same view/proj the
// caller passed; byte-identical to the rasterizer's per-pixel z under
// `pack_depth`, so subsequent raster submits Z-test against the terrain
// correctly.
PSY_FORCEINLINE f32 ndc_z_for_world(const math::Mat4& view, const math::Mat4& proj, math::Vec3 wp) noexcept {
    const math::Vec4 v{wp.x, wp.y, wp.z, 1.0f};
    const math::Vec4 vview = math::mul(view, v);
    const math::Vec4 vclip = math::mul(proj, vview);
    if (vclip.w <= 1e-6f)
        return 1.0f;
    f32 z = vclip.z / vclip.w;
    z = z * 0.5f + 0.5f;
    if (z < 0.0f)
        return 0.0f;
    if (z > 1.0f)
        return 1.0f;
    return z;
}

// Splat-weights → RGBA8 with Lambert sun + distance haze. Matches the
// palette + lighting model sample_06 uses so M6 visual parity is preserved.
PSY_FORCEINLINE u32 splat_to_rgb(detail::SplatWeights s, f32 ndotl, f32 dist) noexcept {
    constexpr f32 grass[3] = {98.0f, 122.0f, 60.0f};
    constexpr f32 rock[3] = {118.0f, 102.0f, 82.0f};
    constexpr f32 sand[3] = {198.0f, 178.0f, 120.0f};
    constexpr f32 snow[3] = {230.0f, 232.0f, 240.0f};
    f32 r = grass[0] * s.w[0] + rock[0] * s.w[1] + sand[0] * s.w[2] + snow[0] * s.w[3];
    f32 g = grass[1] * s.w[0] + rock[1] * s.w[1] + sand[1] * s.w[2] + snow[1] * s.w[3];
    f32 b = grass[2] * s.w[0] + rock[2] * s.w[1] + sand[2] * s.w[2] + snow[2] * s.w[3];

    constexpr f32 sun_r = 1.20f, sun_g = 0.92f, sun_b = 0.74f;
    constexpr f32 amb_r = 0.32f, amb_g = 0.34f, amb_b = 0.46f;
    const f32 nl = ndotl < 0.0f ? 0.0f : ndotl;
    r *= (amb_r + sun_r * nl);
    g *= (amb_g + sun_g * nl);
    b *= (amb_b + sun_b * nl);

    constexpr f32 haze_r = 200.0f, haze_g = 150.0f, haze_b = 110.0f;
    const f32 fog = std::min(1.0f, dist / 360.0f);
    r = r * (1.0f - fog) + haze_r * fog;
    g = g * (1.0f - fog) + haze_g * fog;
    b = b * (1.0f - fog) + haze_b * fog;

    auto u8 = [](f32 v) noexcept -> u32 {
        if (v < 0.0f)
            return 0u;
        if (v > 255.0f)
            return 255u;
        return static_cast<u32>(v);
    };
    return u8(r) | (u8(g) << 8) | (u8(b) << 16) | (0xFFu << 24);
}

}  // namespace

void TerrainRaymarch::render(const math::Mat4& view, const math::Mat4& proj) const {
    // Recover state. No state ⇒ silent no-op (matches the Wave-A contract
    // for an unconfigured raymarcher).
    const RaymarchState* st = nullptr;
    for (auto& s : g_ray_slots) {
        if (s.key == this) {
            st = &s.ray;
            break;
        }
    }
    if (!st || !st->desc.heights || !st->target)
        return;
    render::Framebuffer& fb = *st->target;
    if (!fb.pixels || fb.width == 0 || fb.height == 0)
        return;
    if (fb.format != render::PixelFormat::RGBA8)
        return;

    const HeightmapDesc& hm = st->desc;
    const DerivedCamera cam = derive_camera(view, proj);

    // Per-column logarithmic march parameters, lifted unchanged from
    // sample_06 so the captured PNG matches before/after the wire-up.
    constexpr f32 kMaxTerrainY = 32.0f;
    constexpr f32 kStepNear = 0.4f;
    constexpr f32 kStepFar = 6.0f;
    constexpr f32 kStepFalloff = 90.0f;
    constexpr f32 kMaxT = 420.0f;

    // Lambert sun direction: low east-northeast, golden hour. Matches the
    // sample's lighting so colours are continuous when sample_06 drops
    // its inlined `render_terrain` in a follow-up.
    const math::Vec3 sun_dir = math::normalize(math::Vec3{0.55f, 0.50f, 0.20f});

    auto* pixels = reinterpret_cast<u32*>(fb.pixels);
    const u32 W = fb.width;
    const u32 H = fb.height;

    if (jobs::JobSystem::Get().worker_count() == 0u) {
        jobs::JobSystem::Get().start(0);
    }

    const u32 target_cores = terrain_target_cores();
    const u32 batch_rows = terrain_batch_rows(H, target_cores);
    capture_terrain_sched(H, target_cores, batch_rows);

    // Keep source-of-truth scalar hit/depth semantics, but schedule row
    // chunks with latency-pool preference on hetero hosts.
    run_rows_latency_pref(H, batch_rows, [&](u32 y_begin, u32 y_end) {
        constexpr u32 kBatch = 8;
        for (u32 y = y_begin; y < y_end; ++y) {
            for (u32 x0 = 0; x0 < W; x0 += kBatch) {
                const u32 lanes = std::min(kBatch, W - x0);

                alignas(32) f32 dir_x[8]{};
                alignas(32) f32 dir_y[8]{};
                alignas(32) f32 dir_z[8]{};
                bool lane_active[8]{};
                bool lane_hit[8]{};
                alignas(32) f32 hit_t[8]{};
                alignas(32) f32 prev_t[8]{};
                alignas(32) f32 prev_ry[8]{};
                alignas(32) f32 prev_th[8]{};
                alignas(32) f32 wx[8]{};
                alignas(32) f32 wz[8]{};
                alignas(32) f32 th[8]{};

                const f32 origin_th = detail::sample_bilinear(hm, cam.eye.x, cam.eye.z);
                for (u32 i = 0; i < lanes; ++i) {
                    const u32 x = x0 + i;
                    const math::Vec3 dir = primary_ray_dir(cam, x, y, W, H);
                    dir_x[i] = dir.x;
                    dir_y[i] = dir.y;
                    dir_z[i] = dir.z;

                    // Same skip rule as scalar path.
                    if (dir.y > 0.005f && cam.eye.y > kMaxTerrainY) {
                        lane_active[i] = false;
                        continue;
                    }
                    lane_active[i] = true;
                    prev_t[i] = 0.0f;
                    prev_ry[i] = cam.eye.y;
                    prev_th[i] = origin_th;
                }

                f32 t = kStepNear;
                while (t <= kMaxT) {
                    bool any_pending = false;
                    for (u32 i = 0; i < lanes; ++i) {
                        if (lane_active[i] && !lane_hit[i]) {
                            any_pending = true;
                            wx[i] = cam.eye.x + dir_x[i] * t;
                            wz[i] = cam.eye.z + dir_z[i] * t;
                        } else {
                            wx[i] = cam.eye.x;
                            wz[i] = cam.eye.z;
                        }
                    }
                    if (!any_pending)
                        break;

                    detail::simd_sample_bilinear8(hm, wx, wz, th);

                    for (u32 i = 0; i < lanes; ++i) {
                        if (!lane_active[i] || lane_hit[i])
                            continue;
                        const f32 wy = cam.eye.y + dir_y[i] * t;
                        const f32 h_i = th[i];
                        if (wy <= h_i) {
                            const f32 dy_a = prev_ry[i] - prev_th[i];
                            const f32 dy_b = wy - h_i;
                            const f32 denom = dy_a - dy_b;
                            f32 frac = denom > 0.0f ? (dy_a / denom) : 0.0f;
                            if (frac < 0.0f)
                                frac = 0.0f;
                            if (frac > 1.0f)
                                frac = 1.0f;
                            hit_t[i] = prev_t[i] + (t - prev_t[i]) * frac;
                            lane_hit[i] = true;
                        } else {
                            prev_t[i] = t;
                            prev_ry[i] = wy;
                            prev_th[i] = h_i;
                        }
                    }

                    const f32 step =
                        kStepNear + (kStepFar - kStepNear) * std::min(1.0f, t / kStepFalloff);
                    t += step;
                }

                for (u32 i = 0; i < lanes; ++i) {
                    if (!lane_hit[i])
                        continue;
                    const u32 x = x0 + i;
                    const usize idx = static_cast<usize>(y) * W + x;

                    const math::Vec3 hit_pos{
                        cam.eye.x + dir_x[i] * hit_t[i],
                        cam.eye.y + dir_y[i] * hit_t[i],
                        cam.eye.z + dir_z[i] * hit_t[i],
                    };

                    const f32 spacing = hm.spacing > 0.0f ? hm.spacing : 1.0f;
                    const i32 tx = static_cast<i32>(std::floor(hit_pos.x / spacing + 0.5f));
                    const i32 tz = static_cast<i32>(std::floor(hit_pos.z / spacing + 0.5f));
                    const auto splat = detail::splat_at_texel(hm, tx, tz);
                    const math::Vec3 n = detail::normal_at_texel(hm, tx, tz);
                    const f32 ndotl = math::dot(n, sun_dir);

                    pixels[idx] = splat_to_rgb(splat, ndotl, hit_t[i]);
                    if (fb.depth) {
                        const f32 z = ndc_z_for_world(view, proj, hit_pos);
                        fb.depth[idx] = pack_depth_u24(z);
                    }
                }
            }
        }
    });
}

// ─── set_target (sibling free function from TerrainTarget.h) ────────────
void set_target(const TerrainRaymarch& rm, render::Framebuffer* fb) noexcept {
    RaymarchState* st = ray_state_for(&rm);
    if (!st) {
        PSY_LOG_WARN("world_outdoor: TerrainRaymarch slot table full");
        return;
    }
    st->target = fb;
}

// ─── Spline track loader ─────────────────────────────────────────────────
// Wave-A loader: reads a packed flat array of f32 control points + width +
// banking from the VFS. The file format is intentionally trivial for
// Wave A; the editor (lane 18) will replace this with the cooked .psylevel
// stream in Wave B.
//
// Binary layout (little-endian):
//   u32 magic = 'P','S','T','K' (PSynder Track)
//   u32 segment_count
//   for each segment:
//     f32 p0.x p0.y p0.z   p1.x p1.y p1.z
//     f32 p2.x p2.y p2.z   p3.x p3.y p3.z
//     f32 half_width
//     f32 banking_rad
//
// On any read error we leave the output vector untouched. This means the
// sample tracks bundled with the engine can ship inline (see Wave B).
void load_spline_track(std::string_view virtual_path, std::vector<SplineRoadSegment>& segments_out) {
    auto& vfs = asset::Vfs::Get();
    auto blob = vfs.read(virtual_path);
    if (!blob.data || blob.bytes < 8)
        return;

    auto read_u32 = [](const u8* p) noexcept {
        u32 v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };
    auto read_f32 = [](const u8* p) noexcept {
        f32 v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    };

    const u32 magic = read_u32(blob.data);
    if (magic != 0x4B545350u)
        return;  // 'PSTK' (little-endian)
    const u32 nseg = read_u32(blob.data + 4);

    constexpr usize kBytesPerSeg = 14u * sizeof(f32);
    if (blob.bytes < 8u + static_cast<usize>(nseg) * kBytesPerSeg)
        return;

    segments_out.reserve(segments_out.size() + nseg);
    const u8* p = blob.data + 8;
    for (u32 i = 0; i < nseg; ++i) {
        SplineRoadSegment seg{};
        seg.p0 = math::Vec3{read_f32(p + 0), read_f32(p + 4), read_f32(p + 8)};
        seg.p1 = math::Vec3{read_f32(p + 12), read_f32(p + 16), read_f32(p + 20)};
        seg.p2 = math::Vec3{read_f32(p + 24), read_f32(p + 28), read_f32(p + 32)};
        seg.p3 = math::Vec3{read_f32(p + 36), read_f32(p + 40), read_f32(p + 44)};
        seg.half_width = read_f32(p + 48);
        seg.banking_rad = read_f32(p + 52);
        segments_out.push_back(seg);
        p += kBytesPerSeg;
    }
}

}  // namespace psynder::world::outdoor