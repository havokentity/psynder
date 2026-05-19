// SPDX-License-Identifier: MIT
// Psynder — Lane 04 unit: job DAG dependency ordering + no-leak across many
// frames (slots get recycled).

#include <catch2/catch_test_macros.hpp>

#include "jobs/JobSystem.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace psynder;
using namespace psynder::jobs;

namespace {

struct OrderState {
    std::atomic<int> a_finished{0};
    std::atomic<int> b_started_after_a{0};
    std::atomic<int> c_started_after_b{0};
};

void task_a(void* u) noexcept {
    auto* s = static_cast<OrderState*>(u);
    // Simulate a little work so the dep edge actually has to gate the next.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    s->a_finished.store(1, std::memory_order_release);
}

void task_b(void* u) noexcept {
    auto* s = static_cast<OrderState*>(u);
    if (s->a_finished.load(std::memory_order_acquire) == 1) {
        s->b_started_after_a.store(1, std::memory_order_release);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void task_c(void* u) noexcept {
    auto* s = static_cast<OrderState*>(u);
    if (s->b_started_after_a.load(std::memory_order_acquire) == 1) {
        s->c_started_after_b.store(1, std::memory_order_release);
    }
}

}  // namespace

TEST_CASE("submit(desc, dep) enforces dependency order", "[jobs][graph]") {
    auto& js = JobSystem::Get();
    js.start();

    OrderState st;
    JobDesc da{ task_a, &st, "a", 0 };
    JobDesc db{ task_b, &st, "b", 0 };
    JobDesc dc{ task_c, &st, "c", 0 };

    JobHandle ha = js.submit(da);
    JobHandle hb = js.submit(db, ha);
    JobHandle hc = js.submit(dc, hb);

    js.wait(hc);

    REQUIRE(st.a_finished.load() == 1);
    REQUIRE(st.b_started_after_a.load() == 1);
    REQUIRE(st.c_started_after_b.load() == 1);

    js.stop();
}

TEST_CASE("fan-out: many children of one parent all wait correctly",
          "[jobs][graph]") {
    auto& js = JobSystem::Get();
    js.start();

    std::atomic<int> parent_done{0};
    std::atomic<int> children_seeing_parent_done{0};
    constexpr int kFan = 32;

    struct ParentCtx { std::atomic<int>* d; };
    struct ChildCtx  { std::atomic<int>* d; std::atomic<int>* c; };

    ParentCtx pctx{ &parent_done };
    std::vector<ChildCtx> cctxs(kFan);
    for (auto& c : cctxs) { c.d = &parent_done; c.c = &children_seeing_parent_done; }

    auto parent_fn = +[](void* u) noexcept {
        auto* p = static_cast<ParentCtx*>(u);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        p->d->store(1, std::memory_order_release);
    };
    auto child_fn = +[](void* u) noexcept {
        auto* c = static_cast<ChildCtx*>(u);
        if (c->d->load(std::memory_order_acquire) == 1) {
            c->c->fetch_add(1, std::memory_order_relaxed);
        }
    };

    JobDesc pd{ parent_fn, &pctx, "parent", 0 };
    JobHandle hp = js.submit(pd);
    std::vector<JobHandle> hc(kFan);
    for (int i = 0; i < kFan; ++i) {
        JobDesc cd{ child_fn, &cctxs[static_cast<size_t>(i)], "child", 0 };
        hc[static_cast<size_t>(i)] = js.submit(cd, hp);
    }
    for (auto h : hc) js.wait(h);

    REQUIRE(children_seeing_parent_done.load() == kFan);

    js.stop();
}

TEST_CASE("repeated submit/wait does not leak slots across frames",
          "[jobs][graph][leak]") {
    auto& js = JobSystem::Get();
    js.start();

    // Submit far more jobs in total than the pool can hold simultaneously
    // (kPoolCapacity is 64k internal). If slots leaked we'd run out quickly.
    constexpr int kFrames        = 64;
    constexpr int kJobsPerFrame  = 4096;

    std::atomic<int> counter{0};
    auto bump = +[](void* u) noexcept {
        static_cast<std::atomic<int>*>(u)->fetch_add(1, std::memory_order_relaxed);
    };

    for (int frame = 0; frame < kFrames; ++frame) {
        std::vector<JobHandle> handles;
        handles.reserve(static_cast<size_t>(kJobsPerFrame));
        for (int i = 0; i < kJobsPerFrame; ++i) {
            JobDesc d{ bump, &counter, "leaky", 0 };
            handles.push_back(js.submit(d));
        }
        for (auto h : handles) js.wait(h);
        // Per-frame barrier — every submitted job must have run exactly once
        // before its handle's wait() returns. This catches both "lost a job"
        // and "ran twice" regressions immediately rather than at the final
        // total.
        REQUIRE(counter.load() == (frame + 1) * kJobsPerFrame);
    }

    REQUIRE(counter.load() == kFrames * kJobsPerFrame);

    js.stop();
}

TEST_CASE("dep on already-completed handle still dispatches the child",
          "[jobs][graph][race]") {
    auto& js = JobSystem::Get();
    js.start();

    std::atomic<int> ran{0};
    auto fn = +[](void* u) noexcept {
        static_cast<std::atomic<int>*>(u)->fetch_add(1, std::memory_order_relaxed);
    };

    JobDesc parent{ fn, &ran, "p", 0 };
    JobHandle hp = js.submit(parent);
    js.wait(hp);  // parent definitely done now

    JobDesc child{ fn, &ran, "c", 0 };
    JobHandle hc = js.submit(child, hp);
    js.wait(hc);

    REQUIRE(ran.load() == 2);

    js.stop();
}
