// SPDX-License-Identifier: MIT
// Psynder — internal header. Chase-Lev work-stealing deque, lane 04 owned.
//
// Reference: D. Chase, Y. Lev, "Dynamic Circular Work-Stealing Deque"
// (SPAA '05), and the WG21-friendly C++ presentations of the algorithm
// (e.g. Le, Pop, Cohen, Nardelli, "Correct and Efficient Work-Stealing
// for Weak Memory Models", PPoPP '13; Sean Parent's CppCon walkthroughs;
// Tristan Bonnafé's public-domain implementations). The pattern used here:
//
//   - Owner thread pushes onto / pops from the bottom of the deque.
//   - Thief threads steal from the top.
//   - bottom_ and top_ are atomic; the buffer is a power-of-two ring.
//   - We do NOT resize the buffer; jobs use fixed-size pools and a
//     correctly-sized deque (capacity_pow2 set at start()).
//
// Memory order is the textbook PPoPP'13 mapping:
//   push:   relaxed load/store on bottom, release fence + relaxed store
//   take:   bottom-- (relaxed), seq_cst fence, cmpxchg(top) on race
//   steal:  acquire load top, acquire load bottom, relaxed load slot,
//           cmpxchg(top) to claim.

#pragma once

#include "AlignedAlloc_internal.h"
#include "core/Types.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace psynder::jobs::detail {

// A single deque entry is just a job pool slot index (u32). Sentinel 0 means
// "no job" (job IDs are 1-based in the pool).
struct PSY_CACHELINE_ALIGN ChaseLevDeque {
    // Result of try_pop / try_steal. We return an index plus a status because
    // both 0-job and empty-deque need to be distinguishable from a race.
    enum class Status : u8 { Ok, Empty, Aborted };
    struct PopResult { u32 index; Status status; };

    // Allocate the ring buffer. `capacity` MUST be a power of two.
    void init(u32 capacity) noexcept {
        // Round capacity up to next power of two if caller didn't.
        u32 c = 1;
        while (c < capacity) c <<= 1;
        capacity_      = c;
        mask_          = c - 1;
        // Allocate aligned to a cache line so adjacent deques don't share.
        void* mem = aligned_xalloc(kCacheLine, sizeof(std::atomic<u32>) * c);
        buffer_ = static_cast<std::atomic<u32>*>(mem);
        for (u32 i = 0; i < c; ++i) {
            new (&buffer_[i]) std::atomic<u32>(0);
        }
        top_.store(0, std::memory_order_relaxed);
        bottom_.store(0, std::memory_order_relaxed);
    }

    void deinit() noexcept {
        if (buffer_) {
            for (u32 i = 0; i < capacity_; ++i) {
                buffer_[i].~atomic();
            }
            aligned_xfree(buffer_);
            buffer_ = nullptr;
        }
        capacity_ = 0;
        mask_     = 0;
    }

    // Owner-only. Returns false if the deque is full.
    bool push(u32 idx) noexcept {
        const i64 b = bottom_.load(std::memory_order_relaxed);
        const i64 t = top_.load(std::memory_order_acquire);
        if (b - t >= static_cast<i64>(capacity_)) {
            return false;  // full
        }
        buffer_[static_cast<u64>(b) & mask_].store(idx, std::memory_order_relaxed);
        // Release-store on bottom publishes the buffer write to thieves
        // that acquire-load bottom (the matching synchronisation point).
        bottom_.store(b + 1, std::memory_order_release);
        return true;
    }

    // Owner-only. Pops from the bottom (LIFO order, hot for cache).
    PopResult try_pop() noexcept {
        i64 b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        i64 t = top_.load(std::memory_order_relaxed);
        if (t <= b) {
            // Non-empty.
            u32 idx = buffer_[static_cast<u64>(b) & mask_].load(
                std::memory_order_relaxed);
            if (t == b) {
                // Last element — race with a thief.
                i64 expected = t;
                if (!top_.compare_exchange_strong(expected, t + 1,
                                                  std::memory_order_seq_cst,
                                                  std::memory_order_relaxed)) {
                    // Lost the race — thief got it.
                    bottom_.store(t + 1, std::memory_order_relaxed);
                    return {0, Status::Empty};
                }
                bottom_.store(t + 1, std::memory_order_relaxed);
            }
            return {idx, Status::Ok};
        }
        // Empty — restore bottom and report.
        bottom_.store(t, std::memory_order_relaxed);
        return {0, Status::Empty};
    }

    // Thief-side. Returns Ok with a job, Empty (definitely no work), or
    // Aborted (race lost, caller should retry).
    PopResult try_steal() noexcept {
        i64 t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        i64 b = bottom_.load(std::memory_order_acquire);
        if (t < b) {
            u32 idx = buffer_[static_cast<u64>(t) & mask_].load(
                std::memory_order_relaxed);
            i64 expected = t;
            if (!top_.compare_exchange_strong(expected, t + 1,
                                              std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                return {0, Status::Aborted};
            }
            return {idx, Status::Ok};
        }
        return {0, Status::Empty};
    }

    // Snapshot — owner can read this without synchronisation as an estimate.
    i64 size_estimate() const noexcept {
        i64 b = bottom_.load(std::memory_order_relaxed);
        i64 t = top_.load(std::memory_order_relaxed);
        return b - t;
    }

  private:
    // bottom_ and top_ get their own cache line to avoid false sharing
    // between owner (writes bottom) and thieves (write top).
    alignas(kCacheLine) std::atomic<i64> top_{0};
    alignas(kCacheLine) std::atomic<i64> bottom_{0};
    std::atomic<u32>* buffer_   = nullptr;
    u32               capacity_ = 0;
    u64               mask_     = 0;
};

}  // namespace psynder::jobs::detail
