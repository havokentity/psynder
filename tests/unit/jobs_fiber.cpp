// SPDX-License-Identifier: MIT
// Psynder — Lane 04 unit: fiber yield/resume round-trip preserves locals.
//
// The DESIGN.md §2.4 use case is a lightmap baker that can suspend itself
// between rays without stalling its worker. We exercise the primitive
// directly here: a fiber computes a partial sum, yields, the host
// resumes it, and the fiber finishes — its stack-local accumulator must
// retain its value across the yield.

#include <catch2/catch_test_macros.hpp>

#include "jobs/Fiber_internal.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <vector>

using psynder::jobs::detail::Fiber;

namespace {

// Fiber-side context.
struct AdderState {
    int step = 0;
    int sum = 0;
    int ok = 0;
};

// Entry point: the fiber computes 1+2+3+...+N in chunks, yielding back to
// the host between chunks. Local variables across the yield boundary must
// preserve their values.
void adder_fiber(void* user) noexcept {
    auto* s = static_cast<AdderState*>(user);
    // Stack locals (the test): a deliberately large array so we can detect
    // stack corruption if the fiber's stack is mishandled.
    int locals[64];
    for (int i = 0; i < 64; ++i)
        locals[i] = i + 1;
    int acc = 0;
    // Phase A: accumulate locals[0..31].
    for (int i = 0; i < 32; ++i)
        acc += locals[i];
    s->step = 1;
    Fiber::yield();  // host resumes us — locals[] and `acc` must survive
    // Phase B: continue with locals[32..63].
    int phase_b_seed = acc;
    for (int i = 32; i < 64; ++i)
        acc += locals[i];
    s->step = 2;
    Fiber::yield();
    // Phase C: verify locals[] is unchanged.
    int verify_ok = 1;
    for (int i = 0; i < 64; ++i) {
        if (locals[i] != i + 1) {
            verify_ok = 0;
            break;
        }
    }
    // 1+2+...+64 == 64*65/2 == 2080.
    if (verify_ok && acc == 2080 && phase_b_seed == (32 * 33) / 2) {
        s->ok = 1;
    }
    s->sum = acc;
}

}  // namespace

TEST_CASE("Fiber yield/resume preserves stack locals", "[jobs][fiber]") {
    // Host preparation: on Win32 we must convert the thread to a fiber
    // before any switch. POSIX no-op.
    Fiber::make_thread_a_fiber_host();

    AdderState s;
    auto* f = Fiber::create(0u, adder_fiber, &s);
    REQUIRE(f != nullptr);

    REQUIRE(s.step == 0);
    REQUIRE_FALSE(f->done());

    // First resume — fiber runs until first yield.
    f->resume();
    REQUIRE(s.step == 1);
    REQUIRE_FALSE(f->done());

    // Second resume — fiber runs until second yield.
    f->resume();
    REQUIRE(s.step == 2);
    REQUIRE_FALSE(f->done());

    // Third resume — fiber runs to completion, marks done.
    f->resume();
    REQUIRE(s.ok == 1);
    REQUIRE(s.sum == 2080);
    REQUIRE(f->done());

    Fiber::destroy(f);
    Fiber::unmake_thread_a_fiber_host();
}

TEST_CASE("Fiber: multiple fibers, alternating resume", "[jobs][fiber]") {
    Fiber::make_thread_a_fiber_host();

    constexpr int kFibers = 8;
    // Each fiber yields 4 times inside its loop. After the loop returns,
    // platform_entry sets done_=1 and yields once more (the terminal
    // yield) — so 5 resumes are required to fully drain each fiber.
    constexpr int kIters = 4;
    constexpr int kResumes = kIters + 1;
    struct State {
        int counter = 0;
        int rounds = 0;
    };
    std::vector<State> states(kFibers);

    auto entry = +[](void* u) noexcept {
        auto* s = static_cast<State*>(u);
        for (int i = 0; i < kIters; ++i) {
            s->counter += (i + 1);
            ++s->rounds;
            Fiber::yield();
        }
    };

    std::vector<Fiber*> fibers(kFibers, nullptr);
    for (int i = 0; i < kFibers; ++i) {
        fibers[i] = Fiber::create(0u, entry, &states[i]);
        REQUIRE(fibers[i] != nullptr);
    }
    // Round-robin resume: each iteration of the outer loop advances every
    // fiber by one yield boundary. After kResumes outer iterations all
    // fibers must be done.
    for (int r = 0; r < kResumes; ++r) {
        for (int i = 0; i < kFibers; ++i) {
            fibers[i]->resume();
        }
    }
    for (int i = 0; i < kFibers; ++i) {
        REQUIRE(states[i].rounds == kIters);
        // 1 + 2 + 3 + 4 == 10
        REQUIRE(states[i].counter == 10);
        REQUIRE(fibers[i]->done());
        Fiber::destroy(fibers[i]);
    }

    Fiber::unmake_thread_a_fiber_host();
}

TEST_CASE("Fiber::current() reports the running fiber", "[jobs][fiber]") {
    Fiber::make_thread_a_fiber_host();

    REQUIRE(Fiber::current() == nullptr);

    struct Ctx {
        Fiber** self_observed;
    };
    auto entry = +[](void* u) noexcept {
        auto* c = static_cast<Ctx*>(u);
        *(c->self_observed) = Fiber::current();
    };

    Fiber* observed = nullptr;
    Ctx ctx{&observed};
    auto* f = Fiber::create(0u, entry, &ctx);
    REQUIRE(f != nullptr);
    f->resume();
    REQUIRE(f->done());
    REQUIRE(observed == f);
    // After the fiber returned, current() should be back to nullptr on this
    // thread (the platform_entry trampoline's last yield restored host).
    REQUIRE(Fiber::current() == nullptr);

    Fiber::destroy(f);
    Fiber::unmake_thread_a_fiber_host();
}
