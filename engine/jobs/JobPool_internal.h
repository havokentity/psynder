// SPDX-License-Identifier: MIT
// Psynder — internal header. Fixed-size pool of Job nodes with generation
// stamps, recycled across frames. Lane 04 owned.
//
// A `Job` lives in a slab of `kJobCapacity` slots. Submission claims a slot
// from a free list; completion increments the slot's generation and pushes
// it back. `JobHandle` is (slot_index, generation), so a stale handle never
// matches a reused slot.

#pragma once

#include "AlignedAlloc_internal.h"
#include "JobSystem.h"
#include "core/Types.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>

namespace psynder::jobs::detail {

// Per-slot state. The data layout matters here: counters are atomic and
// hit by many workers (publisher writes user/fn once, then many readers
// observe pending_deps_ falling to zero and the task gets dispatched).
struct PSY_CACHELINE_ALIGN Job {
    // Task payload — written once at submit, read many times.
    JobFn fn = nullptr;
    void* user = nullptr;
    const char* name = "job";
    u32 priority = 0;

    // DAG edges.
    //   pending_deps_   — count of unfinished prerequisites; runs at zero.
    //   children_count_ — published count; readers must see fn before this.
    //   children_       — small inline array of child slot indices; overflow
    //                     spills into the heap (DAGs of widths < 4 are the
    //                     common case for engine code, but we tolerate any
    //                     fan-out via the spill list).
    std::atomic<i32> pending_deps{0};

    static constexpr u32 kInlineChildren = 4;
    u32 inline_children[kInlineChildren] = {0, 0, 0, 0};
    u32 inline_child_count = 0;

    // Heap spill. Owned by this Job; freed when the job recycles.
    u32* spill_children = nullptr;
    u32 spill_count = 0;
    u32 spill_capacity = 0;

    // Generation counter for handle freshness. Bumped on completion.
    std::atomic<u32> gen{1};

    // Completion flag — set after the user fn returns. Waiters spin on this.
    std::atomic<u32> done{0};

    // Wave A: a single fence handle that the public wait() blocks on.
    // (No condition variable yet — we busy-help, see worker loop.)

    // Wave B: which pool class this job was submitted to. 0 = unified
    // (`submit`), 1 = latency / P-core preferring (`submit_latency`),
    // 2 = throughput / E-core preferring (`submit_throughput`). On
    // homogeneous boxes 1 and 2 collapse onto 0 at dispatch time.
    u8 pool_class = 0;

    void reset() noexcept {
        fn = nullptr;
        user = nullptr;
        name = "job";
        priority = 0;
        pending_deps.store(0, std::memory_order_relaxed);
        inline_child_count = 0;
        for (u32 i = 0; i < kInlineChildren; ++i)
            inline_children[i] = 0;
        if (spill_children) {
            std::free(spill_children);
            spill_children = nullptr;
        }
        spill_count = 0;
        spill_capacity = 0;
        pool_class = 0;
        done.store(0, std::memory_order_relaxed);
    }

    void add_child(u32 slot) noexcept {
        if (inline_child_count < kInlineChildren) {
            inline_children[inline_child_count++] = slot;
            return;
        }
        if (spill_count == spill_capacity) {
            u32 new_cap = spill_capacity ? spill_capacity * 2 : 8;
            auto* nb = static_cast<u32*>(std::realloc(spill_children, new_cap * sizeof(u32)));
            // realloc failure on a u32 array of tiny size on a desktop OS
            // is effectively fatal; in Wave A we accept the crash.
            spill_children = nb;
            spill_capacity = new_cap;
        }
        spill_children[spill_count++] = slot;
    }
};

// A pool of Job slots indexed 1..N. Slot 0 is reserved as "null handle".
class JobPool {
   public:
    void init(u32 capacity) noexcept {
        capacity_ = capacity;
        // Slot 0 is the sentinel; valid slots are [1, capacity_].
        slots_ = static_cast<Job*>(aligned_xalloc(kCacheLine, sizeof(Job) * (capacity_ + 1)));
        for (u32 i = 0; i <= capacity_; ++i) {
            new (&slots_[i]) Job();
        }
        free_list_top_.store(static_cast<i64>(capacity_), std::memory_order_relaxed);
        free_list_ = static_cast<u32*>(aligned_xalloc(kCacheLine, sizeof(u32) * capacity_));
        // Push slots 1..N onto the stack so claim() pops them in N..1 order.
        for (u32 i = 0; i < capacity_; ++i) {
            free_list_[i] = i + 1;
        }
    }

    void deinit() noexcept {
        if (slots_) {
            for (u32 i = 0; i <= capacity_; ++i) {
                slots_[i].~Job();
            }
            aligned_xfree(slots_);
            slots_ = nullptr;
        }
        if (free_list_) {
            aligned_xfree(free_list_);
            free_list_ = nullptr;
        }
        capacity_ = 0;
    }

    // Claim a free slot. Returns 0 if exhausted (caller should fall back to
    // synchronous execution or fail). Briefly takes the pool mutex so a
    // concurrent releaser cannot overwrite the cell we read.
    u32 claim() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        i64 top = free_list_top_.load(std::memory_order_relaxed);
        if (top <= 0)
            return 0;
        i64 new_top = top - 1;
        u32 slot = free_list_[new_top];
        free_list_top_.store(new_top, std::memory_order_release);
        return slot;
    }

    void release(u32 slot) noexcept {
        // Increment generation so any stale handle to this slot becomes invalid.
        slots_[slot].gen.fetch_add(1, std::memory_order_acq_rel);
        // The free list itself uses a brief mutex on the claim/release path.
        // A truly lock-free Treiber stack would need hazard pointers or epoch
        // reclamation to avoid ABA on the free_list_ slot cell, and the
        // hottest path of the engine (job dispatch + execute via per-worker
        // deques) is still entirely lock-free. The contention here is bounded
        // by the rate of slot recycling, which is one acquire/release per
        // submit + completion — comfortably off the per-tile/per-vertex hot
        // path. See PR body for the Wave-B plan to make this lock-free.
        std::lock_guard<std::mutex> lk(mu_);
        free_list_[free_list_top_.load(std::memory_order_relaxed)] = slot;
        free_list_top_.fetch_add(1, std::memory_order_release);
    }

    Job& at(u32 slot) noexcept { return slots_[slot]; }
    const Job& at(u32 slot) const noexcept { return slots_[slot]; }
    u32 capacity() const noexcept { return capacity_; }

   private:
    Job* slots_ = nullptr;
    u32* free_list_ = nullptr;
    std::atomic<i64> free_list_top_{0};
    u32 capacity_ = 0;
    std::mutex mu_;  // brief — protects free list claim/release
};

}  // namespace psynder::jobs::detail
