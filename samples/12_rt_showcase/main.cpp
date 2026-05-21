// SPDX-License-Identifier: MIT
// Psynder — Sample 12. Raytracing showcase: a dense scene through the BVH8
// TLAS to stress the pure-CPU raytracer harder than sample_05.
//
// The scene: a dark ground plane plus a parametric field of cubes and
// spheres of varying height (two BLAS meshes — a unit cube and a unit
// sphere — reused across dozens of TLAS instances; that reuse is exactly
// what the TLAS is for). The field is lit by six orbiting colored point
// lights, each casting traced shadows. Per pixel we resolve primary
// visibility against the TLAS, then trace per-light shadow rays gathered
// into 8-wide ShadowPacket8 batches via
// `psynder::render::rt::trace_shadow_packet`. Shading is Lambert plus
// inverse-square attenuation, mirroring sample_05. The skybox is a vertical
// gradient.
//
// To keep the run real-time (and the smoke run fast), the lit pass runs at
// quarter resolution and is bilinear-upsampled into the final framebuffer.
//
// CLI flags:
//   --smoke-frames=N         Headless CI run for N frames then exit.
//   --smoke-frames N         Space-separated form (matches Goldens.cmake).
//   --smoke-capture-out PATH Write the final framebuffer to PATH as PNG.
//   --rt-ao=0|1              Override sample AO CVar for capture/perf checks.
//   --rt-ao-debug=0|1        Show AO visibility as grayscale when AO is enabled.
//   --rt-ao-samples=N        Override AO sample count for capture/perf checks.
//   --rt-ao-radius=F         Override AO radius for capture/perf checks.
//   --rt-ao-strength=F       Override AO ambient strength for capture/perf checks.
//   --rt-ao-lit-strength=F   Override AO direct-light strength for capture/perf checks.
//   --rt-cores=N             Override sample RT worker chunk target for smoke/perf checks.

#include "common/PngWriter.h"

#include "core/console/Console.h"
#include "core/hardware/CpuFeatures.h"
#include "core/Log.h"
#include "core/Types.h"
#include "editor/core/Editor.h"
#include "editor/core/SampleHook.h"
#include "jobs/JobSystem.h"
#include "jobs/JobSystemHetero.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"
#include "render/rt/Bvh.h"
#include "ui/console/ConsoleOverlay.h"
#include "ui/imm/DebugHud.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace psynder::render::rt {
void ensure_denoise_console_commands_registered();
}

