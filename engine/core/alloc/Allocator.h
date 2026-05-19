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
    LinearArena(const LinearArena&)            = delete;
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

    void  reset() noexcept;
    usize used() const noexcept;
    usize capacity() const noexcept;

private:
    u8*  base_     = nullptr;
    u8*  head_     = nullptr;
    usize cap_     = 0;
    Tag  tag_      = Tag::Misc;
    bool owns_     = false;
};

// ─── TypedPool<T> — fixed-size free-list pool ───────────────────────────
template <class T>
class TypedPool {
public:
    void  init(void* base, usize element_count) noexcept;
    T*    acquire() noexcept;
    void  release(T* p) noexcept;
    void  reset() noexcept;
    usize live() const noexcept;
private:
    T*    storage_ = nullptr;
    u32*  free_    = nullptr;
    usize cap_     = 0;
    usize next_    = 0;
};

// ─── PageAllocator — OS-mediated, hugepages where supported ──────────────
struct PageBlock {
    void* ptr   = nullptr;
    usize bytes = 0;
};
PageBlock page_alloc(usize bytes, bool prefer_hugepage = true);
void      page_free(PageBlock block);

// ─── Per-worker scratch — call after the job system inits ────────────────
LinearArena& worker_scratch();    // current worker's per-job arena

// ─── Per-frame scratch — reset each frame boundary by the engine ─────────
LinearArena& frame_scratch();

// ─── Budgets / tracking ──────────────────────────────────────────────────
void   set_budget(Tag tag, usize bytes);
usize  current_usage(Tag tag) noexcept;
usize  peak_usage(Tag tag) noexcept;

}  // namespace psynder::mem
