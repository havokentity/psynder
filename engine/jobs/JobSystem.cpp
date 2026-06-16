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
// Fibers (DESIGN.md §2.4): Wave B adds an in-tree Fiber primitive
// (engine/jobs/Fiber*) — Win32 fibers / POSIX ucontext, used by the
// lightmap baker and BVH refit to yield mid-bounce without stalling a
// worker. The DAG scheduler below is untouched; fibers are an orthogonal
// concurrency tool a job can grab when it needs to suspend itself.
//
// Heterogeneous P/E cores (DESIGN.md §2.4 closing paragraph): Wave B also
// adds side-pools served by the new `submit_latency` / `submit_throughput`
// free functions in `JobSystemHetero.h`. On homogeneous boxes the side
// pools collapse onto the unified pool and the new calls behave identically
// to `submit`.

#include "JobSystem.h"

#include "AlignedAlloc_internal.h"
#include "ChaseLevDeque_internal.h"
#include "HeteroPool_internal.h"
#include "JobPool_internal.h"
#include "JobSystemHetero.h"
#include "core/hardware/CpuFeatures.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
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
//
// Wave B layered the heterogeneous pools on top of the Wave A unified
// worker set. Each Worker carries a `cls` tag — Unified, Latency, or
// Throughput. Three inboxes feed the workers: the unified `main_inbox`
// (filled by `submit`), plus `latency_inbox` and `throughput_inbox`
// filled by `submit_latency` / `submit_throughput`. Stealing rules:
//
//   - Workers always pop their own deque (LIFO).
//   - A worker drains its CLASS inbox first (latency worker → latency_inbox,
//     throughput → throughput_inbox, unified → main_inbox).
//   - Then it drains the unified inbox (everyone helps unified work).
//   - Then it steals from a sibling deque — preferring same-class siblings
//     to keep work where it was scheduled.
//
// The unified pool — when hetero is not active or when `submit` is called —
// remains the canonical fallback so existing call sites keep working without
// changes. On homogeneous boxes the latency/throughput inboxes still exist
// but Wave-B `submit_latency`/`submit_throughput` route directly to the
// unified inbox (no extra hops).

enum class WorkerClass : u8 {
    Unified = 0,
    Latency = 1,
    Throughput = 2,
};

struct PSY_CACHELINE_ALIGN Worker {
    std::thread thread;
    detail::ChaseLevDeque deque;
    // Local RNG for victim selection; seeded with worker id.
    std::minstd_rand rng;
    WorkerClass cls = WorkerClass::Unified;
};

struct SchedulerState {
    detail::JobPool pool;
    Worker* workers = nullptr;
    u32 worker_count = 0;
    std::atomic<u32> live_jobs{0};
    std::atomic<u32> running{0};  // 1 while workers should keep looping
    // Main-thread inbox. Chase-Lev requires a single owner; the main thread
    // is its only "push"-er. Workers only steal from it (top side). This
    // keeps the workers' own deques single-owner — only the worker pushes/pops
    // its own deque; thieves steal from the top.
    detail::ChaseLevDeque main_inbox;
    // Hetero inboxes — populated by submit_latency / submit_throughput
    // when the host is heterogeneous. On a homogeneous host these still
    // get init'd but the hetero submit functions route to main_inbox.
    detail::ChaseLevDeque latency_inbox;
    detail::ChaseLevDeque throughput_inbox;
    // Cross-worker submission: workers submitting (e.g. parallel_for from
    // inside a job) push to their own deque. Submissions from arbitrary
    // non-worker threads other than main are serialized via the inbox_mu.
    std::mutex inbox_mu;
    // Sleeping support: when workers find nothing to steal, they park.
    std::mutex sleep_mu;
    std::condition_variable sleep_cv;
    std::atomic<u32> sleeping{0};

    // Hetero pool topology, populated at start(). When `hetero_active` is
    // false, submit_latency / submit_throughput route to main_inbox and
    // every worker is `Unified`.
    bool hetero_active = false;
    u32 latency_workers = 0;     // count of cls==Latency
    u32 throughput_workers = 0;  // count of cls==Throughput
    detail::HeteroTopology topology{};