namespace {

console::CVar* g_rt_parallel = nullptr;
console::CVar* g_rt_cores_hint = nullptr;
console::CVar* g_rt_batch_rows = nullptr;
console::CVar* g_rt_min_rows = nullptr;
console::CVar* g_rt_parallel_min_rows_gate = nullptr;
console::CVar* g_rt_parallel_max_chunks = nullptr;
console::CVar* g_rt_ao = nullptr;
console::CVar* g_rt_ao_samples = nullptr;
console::CVar* g_rt_ao_radius = nullptr;
console::CVar* g_rt_ao_strength = nullptr;
console::CVar* g_rt_ao_lit_strength = nullptr;
console::CVar* g_rt_ao_denoise = nullptr;
console::CVar* g_rt_ao_debug = nullptr;

u32 rt_parallel_chunk_limit(u32 work_items);

void ensure_rt_parallel_cvars() {
    static bool once = false;
    if (once)
        return;
    once = true;
    auto& con = console::Console::Get();
    g_rt_parallel = con.RegisterCVar("r_rt_sample_parallel",
                                     "1",
                                     "Enable sample RT pass row-parallel scheduling (0/1)",
                                     console::CVAR_ARCHIVE);
    g_rt_cores_hint = con.RegisterCVar("r_rt_sample_cpu_cores_hint",
                                       "0",
                                       "Preferred sample RT pass cores (0=auto perf-cores-2)",
                                       console::CVAR_ARCHIVE);
    g_rt_batch_rows = con.RegisterCVar("r_rt_sample_batch_rows",
                                       "0",
                                       "Rows per sample RT pass chunk (0=auto)",
                                       console::CVAR_ARCHIVE);
    g_rt_min_rows = con.RegisterCVar("r_rt_sample_min_rows_per_core",
                                     "8",
                                     "Minimum sample RT pass rows per worker chunk in auto mode",
                                     console::CVAR_ARCHIVE);
    g_rt_parallel_min_rows_gate = con.RegisterCVar("r_rt_sample_parallel_min_rows",
                                                   "0",
                                                   "Disable RT sample row parallelism below this row count (0=always parallel)",
                                                   console::CVAR_ARCHIVE);
    g_rt_parallel_max_chunks = con.RegisterCVar("r_rt_sample_parallel_max_chunks",
                                                "4",
                                                 "Maximum auto-mode chunks per RT sample pass (explicit core hint overrides this)",
                                                console::CVAR_ARCHIVE);
    g_rt_ao = con.RegisterCVar("r_rt_sample_ao",
                               "0",
                               "Enable sample RT ambient occlusion (0/1)",
                               console::CVAR_ARCHIVE);
    g_rt_ao_samples = con.RegisterCVar("r_rt_sample_ao_samples",
                                        "4",
                                       "AO rays per low-res hit pixel (1..8)",
                                       console::CVAR_ARCHIVE);
    g_rt_ao_radius = con.RegisterCVar("r_rt_sample_ao_radius",
                                      "3.0",
                                      "AO ray max distance in world units",
                                      console::CVAR_ARCHIVE);
    g_rt_ao_strength = con.RegisterCVar("r_rt_sample_ao_strength",
                                        "1.00",
                                        "AO ambient darkening strength (0..1)",
                                        console::CVAR_ARCHIVE);
    g_rt_ao_lit_strength = con.RegisterCVar("r_rt_sample_ao_lit_strength",
                                            "0.75",
                                            "Extra AO multiplier applied to direct lighting for visible contact AO (0..1)",
                                            console::CVAR_ARCHIVE);
    g_rt_ao_denoise = con.RegisterCVar("r_rt_sample_ao_denoise",
                                       "1",
                                       "Edge-aware denoise sample AO visibility (0/1)",
                                       console::CVAR_ARCHIVE);
    g_rt_ao_debug = con.RegisterCVar("r_rt_sample_ao_debug",
                                     "0",
                                     "Show AO visibility as grayscale when sample AO is enabled (0/1)",
                                     console::CVAR_ARCHIVE);
    con.RegisterCommand("r_rt_sample_sched_dump",
                        "Print sample RT scheduler settings",
                        [](std::span<const std::string_view>, console::Output& out) {
                            const int en = g_rt_parallel ? g_rt_parallel->GetInt() : 1;
                            const int ch = g_rt_cores_hint ? g_rt_cores_hint->GetInt() : 0;
                            const int br = g_rt_batch_rows ? g_rt_batch_rows->GetInt() : 0;
                            const int mr = g_rt_min_rows ? g_rt_min_rows->GetInt() : 8;
                            const int gate = g_rt_parallel_min_rows_gate ? g_rt_parallel_min_rows_gate->GetInt() : 192;
                            const int max_chunks = g_rt_parallel_max_chunks ? g_rt_parallel_max_chunks->GetInt() : 4;
                            const u32 effective_jobs = rt_parallel_chunk_limit(144u);
                            const int ao = g_rt_ao ? g_rt_ao->GetInt() : 1;
                            const int ao_samples = g_rt_ao_samples ? g_rt_ao_samples->GetInt() : 2;
                            const f32 ao_radius = g_rt_ao_radius ? g_rt_ao_radius->GetFloat() : 2.2f;
                            const f32 ao_strength = g_rt_ao_strength ? g_rt_ao_strength->GetFloat() : 0.7f;
                            const f32 ao_lit_strength = g_rt_ao_lit_strength ? g_rt_ao_lit_strength->GetFloat() : 0.75f;
                            const int ao_denoise = g_rt_ao_denoise ? g_rt_ao_denoise->GetInt() : 1;
                            const int ao_debug = g_rt_ao_debug ? g_rt_ao_debug->GetInt() : 0;
                            const auto hc = jobs::hetero_detected_counts();
                            out.FormatLine(
                                "rt_sample_sched: parallel={}, hint_cores={}, batch_rows={}, min_rows={}, "
                                "min_parallel_rows={}, max_chunks={}, effective_tile_jobs={} | "
                                "ao={}, ao_samples={}, ao_radius={}, "
                                "ao_strength={}, ao_lit_strength={}, ao_denoise={}, ao_debug={} | "
                                "workers={} hetero={} (p={}, e={})",
                                en,
                                ch,
                                br,
                                mr,
                                gate,
                                max_chunks,
                                 effective_jobs,
                                ao,
                                ao_samples,
                                ao_radius,
                                ao_strength,
                                ao_lit_strength,
                                ao_denoise,
                                ao_debug,
                                jobs::JobSystem::Get().worker_count(),
                                jobs::hetero_is_active() ? 1 : 0,
                                hc.p_cores,
                                hc.e_cores);
                        });
}

u32 rt_ao_sample_count() noexcept {
    const int samples = g_rt_ao_samples ? g_rt_ao_samples->GetInt() : 4;
    return std::clamp(static_cast<u32>(samples > 0 ? samples : 4), 1u, 8u);
}

f32 rt_ao_radius() noexcept {
    const f32 radius = g_rt_ao_radius ? g_rt_ao_radius->GetFloat() : 3.0f;
    return std::clamp(radius, 0.05f, 20.0f);
}

f32 rt_ao_strength() noexcept {
    const f32 strength = g_rt_ao_strength ? g_rt_ao_strength->GetFloat() : 1.0f;
    return std::clamp(strength, 0.0f, 1.0f);
}

f32 rt_ao_lit_strength() noexcept {
    const f32 strength = g_rt_ao_lit_strength ? g_rt_ao_lit_strength->GetFloat() : 0.75f;
    return std::clamp(strength, 0.0f, 1.0f);
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

u32 rt_target_cores() {
    const int hint = g_rt_cores_hint ? g_rt_cores_hint->GetInt() : 0;
    const u32 workers = jobs::JobSystem::Get().worker_count();
    const u32 runtime_max = workers > 0u ? (workers + 1u) : 1u;
    if (hint > 0)
        return std::clamp(static_cast<u32>(hint), 1u, runtime_max);
    const auto hc = jobs::hetero_detected_counts();
    u32 perf_like = hc.p_cores > 0u ? hc.p_cores : hardware::detect().cores_physical;
    if (perf_like == 0u)
        perf_like = 1u;
    const u32 auto_target = perf_like > 2u ? (perf_like - 2u) : perf_like;
    return std::clamp(auto_target, 1u, runtime_max);
}

bool rt_has_explicit_cores_hint() noexcept {
    return g_rt_cores_hint && g_rt_cores_hint->GetInt() > 0;
}

u32 rt_parallel_chunk_limit(u32 work_items) {
    if (work_items <= 1u)
        return work_items;
    u32 chunks = std::clamp(rt_target_cores(), 1u, work_items);

    // The explicit cores hint is a hard cap for sample RT work. The separate
    // max-chunks CVar is an auto-mode throttle, so it no longer masks runtime
    // changes like `r_rt_sample_cpu_cores_hint 8` behind the old default of 4.
    if (!rt_has_explicit_cores_hint()) {
        const int max_chunks_i = g_rt_parallel_max_chunks ? g_rt_parallel_max_chunks->GetInt() : 4;
        if (max_chunks_i > 0)
            chunks = std::min(chunks, static_cast<u32>(max_chunks_i));
    }
    return std::clamp(chunks, 1u, work_items);
}

[[maybe_unused]] u32 rt_rows_grain(u32 rows) {
    const int b = g_rt_batch_rows ? g_rt_batch_rows->GetInt() : 0;
    if (b > 0)
        return std::clamp(static_cast<u32>(b), 1u, std::max(1u, rows));
    const int m = g_rt_min_rows ? g_rt_min_rows->GetInt() : 8;
    const u32 min_rows = std::clamp(static_cast<u32>(m > 0 ? m : 8), 1u, std::max(1u, rows));
    const u32 target_chunks = rt_parallel_chunk_limit(rows);
    const u32 rows_from_cores = std::max(1u, (rows + target_chunks - 1u) / target_chunks);
    u32 grain = std::clamp(std::max(min_rows, rows_from_cores), 1u, std::max(1u, rows));
    return std::clamp(grain, 1u, std::max(1u, rows));
}

template <class Fn>
void rt_parallel_rows(u32 rows, Fn&& body) {
    const int gate_i = g_rt_parallel_min_rows_gate ? g_rt_parallel_min_rows_gate->GetInt() : 192;
    const u32 gate = gate_i > 0 ? static_cast<u32>(gate_i) : 0u;
    if (!(g_rt_parallel && g_rt_parallel->GetBool())) {
        body(0u, rows);
        return;
    }
    if (gate > 0u && rows < gate) {
        body(0u, rows);
        return;
    }
    if (jobs::JobSystem::Get().worker_count() == 0u)
        jobs::JobSystem::Get().start(0);
    const u32 grain = rt_rows_grain(rows);
    jobs::JobSystem::Get().parallel_for(0, rows, grain, [&](usize y0, usize y1) {
        body(static_cast<u32>(y0), static_cast<u32>(y1));
    });
}

template <class Fn>
void rt_parallel_tiles(u32 width, u32 height, u32 tile, Fn&& body) {
    const u32 tiles_x = (width + tile - 1u) / tile;
    const u32 tiles_y = (height + tile - 1u) / tile;
    const u32 tile_count = tiles_x * tiles_y;
    if (!(g_rt_parallel && g_rt_parallel->GetBool()) || tile_count <= 1u) {
        body(0u, 0u, width, height);
        return;
    }
    if (jobs::JobSystem::Get().worker_count() == 0u)
        jobs::JobSystem::Get().start(0);
    const u32 active_jobs = rt_parallel_chunk_limit(tile_count);
    constexpr u32 kTileBatch = 2u;
    std::atomic<u32> next_tile{0u};
    jobs::JobSystem::Get().parallel_for(0, active_jobs, 1, [&](usize, usize) {
        for (;;) {
            const u32 first_tile = next_tile.fetch_add(kTileBatch, std::memory_order_relaxed);
            if (first_tile >= tile_count)
                break;
            const u32 last_tile = std::min(tile_count, first_tile + kTileBatch);
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
}

// ─── CLI parsing ─────────────────────────────────────────────────────────
struct Args {
    u32 smoke_frames = 0;
    std::string capture_out;
    int rt_ao = -1;
    int rt_ao_debug = -1;
    std::string rt_cores_hint;
    std::string rt_ao_samples;
    std::string rt_ao_radius;
    std::string rt_ao_strength;
    std::string rt_ao_lit_strength;
};

u32 parse_uint(std::string_view v) noexcept {
    u32 out = 0;
    for (char c : v) {
        if (c < '0' || c > '9')
            return 0;
        out = out * 10u + static_cast<u32>(c - '0');
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a{};
    constexpr std::string_view kFlag = "--smoke-frames=";
    constexpr std::string_view kFlagSp = "--smoke-frames";
    constexpr std::string_view kCapEq = "--smoke-capture-out=";
    constexpr std::string_view kCapSp = "--smoke-capture-out";
    constexpr std::string_view kAoEq = "--rt-ao=";
    constexpr std::string_view kAoSp = "--rt-ao";
    constexpr std::string_view kAoDebugEq = "--rt-ao-debug=";
    constexpr std::string_view kAoDebugSp = "--rt-ao-debug";
    constexpr std::string_view kAoSamplesEq = "--rt-ao-samples=";
    constexpr std::string_view kAoSamplesSp = "--rt-ao-samples";
    constexpr std::string_view kAoRadiusEq = "--rt-ao-radius=";
    constexpr std::string_view kAoRadiusSp = "--rt-ao-radius";
    constexpr std::string_view kAoStrengthEq = "--rt-ao-strength=";
    constexpr std::string_view kAoStrengthSp = "--rt-ao-strength";
    constexpr std::string_view kAoLitStrengthEq = "--rt-ao-lit-strength=";
    constexpr std::string_view kAoLitStrengthSp = "--rt-ao-lit-strength";
    constexpr std::string_view kRtCoresEq = "--rt-cores=";
    constexpr std::string_view kRtCoresSp = "--rt-cores";
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s.starts_with(kFlag)) {
            a.smoke_frames = parse_uint(s.substr(kFlag.size()));
        } else if (s == kFlagSp && i + 1 < argc) {
            a.smoke_frames = parse_uint(std::string_view{argv[++i]});
        } else if (s.starts_with(kCapEq)) {
            a.capture_out = std::string(s.substr(kCapEq.size()));
        } else if (s == kCapSp && i + 1 < argc) {
            a.capture_out = argv[++i];
        } else if (s.starts_with(kAoEq)) {
            a.rt_ao = parse_uint(s.substr(kAoEq.size())) != 0u ? 1 : 0;
        } else if (s == kAoSp && i + 1 < argc) {
            a.rt_ao = parse_uint(std::string_view{argv[++i]}) != 0u ? 1 : 0;
        } else if (s.starts_with(kAoDebugEq)) {
            a.rt_ao_debug = parse_uint(s.substr(kAoDebugEq.size())) != 0u ? 1 : 0;
        } else if (s == kAoDebugSp && i + 1 < argc) {
            a.rt_ao_debug = parse_uint(std::string_view{argv[++i]}) != 0u ? 1 : 0;
        } else if (s.starts_with(kAoSamplesEq)) {
            a.rt_ao_samples = std::string(s.substr(kAoSamplesEq.size()));
        } else if (s == kAoSamplesSp && i + 1 < argc) {
            a.rt_ao_samples = argv[++i];
        } else if (s.starts_with(kAoRadiusEq)) {
            a.rt_ao_radius = std::string(s.substr(kAoRadiusEq.size()));
        } else if (s == kAoRadiusSp && i + 1 < argc) {
            a.rt_ao_radius = argv[++i];
        } else if (s.starts_with(kAoStrengthEq)) {
            a.rt_ao_strength = std::string(s.substr(kAoStrengthEq.size()));
        } else if (s == kAoStrengthSp && i + 1 < argc) {
            a.rt_ao_strength = argv[++i];
        } else if (s.starts_with(kAoLitStrengthEq)) {
            a.rt_ao_lit_strength = std::string(s.substr(kAoLitStrengthEq.size()));
        } else if (s == kAoLitStrengthSp && i + 1 < argc) {
            a.rt_ao_lit_strength = argv[++i];
        } else if (s.starts_with(kRtCoresEq)) {
            a.rt_cores_hint = std::string(s.substr(kRtCoresEq.size()));
        } else if (s == kRtCoresSp && i + 1 < argc) {
            a.rt_cores_hint = argv[++i];
        }
    }
    return a;
}

void apply_rt_arg_overrides(const Args& args) {
    if (!args.rt_cores_hint.empty() && g_rt_cores_hint) {
        g_rt_cores_hint->value = args.rt_cores_hint;
    }
    if (args.rt_ao >= 0 && g_rt_ao) {
        g_rt_ao->value = args.rt_ao != 0 ? "1" : "0";
    }
    if (args.rt_ao_debug >= 0 && g_rt_ao_debug) {
        g_rt_ao_debug->value = args.rt_ao_debug != 0 ? "1" : "0";
    }
    if (!args.rt_ao_samples.empty() && g_rt_ao_samples) {
        g_rt_ao_samples->value = args.rt_ao_samples;
    }
    if (!args.rt_ao_radius.empty() && g_rt_ao_radius) {
        g_rt_ao_radius->value = args.rt_ao_radius;
    }
    if (!args.rt_ao_strength.empty() && g_rt_ao_strength) {
        g_rt_ao_strength->value = args.rt_ao_strength;
    }
    if (!args.rt_ao_lit_strength.empty() && g_rt_ao_lit_strength) {
        g_rt_ao_lit_strength->value = args.rt_ao_lit_strength;
    }
}

// ─── Render config ───────────────────────────────────────────────────────
// Internal framebuffer (small — raytracing per pixel is expensive). The
// shadow pass runs at a quarter of these dimensions and is bilinear-
// upsampled into the final image. Same resolution as sample_05 so the
// per-frame budget stays real-time even with the much larger instance set.
constexpr u32 kFbW = 1024;
constexpr u32 kFbH = 512;
constexpr u32 kShadowW = kFbW / 2;  // 256
constexpr u32 kShadowH = kFbH / 2;  // 144
constexpr u32 kRtTile = 16;
constexpr u32 kNumLights = 6;

// Field layout: a kFieldDim × kFieldDim grid of pillars on the ground.
// Each cell is either a cube or a sphere, chosen by parity, at a height
// driven by a smooth radial function. With kFieldDim = 7 that is 49 field
// instances + 1 ground = 50 TLAS instances total.
constexpr u32 kFieldDim = 7;
constexpr u32 kFieldCount = kFieldDim * kFieldDim;
constexpr u32 kNumInstances = kFieldCount + 1u;  // + ground
constexpr f32 kCellSpacing = 2.0f;

// Pack RGBA8 little-endian.
PSY_FORCEINLINE u32 pack_rgba8(u32 r, u32 g, u32 b, u32 a = 0xFFu) noexcept {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}
PSY_FORCEINLINE u32 clamp_u8(f32 v) noexcept {
    if (v < 0.0f)
        return 0u;
    if (v > 255.0f)
        return 255u;
    return static_cast<u32>(v);
}

// ─── Scene geometry ──────────────────────────────────────────────────────
// Generate a unit cube's 12 triangles centered at the origin with half-
// extent 0.5 (so it spans [-0.5, +0.5] on each axis).
void emit_unit_cube(std::vector<render::rt::Triangle>& out) {
    using math::Vec3;
    // 8 corners of the unit cube.
    const Vec3 c000{-0.5f, -0.5f, -0.5f}, c100{+0.5f, -0.5f, -0.5f};
    const Vec3 c010{-0.5f, +0.5f, -0.5f}, c110{+0.5f, +0.5f, -0.5f};
    const Vec3 c001{-0.5f, -0.5f, +0.5f}, c101{+0.5f, -0.5f, +0.5f};
    const Vec3 c011{-0.5f, +0.5f, +0.5f}, c111{+0.5f, +0.5f, +0.5f};
    // -Z face (back)
    out.push_back({c000, c110, c100});
    out.push_back({c000, c010, c110});
    // +Z face (front)
    out.push_back({c001, c101, c111});
    out.push_back({c001, c111, c011});
    // -X face (left)
    out.push_back({c000, c001, c011});
    out.push_back({c000, c011, c010});
    // +X face (right)
    out.push_back({c100, c111, c101});
    out.push_back({c100, c110, c111});
    // -Y face (bottom)
    out.push_back({c000, c101, c001});
    out.push_back({c000, c100, c101});
    // +Y face (top)
    out.push_back({c010, c011, c111});
    out.push_back({c010, c111, c110});
}

// Generate a unit-radius UV sphere centered at the origin (radius 0.5 so it
// matches the unit cube's footprint and the same per-instance scale maps
// cleanly to a diameter). `stacks` latitude bands × `slices` longitude
// segments triangulated into a closed shell.
void emit_unit_sphere(std::vector<render::rt::Triangle>& out, u32 stacks, u32 slices) {
    using math::Vec3;
    const f32 radius = 0.5f;
    auto vert = [&](u32 stack, u32 slice) -> Vec3 {
        const f32 v = static_cast<f32>(stack) / static_cast<f32>(stacks);  // 0..1 (pole→pole)
        const f32 u = static_cast<f32>(slice) / static_cast<f32>(slices);  // 0..1 around
        const f32 phi = v * math::kPi;                                     // 0..pi latitude
        const f32 theta = u * 2.0f * math::kPi;                            // 0..2pi longitude
        const f32 sp = std::sin(phi);
        return Vec3{radius * sp * std::cos(theta), radius * std::cos(phi), radius * sp * std::sin(theta)};
    };
    for (u32 st = 0; st < stacks; ++st) {
        for (u32 sl = 0; sl < slices; ++sl) {
            const Vec3 a = vert(st, sl);
            const Vec3 b = vert(st + 1, sl);
            const Vec3 c = vert(st + 1, sl + 1);
            const Vec3 d = vert(st, sl + 1);
            // Top cap degenerates the first band's lower edge, bottom cap
            // the last band's upper edge; the engine BVH tolerates the few
            // sliver triangles and the visual cost is nil.
            if (st != 0)
                out.push_back({a, b, c});
            if (st != stacks - 1)
                out.push_back({a, c, d});
        }
    }
}

// Two big triangles that form a ground quad on the XZ plane at y = 0.
void emit_ground(std::vector<render::rt::Triangle>& out, f32 half) {
    using math::Vec3;
    const Vec3 p00{-half, 0.0f, -half}, p10{+half, 0.0f, -half};
    const Vec3 p01{-half, 0.0f, +half}, p11{+half, 0.0f, +half};
    out.push_back({p00, p11, p10});
    out.push_back({p00, p01, p11});
}

// One field cell: which BLAS to use, where to put it, how big, what color.
struct FieldInstance {
    math::Vec3 center;
    f32 size;     // edge length / sphere diameter
    u32 color;    // RGBA8
    bool sphere;  // true → sphere BLAS, false → cube BLAS
};

// Build the parametric field: a centered grid of pillars whose heights rise
// toward the middle, alternating cube / sphere by cell parity, with a hue
// that cycles across the grid so the lights pick out colored highlights.
std::array<FieldInstance, kFieldCount> make_field_instances() {
    std::array<FieldInstance, kFieldCount> field{};
    const f32 half = static_cast<f32>(kFieldDim - 1u) * 0.5f;
    for (u32 gz = 0; gz < kFieldDim; ++gz) {
        for (u32 gx = 0; gx < kFieldDim; ++gx) {
            const u32 i = gz * kFieldDim + gx;
            const f32 fx = (static_cast<f32>(gx) - half) * kCellSpacing;
            const f32 fz = (static_cast<f32>(gz) - half) * kCellSpacing;
            // Radial falloff → taller pillars near the center.
            const f32 rr = std::sqrt(fx * fx + fz * fz);
            const f32 h = 0.6f + 1.7f * std::exp(-0.18f * rr * rr * 0.25f);
            const bool sphere = ((gx + gz) & 1u) != 0u;
            const f32 size = sphere ? 0.95f : 0.85f;
            // Hue ramp across the grid (cheap HSV-ish via cosine lobes).
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kFieldCount);
            const f32 cr = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.00f));
            const f32 cg = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.33f));
            const f32 cb = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.66f));
            FieldInstance fi{};
            fi.sphere = sphere;
            fi.size = size;
            // Rest the object on the ground: cube center at h/2-ish gives a
            // pillar of height h; spheres sit so their base touches y=0.
            fi.center = {fx, h * 0.5f, fz};
            fi.color = pack_rgba8(clamp_u8(40.0f + 200.0f * cr),
                                  clamp_u8(40.0f + 200.0f * cg),
                                  clamp_u8(40.0f + 200.0f * cb));
            field[i] = fi;
        }
    }
    return field;
}

