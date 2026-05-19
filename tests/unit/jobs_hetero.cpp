// SPDX-License-Identifier: MIT
// Psynder — Lane 04 unit: heterogeneous P/E pool routing.
//
// Two flavours of test:
//   1. Detection sanity — `hetero_detected_counts()` should at least
//      report a sane total core count. On Apple Silicon (the dev box) we
//      additionally expect p_cores + e_cores == total.
//   2. Mixed-priority submission — under load, both pools accept work
//      and complete it. We verify forward progress on both classes
//      regardless of whether the host is hetero (homogeneous fallback
//      routes both classes to the unified pool, which still progresses).

#include <catch2/catch_test_macros.hpp>

#include "jobs/JobSystem.h"
#include "jobs/JobSystemHetero.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace psynder;
using namespace psynder::jobs;

TEST_CASE("hetero_detected_counts reports a usable total", "[jobs][hetero]") {
    auto c = hetero_detected_counts();
    REQUIRE(c.total >= 1u);
    // If the host reports a hetero split, the per-class sum must match the
    // total. On homogeneous boxes p_cores == e_cores == 0 and we don't
    // assert anything about their relationship to total.
    if (c.p_cores > 0u && c.e_cores > 0u) {
        REQUIRE(c.p_cores + c.e_cores == c.total);
    }
}

TEST_CASE("submit_latency / submit_throughput both make forward progress",
          "[jobs][hetero]") {
    auto& js = JobSystem::Get();
    js.start();

    // On homogeneous hosts hetero_is_active() is false and both submits
    // route to the unified inbox. The test below checks completion in both
    // configurations.
    constexpr int kJobsPerClass = 256;

    std::atomic<int> latency_count{0};
    std::atomic<int> throughput_count{0};

    auto latency_fn = +[](void* u) noexcept {
        static_cast<std::atomic<int>*>(u)->fetch_add(
            1, std::memory_order_relaxed);
        // Simulate a small slice of frame-critical work.
        for (volatile int i = 0; i < 256; ++i) { (void)i; }
    };
    auto throughput_fn = +[](void* u) noexcept {
        static_cast<std::atomic<int>*>(u)->fetch_add(
            1, std::memory_order_relaxed);
        for (volatile int i = 0; i < 2048; ++i) { (void)i; }
    };

    std::vector<JobHandle> handles;
    handles.reserve(static_cast<size_t>(kJobsPerClass * 2));
    // Interleave class submission to force both pools active simultaneously.
    for (int i = 0; i < kJobsPerClass; ++i) {
        JobDesc lat{ latency_fn,    &latency_count,    "lat", 0 };
        JobDesc thr{ throughput_fn, &throughput_count, "thr", 0 };
        handles.push_back(submit_latency(lat));
        handles.push_back(submit_throughput(thr));
    }
    for (auto h : handles) js.wait(h);

    REQUIRE(latency_count.load()    == kJobsPerClass);
    REQUIRE(throughput_count.load() == kJobsPerClass);

    js.stop();
}

TEST_CASE("hetero pool diagnostics are coherent across start/stop",
          "[jobs][hetero]") {
    auto& js = JobSystem::Get();

    // Before start: pool diagnostics should report 0 workers (regardless
    // of whether the host is hetero — workers haven't been created yet).
    // Note: js.start() is lazy; some prior test may have left the pool
    // in a torn-down state. We don't assert pre-start counts because the
    // JobSystem is a singleton and the detect path may have populated
    // topology already.

    js.start();
    bool active = hetero_is_active();
    u32  lat_w  = hetero_latency_workers();
    u32  thr_w  = hetero_throughput_workers();

    if (active) {
        // Hetero active: both pools have at least one worker.
        REQUIRE(lat_w >= 1u);
        REQUIRE(thr_w >= 1u);
        REQUIRE(lat_w + thr_w == js.worker_count());
    } else {
        // Homogeneous fallback: counts reported as 0 (unified pool serves).
        REQUIRE(lat_w == 0u);
        REQUIRE(thr_w == 0u);
        REQUIRE(js.worker_count() >= 1u);
    }
    js.stop();
}

TEST_CASE("mixed submit / submit_latency / submit_throughput keep both pools "
          "active", "[jobs][hetero]") {
    auto& js = JobSystem::Get();
    js.start();

    // We can't reliably observe *which* core ran a given job from inside a
    // unit test (the OS scheduler ultimately decides), so this test focuses
    // on the scheduler-level guarantee: when work is submitted to both
    // pools simultaneously, both pools complete their share without
    // deadlock. The fan-out exercises:
    //   - Workers of class L stealing class-T inbox work (cross-class
    //     fallback) when L runs dry first, and vice versa.
    //   - The unified inbox accepts unclassified `submit()` work and any
    //     worker drains it.

    constexpr int kTotalPerClass = 1024;

    std::atomic<int> u_count{0};
    std::atomic<int> l_count{0};
    std::atomic<int> t_count{0};

    auto bump = +[](void* u) noexcept {
        static_cast<std::atomic<int>*>(u)->fetch_add(
            1, std::memory_order_relaxed);
    };

    std::vector<JobHandle> handles;
    handles.reserve(static_cast<size_t>(kTotalPerClass * 3));
    for (int i = 0; i < kTotalPerClass; ++i) {
        JobDesc d_unified{ bump, &u_count, "u", 0 };
        JobDesc d_lat    { bump, &l_count, "l", 0 };
        JobDesc d_thr    { bump, &t_count, "t", 0 };
        handles.push_back(js.submit(d_unified));
        handles.push_back(submit_latency(d_lat));
        handles.push_back(submit_throughput(d_thr));
    }
    for (auto h : handles) js.wait(h);

    REQUIRE(u_count.load() == kTotalPerClass);
    REQUIRE(l_count.load() == kTotalPerClass);
    REQUIRE(t_count.load() == kTotalPerClass);

    js.stop();
}

TEST_CASE("dependency edge from latency parent to throughput child",
          "[jobs][hetero][graph]") {
    auto& js = JobSystem::Get();
    js.start();

    std::atomic<int> parent_done{0};
    std::atomic<int> child_saw_parent{0};

    auto parent_fn = +[](void* u) noexcept {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        static_cast<std::atomic<int>*>(u)->store(1, std::memory_order_release);
    };
    struct ChildCtx { std::atomic<int>* p; std::atomic<int>* c; };
    auto child_fn = +[](void* u) noexcept {
        auto* cc = static_cast<ChildCtx*>(u);
        if (cc->p->load(std::memory_order_acquire) == 1) {
            cc->c->fetch_add(1, std::memory_order_relaxed);
        }
    };

    ChildCtx ctx{ &parent_done, &child_saw_parent };
    JobDesc dp{ parent_fn, &parent_done, "parent", 0 };
    JobHandle hp = submit_latency(dp);
    JobDesc dc{ child_fn, &ctx, "child", 0 };
    JobHandle hc = submit_throughput(dc, hp);

    js.wait(hc);
    REQUIRE(parent_done.load() == 1);
    REQUIRE(child_saw_parent.load() == 1);

    js.stop();
}
