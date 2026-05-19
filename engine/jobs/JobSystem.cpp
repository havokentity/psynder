// SPDX-License-Identifier: MIT
// Psynder — Lane 04. Real implementation of the public JobSystem behind
// engine/jobs/JobSystem.h. See DESIGN.md §2.4 (threading) and §4 (per-worker
// arenas).
//
// Architecture (Wave A):
//
//   - One std::thread per physical core, minus 1 (main thread reserved for
//     OS/input). Caller can override with start(N).
//   - Each worker owns a Chase-Lev work-stealing deque. Owner thread pushes
//     and pops from the bottom; thieves steal from the top — the canonical
//     single-producer-multi-consumer pattern (Chase & Lev 2005, refined by
//     Le et al., PPoPP'13).
//   - Submissions from the main thread (or any non-worker) go through a
//     dedicated main_inbox deque, also Chase-Lev — main is its only producer,
//     workers steal from it. This preserves the "single owner per deque"
//     invariant; mixing main + worker pushes into the same deque would break
//     the algorithm's correctness.
//   - Jobs live in a single global pool (JobPool) of cache-line-aligned
//     slots, indexed 1..N (slot 0 is the null sentinel). Submission claims
//     a slot, fills it in, wires dependencies, and dispatches.
//   - Dependencies are a true DAG: submit(desc, dep) makes the new job a
//     child of dep. If dep is already complete, pending_deps stays 0 and the
//     child is pushed immediately. Otherwise the child slot is appended to
//     dep's children[] and dispatched only after dep finishes — the worker
//     decrements each child's pending_deps and pushes children that hit 0.
//     Reuses node memory: completed slots return to the pool via gen-bump.
//   - parallel_for splits [begin, end) into ceil((end-begin)/grain) chunks,
//     submits each as an independent job, then busy-helps until they all
//     finish.
//
// Wait semantics: wait() does not block on a condvar. It pumps jobs from the
// pool (LIFO from the caller's own deque, then stealing) while spinning on
// the target's `done` flag, which keeps every core fed and avoids deadlock
// when a job waits on another job that needs us.
//
// Fibers (DESIGN.md §2.4): deferred to Wave B. parallel_for and the DAG
// scheduler are sufficient for raster, vertex, narrowphase, etc., which is
// what M0/M1 need. See PR body.

#include "JobSystem.h"

#include "ChaseLevDeque_internal.h"
#include "JobPool_internal.h"
#include "core/hardware/CpuFeatures.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <random>
#include <thread>

