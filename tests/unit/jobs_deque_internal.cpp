// SPDX-License-Identifier: MIT
// Psynder — Lane 04 internal: Chase-Lev deque stress test, owned + concurrent
// thieves. Ensures the deque algorithm itself doesn't double-deliver or lose
// elements under heavy contention before we trust the wider JobSystem on it.

#include <catch2/catch_test_macros.hpp>

#include "jobs/ChaseLevDeque_internal.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace psynder;
using namespace psynder::jobs::detail;

TEST_CASE("ChaseLevDeque: single-owner pushes are conserved by thieves",
          "[jobs][deque][internal]") {
    ChaseLevDeque d;
    d.init(1 << 16);

    constexpr u32 kPushes = 100000;
    std::atomic<u32> got_total{0};
    std::atomic<u32> seen_counts[kPushes]{};

    constexpr int kThieves = 8;
    std::atomic<bool> stop{false};
    std::vector<std::thread> thieves;
    for (int i = 0; i < kThieves; ++i) {
        thieves.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                auto r = d.try_steal();
                if (r.status == ChaseLevDeque::Status::Ok) {
                    seen_counts[r.index - 1].fetch_add(1,
                        std::memory_order_relaxed);
                    got_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
            // Drain remaining after stop flag.
            for (;;) {
                auto r = d.try_steal();
                if (r.status == ChaseLevDeque::Status::Empty) break;
                if (r.status == ChaseLevDeque::Status::Ok) {
                    seen_counts[r.index - 1].fetch_add(1,
                        std::memory_order_relaxed);
                    got_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Owner pushes 1..kPushes.
    for (u32 v = 1; v <= kPushes; ++v) {
        while (!d.push(v)) {
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : thieves) t.join();

    // Owner may have left tail of work — drain it.
    for (;;) {
        auto r = d.try_pop();
        if (r.status == ChaseLevDeque::Status::Empty) break;
        if (r.status == ChaseLevDeque::Status::Ok) {
            seen_counts[r.index - 1].fetch_add(1, std::memory_order_relaxed);
            got_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    REQUIRE(got_total.load() == kPushes);
    for (u32 v = 0; v < kPushes; ++v) {
        u32 c = seen_counts[v].load();
        if (c != 1) {
            // Print first failure for diagnosis.
            INFO("value " << (v + 1) << " seen " << c << " times");
        }
        REQUIRE(c == 1);
    }

    d.deinit();
}
