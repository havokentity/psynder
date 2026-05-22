// SPDX-License-Identifier: MIT
// Psynder — shadow-visibility denoiser. Lane 08 owns.
//
// Edge-aware à-trous wavelet filter, **2 passes** guided by depth + normal
// (DESIGN.md §8.2). The à-trous (Dammertz 2010 "Edge-Avoiding À-Trous
// Wavelet Transform for fast Global Illumination Filtering") is a separable
// stationary-kernel filter where successive passes use the *same* 5-tap
// Gaussian kernel but with *exponentially widening* stride between samples
// (step = 1, 2, 4, …). Two passes give us a roughly 9-tap effective radius
// while remaining edge-aware via depth + normal weights.
//
// Properties relied on by tests:
//   * Bilateral weights are all ≥ 0; output is a weighted convex
//     combination of input samples, so output ∈ [min(in), max(in)] over
//     the kernel window. In particular, where every sample has visibility
//     ≥ V0, the filtered output is also ≥ V0. For shadow-visibility in
//     [0, 1] this gives the monotonicity required by the unit test.
//   * Output never goes negative (inputs assumed in [0, 1]).

#include "Bvh.h"

#include "core/console/Console.h"
#include "core/hardware/CpuFeatures.h"
#include "jobs/JobSystem.h"
#include "jobs/JobSystemHetero.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

