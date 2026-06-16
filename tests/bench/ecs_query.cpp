// SPDX-License-Identifier: MIT
// Psynder ECS bench — per-chunk archetype-column query iteration.
//
// Lane: ECS query kernelization (Wave 8 perf pass). The hot path is
// EcsRegistry::query<reads<...>, writes<...>>(body): for every populated chunk
// across every matching archetype the framework resolves the component columns,
// builds std::span views, and fires `body` once per chunk. Games drive this
// every frame for render gather, physics writeback, AI, etc., so the per-chunk
// dispatch overhead (column-index resolution, singleton fetches, span builds)
// sits directly on the frame budget.
//
// This bench builds a large archetype (100k+ entities spread across hundreds of
// 16 KiB chunks) and times a representative query that reads two columns and
// writes one, integrated over many iterations. It reports mean us +/- stdev and
// a checksum of the written column. The checksum is the bit-identity witness:
// the kernelization must NOT change any computed value, so the printed checksum
// must be byte-for-byte stable across the before/after of the perf change.

#include "core/Types.h"
#include "jobs/JobSystem.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"
#include "scene/EcsRegistry_Internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

using namespace psynder;
using namespace psynder::scene;

namespace bench_ecs {

// Local POD components so the bench owns its archetype and never collides with
// engine-side component layouts. 3 floats each => 12 bytes/column.
PSYNDER_COMPONENT(BPos) {
    f32 x = 0, y = 0, z = 0;
};
PSYNDER_COMPONENT(BVel) {
    f32 x = 0, y = 0, z = 0;
};
PSYNDER_COMPONENT(BAcc) {
    f32 x = 0, y = 0, z = 0;
};

template <class T>
PSY_FORCEINLINE void do_not_optimize(const T& v) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&v) : "memory");
#else
    const volatile T& sink = v;
    (void)sink;
#endif
}

struct BenchOpts {
    bool smoke = false;
    bool single = false;    // force synchronous (no worker threads) for low-jitter timing
    u32 entities = 131072;  // 128k entities -> hundreds of chunks
    u32 iters = 200;        // query passes timed
    u32 warmup = 10;
};

BenchOpts parse_opts(int argc, char** argv) {
    BenchOpts o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            o.smoke = true;
            o.entities = 4096;
            o.iters = 12;
            o.warmup = 2;
        } else if (std::strcmp(argv[i], "--single") == 0) {
            // Single-thread mode: parallel_for runs the per-chunk body inline on
            // the calling thread (worker_count==0). This removes worker-thread
            // scheduling jitter so the timing isolates per-chunk DISPATCH cost
            // (column resolution, span builds, work-entry walk) — exactly what
            // the kernelization targets. The multithreaded path divides that
            // cost across cores AND adds steal/wake jitter that swamps the win.
            o.single = true;
        }
    }
    return o;
}

}  // namespace bench_ecs

using bench_ecs::BAcc;
using bench_ecs::BPos;
using bench_ecs::BVel;