namespace psynder::jobs {

namespace {

// ─── Local stub for mem::worker_scratch_init ─────────────────────────────
//
// The brief instructs: "At worker init, call mem::worker_scratch_init(worker_id)
// if available (lane 01 should provide; if not, use local stub)." The
// frozen Allocator.h (lane 01 public surface) does NOT currently export a
// per-worker init hook — it has worker_scratch() returning a singleton. We
// keep a local no-op shim here so lane 01 can later swap in the real
// initializer without us touching their public header.
void worker_scratch_init_local(u32 /*worker_id*/) noexcept {
    // No-op for Wave A; the engine's mem::worker_scratch() currently returns
    // a global arena. When lane 01 lands the per-worker arenas, this call
    // site is what they replace with the real init.
}

// Per-worker thread-local context. The worker ID + a pointer back to the
// JobSystem internals are used by submit() called from inside a job.
thread_local u32 t_worker_id = ~0u;

// ─── Internal scheduler state ────────────────────────────────────────────
struct PSY_CACHELINE_ALIGN Worker {
    std::thread          thread;
    detail::ChaseLevDeque deque;
    // Local RNG for victim selection; seeded with worker id.
    std::minstd_rand     rng;
};

struct SchedulerState {
    detail::JobPool       pool;
    Worker*               workers       = nullptr;
    u32                   worker_count  = 0;
    std::atomic<u32>      live_jobs{0};
    std::atomic<u32>      running{0};   // 1 while workers should keep looping
    // Main-thread inbox. Chase-Lev requires a single owner; the main thread
    // is its only "push"-er. Workers only steal from it (top side). This
    // keeps the workers' own deques single-owner — only the worker pushes/pops
    // its own deque; thieves steal from the top.
    detail::ChaseLevDeque main_inbox;
    // Cross-worker submission: workers submitting (e.g. parallel_for from
    // inside a job) push to their own deque. Submissions from arbitrary
    // non-worker threads other than main are serialized via the inbox_mu.
    std::mutex            inbox_mu;
    // Sleeping support: when workers find nothing to steal, they park.
    std::mutex            sleep_mu;
    std::condition_variable sleep_cv;
    std::atomic<u32>      sleeping{0};
};

SchedulerState g_sched;

// Pool size: 64k slots is plenty for any single frame. Power of two for the
// deque mask; the JobPool is unrelated to that.
constexpr u32 kPoolCapacity   = 64 * 1024;
// Deque ring per worker. Sized to hold a frame's worst case for one worker
// without overflow. 8k is comfortably above what raster + ECS + physics
// will push per worker per frame.
constexpr u32 kDequeCapacity  = 8 * 1024;

// Make a handle from (slot, gen).
constexpr JobHandle make_handle(u32 slot, u32 gen) noexcept {
    return JobHandle{slot, gen};
}

// Dispatch a slot that has reached zero pending deps. From a worker, push
// onto own deque. From the main thread (or any non-worker), push onto the
// main-thread inbox — workers steal from it. Non-main non-worker threads
// must hold inbox_mu to push, but Wave A's only producers are main + workers
// so this branch is rare. The owner invariant (single push-er per deque) is
// preserved either way.
detail::ChaseLevDeque* select_deque_for_push(bool& need_inbox_lock) noexcept {
    u32 me = t_worker_id;
    if (me != ~0u) {
        need_inbox_lock = false;
        return &g_sched.workers[me].deque;
    }
    need_inbox_lock = true;  // non-worker — lock the inbox writer slot
    return &g_sched.main_inbox;
}

void dispatch_ready(u32 slot) noexcept {
    bool need_inbox_lock = false;
    auto* dp = select_deque_for_push(need_inbox_lock);
    auto& d = *dp;
    bool pushed = false;
    {
        // Serialize non-worker pushers (main, and any other thread that calls
        // submit() outside a worker). Workers don't take this lock and push
        // their own single-owner deque instead.
        std::unique_lock<std::mutex> ulk;
        if (need_inbox_lock) {
            ulk = std::unique_lock<std::mutex>(g_sched.inbox_mu);
        }
        pushed = d.push(slot);
    }
    if (!pushed) {
        // Deque full: fall back to running synchronously to avoid losing the
        // job. This is the safety valve; we size the deque so it doesn't
        // happen in practice for Wave A workloads.
        auto& job = g_sched.pool.at(slot);
        if (job.fn) job.fn(job.user);
        job.done.store(1, std::memory_order_release);
        for (u32 i = 0; i < job.inline_child_count; ++i) {
            u32 c = job.inline_children[i];
            auto& cj = g_sched.pool.at(c);
            if (cj.pending_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                dispatch_ready(c);
            }
        }
        for (u32 i = 0; i < job.spill_count; ++i) {
            u32 c = job.spill_children[i];
            auto& cj = g_sched.pool.at(c);
            if (cj.pending_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                dispatch_ready(c);
            }
        }
        g_sched.pool.release(slot);
        g_sched.live_jobs.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    // Wake one sleeper if any.
    if (g_sched.sleeping.load(std::memory_order_acquire) > 0) {
        std::lock_guard<std::mutex> lk(g_sched.sleep_mu);
        g_sched.sleep_cv.notify_one();
    }
}

// Run the user fn for a slot, propagate completion to its children, and
// recycle the slot.
void execute_slot(u32 slot) noexcept {
    auto& job = g_sched.pool.at(slot);
    if (job.fn) job.fn(job.user);
    // Mark done before releasing — waiters spin on this.
    job.done.store(1, std::memory_order_release);
    // Walk children. Each child whose pending_deps hits zero is dispatched.
    for (u32 i = 0; i < job.inline_child_count; ++i) {
        u32 c = job.inline_children[i];
        auto& cj = g_sched.pool.at(c);
        if (cj.pending_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dispatch_ready(c);
        }
    }
    for (u32 i = 0; i < job.spill_count; ++i) {
        u32 c = job.spill_children[i];
        auto& cj = g_sched.pool.at(c);
        if (cj.pending_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dispatch_ready(c);
        }
    }
    // Recycle the slot. Bumping the gen counter invalidates any stale handle.
    g_sched.pool.release(slot);
    g_sched.live_jobs.fetch_sub(1, std::memory_order_acq_rel);
}

// Try to find work: own deque, main inbox, then a random victim.
bool try_one_job(u32 my_id) noexcept {
    // Own deque first (LIFO — keeps the cache hot).
    auto r = g_sched.workers[my_id].deque.try_pop();
    if (r.status == detail::ChaseLevDeque::Status::Ok) {
        execute_slot(r.index);
        return true;
    }
    // Drain the main inbox: jobs submitted from non-worker threads land here.
    {
        auto s = g_sched.main_inbox.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
    }
    // Steal from a sibling worker.
    u32 n = g_sched.worker_count;
    if (n > 1) {
        std::uniform_int_distribution<u32> dist(0, n - 1);
        u32 victim = dist(g_sched.workers[my_id].rng);
        if (victim == my_id) victim = (victim + 1) % n;
        auto s = g_sched.workers[victim].deque.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
    }
    return false;
}

// Worker main loop.
void worker_main(u32 id) noexcept {
    t_worker_id = id;
    worker_scratch_init_local(id);

    while (g_sched.running.load(std::memory_order_acquire) != 0u) {
        if (try_one_job(id)) {
            continue;
        }
        // Brief spin before parking — most empty slots are race-empty, not
        // truly empty.
        bool found = false;
        for (int spin = 0; spin < 64; ++spin) {
            std::this_thread::yield();
            if (try_one_job(id)) { found = true; break; }
        }
        if (found) continue;
        // Park.
        std::unique_lock<std::mutex> lk(g_sched.sleep_mu);
        g_sched.sleeping.fetch_add(1, std::memory_order_acq_rel);
        // Re-check after taking the lock to avoid losing a wake.
        if (g_sched.running.load(std::memory_order_acquire) == 0u) {
            g_sched.sleeping.fetch_sub(1, std::memory_order_acq_rel);
            break;
        }
        // Wait until live_jobs grows or we're told to shut down.
        g_sched.sleep_cv.wait_for(lk, std::chrono::milliseconds(2));
        g_sched.sleeping.fetch_sub(1, std::memory_order_acq_rel);
    }
}

// Called from non-worker contexts (main thread, mostly during wait()) to
// help drain the pool while spinning on a target. Returns true if it did
// work. We may NOT pop from any worker's bottom — only steal from tops.
bool help_one_job() noexcept {
    // Inbox first (work the current thread itself produced is here).
    {
        auto s = g_sched.main_inbox.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
    }
    u32 n = g_sched.worker_count;
    for (u32 i = 0; i < n; ++i) {
        auto s = g_sched.workers[i].deque.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
    }
    return false;
}

}  // namespace

// ─── Public API ──────────────────────────────────────────────────────────

JobSystem& JobSystem::Get() {
    static JobSystem s;
    return s;
}

void JobSystem::start(u32 worker_count) {
    if (g_sched.worker_count > 0) return;  // already started

    if (worker_count == 0) {
        const auto& cpu = psynder::hardware::detect();
        u32 cores = cpu.cores_physical ? cpu.cores_physical
                                       : std::thread::hardware_concurrency();
        if (cores < 2) cores = 2;
        // One worker per physical core, main thread reserved.
        worker_count = cores > 1 ? cores - 1 : 1;
    }

    g_sched.pool.init(kPoolCapacity);
    g_sched.main_inbox.init(kDequeCapacity);
    g_sched.workers = static_cast<Worker*>(
        std::aligned_alloc(kCacheLine,
                           ((sizeof(Worker) * worker_count + kCacheLine - 1) /
                            kCacheLine) *
                               kCacheLine));
    for (u32 i = 0; i < worker_count; ++i) {
        new (&g_sched.workers[i]) Worker();
        g_sched.workers[i].deque.init(kDequeCapacity);
        g_sched.workers[i].rng.seed(0x9E3779B9u ^ i);
    }
    g_sched.worker_count = worker_count;
    g_sched.running.store(1, std::memory_order_release);

    for (u32 i = 0; i < worker_count; ++i) {
        g_sched.workers[i].thread = std::thread(worker_main, i);
    }
}

void JobSystem::stop() {
    if (g_sched.worker_count == 0) return;

    // Drain anything still in flight. The main thread helps so we don't deadlock.
    while (g_sched.live_jobs.load(std::memory_order_acquire) > 0u) {
        if (!help_one_job()) std::this_thread::yield();
    }

    g_sched.running.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_sched.sleep_mu);
        g_sched.sleep_cv.notify_all();
    }
    for (u32 i = 0; i < g_sched.worker_count; ++i) {
        if (g_sched.workers[i].thread.joinable()) {
            g_sched.workers[i].thread.join();
        }
        g_sched.workers[i].deque.deinit();
        g_sched.workers[i].~Worker();
    }
    std::free(g_sched.workers);
    g_sched.workers = nullptr;
    g_sched.main_inbox.deinit();
    g_sched.pool.deinit();
    g_sched.worker_count = 0;
}