// ─── Mat4 helpers (just the ops we need; engine API is frozen). ──────────
// Non-uniform scale on Y lets the cube pillars stretch into columns while
// keeping the X/Z footprint at `size`.
math::Mat4 mat4_trs(math::Vec3 t, f32 scale_xz, f32 scale_y) {
    math::Mat4 m{};
    m.m[0] = scale_xz;
    m.m[1] = 0;
    m.m[2] = 0;
    m.m[3] = 0;
    m.m[4] = 0;
    m.m[5] = scale_y;
    m.m[6] = 0;
    m.m[7] = 0;
    m.m[8] = 0;
    m.m[9] = 0;
    m.m[10] = scale_xz;
    m.m[11] = 0;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    m.m[15] = 1;
    return m;
}

// ─── Camera ──────────────────────────────────────────────────────────────
struct Camera {
    math::Vec3 origin;
    math::Vec3 forward;
    math::Vec3 right;
    math::Vec3 up;
    f32 fov_tan;  // tan(half-fov-vertical)
    f32 aspect;
};

Camera make_orbit_camera(f32 t_seconds, f32 aspect) {
    // Pull back further than sample_05 to frame the whole field.
    const f32 radius = 13.0f;
    const f32 height = 6.5f;
    const f32 angle = t_seconds * 0.16f;
    const math::Vec3 eye{std::cos(angle) * radius, height, std::sin(angle) * radius};
    const math::Vec3 target{0.0f, 1.2f, 0.0f};
    const math::Vec3 world_up{0.0f, 1.0f, 0.0f};

    const math::Vec3 fwd = math::normalize(math::sub(target, eye));
    const math::Vec3 right = math::normalize(math::cross(fwd, world_up));
    const math::Vec3 up = math::cross(right, fwd);

    Camera c{};
    c.origin = eye;
    c.forward = fwd;
    c.right = right;
    c.up = up;
    c.fov_tan = std::tan(48.0f * math::kDegToRad * 0.5f);
    c.aspect = aspect;
    return c;
}