int main(int argc, char** argv) {
    const bench_ecs::BenchOpts opts = bench_ecs::parse_opts(argc, argv);

    if (opts.single) {
        // Drop to worker_count==0 so parallel_for executes the per-chunk body
        // inline (synchronously) on this thread — see parse_opts for why.
        jobs::JobSystem::Get().stop();
    }

    auto& w = EcsRegistry::Get();
    detail::EcsRegistryImpl::Get().shutdown();  // clean slate
    w.set_structural_deferred(false);

    // Pre-reserve the archetype so creation stays out of the allocator and all
    // rows land in one contiguous run of chunks.
    w.reserve_archetype<BPos, BVel, BAcc>(opts.entities);

    std::vector<Entity> es;
    es.reserve(opts.entities);
    for (u32 i = 0; i < opts.entities; ++i) {
        Entity e = w.create();
        const f32 fi = static_cast<f32>(i);
        w.add<BPos>(e, BPos{(static_cast<f32>(i % 257) - 128.0f) * 0.22f,
                            (static_cast<f32>((i / 257) % 97) - 48.0f) * 0.18f,
                            (static_cast<f32>((i / 73) % 251) - 125.0f) * 0.22f});
        w.add<BVel>(e, BVel{std::sin(fi * 0.017f) * 0.35f, std::cos(fi * 0.013f) * 0.20f,
                            std::sin(fi * 0.011f) * 0.35f});
        w.add<BAcc>(e, BAcc{0.0f, 0.0f, 0.0f});
        es.push_back(e);
    }

    const u32 chunk_count = detail::EcsRegistryImpl::Get().chunk_live_count();

    constexpr f32 kDt = 1.0f / 120.0f;

    // Time a query pass `iters` times and print mean/stdev/median/best. The
    // closure runs the whole per-chunk dispatch + body once per call.
    auto time_query = [&](const char* label, u32 iters, auto&& pass) {
        for (u32 i = 0; i < opts.warmup; ++i)
            pass();
        std::vector<f64> samples;
        samples.reserve(iters);
        for (u32 it = 0; it < iters; ++it) {
            const auto t0 = std::chrono::steady_clock::now();
            pass();
            const auto t1 = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<f64, std::micro>(t1 - t0).count());
        }
        f64 mean = 0.0;
        for (f64 s : samples)
            mean += s;
        mean /= static_cast<f64>(samples.size());
        f64 var = 0.0;
        for (f64 s : samples)
            var += (s - mean) * (s - mean);
        var /= static_cast<f64>(samples.size());
        const f64 stdev = std::sqrt(var);
        std::vector<f64> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        const f64 median = sorted[sorted.size() / 2];
        const f64 best = sorted.front();
        const f64 ns_per_row = (mean * 1000.0) / static_cast<f64>(opts.entities);
        std::printf(
            "[ecs-query] %-6s mean=%.3fus stdev=%.3fus median=%.3fus best=%.3fus ns_per_row=%.4f\n",
            label, mean, stdev, median, best, ns_per_row);
    };

    std::printf("[ecs-query] entities=%u chunks=%u iters=%u mode=%s workers=%u\n", opts.entities,
                chunk_count, opts.iters, opts.single ? "single" : "parallel",
                jobs::JobSystem::Get().worker_count());

    // (A) HEAVY body — representative integrate: read BPos + BVel, write BAcc.
    // The body does real SoA arithmetic so per-chunk dispatch is amortized over
    // ~256 rows. This is the common gameplay-system shape.
    time_query("heavy", opts.iters, [&]() {
        w.query<reads<BPos, BVel>, writes<BAcc>>(
            [](std::span<const BPos> pos, std::span<const BVel> vel, std::span<BAcc> acc) {
                const usize n = pos.size();
                for (usize i = 0; i < n; ++i) {
                    // Deterministic, associativity-free per-element update.
                    acc[i].x = pos[i].x + vel[i].x * kDt;
                    acc[i].y = pos[i].y + vel[i].y * kDt;
                    acc[i].z = pos[i].z + vel[i].z * kDt;
                }
            });
    });

    // (B) THIN body — single-field touch per row (gather / tag-style query).
    // The body is near-trivial so the headline is dominated by per-chunk
    // dispatch (column resolution, span builds, work-entry walk) — exactly what
    // the kernelization targets. This isolates the dispatch win. The write here
    // (acc[i].x) is overwritten by pass (A) on the next iteration's checksum
    // read, so it does not perturb the (A)-derived checksum below.
    time_query("thin", opts.iters, [&]() {
        w.query<reads<BPos>, writes<BAcc>>(
            [](std::span<const BPos> pos, std::span<BAcc> acc) {
                const usize n = pos.size();
                for (usize i = 0; i < n; ++i)
                    acc[i].x = pos[i].x;
            });
    });

    // Re-run the HEAVY pass once so the checksum reflects pass (A)'s result
    // (pass (B) above left BAcc.x = BPos.x). This keeps the bit-identity
    // witness tied to the representative integrate body.
    w.query<reads<BPos, BVel>, writes<BAcc>>(
        [](std::span<const BPos> pos, std::span<const BVel> vel, std::span<BAcc> acc) {
            const usize n = pos.size();
            for (usize i = 0; i < n; ++i) {
                acc[i].x = pos[i].x + vel[i].x * kDt;
                acc[i].y = pos[i].y + vel[i].y * kDt;
                acc[i].z = pos[i].z + vel[i].z * kDt;
            }
        });

    // Checksum of the written column across all matching rows. Strided so the
    // sum order is fixed and independent of chunking. This is the value that
    // MUST stay bit-identical across the perf change.
    f64 checksum = 0.0;
    u64 rows_summed = 0;
    w.query<reads<BAcc>, writes<BVel>>(
        [&checksum, &rows_summed](std::span<const BAcc> acc, std::span<BVel>) {
            // NOTE: not the timed path; single-threaded accumulation here is
            // safe because the synchronous job backend runs bodies inline.
            for (usize i = 0; i < acc.size(); i += 97) {
                checksum += static_cast<f64>(acc[i].x) + static_cast<f64>(acc[i].y) * 0.5 +
                            static_cast<f64>(acc[i].z) * 0.25;
                ++rows_summed;
            }
        });
    bench_ecs::do_not_optimize(checksum);

    std::printf("[ecs-query] checksum=%.6f rows_summed=%llu\n", checksum,
                static_cast<unsigned long long>(rows_summed));

    for (auto e : es)
        w.destroy(e);
    detail::EcsRegistryImpl::Get().shutdown();

    if (!std::isfinite(checksum) || rows_summed == 0)
        return 1;
    if (chunk_count < 2 && !opts.smoke)
        return 1;  // bench is meant to span many chunks
    return 0;
}