namespace psynder::render::rt {

namespace {

PSY_CVAR(r_rt_denoise_parallel,
         "1",
         "Enable parallel row scheduling for RT denoiser (0/1)",
         console::CVarFlags::Archive);
PSY_CVAR(r_rt_denoise_cpu_cores_hint,
         "0",
         "Preferred RT denoiser cores (0=auto; auto uses perf-cores-2)",
         console::CVarFlags::Archive);
PSY_CVAR(r_rt_denoise_batch_rows,
         "0",
         "Rows per RT denoiser job batch (0=auto from core hint)",
         console::CVarFlags::Archive);
PSY_CVAR(r_rt_denoise_min_rows_per_core,
         "8",
         "Minimum rows per RT denoiser worker chunk in auto mode",
         console::CVarFlags::Archive);

struct DenoiseSchedSnapshot {
    bool valid = false;
    u32 width = 0;
    u32 height = 0;
    int core_hint = 0;
    u32 target_cores = 0;
    int min_rows = 0;
    u32 batch_rows = 0;
    u32 chunks = 0;
    bool hetero = false;
    u32 p_cores = 0;
    u32 e_cores = 0;
    u32 workers = 0;
    u32 latency_workers = 0;
    u32 throughput_workers = 0;
};

DenoiseSchedSnapshot g_denoise_sched{};

u32 clamp_core_hint_to_runtime(u32 requested) noexcept {
    const u32 workers = jobs::JobSystem::Get().worker_count();
    const u32 runtime_max = workers > 0u ? (workers + 1u) : 1u;
    return std::clamp(requested, 1u, runtime_max);
}

u32 denoise_target_cores() noexcept {
    const int hint = r_rt_denoise_cpu_cores_hint ? r_rt_denoise_cpu_cores_hint->GetInt() : 0;
    if (hint > 0)
        return clamp_core_hint_to_runtime(static_cast<u32>(hint));

    u32 perf_like = 0;
    const jobs::HeteroCounts hc = jobs::hetero_detected_counts();
    if (hc.p_cores > 0u)
        perf_like = hc.p_cores;
    else
        perf_like = hardware::detect().cores_physical;
    if (perf_like == 0u)
        perf_like = 1u;
    const u32 auto_target = perf_like > 2u ? (perf_like - 2u) : perf_like;
    return clamp_core_hint_to_runtime(auto_target);
}

u32 denoise_batch_rows(u32 total_rows, u32 target_cores) noexcept {
    const int batch_hint = r_rt_denoise_batch_rows ? r_rt_denoise_batch_rows->GetInt() : 0;
    if (batch_hint > 0)
        return std::clamp(static_cast<u32>(batch_hint), 1u, std::max(1u, total_rows));

    const int min_rows_hint =
        r_rt_denoise_min_rows_per_core ? r_rt_denoise_min_rows_per_core->GetInt() : 8;
    const u32 min_rows = std::clamp(static_cast<u32>(min_rows_hint > 0 ? min_rows_hint : 8),
                                    1u,
                                    std::max(1u, total_rows));
    const u32 rows_from_cores = std::max(1u, (total_rows + target_cores - 1u) / target_cores);
    return std::clamp(std::max(min_rows, rows_from_cores), 1u, std::max(1u, total_rows));
}

template <class Fn>
void run_rows_latency_pref(u32 total_rows, u32 batch_rows, Fn& fn) {
    if (total_rows == 0u)
        return;
    const u32 grain = std::max(1u, batch_rows);
    const u32 chunks = (total_rows + grain - 1u) / grain;
    if (chunks <= 1u) {
        fn(0u, total_rows);
        return;
    }

    if (!jobs::hetero_is_active()) {
        jobs::JobSystem::Get().parallel_for(0, total_rows, grain, [&](usize y0, usize y1) {
            fn(static_cast<u32>(y0), static_cast<u32>(y1));
        });
        return;
    }

    struct RowTask {
        u32 y0 = 0;
        u32 y1 = 0;
        const void* body = nullptr;
    };
    auto run = +[](void* u) noexcept {
        auto* t = static_cast<RowTask*>(u);
        auto* f = static_cast<const Fn*>(t->body);
        (*f)(t->y0, t->y1);
    };

    std::vector<RowTask> tasks(chunks);
    std::vector<jobs::JobHandle> handles(chunks);
    for (u32 i = 0; i < chunks; ++i) {
        const u32 y0 = i * grain;
        const u32 y1 = std::min(total_rows, y0 + grain);
        tasks[i].y0 = y0;
        tasks[i].y1 = y1;
        tasks[i].body = &fn;
        jobs::JobDesc d{};
        d.fn = run;
        d.user = &tasks[i];
        d.name = "rt_denoise_rows";
        handles[i] = jobs::submit_latency(d);
    }
    auto& js = jobs::JobSystem::Get();
    for (const jobs::JobHandle h : handles)
        js.wait(h);
}

void capture_sched(u32 width, u32 height, u32 target_cores, u32 batch_rows) {
    const jobs::HeteroCounts hc = jobs::hetero_detected_counts();
    const int core_hint = r_rt_denoise_cpu_cores_hint ? r_rt_denoise_cpu_cores_hint->GetInt() : 0;
    const int min_rows = r_rt_denoise_min_rows_per_core ? r_rt_denoise_min_rows_per_core->GetInt() : 8;
    g_denoise_sched.valid = true;
    g_denoise_sched.width = width;
    g_denoise_sched.height = height;
    g_denoise_sched.core_hint = core_hint;
    g_denoise_sched.target_cores = target_cores;
    g_denoise_sched.min_rows = min_rows;
    g_denoise_sched.batch_rows = batch_rows;
    g_denoise_sched.chunks = batch_rows > 0u ? ((height + batch_rows - 1u) / batch_rows) : height;
    g_denoise_sched.hetero = jobs::hetero_is_active();
    g_denoise_sched.p_cores = hc.p_cores;
    g_denoise_sched.e_cores = hc.e_cores;
    g_denoise_sched.workers = jobs::JobSystem::Get().worker_count();
    g_denoise_sched.latency_workers = jobs::hetero_latency_workers();
    g_denoise_sched.throughput_workers = jobs::hetero_throughput_workers();
}

void ensure_denoise_console_init() {
    static bool once = false;
    if (once)
        return;
    once = true;
    auto& con = console::Console::Get();
    con.RegisterCommand(
        "r_rt_denoise_sched_dump",
        "Print RT denoiser scheduler snapshot from last run",
        [](std::span<const std::string_view> /*args*/, console::Output& out) {
            if (!g_denoise_sched.valid) {
                out.PrintLine("rt_denoise_sched: no run yet");
                return;
            }
            const auto& s = g_denoise_sched;
            out.FormatLine(
                "rt_denoise_sched: {}x{}, hint_cores={}, target_cores={}, min_rows={}, "
                "batch_rows={}, "
                "chunks={}, hetero={} (p={}, e={}), workers={} (latency={}, throughput={})",
                s.width,
                s.height,
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
}

PSY_FORCEINLINE
f32 gauss5(i32 d) noexcept {
    // 1/16, 4/16, 6/16, 4/16, 1/16 — discrete Gaussian.
    constexpr f32 w[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};
    const i32 idx = std::clamp(d + 2, 0, 4);
    return w[idx];
}

// Bilateral weight from a depth delta.
// σ_z = 1/32 — about a third of a unit, matches typical world-space depth
// scales in shadow space. Falls off as e^{-|Δz| / σ_z}.
PSY_FORCEINLINE
f32 depth_weight(f32 dz) noexcept {
    return std::exp(-std::fabs(dz) * 32.0f);
}

// Bilateral weight from the angle between two normals. Uses (n·n')^4 to
// give a tight lobe — sharp creases stop the filter.
PSY_FORCEINLINE
f32 normal_weight(const f32* n0, const f32* n1) noexcept {
    const f32 d = n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2];
    const f32 c = std::clamp(d, 0.0f, 1.0f);
    return c * c * c * c;
}

// One à-trous pass. `step` is the stride between samples in pixels — the
// classic à-trous schedule doubles step each pass (1, 2, 4, …). With a
// 5-tap base kernel this produces a 1+4*step radius per pass.
void atrous_pass(
    const f32* in, f32* out, const f32* depth, const f32* normals, u32 width, u32 height, i32 step) {
    const auto run_rows = [&](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u32 idx = y * width + x;
                const f32 d0 = depth[idx];
                const f32 n0[3] = {normals[idx * 3], normals[idx * 3 + 1], normals[idx * 3 + 2]};
                f32 sum_w = 0.0f;
                f32 sum_v = 0.0f;
                for (i32 dy = -2; dy <= 2; ++dy) {
                    for (i32 dx = -2; dx <= 2; ++dx) {
                        const i32 sx = static_cast<i32>(x) + dx * step;
                        const i32 sy = static_cast<i32>(y) + dy * step;
                        if (sx < 0 || sx >= static_cast<i32>(width))
                            continue;
                        if (sy < 0 || sy >= static_cast<i32>(height))
                            continue;
                        const u32 sidx = static_cast<u32>(sy) * width + static_cast<u32>(sx);
                        const f32 d1 = depth[sidx];
                        const f32 n1[3] = {normals[sidx * 3],
                                           normals[sidx * 3 + 1],
                                           normals[sidx * 3 + 2]};
                        const f32 wd = depth_weight(d1 - d0);
                        const f32 wn = normal_weight(n0, n1);
                        const f32 wg = gauss5(dx) * gauss5(dy);
                        const f32 w = wg * wd * wn;
                        sum_w += w;
                        sum_v += w * in[sidx];
                    }
                }
                // Center pixel always has full weight (wg=6/16*6/16=36/256,
                // wd=1, wn=1), so sum_w > 0 always; the guard is defensive.
                out[idx] = (sum_w > 0.0f) ? (sum_v / sum_w) : in[idx];
                // Clamp to [0, max-in-kernel] is implicit because output is a
                // non-negative convex combination of non-negative inputs.
            }
        }
    };

    if (!(r_rt_denoise_parallel && r_rt_denoise_parallel->GetBool())) {
        run_rows(0u, height);
        return;
    }

    if (jobs::JobSystem::Get().worker_count() == 0u)
        jobs::JobSystem::Get().start(0);

    const u32 target_cores = denoise_target_cores();
    const u32 batch_rows = denoise_batch_rows(height, target_cores);
    capture_sched(width, height, target_cores, batch_rows);
    run_rows_latency_pref(height, batch_rows, run_rows);
}

}  // namespace

void denoise_shadows(const DenoiseInput& in, f32* output_visibility) {
    ensure_denoise_console_init();
    if (!in.shadow_visibility || !in.depth || !in.normals || !output_visibility)
        return;
    if (in.width == 0 || in.height == 0)
        return;

    const usize n = static_cast<usize>(in.width) * in.height;
    // Two-pass à-trous: pass 1 step=1, pass 2 step=2 (classic schedule).
    // Use the output as a ping-pong target alongside a thread-local temp.
    static thread_local std::vector<f32> tmp;
    if (tmp.size() < n)
        tmp.resize(n);

    atrous_pass(in.shadow_visibility, tmp.data(), in.depth, in.normals, in.width, in.height, 1);
    atrous_pass(tmp.data(), output_visibility, in.depth, in.normals, in.width, in.height, 2);
}

void ensure_denoise_console_commands_registered() {
    ensure_denoise_console_init();
}

}  // namespace psynder::render::rt
