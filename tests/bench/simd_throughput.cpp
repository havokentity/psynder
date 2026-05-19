// SPDX-License-Identifier: MIT
// Psynder — Lane 03 microbench. Measures throughput of add / mul / fma and
// the reduce_add dispatcher kernel for a working set just bigger than L1.
//
// Two invocation modes:
//   * `psynder_bench --smoke`  — runs every bench case once with a tiny
//     working set so the CI smoke gate finishes in < 1 s. The smoke gate
//     only checks that the binary doesn't trip an assertion or crash; the
//     real perf numbers come from the full run.
//   * `psynder_bench`          — full run, prints CSV-shaped lines to
//     stdout for ingestion by the perf-dashboard tool. Each line is:
//     `lane,case,tier,elements,ns_per_elem`
//
// Lane ownership note: this file ships `main()` because at Wave-A we are
// the first / only lane dropping a bench `.cpp` here. If another lane lands
// a parallel `main()` the orchestrator will mediate (a shared registry-
// driven main belongs to the bench-infra owner, not to lane 03).

#include "simd/Dispatch.h"
#include "simd/Simd.h"
#include "simd/Simd_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace psynder;

namespace {

struct BenchOpts {
    bool   smoke = false;
    usize  n     = 1u << 16;       // 64K floats — 256 KB, fits L2 comfortably
    int    iters = 200;            // outer loop count for averaging
};

BenchOpts parse_opts(int argc, char** argv) {
    BenchOpts o;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            o.smoke = true;
            o.n     = 1024;
            o.iters = 4;
        }
    }
    return o;
}

// High-resolution monotonic timer wrapper. We sample steady_clock around
// the inner loop and emit the median of N runs to dampen the platform's
// scheduling noise. (Apple Silicon under macOS has fairly low noise — on
// Linux/x86 we'd add core pinning here.)
template <typename Fn>
double median_ns_per_elem(Fn&& fn, usize elements, int iters) {
    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(iters));
    for (int it = 0; it < iters; ++it) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        auto t1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        samples.push_back(ns / static_cast<double>(elements));
    }
    std::nth_element(samples.begin(),
                     samples.begin() + samples.size() / 2,
                     samples.end());
    return samples[samples.size() / 2];
}

// Force a read of `x` so the compiler doesn't elide the kernel call when
// the result is otherwise unused. asm("" :: "r,m"(x)) is the standard idiom.
template <typename T>
PSY_FORCEINLINE void do_not_optimize(const T& x) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    asm volatile("" : : "r,m"(x) : "memory");
#else
    static_cast<volatile const T&>(x);
#endif
}

void bench_case_add(const BenchOpts& o) {
    std::vector<f32> a(o.n), b(o.n), y(o.n);
    for (usize i = 0; i < o.n; ++i) { a[i] = float(i); b[i] = float(o.n - i); }

    auto fn = [&]() {
        simd::add_buffer(a.data(), b.data(), y.data(), o.n);
        do_not_optimize(y[0]);
    };
    double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("simd,add_buffer,%s,%zu,%.3f\n",
                simd::tier_name(simd::current_tier()),
                static_cast<size_t>(o.n), ns_per);
}

void bench_case_mul(const BenchOpts& o) {
    // mul has no public dispatcher routine — measure the per-lane mul4
    // intrinsic in a hand-rolled loop so we still capture throughput.
    std::vector<f32> a(o.n), b(o.n), y(o.n);
    for (usize i = 0; i < o.n; ++i) { a[i] = float(i); b[i] = float(o.n - i); }

    auto fn = [&]() {
        for (usize i = 0; i + 4 <= o.n; i += 4) {
            simd::store_unaligned4(
                y.data() + i,
                simd::mul4(simd::load_unaligned4(a.data() + i),
                           simd::load_unaligned4(b.data() + i)));
        }
        do_not_optimize(y[0]);
    };
    double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("simd,mul4_loop,%s,%zu,%.3f\n",
                simd::tier_name(simd::current_tier()),
                static_cast<size_t>(o.n), ns_per);
}

void bench_case_fma(const BenchOpts& o) {
    std::vector<f32> a(o.n), b(o.n), c(o.n), y(o.n);
    for (usize i = 0; i < o.n; ++i) {
        a[i] = 1.0f + 0.001f * float(i);
        b[i] = 2.0f + 0.002f * float(i);
        c[i] = 3.0f + 0.003f * float(i);
    }

    auto fn = [&]() {
        simd::fma_buffer(a.data(), b.data(), c.data(), y.data(), o.n);
        do_not_optimize(y[0]);
    };
    double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("simd,fma_buffer,%s,%zu,%.3f\n",
                simd::tier_name(simd::current_tier()),
                static_cast<size_t>(o.n), ns_per);
}

void bench_case_reduce_add(const BenchOpts& o) {
    std::vector<f32> a(o.n);
    for (usize i = 0; i < o.n; ++i) a[i] = 1.0f / float(i + 1);
    auto fn = [&]() {
        f32 s = simd::reduce_add(a.data(), o.n);
        do_not_optimize(s);
    };
    double ns_per = median_ns_per_elem(fn, o.n, o.iters);
    std::printf("simd,reduce_add,%s,%zu,%.3f\n",
                simd::tier_name(simd::current_tier()),
                static_cast<size_t>(o.n), ns_per);
}

}  // namespace

int main(int argc, char** argv) {
    BenchOpts o = parse_opts(argc, argv);

    // Make sure the dispatcher has run before we measure.
    simd::dispatch_init();

    if (!o.smoke) {
        std::printf("# psynder bench — simd lane\n");
        std::printf("# tier=%s, n=%zu, iters=%d\n",
                    simd::tier_name(simd::current_tier()),
                    static_cast<size_t>(o.n), o.iters);
        std::printf("# columns: lane,case,tier,elements,ns_per_elem\n");
    }

    bench_case_add(o);
    bench_case_mul(o);
    bench_case_fma(o);
    bench_case_reduce_add(o);
    return 0;
}