JobHandle JobSystem::submit(const JobDesc& desc, JobHandle dep) {
    // Lazy-start if a caller forgot — keeps tests/samples that submit
    // straight off the bat from no-oping.
    if (g_sched.worker_count == 0) start(0);

    u32 slot = g_sched.pool.claim();
    if (slot == 0) {
        // Pool exhausted: run synchronously to keep correctness.
        if (desc.fn) desc.fn(desc.user);
        return JobHandle{};
    }
    auto& job = g_sched.pool.at(slot);
    job.reset();
    job.fn       = desc.fn;
    job.user     = desc.user;
    job.name     = desc.name;
    job.priority = desc.priority;
    u32 gen      = job.gen.load(std::memory_order_acquire);

    // Wire dep, if any. The fence here ensures the parent observes our
    // child entry before we observe its done flag.
    if (dep.valid() && dep.id != 0u && dep.id <= g_sched.pool.capacity()) {
        auto& parent = g_sched.pool.at(dep.id);
        // Verify the handle is fresh. If gen doesn't match the parent's
        // current gen, the parent has already completed and recycled — we
        // treat that as "no dep".
        u32 parent_gen = parent.gen.load(std::memory_order_acquire);
        if (parent_gen == dep.gen) {
            // Bump our pending count BEFORE we publish ourselves to the
            // parent, so a racing completion that pulls our entry out of
            // children[] sees a count it can decrement.
            job.pending_deps.store(1, std::memory_order_relaxed);
            parent.add_child(slot);
            // Re-check: did the parent finish while we were wiring up? If
            // done is set, the completion path already iterated children
            // (it may or may not have seen our addition — that race is the
            // reason for the second check). We are responsible for our own
            // dispatch if the parent has finished without decrementing us.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (parent.done.load(std::memory_order_acquire) != 0u) {
                // Parent done. If we are still pending, decrement; if we
                // were already decremented by the completion loop, this is
                // safe (extra decrement) — but the completion loop only
                // ever sees and decrements children that were in the array
                // when it ran. So if done==1 we may or may not have been
                // seen. The clean approach: if pending_deps is still 1,
                // we own dispatching ourselves.
                i32 expected = 1;
                if (job.pending_deps.compare_exchange_strong(
                        expected, 0, std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    g_sched.live_jobs.fetch_add(1, std::memory_order_acq_rel);
                    dispatch_ready(slot);
                    return make_handle(slot, gen);
                }
                // pending_deps was already 0 — completion saw us and decremented.
                // It will dispatch us.
                g_sched.live_jobs.fetch_add(1, std::memory_order_acq_rel);
                return make_handle(slot, gen);
            }
            g_sched.live_jobs.fetch_add(1, std::memory_order_acq_rel);
            return make_handle(slot, gen);
        }
        // Stale dep — treat as none.
    }

    // No dep — dispatch immediately.
    g_sched.live_jobs.fetch_add(1, std::memory_order_acq_rel);
    dispatch_ready(slot);
    return make_handle(slot, gen);
}

