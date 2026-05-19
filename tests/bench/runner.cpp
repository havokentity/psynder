// SPDX-License-Identifier: MIT
// Psynder — bench gate harness.
//
// Runs per-tile raster + per-island physics microbenchmarks, prints a
// human-readable summary on stderr, and emits machine-readable JSON on
// stdout (or to --json-out=PATH). CI parses the JSON against a baseline
// and fails the PR when any bench regresses by > 2% (DESIGN.md §14).
//
// The two microbenches:
//
//   raster_tile_clear     — pixels-per-second on 64×64 framebuffer clear.
//                           Stand-in for the per-tile inner loop until lane
//                           07's full tile binner lands; once it does, this
//                           bench gets retargeted to a full per-tile draw.
//
//   physics_island_step   — N rigid bodies integrated through a fixed dt
//                           (Euler + naive O(N²) pair pass). The shape and
//                           memory layout mirror what lane 13's per-island
//                           solver will exercise; the regression signal is
//                           "is the basic SoA loop still fast?". Once lane
//                           13 ships its real World::step(), this bench
//                           will switch to driving the public API.
//
// CLI flags:
//   --smoke                 Fast mode for ctest (1 iter per bench).
//   --iters=N               Override iter count (default: per-bench tuned).
//   --json-out=PATH         Write JSON to PATH instead of stdout.
//   --baseline=PATH         Compare against the JSON baseline at PATH and
//                           exit non-zero on >2% regression. CI uses this.
//   --regression-pct=F      Override regression budget (default 2.0).

#include "core/Log.h"
#include "core/Types.h"
#include "math/Math.h"
#include "render/Framebuffer.h"
#include "render/raster/Raster.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace psynder;

