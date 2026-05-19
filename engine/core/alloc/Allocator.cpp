// SPDX-License-Identifier: MIT
// Psynder — allocator impl. Lane 01 owns the real implementations
// (hugepage-aware page allocator, NUMA binding, per-worker arenas, free-list
// pool, tracking). Phase-0 stub provides enough for sample_00 to link.

#include "Allocator.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace psynder::mem {

// ─── LinearArena ─────────────────────────────────────────────────────────
LinearArena::LinearArena(void* base, usize bytes, Tag tag) noexcept
    : base_(static_cast<u8*>(base)), head_(static_cast<u8*>(base)),
      cap_(bytes), tag_(tag), owns_(false) {}

LinearArena::~LinearArena() {
    if (owns_ && base_) std::free(base_);
}

LinearArena::LinearArena(LinearArena&& o) noexcept
    : base_(o.base_), head_(o.head_), cap_(o.cap_), tag_(o.tag_), owns_(o.owns_) {
    o.base_ = nullptr;
    o.head_ = nullptr;
    o.cap_  = 0;
    o.owns_ = false;
}

LinearArena& LinearArena::operator=(LinearArena&& o) noexcept {
    if (this != &o) {
        if (owns_ && base_) std::free(base_);
        base_ = o.base_;
        head_ = o.head_;
        cap_  = o.cap_;
        tag_  = o.tag_;
        owns_ = o.owns_;
        o.base_ = nullptr;
        o.head_ = nullptr;
        o.cap_  = 0;
        o.owns_ = false;
    }
    return *this;
}

void* LinearArena::alloc(usize bytes, usize align) noexcept {
    if (!base_) return nullptr;
    auto cur     = reinterpret_cast<std::uintptr_t>(head_);
    auto aligned = (cur + align - 1) & ~static_cast<std::uintptr_t>(align - 1);
    auto next    = aligned + bytes;
    if (next > reinterpret_cast<std::uintptr_t>(base_) + cap_) return nullptr;
    head_ = reinterpret_cast<u8*>(next);
    return reinterpret_cast<void*>(aligned);
}

void  LinearArena::reset() noexcept   { head_ = base_; }
usize LinearArena::used() const noexcept     { return static_cast<usize>(head_ - base_); }
usize LinearArena::capacity() const noexcept { return cap_; }

// ─── Page allocator (Phase-0 fallback to plain malloc; lane 01 swaps) ────
PageBlock page_alloc(usize bytes, bool /*prefer_hugepage*/) {
    void* p = std::aligned_alloc(kPage, ((bytes + kPage - 1) / kPage) * kPage);
    if (!p) return {};
    std::memset(p, 0, ((bytes + kPage - 1) / kPage) * kPage);
    return {p, ((bytes + kPage - 1) / kPage) * kPage};
}

void page_free(PageBlock block) {
    if (block.ptr) std::free(block.ptr);
}

// ─── Per-worker / per-frame scratch (single-threaded stubs in Phase 0) ──
namespace {
LinearArena& global_default_arena() {
    static u8 storage[1 << 20];  // 1 MiB scratch
    static LinearArena arena(storage, sizeof storage, Tag::Misc);
    return arena;
}
}  // namespace

LinearArena& worker_scratch() { return global_default_arena(); }
LinearArena& frame_scratch()  { return global_default_arena(); }

// ─── Budgets / usage tracking (no-op in Phase 0) ─────────────────────────
namespace {
struct Counters {
    std::atomic<usize> current{0};
    std::atomic<usize> peak{0};
    std::atomic<usize> budget{0};
};
Counters g_counters[static_cast<usize>(Tag::Count)];
}  // namespace

void set_budget(Tag tag, usize bytes) {
    g_counters[static_cast<usize>(tag)].budget.store(bytes, std::memory_order_relaxed);
}
usize current_usage(Tag tag) noexcept {
    return g_counters[static_cast<usize>(tag)].current.load(std::memory_order_relaxed);
}
usize peak_usage(Tag tag) noexcept {
    return g_counters[static_cast<usize>(tag)].peak.load(std::memory_order_relaxed);
}

// Explicit instantiations for the common TypedPool<T>s would go here;
// lane 01 will template-implement these in the header or per-archetype.

}  // namespace psynder::mem