// Build a primary ray from a [0,1]^2 NDC pixel coordinate.
render::rt::Ray primary_ray(const Camera& cam, f32 nx, f32 ny) {
    // Pixel (0,0) is top-left; flip y to point upward in world space.
    const f32 sx = (2.0f * nx - 1.0f) * cam.aspect * cam.fov_tan;
    const f32 sy = (1.0f - 2.0f * ny) * cam.fov_tan;
    math::Vec3 dir{};
    dir.x = cam.forward.x + cam.right.x * sx + cam.up.x * sy;
    dir.y = cam.forward.y + cam.right.y * sx + cam.up.y * sy;
    dir.z = cam.forward.z + cam.right.z * sx + cam.up.z * sy;
    dir = math::normalize(dir);
    render::rt::Ray r{};
    r.origin = cam.origin;
    r.direction = dir;
    r.t_min = 1e-3f;
    r.t_max = 1e3f;
    return r;
}

render::rt::Ray ao_ray(const math::Vec3& position,
                       const math::Vec3& normal,
                       u32 pixel_index,
                       u32 sample_index,
                       f32 radius) noexcept {
    const math::Vec3 n = math::normalize(normal);
    const math::Vec3 up = std::fabs(n.y) < 0.95f ? math::Vec3{0.0f, 1.0f, 0.0f}
                                                 : math::Vec3{1.0f, 0.0f, 0.0f};
    const math::Vec3 tangent = math::normalize(math::cross(up, n));
    const math::Vec3 bitangent = math::cross(n, tangent);

    const u32 seed = pixel_index * 0x9e3779b9u + sample_index * 0x85ebca6bu + 0x632be59bu;
    const f32 u1 = hash_unit_float(seed);
    const f32 u2 = hash_unit_float(seed ^ 0xa511e9b3u);
    // Contact AO needs horizon/grazing coverage more than pure cosine-weighted
    // diffuse coverage; otherwise the six direct lights hide the subtle effect.
    // Bias z toward the tangent plane while still staying safely above it.
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

    render::rt::Ray ray{};
    ray.origin = {
        position.x + n.x * 2.0e-3f,
        position.y + n.y * 2.0e-3f,
        position.z + n.z * 2.0e-3f,
    };
    ray.direction = dir;
    ray.t_min = 1.0e-4f;
    ray.t_max = radius;
    return ray;
}

