// SPDX-License-Identifier: MIT
// Psynder — Lane 04 unit: parallel_for sum correctness across many chunk
// sizes and array sizes, exercising the Chase-Lev scheduler under contention.

#include <catch2/catch_test_macros.hpp>

#include "jobs/JobSystem.h"

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using namespace psynder;
using namespace psynder::jobs;

namespace {

// Reference sum of [0..N).
constexpr std::uint64_t expected_sum(std::uint64_t n) {
    return n * (n - 1) / 2;
}

}  // namespace

TEST_CASE("parallel_for sums an array correctly", "[jobs][parallel_for]") {
    auto& js = JobSystem::Get();
    js.start();  // 0 = auto

    SECTION("small array, single chunk path") {
        constexpr usize N = 100;
        std::vector<std::uint64_t> v(N);
        std::iota(v.begin(), v.end(), 0ull);
        std::atomic<std::uint64_t> sum{0};
        js.parallel_for(0, N, 128, [&](usize lo, usize hi) {
            std::uint64_t local = 0;
            for (usize i = lo; i < hi; ++i)
                local += v[i];
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == expected_sum(N));
    }

    SECTION("medium array, many small chunks") {
        constexpr usize N = 50'000;
        std::atomic<std::uint64_t> sum{0};
        js.parallel_for(0, N, 64, [&](usize lo, usize hi) {
            std::uint64_t local = 0;
            for (usize i = lo; i < hi; ++i)
                local += static_cast<std::uint64_t>(i);
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == expected_sum(N));
    }

    SECTION("large array, mid grain") {
        constexpr usize N = 1'000'000;
        std::atomic<std::uint64_t> sum{0};
        js.parallel_for(0, N, 1024, [&](usize lo, usize hi) {
            std::uint64_t local = 0;
            for (usize i = lo; i < hi; ++i)
                local += static_cast<std::uint64_t>(i);
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == expected_sum(N));
    }

    SECTION("non-power-of-two range with awkward grain") {
        constexpr usize N = 12'345;
        std::atomic<std::uint64_t> sum{0};
        js.parallel_for(0, N, 77, [&](usize lo, usize hi) {
            std::uint64_t local = 0;
            for (usize i = lo; i < hi; ++i)
                local += static_cast<std::uint64_t>(i);
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == expected_sum(N));
    }

    SECTION("range that does not start at zero") {
        constexpr usize lo_n = 1000, hi_n = 9000;
        std::atomic<std::uint64_t> sum{0};
        js.parallel_for(lo_n, hi_n, 128, [&](usize lo, usize hi) {
            std::uint64_t local = 0;
            for (usize i = lo; i < hi; ++i)
                local += static_cast<std::uint64_t>(i);
            sum.fetch_add(local, std::memory_order_relaxed);
        });
        REQUIRE(sum.load() == expected_sum(hi_n) - expected_sum(lo_n));
    }

    js.stop();
}

TEST_CASE("parallel_for has worker count > 0", "[jobs][config]") {
    auto& js = JobSystem::Get();
    js.start();
    REQUIRE(js.worker_count() >= 1);
    js.stop();
}