    // Join any still-running workers BEFORE the mutex / condition_variable
    // members below are destroyed. A process that used the job system but never
    // called stop() (e.g. the unit-test binary, which drives parallel_for via
    // ECS queries / physics) otherwise tears down g_sched with workers still
    // parked on sleep_cv/sleep_mu; on exit those workers lock an already-
    // destroyed mutex -> "mutex lock failed: Invalid argument" abort (SIGABRT,
    // flaky on timing). stop() zeroes worker_count, so this is a no-op after a
    // clean stop(). We only join here (no deinit/free) — the OS reclaims the
    // rest at process exit, and skipping it avoids double-free vs. stop().
    ~SchedulerState() {
        if (worker_count == 0u || workers == nullptr)
            return;
        running.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(sleep_mu);
            sleep_cv.notify_all();
        }
        for (u32 i = 0; i < worker_count; ++i) {
            if (workers[i].thread.joinable())
                workers[i].thread.join();
        }
    }
};

SchedulerState g_sched;

// Pool size: 64k slots is plenty for any single frame. Power of two for the
// deque mask; the JobPool is unrelated to that.
constexpr u32 kPoolCapacity = 64 * 1024;
// Deque ring per worker. Sized to hold a frame's worst case for one worker
// without overflow. 8k is comfortably above what raster + ECS + physics
// will push per worker per frame.
constexpr u32 kDequeCapacity = 8 * 1024;

// Make a handle from (slot, gen).
constexpr JobHandle make_handle(u32 slot, u32 gen) noexcept {
    return JobHandle{slot, gen};
}

// Pick the right inbox for a non-worker pusher given the job's pool class.
detail::ChaseLevDeque* pick_inbox_for_class(u8 pool_class) noexcept {
    if (!g_sched.hetero_active || pool_class == 0u) {
        return &g_sched.main_inbox;
    }
    return (pool_class == 1u) ? &g_sched.latency_inbox : &g_sched.throughput_inbox;
}

// Dispatch a slot that has reached zero pending deps. From a worker, push
// onto own deque. From the main thread (or any non-worker), push onto the
// inbox matching the slot's pool class — workers steal from it. Non-main
// non-worker threads must hold inbox_mu to push, but Wave A's only producers
// are main + workers so this branch is rare. The owner invariant (single
// push-er per deque) is preserved either way.
detail::ChaseLevDeque* select_deque_for_push(u8 pool_class, bool& need_inbox_lock) noexcept {
    u32 me = t_worker_id;
    if (me != ~0u) {
        need_inbox_lock = false;
        return &g_sched.workers[me].deque;
    }
    need_inbox_lock = true;  // non-worker — lock the inbox writer slot
    return pick_inbox_for_class(pool_class);
}