// ─── Lights ──────────────────────────────────────────────────────────────
struct Light {
    math::Vec3 position;
    f32 intensity;  // per-channel multiplier (already pre-tinted)
    f32 r, g, b;    // 0..1 color
    f32 range;
};

void orbit_lights(f32 t_seconds, std::array<Light, kNumLights>& lights) {
    const f32 base_y = 3.4f;
    // Six lights, each with its own orbit radius / speed / phase / color so
    // the traced shadows fan out across the field from several directions.
    const f32 radii[kNumLights] = {6.0f, 8.5f, 5.0f, 9.5f, 7.0f, 4.0f};
    const f32 speeds[kNumLights] = {0.50f, -0.32f, 0.74f, 0.21f, -0.58f, 0.95f};
    const f32 phases[kNumLights] = {0.0f, 1.047f, 2.094f, 3.142f, 4.189f, 5.236f};
    const f32 heights[kNumLights] =
        {base_y, base_y + 1.0f, base_y - 0.4f, base_y + 1.6f, base_y + 0.3f, base_y - 0.8f};
    const f32 cols[kNumLights][3] = {
        {1.00f, 0.30f, 0.25f},  // red
        {0.25f, 1.00f, 0.35f},  // green
        {0.30f, 0.45f, 1.00f},  // blue
        {1.00f, 0.80f, 0.25f},  // amber
        {0.85f, 0.30f, 1.00f},  // magenta
        {0.25f, 0.95f, 1.00f},  // cyan
    };
    for (u32 i = 0; i < kNumLights; ++i) {
        const f32 a = t_seconds * speeds[i] + phases[i];
        lights[i].position = {std::cos(a) * radii[i], heights[i], std::sin(a) * radii[i]};
        lights[i].r = cols[i][0];
        lights[i].g = cols[i][1];
        lights[i].b = cols[i][2];
        lights[i].intensity = 8.0f;
        lights[i].range = 20.0f;
    }
}

