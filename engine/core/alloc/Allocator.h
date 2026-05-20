// SPDX-License-Identifier: MIT
// Psynder — memory subsystem public API. See DESIGN.md §4.
//
// Lifetime-scoped allocators: LinearArena (frame/job), StackArena (LIFO),
// TypedPool<T> (level), BuddyAllocator (level), PageAllocator (app).
// Lane 01 owns the full implementation; this header freezes the API the
// rest of the engine codes against.

#pragma once

#include "../Types.h"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>  // std::forward — libc++ pulls it in transitively, libstdc++/MSVC do not

namespace psynder::mem {

enum class Tag : u32 {
    Render,
    Physics,
    Audio,
    Ecs,
    Asset,
    Streaming,
    Scripts,
    Tools,
    Misc,
    Count,
};

// ─── LinearArena — bump-pointer, O(1) reset ──────────────────────────────
class LinearArena {
   public:
    LinearArena() = default;
    LinearArena(void* base, usize bytes, Tag tag = Tag::Misc) noexcept;
    ~LinearArena();
    LinearArena(const LinearArena&) = delete;
    LinearArena& operator=(const LinearArena&) = delete;
    LinearArena(LinearArena&&) noexcept;
    LinearArena& operator=(LinearArena&&) noexcept;

    void* alloc(usize bytes, usize align = alignof(std::max_align_t)) noexcept;

    template <class T, class... Args>
    T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "arena objects must be trivially destructible");
        void* p = alloc(sizeof(T), alignof(T));
        return p ? new (p) T(std::forward<Args>(args)...) : nullptr;
    }

    void reset() noexcept;
    usize used() const noexcept;
    usize capacity() const noexcept;

   private:
    u8* base_ = nullptr;
    u8* head_ = nullptr;
    usize cap_ = 0;
    Tag tag_ = Tag::Misc;
    bool owns_ = false;
};

// ─── TypedPool<T> — fixed-size free-list pool ───────────────────────────
// Implementation lives in the header so any TU can instantiate without a
// per-type explicit-instantiation dance in Allocator.cpp. `init` carves a
// backing region into `element_count` slots and threads a free-list index
// vector through it. `acquire` pops the next free index; `release` pushes
// it back. The free-list is a u32-per-slot side table (kept separate from
// the T storage so T can be arbitrarily aligned without bloating the
// indices). All operations are O(1) and single-threaded -- callers are
// expected to bind one pool per worker (DESIGN.md §4.2 "per-worker first,
// shared last"), or wrap with their own lock if cross-thread is needed.
//
// Backing layout: caller passes a single `base` buffer big enough to hold
// `element_count` * (sizeof(T) + sizeof(u32)). Storage lives at base;
// the free-list index table lives immediately after, naturally aligned.
template <class T>
class TypedPool {
   public:
    static constexpr u32 kInvalidIndex = 0xFFFF'FFFFu;

    void init(void* base, usize element_count) noexcept {
        storage_ = static_cast<T*>(base);
        // The free-list index table follows the storage, padded to alignof(u32).
        std::uintptr_t after_storage =
            reinterpret_cast<std::uintptr_t>(storage_) + sizeof(T) * element_count;
        std::uintptr_t aligned =
            (after_storage + alignof(u32) - 1) & ~static_cast<std::uintptr_t>(alignof(u32) - 1);
        free_ = reinterpret_cast<u32*>(aligned);
        cap_ = element_count;
        next_ = 0;
        live_ = 0;
        // Lazy free-list seed: slots [0..cap_) get linked on demand inside
        // acquire() so init() stays O(1) regardless of capacity.
        first_free_ = kInvalidIndex;
    }

    T* acquire() noexcept {
        if (live_ >= cap_)
            return nullptr;
        u32 idx;
        if (first_free_ != kInvalidIndex) {
            idx = first_free_;
            first_free_ = free_[idx];
        } else {
            idx = static_cast<u32>(next_++);
        }
        ++live_;
        return storage_ + idx;
    }

    void release(T* p) noexcept {
        if (!p || !storage_)
            return;
        u32 idx = static_cast<u32>(p - storage_);
        if (idx >= cap_)
            return;  // not from this pool; ignore.
        free_[idx] = first_free_;
        first_free_ = idx;
        if (live_ > 0)
            --live_;
    }

    void reset() noexcept {
        next_ = 0;
        live_ = 0;
        first_free_ = kInvalidIndex;
    }

    usize live() const noexcept { return live_; }
    usize capacity() const noexcept { return cap_; }

   private:
    T* storage_ = nullptr;
    u32* free_ = nullptr;
    usize cap_ = 0;
    usize next_ = 0;
    usize live_ = 0;
    u32 first_free_ = kInvalidIndex;
};

// ─── PageAllocator — OS-mediated, hugepages where supported ──────────────
struct PageBlock {
    void* ptr = nullptr;
    usize bytes = 0;
};
PageBlock page_alloc(usize bytes, bool prefer_hugepage = true);
void page_free(PageBlock block);

// ─── Per-worker scratch — call after the job system inits ────────────────
LinearArena& worker_scratch();  // current worker's per-job arena

// ─── Per-frame scratch — reset each frame boundary by the engine ─────────
LinearArena& frame_scratch();

// ─── Budgets / tracking ──────────────────────────────────────────────────
void set_budget(Tag tag, usize bytes);
usize current_usage(Tag tag) noexcept;
usize peak_usage(Tag tag) noexcept;

}  // namespace psynder::mem