void dispatch_ready(u32 slot) noexcept {
    auto& job = g_sched.pool.at(slot);
    u8 pool_class = job.pool_class;
    bool need_inbox_lock = false;
    auto* dp = select_deque_for_push(pool_class, need_inbox_lock);
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
        if (job.fn)
            job.fn(job.user);
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
    if (job.fn)
        job.fn(job.user);
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

// Try to find work: own deque, class-aligned inbox, unified inbox, then a
// sibling deque (preferring same-class siblings).
bool try_one_job(u32 my_id) noexcept {
    // Own deque first (LIFO — keeps the cache hot).
    auto r = g_sched.workers[my_id].deque.try_pop();
    if (r.status == detail::ChaseLevDeque::Status::Ok) {
        execute_slot(r.index);
        return true;
    }
    const WorkerClass my_cls = g_sched.workers[my_id].cls;
    // Drain the class-aligned hetero inbox first when hetero is active.
    // This keeps latency work on P-cores and throughput on E-cores when
    // both have work pending. Falls through to the unified inbox so a
    // worker never sleeps while there is *any* work to do.
    if (g_sched.hetero_active) {
        detail::ChaseLevDeque* class_inbox = nullptr;
        if (my_cls == WorkerClass::Latency)
            class_inbox = &g_sched.latency_inbox;
        else if (my_cls == WorkerClass::Throughput)
            class_inbox = &g_sched.throughput_inbox;
        if (class_inbox) {
            auto s = class_inbox->try_steal();
            if (s.status == detail::ChaseLevDeque::Status::Ok) {
                execute_slot(s.index);
                return true;
            }
        }
    }
    // Drain the unified inbox: jobs submitted via plain `submit()` land here.
    {
        auto s = g_sched.main_inbox.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
    }
    // Cross-class hetero inboxes: a latency worker that finds nothing in
    // its own class should help drain throughput work (and vice versa)
    // rather than parking, so we don't waste cores when only one class has
    // load. Same-class is tried above; this is the fallback.
    if (g_sched.hetero_active) {
        detail::ChaseLevDeque* other = nullptr;
        if (my_cls == WorkerClass::Latency)
            other = &g_sched.throughput_inbox;
        else if (my_cls == WorkerClass::Throughput)
            other = &g_sched.latency_inbox;
        if (other) {
            auto s = other->try_steal();
            if (s.status == detail::ChaseLevDeque::Status::Ok) {
                execute_slot(s.index);
                return true;
            }
        }
    }
    // Steal from a sibling worker. Bias toward same-class siblings by
    // sampling among them first if hetero is active.
    u32 n = g_sched.worker_count;
    if (n > 1) {
        if (g_sched.hetero_active) {
            // Collect same-class peers (up to a small inline set; we
            // expect at most a few dozen workers).
            u32 peer_ids[64];
            u32 peer_count = 0;
            for (u32 i = 0; i < n && peer_count < 64u; ++i) {
                if (i == my_id)
                    continue;
                if (g_sched.workers[i].cls == my_cls) {
                    peer_ids[peer_count++] = i;
                }
            }
            if (peer_count > 0u) {
                std::uniform_int_distribution<u32> dist(0u, peer_count - 1u);
                u32 victim = peer_ids[dist(g_sched.workers[my_id].rng)];
                auto s = g_sched.workers[victim].deque.try_steal();
                if (s.status == detail::ChaseLevDeque::Status::Ok) {
                    execute_slot(s.index);
                    return true;
                }
            }
        }
        // Random victim from all workers — the cross-class fallback that
        // keeps every core fed even when load is unbalanced across classes.
        std::uniform_int_distribution<u32> dist(0, n - 1);
        u32 victim = dist(g_sched.workers[my_id].rng);
        if (victim == my_id)
            victim = (victim + 1) % n;
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
    // Apply the per-thread P/E hint. On homogeneous boxes every worker is
    // Unified and the hint call is a no-op; on hetero hosts (Apple Silicon,
    // Intel Core Ultra, Snapdragon X) we tag latency workers as
    // P-core-preferring and throughput workers as E-core-preferring.
    WorkerClass cls = g_sched.workers[id].cls;
    if (cls == WorkerClass::Latency) {
        detail::apply_worker_class_hint(detail::WorkerClass::Latency);
    } else if (cls == WorkerClass::Throughput) {
        detail::apply_worker_class_hint(detail::WorkerClass::Throughput);
    }

    while (g_sched.running.load(std::memory_order_acquire) != 0u) {
        if (try_one_job(id)) {
            continue;
        }
        // Brief spin before parking — most empty slots are race-empty, not
        // truly empty.
        bool found = false;
        for (int spin = 0; spin < 64; ++spin) {
            std::this_thread::yield();
            if (try_one_job(id)) {
                found = true;
                break;
            }
        }
        if (found)
            continue;
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
    // Unified inbox first (work the current thread itself produced is here).
    {
        auto s = g_sched.main_inbox.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
    }
    // Hetero inboxes — main may have submitted to either pool.
    if (g_sched.hetero_active) {
        auto s = g_sched.latency_inbox.try_steal();
        if (s.status == detail::ChaseLevDeque::Status::Ok) {
            execute_slot(s.index);
            return true;
        }
        s = g_sched.throughput_inbox.try_steal();
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
    if (g_sched.worker_count > 0)
        return;  // already started

    // Detect heterogeneous topology up front. If the host reports a P/E
    // split we sub-divide the worker pool into latency / throughput
    // workers; otherwise every worker is Unified and the hetero inboxes
    // collapse onto main_inbox semantically.
    g_sched.topology = detail::detect_hetero_topology();

    if (worker_count == 0) {
        const auto& cpu = psynder::hardware::detect();
        u32 cores = cpu.cores_physical ? cpu.cores_physical : std::thread::hardware_concurrency();
        if (cores < 2)
            cores = 2;
        // One worker per physical core, main thread reserved.
        worker_count = cores > 1 ? cores - 1 : 1;
    }

    // Decide hetero split. Activation rules:
    //   - Topology reports both P and E cores.
    //   - Total worker count is at least 4 so each pool has ≥ 1 worker
    //     after main-thread reservation. Below 4 we keep everyone Unified
    //     to avoid degenerate single-worker hetero pools that can't
    //     parallelise their own class.
    u32 p_workers = 0;
    u32 e_workers = 0;
    bool hetero = g_sched.topology.is_hetero() && worker_count >= 4u;
    if (hetero) {
        // Mirror the host ratio into worker assignment. We reserve one
        // P-core slot for the main thread (matches Wave A's "one core
        // reserved" rule and keeps frame setup off the workers).
        const u32 host_p = g_sched.topology.p_cores;
        const u32 host_e = g_sched.topology.e_cores;
        const u32 host_total = host_p + host_e;
        // Scale worker_count back into P/E pools by the host ratio. Round
        // P up so latency-critical work always gets at least one worker.
        p_workers = (worker_count * host_p + host_total - 1u) / host_total;
        // Main thread occupies one P-core slot; subtract.
        if (p_workers > 0u)
            p_workers -= 1u;
        if (p_workers == 0u)
            p_workers = 1u;
        e_workers = worker_count - p_workers;
        if (e_workers == 0u) {
            // Topology reported E-cores but worker math zeroed them out
            // (small worker_count). Steal one from p to keep both pools
            // populated, otherwise we'd silently lose throughput pool.
            if (p_workers > 1u) {
                --p_workers;
                e_workers = 1u;
            } else {
                // Genuinely too few workers — fall back to Unified.
                hetero = false;
                p_workers = e_workers = 0u;
            }
        }
    }

    g_sched.pool.init(kPoolCapacity);
    g_sched.main_inbox.init(kDequeCapacity);
    g_sched.latency_inbox.init(kDequeCapacity);
    g_sched.throughput_inbox.init(kDequeCapacity);
    g_sched.hetero_active = hetero;
    g_sched.latency_workers = hetero ? p_workers : 0u;
    g_sched.throughput_workers = hetero ? e_workers : 0u;

    g_sched.workers =
        static_cast<Worker*>(detail::aligned_xalloc(kCacheLine, sizeof(Worker) * worker_count));
    for (u32 i = 0; i < worker_count; ++i) {
        new (&g_sched.workers[i]) Worker();
        g_sched.workers[i].deque.init(kDequeCapacity);
        g_sched.workers[i].rng.seed(0x9E3779B9u ^ i);
        if (hetero) {
            // First p_workers indices serve the latency pool; the rest
            // serve the throughput pool.
            g_sched.workers[i].cls = (i < p_workers) ? WorkerClass::Latency : WorkerClass::Throughput;
        } else {
            g_sched.workers[i].cls = WorkerClass::Unified;
        }
    }
    g_sched.worker_count = worker_count;
    g_sched.running.store(1, std::memory_order_release);

    for (u32 i = 0; i < worker_count; ++i) {
        g_sched.workers[i].thread = std::thread(worker_main, i);
    }
}

void JobSystem::stop() {
    if (g_sched.worker_count == 0)
        return;

    // Drain anything still in flight. The main thread helps so we don't deadlock.
    while (g_sched.live_jobs.load(std::memory_order_acquire) > 0u) {
        if (!help_one_job())
            std::this_thread::yield();
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
    detail::aligned_xfree(g_sched.workers);
    g_sched.workers = nullptr;
    g_sched.main_inbox.deinit();
    g_sched.latency_inbox.deinit();
    g_sched.throughput_inbox.deinit();
    g_sched.pool.deinit();
    g_sched.worker_count = 0;
    g_sched.hetero_active = false;
    g_sched.latency_workers = 0;
    g_sched.throughput_workers = 0;
}

namespace {

// Shared submit body — tags the job with `pool_class` so dispatch routing
// (and hetero stealing) honor the requested class. The dep-wiring logic
// is identical across submit / submit_latency / submit_throughput.
JobHandle submit_with_class(const JobDesc& desc, JobHandle dep, u8 pool_class) noexcept {
    // Lazy-start if a caller forgot — keeps tests/samples that submit
    // straight off the bat from no-oping.
    if (g_sched.worker_count == 0)
        JobSystem::Get().start(0);

    u32 slot = g_sched.pool.claim();
    if (slot == 0) {
        // Pool exhausted: run synchronously to keep correctness.
        if (desc.fn)
            desc.fn(desc.user);
        return JobHandle{};
    }
    auto& job = g_sched.pool.at(slot);
    job.reset();
    job.fn = desc.fn;
    job.user = desc.user;
    job.name = desc.name;
    job.priority = desc.priority;
    job.pool_class = pool_class;
    u32 gen = job.gen.load(std::memory_order_acquire);

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
            // Re-check: did the parent finish while we were wiring up?
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (parent.done.load(std::memory_order_acquire) != 0u) {
                i32 expected = 1;
                if (job.pending_deps.compare_exchange_strong(expected,
                                                             0,
                                                             std::memory_order_acq_rel,
                                                             std::memory_order_acquire)) {
                    g_sched.live_jobs.fetch_add(1, std::memory_order_acq_rel);
                    dispatch_ready(slot);
                    return make_handle(slot, gen);
                }
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

}  // namespace

JobHandle JobSystem::submit(const JobDesc& desc, JobHandle dep) {
    return submit_with_class(desc, dep, /*pool_class*/ 0u);
}

// ─── Heterogeneous-pool API (JobSystemHetero.h) ──────────────────────────

JobHandle submit_latency(const JobDesc& desc, JobHandle dep) noexcept {
    if (g_sched.worker_count == 0)
        JobSystem::Get().start(0);
    // On homogeneous hosts, route to the unified pool (pool_class 0).
    u8 cls = g_sched.hetero_active ? 1u : 0u;
    return submit_with_class(desc, dep, cls);
}

JobHandle submit_throughput(const JobDesc& desc, JobHandle dep) noexcept {
    if (g_sched.worker_count == 0)
        JobSystem::Get().start(0);
    u8 cls = g_sched.hetero_active ? 2u : 0u;
    return submit_with_class(desc, dep, cls);
}

bool hetero_is_active() noexcept {
    return g_sched.hetero_active;
}

u32 hetero_latency_workers() noexcept {
    return g_sched.latency_workers;
}

u32 hetero_throughput_workers() noexcept {
    return g_sched.throughput_workers;
}

HeteroCounts hetero_detected_counts() noexcept {
    HeteroCounts c;
    // detect_hetero_topology is idempotent / cached so calling here is cheap
    // even if start() hasn't run yet.
    auto t = detail::detect_hetero_topology();
    c.p_cores = t.p_cores;
    c.e_cores = t.e_cores;
    c.total = t.total;
    return c;
}

void JobSystem::wait(JobHandle h) {
    if (!h.valid() || h.id == 0u || h.id > g_sched.pool.capacity())
        return;
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

void JobSystem::parallel_for(usize begin,
                             usize end,
                             usize grain,
                             const std::function<void(usize, usize)>& body) {
    if (begin >= end || !body)
        return;
    if (grain == 0)
        grain = 1;

    const usize total = end - begin;
    const usize chunks = (total + grain - 1) / grain;

    if (chunks == 1 || g_sched.worker_count == 0) {
        body(begin, end);
        return;
    }

    // Small task descriptor per chunk, plus a handle per chunk. The DESIGN's
    // zero-per-frame-heap-garbage rule (DESIGN 3.4) forbids the previous
    // unconditional std::malloc/free pair here. We use small-buffer
    // optimization: the storage lives on THIS call's stack frame for the
    // overwhelmingly common small case (<= kSboChunks) and only falls back to
    // the C heap when the chunk count exceeds the SBO threshold.
    //
    // Re-entrancy / concurrency: parallel_for can be issued from inside a
    // running job (nested) and from any worker thread concurrently. The SBO
    // storage is a plain automatic array, so every (possibly nested, possibly
    // cross-thread) call owns a distinct, private copy on its own stack frame.
    // No shared mutable scratch is touched, so there is no data race and no
    // corruption under nesting — identical thread-safety to the old per-call
    // malloc, minus the allocation.
    struct Chunk {
        usize lo;
        usize hi;
        const std::function<void(usize, usize)>* body;
    };

    // Inline capacity. 64 chunks covers virtually every parallel_for in the
    // engine (raster tiles, ECS query work-lists, physics writeback all sit
    // well under this on typical scenes). 64 * (sizeof(Chunk) + sizeof(JobHandle))
    // is a few KiB of stack — safe for a worker stack.
    constexpr usize kSboChunks = 64;
    Chunk sbo_tasks[kSboChunks];
    JobHandle sbo_handles[kSboChunks];

    Chunk* tasks = sbo_tasks;
    JobHandle* handles = sbo_handles;
    const bool spilled = chunks > kSboChunks;
    if (spilled) {
        tasks = static_cast<Chunk*>(std::malloc(sizeof(Chunk) * chunks));
        handles = static_cast<JobHandle*>(std::malloc(sizeof(JobHandle) * chunks));
    }

    auto runner = +[](void* u) noexcept {
        auto* c = static_cast<Chunk*>(u);
        (*c->body)(c->lo, c->hi);
    };

    for (usize i = 0; i < chunks; ++i) {
        tasks[i].lo = begin + i * grain;
        tasks[i].hi = (i + 1) * grain > total ? end : begin + (i + 1) * grain;
        tasks[i].body = &body;
        JobDesc d;
        d.fn = runner;
        d.user = &tasks[i];
        d.name = "parallel_for";
        handles[i] = submit(d);
    }

    for (usize i = 0; i < chunks; ++i) {
        wait(handles[i]);
    }

    if (spilled) {
        std::free(tasks);
        std::free(handles);
    }
}

u32 JobSystem::worker_count() const noexcept {
    return g_sched.worker_count;
}

u32 JobSystem::current_worker() const noexcept {
    return t_worker_id;
}

}  // namespace psynder::jobs