// ─── Skybox / background ─────────────────────────────────────────────────
// Vertical gradient — deep navy at the horizon, fading to nearly black
// overhead. Sample by ray direction Y.
u32 sample_sky(math::Vec3 dir) {
    const f32 t = 0.5f * (dir.y + 1.0f);  // 0..1
    // Horizon (t small): slightly brighter navy. Zenith (t large): black.
    const f32 horizon[3] = {7.0f, 11.0f, 30.0f};
    const f32 zenith[3] = {1.0f, 1.0f, 7.0f};
    const f32 r = horizon[0] * (1.0f - t) + zenith[0] * t;
    const f32 g = horizon[1] * (1.0f - t) + zenith[1] * t;
    const f32 b = horizon[2] * (1.0f - t) + zenith[2] * t;
    return pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

// ─── Bilinear upsample (low-res lit pass → final framebuffer) ────────────
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
            const f32 r = r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11;
            const f32 g = g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11;
            const f32 b = b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11;
            dst[static_cast<usize>(y) * dw + x] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const u32 smoke_frames = args.smoke_frames;
    ensure_rt_parallel_cvars();
    apply_rt_arg_overrides(args);
    render::rt::ensure_denoise_console_commands_registered();

    platform::WindowDesc desc{};
    desc.title = "Psynder — sample 12 (raytracing showcase)";
    desc.window_width = 1280;
    desc.window_height = 720;
    desc.render_width = kFbW;
    desc.render_height = kFbH;
    desc.scale_mode = platform::ScaleMode::Linear;

    auto* window = platform::create_window(desc);
    if (!window) {
        PSY_LOG_ERROR("sample_12: failed to create window");
        return EXIT_FAILURE;
    }

    // ── Build the static scene geometry. ────────────────────────────────
    // Just THREE BLAS meshes — a unit cube, a unit sphere, and the ground —
    // are reused across all kFieldCount field instances via per-instance
    // TLAS transforms. That BLAS reuse is the point of the TLAS: dozens of
    // visible objects, a handful of meshes.
    std::array<FieldInstance, kFieldCount> field = make_field_instances();

    std::vector<render::rt::Triangle> cube_tris;
    emit_unit_cube(cube_tris);

    std::vector<render::rt::Triangle> sphere_tris;
    emit_unit_sphere(sphere_tris, /*stacks=*/10, /*slices=*/14);

    std::vector<render::rt::Triangle> ground_tris;
    emit_ground(ground_tris, /*half=*/14.0f);

    render::rt::Bvh8 cube_blas;
    cube_blas.build(cube_tris.data(), static_cast<u32>(cube_tris.size()));
    render::rt::Bvh8 sphere_blas;
    sphere_blas.build(sphere_tris.data(), static_cast<u32>(sphere_tris.size()));
    render::rt::Bvh8 ground_blas;
    ground_blas.build(ground_tris.data(), static_cast<u32>(ground_tris.size()));

    // TLAS: kFieldCount field instances + 1 ground.
    std::array<render::rt::Tlas::InstanceDesc, kNumInstances> insts{};
    for (u32 i = 0; i < kFieldCount; ++i) {
        const FieldInstance& fi = field[i];
        insts[i].blas = fi.sphere ? &sphere_blas : &cube_blas;
        // Cubes stretch vertically into pillars (footprint stays `size`);
        // spheres scale uniformly so they stay round.
        const f32 scale_y = fi.sphere ? fi.size : (fi.center.y * 2.0f);
        insts[i].transform = mat4_trs(fi.center, fi.size, scale_y);
    }
    insts[kFieldCount].blas = &ground_blas;
    insts[kFieldCount].transform = math::identity4();

    render::rt::Tlas tlas;
    tlas.build(insts.data(), static_cast<u32>(insts.size()));

    // ── CPU framebuffers. ───────────────────────────────────────────────
    std::vector<u32> final_pixels(static_cast<usize>(kFbW) * kFbH, 0u);
    std::vector<u32> shadow_pixels(static_cast<usize>(kShadowW) * kShadowH, 0u);

    render::Framebuffer fb{};
    fb.width = kFbW;
    fb.height = kFbH;
    fb.pitch = kFbW * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(final_pixels.data());

    PSY_LOG_INFO("Psynder sample 12 running{} — {} TLAS instances, {} lights",
                 smoke_frames > 0 ? fmt::format(" — smoke mode, {} frames", smoke_frames)
                                  : std::string{},
                 kNumInstances,
                 kNumLights);
    PSY_LOG_INFO("sample_12 AO: enabled={}, debug={}, samples={}, radius={}, strength={}, lit_strength={}, denoise={}",
                 g_rt_ao && g_rt_ao->GetBool() ? 1 : 0,
                 g_rt_ao_debug && g_rt_ao_debug->GetBool() ? 1 : 0,
                 rt_ao_sample_count(),
                 rt_ao_radius(),
                 rt_ao_strength(),
                 rt_ao_lit_strength(),
                 g_rt_ao_denoise && g_rt_ao_denoise->GetBool() ? 1 : 0);

    const u64 t0 = platform::Clock::ticks_now();
    u32 frame = 0;

    const f32 aspect = static_cast<f32>(kFbW) / static_cast<f32>(kFbH);

    std::array<Light, kNumLights> lights{};

    // Pre-allocate the per-pixel primary-hit info so we can drive shadow
    // packets in row-major batches of 8.
    struct PixelHit {
        bool hit;
        math::Vec3 position;
        math::Vec3 normal;
        f32 r, g, b;  // ambient surface color in 0..1
    };
    std::vector<PixelHit> hits(static_cast<usize>(kShadowW) * kShadowH);
    std::vector<f32> accum(static_cast<usize>(kShadowW) * kShadowH * 3u, 0.0f);
    std::vector<f32> ao_raw;
    std::vector<f32> ao_filtered;
    std::vector<f32> hit_depth;
    std::vector<f32> hit_normals;

    // 60-sample ring of frame-times (ms) for the debug HUD strip chart.
    constexpr u32 kFrameHistory = 60;
    std::array<f32, kFrameHistory> frame_ms_ring{};
    u64 prev_frame_ticks = t0;
    // Smoke-mode frame-time stand-in (60 FPS budget = 1/60 s).
    constexpr f32 kSmokeFrameMs = 1000.0f / 60.0f;

    while (!window->should_close()) {
        window->poll_events();

        // Per-frame wall-clock delta for HUD stats. Smoke runs are
        // frame-indexed so use the 60 FPS budget stand-in for determinism.
        const u64 now_ticks = platform::Clock::ticks_now();
        const f32 frame_ms =
            smoke_frames > 0
                ? kSmokeFrameMs
                : static_cast<f32>(platform::Clock::seconds(now_ticks - prev_frame_ticks) * 1000.0);
        prev_frame_ticks = now_ticks;
        frame_ms_ring[frame % kFrameHistory] = frame_ms;

        // ESC quits — unless the console is open, where Esc closes it instead.
        if (auto* in = platform::input();
            in && in->key_down(platform::KeyCode::Escape) && !ui::console::is_open()) {
            break;
        }

        // Editor F2/~ toggle + PLAY/EDIT badge bottom-right. EDIT mode
        // pins time so the user can inspect the BVH with a frozen scene.
        const editor::Mode edit_mode =
            platform::input() ? editor::sample_step(*platform::input(), fb) : editor::Mode::Play;

        // Smoke runs pin time to frame index so the captured PNG is
        // deterministic across hosts.
        const f64 t = (edit_mode == editor::Mode::Edit) ? 0.0
                      : smoke_frames > 0
                          ? static_cast<f64>(frame) * (1.0 / 60.0)
                          : platform::Clock::seconds(platform::Clock::ticks_now() - t0);

        orbit_lights(static_cast<f32>(t), lights);
        const Camera cam = make_orbit_camera(static_cast<f32>(t), aspect);
        apply_rt_arg_overrides(args);
        const bool ao_enabled = g_rt_ao && g_rt_ao->GetBool();
        const bool ao_debug = ao_enabled && g_rt_ao_debug && g_rt_ao_debug->GetBool();
        if (ao_enabled && ao_raw.empty()) {
            const usize pixels = static_cast<usize>(kShadowW) * kShadowH;
            ao_raw.assign(pixels, 1.0f);
            ao_filtered.assign(pixels, 1.0f);
            hit_depth.assign(pixels, 1.0e6f);
            hit_normals.assign(pixels * 3u, 0.0f);
        }

        // ── Pass 1: primary visibility @ low-res (256×144). ─────────────
        // Cast one primary ray per low-res pixel against the TLAS and
        // stash the hit. Pixels that miss draw the sky immediately.
        rt_parallel_tiles(kShadowW, kShadowH, kRtTile, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = x0; x < x1; ++x) {
                    const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kShadowW);
                    const f32 ny = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kShadowH);
                    const render::rt::Ray ray = primary_ray(cam, nx, ny);
                    const render::rt::Hit h = tlas.intersect(ray);

                    const usize idx = static_cast<usize>(y) * kShadowW + x;
                    PixelHit ph{};
                    ph.hit = h.hit;
                    if (!h.hit) {
                        shadow_pixels[idx] = sample_sky(ray.direction);
                        if (ao_enabled) {
                            hit_depth[idx] = 1.0e6f;
                            hit_normals[idx * 3u + 0u] = 0.0f;
                            hit_normals[idx * 3u + 1u] = 1.0f;
                            hit_normals[idx * 3u + 2u] = 0.0f;
                        }
                    } else {
                        // Surface color: field[instance] tint, or dim grey for
                        // the ground (the last instance index).
                        u32 color = pack_rgba8(55, 55, 65);  // ground default
                        if (h.instance < kFieldCount) {
                            color = field[h.instance].color;
                        }
                        ph.r = static_cast<f32>(color & 0xFFu) / 255.0f;
                        ph.g = static_cast<f32>((color >> 8) & 0xFFu) / 255.0f;
                        ph.b = static_cast<f32>((color >> 16) & 0xFFu) / 255.0f;
                        // Hit position along the primary ray.
                        ph.position = {
                            ray.origin.x + ray.direction.x * h.t,
                            ray.origin.y + ray.direction.y * h.t,
                            ray.origin.z + ray.direction.z * h.t,
                        };
                        ph.normal = math::normalize(h.normal);
                        if (ao_enabled) {
                            hit_depth[idx] = h.t;
                            hit_normals[idx * 3u + 0u] = ph.normal.x;
                            hit_normals[idx * 3u + 1u] = ph.normal.y;
                            hit_normals[idx * 3u + 2u] = ph.normal.z;
                        }
                    }
                    hits[idx] = ph;
                }
            }
        });

        if (smoke_frames > 0 && frame == 0) {
            const u32 tiles_x = (kShadowW + kRtTile - 1u) / kRtTile;
            const u32 tiles_y = (kShadowH + kRtTile - 1u) / kRtTile;
            const u32 tile_count = tiles_x * tiles_y;
            PSY_LOG_INFO("sample_12 RT scheduler: workers={}, hint={}, target_cores={}, tile_jobs={}",
                         jobs::JobSystem::Get().worker_count(),
                         g_rt_cores_hint ? g_rt_cores_hint->GetInt() : 0,
                         rt_target_cores(),
                         rt_parallel_chunk_limit(tile_count));
        }

        // ── Pass 1b: short-ray ambient occlusion @ low-res. ─────────────
        // AO is deliberately budgeted: up to 8 deterministic hemisphere
        // rays per visible low-res pixel, packetized through the same TLAS
        // shadow path. It only darkens the ambient term below, so direct
        // colored light remains crisp.
        if (ao_enabled) {
            const u32 ao_samples = rt_ao_sample_count();
            const f32 ao_radius = rt_ao_radius();
            std::fill(ao_raw.begin(), ao_raw.end(), 1.0f);
            std::fill(ao_filtered.begin(), ao_filtered.end(), 1.0f);
            rt_parallel_tiles(kShadowW, kShadowH, kRtTile, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
                render::rt::ShadowPacket8 pkt{};
                u32 in_pkt = 0;
                u32 pkt_idx[8]{};
                bool pkt_real[8]{};

                auto dispatch_ao = [&]() {
                    for (u32 i = in_pkt; i < 8; ++i) {
                        pkt.rays[i].origin = {0.0f, 1.0e6f, 0.0f};
                        pkt.rays[i].direction = {0.0f, 1.0f, 0.0f};
                        pkt.rays[i].t_min = 1.0e-4f;
                        pkt.rays[i].t_max = 1.0f;
                        pkt_real[i] = false;
                    }
                    render::rt::trace_shadow_packet(tlas, pkt);
                    const f32 weight = 1.0f / static_cast<f32>(ao_samples);
                    for (u32 i = 0; i < 8; ++i) {
                        if (!pkt_real[i])
                            continue;
                        if (!pkt.occluded[i])
                            ao_raw[pkt_idx[i]] += weight;
                    }
                    in_pkt = 0;
                };

                for (u32 y = y0; y < y1; ++y) {
                    for (u32 x = x0; x < x1; ++x) {
                        const usize px = static_cast<usize>(y) * kShadowW + x;
                        const PixelHit& ph = hits[px];
                        if (!ph.hit)
                            continue;

                        ao_raw[px] = 0.0f;
                        for (u32 si = 0; si < ao_samples; ++si) {
                            u32& idx = in_pkt;
                            pkt.rays[idx] = ao_ray(ph.position,
                                                   ph.normal,
                                                   static_cast<u32>(px),
                                                   si,
                                                   ao_radius);
                            pkt_idx[idx] = static_cast<u32>(px);
                            pkt_real[idx] = true;
                            ++idx;
                            if (idx == 8)
                                dispatch_ao();
                        }
                    }
                }
                if (in_pkt > 0)
                    dispatch_ao();
            });

            if (g_rt_ao_denoise && g_rt_ao_denoise->GetBool()) {
                render::rt::DenoiseInput ao_in{};
                ao_in.shadow_visibility = ao_raw.data();
                ao_in.depth = hit_depth.data();
                ao_in.normals = hit_normals.data();
                ao_in.width = kShadowW;
                ao_in.height = kShadowH;
                render::rt::denoise_shadows(ao_in, ao_filtered.data());
            } else {
                ao_filtered = ao_raw;
            }

            if (smoke_frames > 0 && frame == 0) {
                usize hit_count = 0;
                f32 ao_min = 1.0f;
                f32 ao_sum = 0.0f;
                for (usize i = 0; i < hits.size(); ++i) {
                    if (!hits[i].hit)
                        continue;
                    const f32 v = std::clamp(ao_filtered[i], 0.0f, 1.0f);
                    ao_min = std::min(ao_min, v);
                    ao_sum += v;
                    ++hit_count;
                }
                PSY_LOG_INFO("sample_12 AO stats: hit_pixels={}, ao_min={}, ao_avg={}",
                             hit_count,
                             ao_min,
                             hit_count > 0 ? ao_sum / static_cast<f32>(hit_count) : 1.0f);
            }
        }

        // ── Pass 2: per-pixel shadow trace (packets of 8). ──────────────
        // For each light, fill ShadowPacket8 with 8 successive hit-pixels'
        // shadow rays, dispatch via trace_shadow_packet, accumulate the
        // lit contribution per pixel.
        std::fill(accum.begin(), accum.end(), 0.0f);

        rt_parallel_tiles(kShadowW, kShadowH, kRtTile, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            render::rt::ShadowPacket8 pkts[kNumLights]{};
            u32 in_pkt[kNumLights] = {0};
            u32 pkt_idx[kNumLights][8]{};
            bool pkt_real[kNumLights][8]{};

            auto dispatch = [&](u32 li) {
                render::rt::ShadowPacket8& pkt = pkts[li];
                const Light& L = lights[li];
                for (u32 i = in_pkt[li]; i < 8; ++i) {
                    pkt.rays[i].origin = {0, 1e6f, 0};
                    pkt.rays[i].direction = {0, 1, 0};
                    pkt.rays[i].t_min = 1e-4f;
                    pkt.rays[i].t_max = 1.0f;
                    pkt_real[li][i] = false;
                }
                render::rt::trace_shadow_packet(tlas, pkt);
                for (u32 i = 0; i < 8; ++i) {
                    if (!pkt_real[li][i])
                        continue;
                    const usize px = pkt_idx[li][i];
                    if (pkt.occluded[i])
                        continue;
                    const PixelHit& ph = hits[px];
                    math::Vec3 to_light = math::sub(L.position, ph.position);
                    const f32 d2 = math::dot(to_light, to_light);
                    const f32 d = std::sqrt(d2);
                    if (d > L.range || d < 1e-4f)
                        continue;
                    const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                    const f32 ndotl = std::max(0.0f, math::dot(ph.normal, ld));
                    if (ndotl <= 0.0f)
                        continue;
                    const f32 atten = 1.0f / (1.0f + d * d * 0.06f);
                    const f32 k = ndotl * atten * L.intensity;
                    accum[px * 3 + 0] += ph.r * L.r * k;
                    accum[px * 3 + 1] += ph.g * L.g * k;
                    accum[px * 3 + 2] += ph.b * L.b * k;
                }
                in_pkt[li] = 0;
            };

            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = x0; x < x1; ++x) {
                    const usize px = static_cast<usize>(y) * kShadowW + x;
                    const PixelHit& ph = hits[px];
                    if (!ph.hit)
                        continue;

                    for (u32 li = 0; li < kNumLights; ++li) {
                        const Light& L = lights[li];
                        math::Vec3 to_light = math::sub(L.position, ph.position);
                        const f32 d2 = math::dot(to_light, to_light);
                        const f32 d = std::sqrt(d2);
                        if (d > L.range || d < 1e-4f)
                            continue;
                        const math::Vec3 ld = math::mul(to_light, 1.0f / d);
                        if (math::dot(ph.normal, ld) <= 0.0f)
                            continue;
                        const math::Vec3 origin = {
                            ph.position.x + ph.normal.x * 1e-3f,
                            ph.position.y + ph.normal.y * 1e-3f,
                            ph.position.z + ph.normal.z * 1e-3f,
                        };

                        u32& idx = in_pkt[li];
                        pkts[li].rays[idx].origin = origin;
                        pkts[li].rays[idx].direction = ld;
                        pkts[li].rays[idx].t_min = 1e-4f;
                        pkts[li].rays[idx].t_max = d - 1e-3f;
                        pkt_idx[li][idx] = static_cast<u32>(px);
                        pkt_real[li][idx] = true;
                        ++idx;
                        if (idx == 8)
                            dispatch(li);
                    }
                }
            }
            for (u32 li = 0; li < kNumLights; ++li) {
                if (in_pkt[li] > 0)
                    dispatch(li);
            }
        });

        // ── Combine: write final low-res lit color. ─────────────────────
        rt_parallel_tiles(kShadowW, kShadowH, kRtTile, [&](u32 x0, u32 y0, u32 x1, u32 y1) {
            for (u32 y = y0; y < y1; ++y) {
                for (u32 x = x0; x < x1; ++x) {
                    const usize idx = static_cast<usize>(y) * kShadowW + x;
                    if (ao_debug) {
                        const f32 ao_visibility = std::clamp(ao_filtered[idx], 0.0f, 1.0f);
                        const u32 v = clamp_u8(ao_visibility * 255.0f);
                        shadow_pixels[idx] = pack_rgba8(v, v, v);
                        continue;
                    }
                    if (!hits[idx].hit)
                        continue;  // sky already written
                    const PixelHit& ph = hits[idx];
                    // Ambient floor so unlit faces aren't pure black.
                    const f32 amb = 0.10f;
                    f32 ambient_scale = amb * 30.0f;
                    f32 direct_scale = 1.0f;
                    if (ao_enabled) {
                        const f32 ao_visibility = std::clamp(ao_filtered[idx], 0.0f, 1.0f);
                        const f32 ao_term = 1.0f - rt_ao_strength() * (1.0f - ao_visibility);
                        ambient_scale *= ao_term;
                        direct_scale = 1.0f - rt_ao_lit_strength() * (1.0f - ao_visibility);
                    }
                    const f32 r = ph.r * ambient_scale + accum[idx * 3 + 0] * 18.0f * direct_scale;
                    const f32 g = ph.g * ambient_scale + accum[idx * 3 + 1] * 18.0f * direct_scale;
                    const f32 b = ph.b * ambient_scale + accum[idx * 3 + 2] * 18.0f * direct_scale;
                    shadow_pixels[idx] = pack_rgba8(clamp_u8(r), clamp_u8(g), clamp_u8(b));
                }
            }
        });

        // ── Pass 3: bilinear upsample low-res into the final FB. ────────
        upsample_bilinear(shadow_pixels.data(), kShadowW, kShadowH, final_pixels.data(), kFbW, kFbH);

        // Debug HUD overlay — `r_debug_hud full` enables. Per-frame stats
        // plus an avg over the populated prefix of the ring.
        {
            ui::imm::DebugHudStats stats{};
            stats.frame_ms = frame_ms;
            stats.avg_frame_ms = [&]() noexcept {
                const u32 n = std::min<u32>(frame + 1u, kFrameHistory);
                if (n == 0u)
                    return 0.0f;
                f32 sum = 0.0f;
                for (u32 i = 0; i < n; ++i)
                    sum += frame_ms_ring[i];
                return sum / static_cast<f32>(n);
            }();
            stats.draw_calls = 1;  // single rasterized full-screen blit
            stats.triangles = 0;   // raytraced — no rasterized geometry
            stats.active_voices = 0;
            ui::imm::draw_debug_hud(fb, stats);
        }

        ui::console::draw(fb);  // drop-down console (`~`) overlays everything
        window->present(fb);

        // Bump unconditionally each iteration so the frame_ms_ring populates
        // in interactive runs (smoke_frames == 0) too.
        ++frame;
        if (smoke_frames > 0 && frame >= smoke_frames) {
            PSY_LOG_INFO("sample_12: smoke target reached ({}); {} instances, {} lights; exiting",
                         smoke_frames,
                         kNumInstances,
                         kNumLights);
            break;
        }
    }

    if (!args.capture_out.empty()) {
        const bool ok = samples::write_png_rgba8_framebuffer(args.capture_out.c_str(),
                                                             final_pixels.data(),
                                                             fb.width,
                                                             fb.height);
        if (!ok) {
            PSY_LOG_ERROR("sample_12: failed to write capture to {}", args.capture_out);
            platform::destroy_window(window);
            return EXIT_FAILURE;
        }
        PSY_LOG_INFO("sample_12: wrote capture to {}", args.capture_out);
    }

    platform::destroy_window(window);
    return EXIT_SUCCESS;
}