void JobSystem::wait(JobHandle h) {
    if (!h.valid() || h.id == 0u || h.id > g_sched.pool.capacity()) return;
    auto& job = g_sched.pool.at(h.id);
    while (job.done.load(std::memory_order_acquire) == 0u &&
           job.gen.load(std::memory_order_acquire) == h.gen) {
        // Help out: try to run a job ourselves rather than just spinning.
        if (t_worker_id != ~0u) {
            // We are inside a worker; pump our own deque + steal.
            try_one_job(t_worker_id);
        } else {
            // Main thread: steal-only.
            if (!help_one_job()) {
                std::this_thread::yield();
            }
        }
    }
}

void JobSystem::parallel_for(usize begin, usize end, usize grain,
                             const std::function<void(usize, usize)>& body) {
    if (begin >= end || !body) return;
    if (grain == 0) grain = 1;

    const usize total  = end - begin;
    const usize chunks = (total + grain - 1) / grain;

    if (chunks == 1 || g_sched.worker_count == 0) {
        body(begin, end);
        return;
    }

    // Allocate a small task descriptor per chunk on the stack via a heap
    // alloc — Wave A does not have a guaranteed frame arena from lane 01,
    // so we use the C heap. This is allocated outside the hot frame loop
    // typically; raster/etc. will sub their own grain.
    struct Chunk {
        usize lo;
        usize hi;
        const std::function<void(usize, usize)>* body;
    };
    auto* tasks = static_cast<Chunk*>(std::malloc(sizeof(Chunk) * chunks));
    auto* handles =
        static_cast<JobHandle*>(std::malloc(sizeof(JobHandle) * chunks));

    auto runner = +[](void* u) noexcept {
        auto* c = static_cast<Chunk*>(u);
        (*c->body)(c->lo, c->hi);
    };

    for (usize i = 0; i < chunks; ++i) {
        tasks[i].lo   = begin + i * grain;
        tasks[i].hi   = (i + 1) * grain > total ? end : begin + (i + 1) * grain;
        tasks[i].body = &body;
        JobDesc d;
        d.fn   = runner;
        d.user = &tasks[i];
        d.name = "parallel_for";
        handles[i] = submit(d);
    }

    for (usize i = 0; i < chunks; ++i) {
        wait(handles[i]);
    }

    std::free(tasks);
    std::free(handles);
}

u32 JobSystem::worker_count() const noexcept {
    return g_sched.worker_count;
}

u32 JobSystem::current_worker() const noexcept {
    return t_worker_id;
}

}  // namespace psynder::jobs