namespace {

struct BenchResult {
    std::string name;
    u64         iterations  = 0;
    f64         ns_per_iter = 0.0;
    f64         throughput  = 0.0;   // bench-defined units per second
    std::string throughput_unit;     // e.g. "pixels/s", "bodies/s"
};

using Clock = std::chrono::steady_clock;

f64 elapsed_ns(Clock::time_point a, Clock::time_point b) {
    using ns = std::chrono::duration<f64, std::nano>;
    return std::chrono::duration_cast<ns>(b - a).count();
}

// Compiler-fence helper — keep results live so the optimizer can't sink
// the workload.
template <class T>
PSY_FORCEINLINE void do_not_optimize(const T& v) noexcept {
    asm volatile("" : : "g"(&v) : "memory");
}

// ─── bench 1: 64×64 tile clear ───────────────────────────────────────────
BenchResult bench_raster_tile_clear(u64 iters) {
    constexpr u32 kTile = 64;
    std::array<u32, kTile * kTile> tile_pixels{};

    render::Framebuffer fb{};
    fb.width  = kTile;
    fb.height = kTile;
    fb.pitch  = kTile * 4;
    fb.format = render::PixelFormat::RGBA8;
    fb.pixels = reinterpret_cast<u8*>(tile_pixels.data());

    // Warm-up.
    for (u64 i = 0; i < std::min<u64>(iters, 16); ++i) {
        render::raster::clear_framebuffer(fb, 0x12345678u);
    }
    do_not_optimize(tile_pixels);

    const auto t0 = Clock::now();
    for (u64 i = 0; i < iters; ++i) {
        render::raster::clear_framebuffer(
            fb, static_cast<u32>(i * 2654435761u));
    }
    do_not_optimize(tile_pixels);
    const auto t1 = Clock::now();
    const f64 ns = elapsed_ns(t0, t1);

    BenchResult r{};
    r.name            = "raster_tile_clear";
    r.iterations      = iters;
    r.ns_per_iter     = ns / static_cast<f64>(iters);
    const f64 pixels  = static_cast<f64>(iters) * static_cast<f64>(kTile * kTile);
    r.throughput      = (ns > 0) ? (pixels * 1.0e9 / ns) : 0.0;
    r.throughput_unit = "pixels/s";
    return r;
}

// ─── bench 2: physics island step ────────────────────────────────────────
// Stand-in until lane 13 ships psynder::physics::World::step(). Mirrors the
// SoA layout the real per-island solver will use (positions, velocities,
// inverse mass) and runs a simple semi-implicit Euler integration with a
// naive O(N²) pair pass — same memory pressure, same cache footprint, just
// without the broadphase / GJK plumbing. The bench gate cares about the
// shape of the loop, not its physical correctness.
BenchResult bench_physics_island_step(u64 iters) {
    constexpr u32 kBodies = 32;

    struct Soa {
        std::array<f32, kBodies> px, py, pz;
        std::array<f32, kBodies> vx, vy, vz;
        std::array<f32, kBodies> inv_mass;
        std::array<f32, kBodies> radius;
    };
    Soa b{};

    for (u32 i = 0; i < kBodies; ++i) {
        b.px[i] = static_cast<f32>(i % 8) - 4.0f;
        b.py[i] = 5.0f;
        b.pz[i] = static_cast<f32>(i / 8) - 4.0f;
        b.vx[i] = 0.0f;
        b.vy[i] = 0.0f;
        b.vz[i] = 0.0f;
        b.inv_mass[i] = 1.0f;
        b.radius[i]   = 0.5f;
    }
    const f32 dt    = 1.0f / 120.0f;
    const f32 gravy = -9.81f;

    auto step = [&]() {
        // Integrate.
        for (u32 i = 0; i < kBodies; ++i) {
            b.vy[i] += gravy * dt;
            b.px[i] += b.vx[i] * dt;
            b.py[i] += b.vy[i] * dt;
            b.pz[i] += b.vz[i] * dt;
            // Floor at y=0 with restitution.
            if (b.py[i] < b.radius[i]) {
                b.py[i] = b.radius[i];
                b.vy[i] = -b.vy[i] * 0.1f;
            }
        }
        // Naive O(N²) pair pass — sphere-sphere overlap + linear push apart.
        for (u32 i = 0; i < kBodies; ++i) {
            for (u32 j = i + 1; j < kBodies; ++j) {
                const f32 dx = b.px[j] - b.px[i];
                const f32 dy = b.py[j] - b.py[i];
                const f32 dz = b.pz[j] - b.pz[i];
                const f32 d2 = dx*dx + dy*dy + dz*dz;
                const f32 rr = b.radius[i] + b.radius[j];
                if (d2 < rr * rr && d2 > 1e-6f) {
                    const f32 d   = std::sqrt(d2);
                    const f32 pen = (rr - d) * 0.5f;
                    const f32 nx = dx / d, ny = dy / d, nz = dz / d;
                    b.px[i] -= nx * pen; b.py[i] -= ny * pen; b.pz[i] -= nz * pen;
                    b.px[j] += nx * pen; b.py[j] += ny * pen; b.pz[j] += nz * pen;
                    // Reflect relative velocity along the contact normal.
                    const f32 rvx = b.vx[j] - b.vx[i];
                    const f32 rvy = b.vy[j] - b.vy[i];
                    const f32 rvz = b.vz[j] - b.vz[i];
                    const f32 vn  = rvx*nx + rvy*ny + rvz*nz;
                    if (vn < 0) {
                        const f32 jimp = -vn * 0.5f;
                        b.vx[i] -= jimp * nx; b.vy[i] -= jimp * ny; b.vz[i] -= jimp * nz;
                        b.vx[j] += jimp * nx; b.vy[j] += jimp * ny; b.vz[j] += jimp * nz;
                    }
                }
            }
        }
    };

    // Warm-up.
    for (u64 i = 0; i < std::min<u64>(iters, 4); ++i) step();

    const auto t0 = Clock::now();
    for (u64 i = 0; i < iters; ++i) step();
    const auto t1 = Clock::now();
    do_not_optimize(b);
    const f64 ns = elapsed_ns(t0, t1);

    BenchResult r{};
    r.name            = "physics_island_step";
    r.iterations      = iters;
    r.ns_per_iter     = ns / static_cast<f64>(iters);
    const f64 bodies  = static_cast<f64>(iters) * static_cast<f64>(kBodies);
    r.throughput      = (ns > 0) ? (bodies * 1.0e9 / ns) : 0.0;
    r.throughput_unit = "bodies/s";
    return r;
}

// ─── JSON emission ───────────────────────────────────────────────────────
// Hand-rolled stringification — no dep. Format is compact, machine-friendly.
std::string emit_json(const std::vector<BenchResult>& results) {
    std::ostringstream os;
    os.precision(6);
    os << std::fixed;
    os << "{\n  \"benches\": [\n";
    for (usize i = 0; i < results.size(); ++i) {
        const BenchResult& r = results[i];
        os << "    {"
           << "\"name\": \"" << r.name << "\", "
           << "\"iterations\": " << r.iterations << ", "
           << "\"ns_per_iter\": " << r.ns_per_iter << ", "
           << "\"throughput\": " << r.throughput << ", "
           << "\"throughput_unit\": \"" << r.throughput_unit << "\""
           << "}";
        if (i + 1 < results.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n}\n";
    return os.str();
}

// Naive single-pass parser that pulls (name → ns_per_iter) pairs out of the
// JSON produced by emit_json above. Brittle to other writers but good enough
// for our own baseline-vs-current diff.
std::vector<std::pair<std::string, f64>> parse_baseline(const std::string& path) {
    std::vector<std::pair<std::string, f64>> out;
    std::ifstream in(path);
    if (!in) return out;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string body = ss.str();

    usize cursor = 0;
    while (true) {
        const usize name_key = body.find("\"name\":", cursor);
        if (name_key == std::string::npos) break;
        const usize q0 = body.find('"', name_key + 7);
        if (q0 == std::string::npos) break;
        const usize q1 = body.find('"', q0 + 1);
        if (q1 == std::string::npos) break;
        std::string name = body.substr(q0 + 1, q1 - q0 - 1);

        const usize ns_key = body.find("\"ns_per_iter\":", q1);
        if (ns_key == std::string::npos) break;
        const f64 val = std::strtod(body.c_str() + ns_key
                                    + std::string_view("\"ns_per_iter\":").size(),
                                    nullptr);
        out.emplace_back(std::move(name), val);
        cursor = ns_key + 1;
    }
    return out;
}

struct Args {
    bool        smoke         = false;
    u64         iters         = 0;        // 0 = per-bench default
    std::string json_out;
    std::string baseline;
    f64         regression_pct = 2.0;
};

Args parse_args(int argc, char** argv) {
    Args a{};
    for (int i = 1; i < argc; ++i) {
        std::string_view s{argv[i]};
        if (s == "--smoke") a.smoke = true;
        else if (s.starts_with("--iters="))
            a.iters = std::strtoull(
                std::string(s.substr(std::string_view("--iters=").size())).c_str(),
                nullptr, 10);
        else if (s.starts_with("--json-out="))
            a.json_out = std::string(s.substr(std::string_view("--json-out=").size()));
        else if (s.starts_with("--baseline="))
            a.baseline = std::string(s.substr(std::string_view("--baseline=").size()));
        else if (s.starts_with("--regression-pct="))
            a.regression_pct = std::strtod(
                std::string(s.substr(std::string_view("--regression-pct=").size())).c_str(),
                nullptr);
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    const u64 raster_iters  = args.smoke ? 8
                            : (args.iters ? args.iters : 50'000);
    const u64 physics_iters = args.smoke ? 4
                            : (args.iters ? args.iters : 5'000);

    std::vector<BenchResult> results;
    results.push_back(bench_raster_tile_clear(raster_iters));
    results.push_back(bench_physics_island_step(physics_iters));

    // Human-readable summary on stderr so it doesn't pollute the JSON pipe.
    for (const BenchResult& r : results) {
        PSY_LOG_INFO("bench: {} — {} iters, {:.2f} ns/iter, {:.3e} {}",
                     r.name, r.iterations, r.ns_per_iter,
                     r.throughput, r.throughput_unit);
    }

    const std::string json = emit_json(results);
    if (!args.json_out.empty()) {
        std::ofstream out(args.json_out, std::ios::binary);
        if (!out) {
            PSY_LOG_ERROR("bench: failed to open --json-out path {}", args.json_out);
            return EXIT_FAILURE;
        }
        out << json;
    } else {
        std::fputs(json.c_str(), stdout);
    }

    // Baseline comparison — CI uses this; smoke mode skips it so a transient
    // measurement noise doesn't fail the test runner.
    if (!args.baseline.empty() && !args.smoke) {
        const auto baseline = parse_baseline(args.baseline);
        if (baseline.empty()) {
            PSY_LOG_WARN("bench: baseline {} empty or unparseable; not gating",
                         args.baseline);
            return EXIT_SUCCESS;
        }
        const f64 budget = args.regression_pct;
        bool regressed   = false;
        for (const BenchResult& r : results) {
            for (const auto& [bn, bv] : baseline) {
                if (bn != r.name) continue;
                if (bv <= 0) continue;
                const f64 pct = 100.0 * (r.ns_per_iter - bv) / bv;
                if (pct > budget) {
                    PSY_LOG_ERROR("bench: {} regressed {:.2f}% "
                                  "(baseline {:.2f}ns, now {:.2f}ns; budget {:.2f}%)",
                                  r.name, pct, bv, r.ns_per_iter, budget);
                    regressed = true;
                } else {
                    PSY_LOG_INFO("bench: {} delta {:+.2f}% vs baseline {:.2f}ns",
                                 r.name, pct, bv);
                }
                break;
            }
        }
        if (regressed) return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
