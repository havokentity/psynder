// SPDX-License-Identifier: MIT
// Psynder — Lane 04 unit: parallel_for small-buffer-optimization (SBO).
//
// parallel_for no longer std::malloc/free's its per-chunk task + handle arrays
// on every call: the storage is an on-stack SBO array (kSboChunks == 64) with a
// C-heap fallback only when the chunk count overflows. These tests prove:
//   1. The HEAP FALLBACK path (chunks > 64) is correct.
//   2. RE-ENTRANT / nested parallel_for (a chunk body issuing another
//      parallel_for) does not corrupt either call's storage — each call owns a
//      private stack frame, so nesting is race-free.
// Both run under ASan/UBSan in the mac-debug preset; any heap-overflow,
// use-after-free, or cross-call clobber would trip the sanitizer here.

#include <catch2/catch_test_macros.hpp>

#include "jobs/JobSystem.h"

#include <atomic>
#include <cstdint>
#include <vector>

using namespace psynder;
using namespace psynder::jobs;

namespace {

constexpr std::uint64_t expected_sum(std::uint64_t n) {
    return n * (n - 1) / 2;
}

}  // namespace

TEST_CASE("parallel_for heap fallback past the SBO threshold", "[jobs][parallel_for][sbo]") {
    auto& js = JobSystem::Get();
    js.start();

    // grain == 1 forces one chunk per element, so N elements => N chunks. The
    // SBO inline capacity is 64; these sizes deliberately straddle and far
    // exceed it to exercise the std::malloc fallback and its growth/free.
    for (usize N : {usize{64}, usize{65}, usize{200}, usize{2000}}) {
        std::atomic<std::uint64_t> sum{0};
        js.parallel_for(0, N, /*grain*/ 1, [&](usize lo, usize hi) {
            std::uint64_t local = 0;
            for (usize i = lo; i < hi; ++i)
                local += static_cast<std::uint64_t>(i);
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == expected_sum(N));
    }

    js.stop();
}

TEST_CASE("parallel_for nested / re-entrant calls do not corrupt each other",
          "[jobs][parallel_for][sbo][reentrant]") {
    auto& js = JobSystem::Get();
    js.start();

    // Outer parallel_for spans enough chunks to spill (grain 1, > 64). Each
    // outer chunk body issues its OWN inner parallel_for that ALSO spills.
    // If the SBO storage were shared/thread-local instead of per-call-stack,
    // the inner call would clobber the outer call's task array mid-flight and
    // either ASan would fire or the totals would be wrong.
    constexpr usize kOuter = 100;  // > 64 => outer spills to heap
    constexpr usize kInner = 80;   // > 64 => each inner spills to heap

    std::atomic<std::uint64_t> outer_sum{0};
    std::atomic<std::uint64_t> inner_sum{0};
    // Counts how many outer iterations observed the FULL inner sum. Catch2's
    // REQUIRE is not thread-safe and must not run inside a worker-thread job
    // body, so we tally into atomics here and assert on the main thread after
    // the parallel_for returns (mirrors scene_query.cpp's discipline).
    std::atomic<std::uint64_t> inner_full_count{0};

    js.parallel_for(0, kOuter, /*grain*/ 1, [&](usize olo, usize ohi) {
        for (usize o = olo; o < ohi; ++o) {
            outer_sum.fetch_add(static_cast<std::uint64_t>(o), std::memory_order_relaxed);
            // Nested parallel_for issued from inside a running job.
            std::atomic<std::uint64_t> local_inner{0};
            js.parallel_for(0, kInner, /*grain*/ 1, [&](usize ilo, usize ihi) {
                std::uint64_t acc = 0;
                for (usize i = ilo; i < ihi; ++i)
                    acc += static_cast<std::uint64_t>(i);
                local_inner.fetch_add(acc, std::memory_order_relaxed);
            });
            // Every inner pass must see the full [0, kInner) sum; record it
            // rather than asserting from this worker thread.
            if (local_inner.load() == expected_sum(kInner))
                inner_full_count.fetch_add(1, std::memory_order_relaxed);
            inner_sum.fetch_add(local_inner.load(), std::memory_order_relaxed);
        }
    });

    REQUIRE(inner_full_count.load() == kOuter);
    REQUIRE(outer_sum.load() == expected_sum(kOuter));
    REQUIRE(inner_sum.load() == expected_sum(kInner) * kOuter);

    js.stop();
}
