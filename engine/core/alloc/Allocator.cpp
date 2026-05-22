// SPDX-License-Identifier: MIT
// Psynder — memory subsystem implementation. See DESIGN.md §4.
//
// What this file provides:
//   - PageAllocator: OS-mediated page-aligned blocks. mmap / VirtualAlloc /
//     mach_vm_allocate. Hugepage hints are opt-in
//     (MADV_HUGEPAGE / MEM_LARGE_PAGES / VM_FLAGS_SUPERPAGE_SIZE_2MB).
//     Falls back to plain aligned alloc when the OS rejects the request.
//   - LinearArena: bump-pointer arena. O(1) reset. Pads its own counters to
//     a cache line so per-worker arenas don't false-share the head pointer.
//   - BuddyAllocator: power-of-two split / merge allocator with a free-list
//     per order. Coarse but correct; lightmap atlas + BVH nodes is the only
//     hot consumer in M3, and it's a cold-path consumer at that.
//   - TypedPool<T>: the template body lives in Allocator.h (header-only so
//     any TU can instantiate); see the comments there.
//   - Per-worker scratch + frame scratch arenas: thread-local storage with
//     lazy backing-page allocation. The lane-04 job system wires
//     `worker_scratch()` to the calling worker's slot at thread-startup; for
//     non-worker threads we keep a thread_local fallback so callers from
//     the main thread don't crash.
//   - Per-tag usage counters: lock-free relaxed atomics. We track current,
//     peak, and budget per tag; the editor heatmap reads these every frame.

#include "Allocator.h"
#include "FlightRecorder.h"
#include "FrameAllocGuard.h"
#include "Heatmap.h"

#include "../Tracy.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace psynder::mem {

namespace {

// ─── Per-tag usage tracking ──────────────────────────────────────────────
// One Counters struct per Tag value, padded to a cache line so updates from
// different workers don't ping each other. Tracked counters are advisory --
// allocators report into them but don't gate on them; the editor heatmap
// reads them, the bench gate fails the build past 1.5x budget (lane 25).
struct alignas(kCacheLine) Counters {
    std::atomic<usize> current{0};
    std::atomic<usize> peak{0};
    std::atomic<usize> budget{0};
};
Counters g_counters[static_cast<usize>(Tag::Count)];

PSY_FORCEINLINE void account_add(Tag tag, usize bytes) {
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return;
    usize cur = g_counters[idx].current.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    // Bump the peak if we just crossed it (best-effort, racy by design).
    usize peak = g_counters[idx].peak.load(std::memory_order_relaxed);
    while (cur > peak &&
           !g_counters[idx].peak.compare_exchange_weak(peak, cur, std::memory_order_relaxed)) {}
}

PSY_FORCEINLINE void account_sub(Tag tag, usize bytes) {
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return;
    g_counters[idx].current.fetch_sub(bytes, std::memory_order_relaxed);
}

PSY_FORCEINLINE usize round_up(usize value, usize align) {
    return (value + align - 1) & ~(align - 1);
}

PSY_FORCEINLINE PageBlock record_page_alloc(PageBlock block) noexcept {
    if (block.ptr && block.bytes)
        frame_alloc_guard_record_alloc(block.bytes);
    return block;
}

}  // namespace

// ─── LinearArena ─────────────────────────────────────────────────────────
// Constructor that takes an externally-managed backing region. We don't own
// the bytes here -- the caller (page allocator, static buffer, owning arena
// from a higher scope) is responsible for keeping the region alive longer
// than this arena.
LinearArena::LinearArena(void* base, usize bytes, Tag tag) noexcept
    : base_(static_cast<u8*>(base))
    , head_(static_cast<u8*>(base))
    , cap_(bytes)
    , tag_(tag)
    , owns_(false) {}

LinearArena::~LinearArena() {
    // owns_ is reserved for a future owning-construct path; the current
    // public constructor is non-owning, so the destructor is a no-op in
    // practice. Kept defensive so a future internal owning constructor
    // hooked through the page allocator releases cleanly.
    if (owns_ && base_) {
        account_sub(tag_, cap_);
        page_free({base_, cap_});
    }
}

LinearArena::LinearArena(LinearArena&& o) noexcept
    : base_(o.base_), head_(o.head_), cap_(o.cap_), tag_(o.tag_), owns_(o.owns_) {
    o.base_ = nullptr;
    o.head_ = nullptr;
    o.cap_ = 0;
    o.owns_ = false;
}

LinearArena& LinearArena::operator=(LinearArena&& o) noexcept {
    if (this != &o) {
        if (owns_ && base_) {
            account_sub(tag_, cap_);
            page_free({base_, cap_});
        }
        base_ = o.base_;
        head_ = o.head_;
        cap_ = o.cap_;
        tag_ = o.tag_;
        owns_ = o.owns_;
        o.base_ = nullptr;
        o.head_ = nullptr;
        o.cap_ = 0;
        o.owns_ = false;
    }
    return *this;
}

void* LinearArena::alloc(usize bytes, usize align) noexcept {
    PSY_TRACE_ZONE_COLOR("mem.LinearArena.alloc", 0x4E9A06u);
    if (!base_ || bytes == 0)
        return nullptr;
    // Power-of-two alignment is a precondition; arenas don't try to handle
    // weird alignments. The bump is unconditional, so misaligned input
    // would silently over-allocate.
    auto cur = reinterpret_cast<std::uintptr_t>(head_);
    auto aligned = (cur + align - 1) & ~static_cast<std::uintptr_t>(align - 1);
    auto next = aligned + bytes;
    if (next > reinterpret_cast<std::uintptr_t>(base_) + cap_)
        return nullptr;
    head_ = reinterpret_cast<u8*>(next);
    // Account the actual consumed-from-arena bytes (alignment hole included),
    // and feed the flight recorder. Both are advisory and lock-free.
    const usize consumed = static_cast<usize>(next - cur);
    account_add(tag_, consumed);
    PSY_FLIGHT_RECORD(+1, bytes, tag_);
    PSY_ZONE_ALLOC(reinterpret_cast<void*>(aligned), bytes);
    return reinterpret_cast<void*>(aligned);
}

void LinearArena::reset() noexcept {
    // Return the arena's used bytes back to the per-tag counters so the
    // heatmap reflects "outstanding from this arena" rather than "ever
    // bumped through this arena". The lifetime peak is untouched on
    // purpose: that's what the bench gate wants to see.
    if (head_ > base_) {
        account_sub(tag_, static_cast<usize>(head_ - base_));
    }
    head_ = base_;
}
usize LinearArena::used() const noexcept {
    return static_cast<usize>(head_ - base_);
}
usize LinearArena::capacity() const noexcept {
    return cap_;
}

// ─── PageAllocator ───────────────────────────────────────────────────────
// Implements the OS-mediated page-aligned reservations the rest of the
// allocator hierarchy is built on. Hugepage placement is best-effort: every
// platform takes a different incantation, and every platform can refuse
// (privilege, fragmentation, sysctl gate, transparent-hugepage off). On
// refusal we silently fall back to the regular-page path -- this is a
// performance hint, not a correctness requirement.
//
// We return the actual byte count we reserved (rounded up to the platform's
// page granularity) so callers can pass the same byte count to page_free.
PageBlock page_alloc(usize bytes, bool prefer_hugepage) {
    PSY_TRACE_ZONE_COLOR("mem.page_alloc", 0xC4A000u);
    if (bytes == 0)
        return {};
    // Record the request up front. The flight recorder tracks request
    // sizes; the asymmetric page_free path records the actual returned
    // bytes (post-rounding), which is what we care about for leak
    // triage.
    PSY_FLIGHT_RECORD(+1, bytes, Tag::Misc);

    // Round up to the (huge)page boundary depending on whether we're trying
    // for hugepages. Hugepage rounding makes the OS hint more likely to
    // succeed; the regular-page path also rounds up so munmap/free can
    // unmap the full reservation cleanly.
    const usize page_size = prefer_hugepage ? kHugePage : kPage;
    const usize aligned = round_up(bytes, page_size);

#if defined(_WIN32)
    // Windows: VirtualAlloc with MEM_LARGE_PAGES requires the
    // SeLockMemoryPrivilege, which is not granted by default. We try it
    // first and fall back to regular pages on failure -- common in dev
    // boxes that haven't enabled the privilege.
    if (prefer_hugepage) {
        SIZE_T large = GetLargePageMinimum();
        if (large > 0) {
            const usize large_aligned = round_up(bytes, large);
            void* p = VirtualAlloc(nullptr,
                                   large_aligned,
                                   MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
                                   PAGE_READWRITE);
            if (p)
                return record_page_alloc({p, large_aligned});
        }
    }
    void* p = VirtualAlloc(nullptr, aligned, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p)
        return {};
    // VirtualAlloc zero-fills committed pages on Windows; no memset needed.
    return record_page_alloc({p, aligned});

#elif defined(__APPLE__)
    // macOS: mach_vm_allocate with VM_FLAGS_SUPERPAGE_SIZE_2MB when the
    // reservation is at least 2 MiB AND the request crossed the kHugePage
    // threshold (so the kernel will actually grant the superpage). Small
    // reservations fall through to mmap so we don't waste pages.
    //
    // Note: the SUPERPAGE flag is x86_64 only on macOS -- arm64 kernels
    // ignore it. We still set it (kernel just drops it on arm64) and
    // separately call madvise(...) below for arm64-aware hugepage hints.
    // Apple Silicon kernels do hugepage placement opportunistically when
    // the VM region is large enough, so the rounding-to-2MB above is what
    // actually matters in practice.
    if (prefer_hugepage && aligned >= kHugePage) {
        mach_vm_address_t addr = 0;
        int flags = VM_FLAGS_ANYWHERE;
#if defined(VM_FLAGS_SUPERPAGE_SIZE_2MB)
        flags |= VM_FLAGS_SUPERPAGE_SIZE_2MB;
#endif
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, aligned, flags);
        if (kr == KERN_SUCCESS) {
            // mach_vm_allocate zero-fills.
            return record_page_alloc({reinterpret_cast<void*>(addr), aligned});
        }
        // Fall through to mmap on superpage refusal.
    }
    void* p = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return {};
    return record_page_alloc({p, aligned});

#else
    // Linux: mmap + madvise(MADV_HUGEPAGE) when caller asked for hugepages
    // and the kernel has transparent-hugepages on. madvise failure is
    // ignored -- the mapping still works, the kernel just won't try to
    // back it with hugepages.
    void* p = mmap(nullptr, aligned, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return {};
#if defined(MADV_HUGEPAGE)
    if (prefer_hugepage && aligned >= kHugePage) {
        (void)madvise(p, aligned, MADV_HUGEPAGE);
    }
#endif
    return record_page_alloc({p, aligned});
#endif
}

void page_free(PageBlock block) {
    PSY_TRACE_ZONE_COLOR("mem.page_free", 0xC4A000u);
    if (!block.ptr || block.bytes == 0)
        return;
    PSY_FLIGHT_RECORD(-1, block.bytes, Tag::Misc);
    frame_alloc_guard_record_free(block.bytes);
    PSY_ZONE_FREE(block.ptr);
#if defined(_WIN32)
    VirtualFree(block.ptr, 0, MEM_RELEASE);
#elif defined(__APPLE__)
    mach_vm_deallocate(mach_task_self(), reinterpret_cast<mach_vm_address_t>(block.ptr), block.bytes);
#else
    munmap(block.ptr, block.bytes);
#endif
}

// ─── BuddyAllocator ──────────────────────────────────────────────────────
// Power-of-two split / merge buddy. We hold a free-list per order; allocate
// walks up to the first non-empty order with a block big enough, splits
// down to the requested order, returns the lower half. Free finds the
// buddy by xor on the block-offset; merges recursively up if the buddy is
// also free.
//
// The allocator state lives in this TU (file-static linkage); the public
// API is page_alloc/page_free at the moment, but in M3 the lightmap atlas
// + BVH node pool will instantiate a BuddyAllocator via the worker_scratch
// hand-off. We expose a tiny C-style facade (buddy_init / buddy_alloc /
// buddy_free) below for the future consumer; the test (core_alloc.cpp)
// exercises it directly.

namespace {

constexpr usize kBuddyMinOrder = 6;   // 64-byte minimum block = one cache line.
constexpr usize kBuddyMaxOrder = 32;  // 4 GiB upper bound -- plenty of headroom.

inline usize order_for(usize bytes) {
    usize order = kBuddyMinOrder;
    usize size = 1ULL << order;
    while (size < bytes && order < kBuddyMaxOrder) {
        ++order;
        size <<= 1;
    }
    return order;
}

}  // namespace

class BuddyAllocator {
   public:
    void init(void* base, usize bytes) noexcept {
        std::memset(free_heads_, 0, sizeof(free_heads_));
        base_ = static_cast<u8*>(base);
        // Total size must be a power of two for the buddy invariant; clamp
        // down to the next power-of-two that fits in `bytes`. The caller
        // typically passes a page_alloc()-shaped reservation so this is a
        // no-op (page_alloc already rounds up to kPage / kHugePage).
        usize size = 1;
        while ((size << 1) <= bytes)
            size <<= 1;
        cap_ = size;
        order_ = 0;
        while ((1ULL << order_) < cap_)
            ++order_;
        // Seed: the whole region is one free block at the top order.
        push_free(order_, base_);
    }

    void* alloc(usize bytes) noexcept {
        if (!base_ || bytes == 0)
            return nullptr;
        const usize want = order_for(bytes);
        if (want > order_)
            return nullptr;
        // Walk up to find a non-empty bucket, then split down.
        usize have = want;
        while (have <= order_ && free_heads_[have] == nullptr)
            ++have;
        if (have > order_)
            return nullptr;
        u8* block = pop_free(have);
        while (have > want) {
            --have;
            // Buddy = block + 2^have. Push it on the free list of the
            // smaller order so the other half can be allocated next time.
            push_free(have, block + (1ULL << have));
        }
        return block;
    }

    void free(void* p, usize bytes) noexcept {
        if (!p || bytes == 0)
            return;
        usize have = order_for(bytes);
        u8* block = static_cast<u8*>(p);
        // Try to merge with our buddy. Stop when the buddy isn't on the
        // free list (i.e. someone else owns it) or we hit the top order.
        while (have < order_) {
            const std::uintptr_t off = static_cast<std::uintptr_t>(block - base_);
            const std::uintptr_t buddy_off = off ^ (1ULL << have);
            u8* buddy = base_ + buddy_off;
            if (!remove_free(have, buddy))
                break;
            // Merge: the lower of the two addresses becomes the merged
            // block one order up.
            if (buddy < block)
                block = buddy;
            ++have;
        }
        push_free(have, block);
    }

   private:
    struct Node {
        Node* next;
    };

    void push_free(usize order, u8* p) noexcept {
        auto* n = reinterpret_cast<Node*>(p);
        n->next = free_heads_[order];
        free_heads_[order] = n;
    }

    u8* pop_free(usize order) noexcept {
        Node* n = free_heads_[order];
        free_heads_[order] = n ? n->next : nullptr;
        return reinterpret_cast<u8*>(n);
    }

    bool remove_free(usize order, u8* p) noexcept {
        Node** cur = &free_heads_[order];
        while (*cur) {
            if (reinterpret_cast<u8*>(*cur) == p) {
                *cur = (*cur)->next;
                return true;
            }
            cur = &(*cur)->next;
        }
        return false;
    }

    u8* base_ = nullptr;
    usize cap_ = 0;
    usize order_ = 0;
    Node* free_heads_[kBuddyMaxOrder + 1] = {nullptr};
};

// C-style facade for the buddy allocator -- the existing public header
// doesn't expose BuddyAllocator yet (frozen), so we expose buddy_{init,
// alloc,free} as a free-function pair through a hidden internal namespace
// that the unit test reaches into via extern "C++" declarations. This is
// the minimum surface that lets the unit test exercise the implementation
// without changing the public header.
namespace detail {

BuddyAllocator& buddy_test_singleton() {
    static BuddyAllocator b;
    return b;
}

void buddy_init(void* base, usize bytes) noexcept {
    buddy_test_singleton().init(base, bytes);
}
void* buddy_alloc(usize bytes) noexcept {
    return buddy_test_singleton().alloc(bytes);
}
void buddy_free(void* p, usize bytes) noexcept {
    buddy_test_singleton().free(p, bytes);
}

}  // namespace detail

// ─── Per-worker / per-frame scratch ──────────────────────────────────────
// Worker scratch: every worker thread that asks for `worker_scratch()` is
// handed its own thread_local arena. The job system (lane 04) will call
// the bind hook below at thread-startup time to pre-allocate the arena
// from hugepages; until then we lazily allocate on first use so non-
// worker callers (main thread, tools) still get a usable arena.
//
// Frame scratch: a single process-global arena, reset at frame boundary by
// the engine (lane 04 / 06). We back it with a 4 MiB regular-page
// reservation -- big enough for draw cmd lists + vertex transforms +
// per-tile binning headers, small enough that the page allocator never
// has to think about hugepage refusal.
namespace {

struct LazyArena {
    LinearArena arena;
    PageBlock block;
    bool ready = false;
};

struct LazyArenaDeleter {
    PageBlock block;
    ~LazyArenaDeleter() {
        if (block.ptr)
            page_free(block);
    }
};

// thread_local LazyArena so each worker / main thread has its own slot.
// Destruction at thread-exit runs the deleter to release the page block.
// The arena itself is non-owning (`owns_=false`) so it doesn't try to
// double-free; the deleter is what actually returns pages to the OS.
thread_local LazyArena tls_worker_scratch;
thread_local LazyArenaDeleter tls_worker_scratch_deleter;

constexpr usize kWorkerScratchBytes = 1ULL << 20;  // 1 MiB
constexpr usize kFrameScratchBytes = 4ULL << 20;   // 4 MiB

void ensure_worker(LazyArena& la, LazyArenaDeleter& deleter) {
    if (la.ready)
        return;
    la.block = page_alloc(kWorkerScratchBytes, /*prefer_hugepage=*/false);
    if (!la.block.ptr)
        return;  // page allocator failed -- arena stays empty
    la.arena = LinearArena(la.block.ptr, la.block.bytes, Tag::Misc);
    la.ready = true;
    deleter.block = la.block;  // hand-off so thread-exit returns pages
    account_add(Tag::Misc, la.block.bytes);
}

LazyArena& frame_arena_slot() {
    static LazyArena f;
    static std::once_flag init;
    std::call_once(init, [&]() {
        f.block = page_alloc(kFrameScratchBytes, /*prefer_hugepage=*/false);
        if (f.block.ptr) {
            f.arena = LinearArena(f.block.ptr, f.block.bytes, Tag::Misc);
            f.ready = true;
            account_add(Tag::Misc, f.block.bytes);
        }
    });
    return f;
}

}  // namespace

LinearArena& worker_scratch() {
    ensure_worker(tls_worker_scratch, tls_worker_scratch_deleter);
    return tls_worker_scratch.arena;
}

LinearArena& frame_scratch() {
    auto& f = frame_arena_slot();
    return f.arena;
}

// ─── Budgets / usage tracking ────────────────────────────────────────────
void set_budget(Tag tag, usize bytes) {
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return;
    g_counters[idx].budget.store(bytes, std::memory_order_relaxed);
}

usize current_usage(Tag tag) noexcept {
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return 0;
    return g_counters[idx].current.load(std::memory_order_relaxed);
}

usize peak_usage(Tag tag) noexcept {
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return 0;
    return g_counters[idx].peak.load(std::memory_order_relaxed);
}

// ─── Heatmap (DESIGN.md §4.7 live heatmap + per-frame dump) ──────────────
// tag_stat / tag_stats are pure reads against the same atomics that back
// current_usage / peak_usage above; we just package the three values into
// one struct so the editor doesn't have to issue six separate calls per
// frame to render its bar chart. reset_peak_* clamp the lifetime peak
// down to the current value, used to drive the "peak since last frame"
// editor mode without touching the bench-gate lifetime view.
TagStat tag_stat(Tag tag) noexcept {
    TagStat s;
    s.tag = tag;
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return s;
    s.current = g_counters[idx].current.load(std::memory_order_relaxed);
    s.peak = g_counters[idx].peak.load(std::memory_order_relaxed);
    s.budget = g_counters[idx].budget.load(std::memory_order_relaxed);
    return s;
}

std::array<TagStat, static_cast<usize>(Tag::Count)> tag_stats() noexcept {
    std::array<TagStat, static_cast<usize>(Tag::Count)> out{};
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = tag_stat(static_cast<Tag>(i));
    }
    return out;
}

void reset_peak(Tag tag) noexcept {
    auto idx = static_cast<usize>(tag);
    if (idx >= static_cast<usize>(Tag::Count))
        return;
    // Clamp peak down to the current value. The slight race between the
    // current load and the peak store is fine: a peak slightly below the
    // true watermark just means the next bump from account_add() will
    // re-raise it, no correctness impact.
    usize cur = g_counters[idx].current.load(std::memory_order_relaxed);
    g_counters[idx].peak.store(cur, std::memory_order_relaxed);
}

void reset_peak_all() noexcept {
    for (usize i = 0; i < static_cast<usize>(Tag::Count); ++i) {
        reset_peak(static_cast<Tag>(i));
    }
}

// ─── Flight recorder ─────────────────────────────────────────────────────
// Lock-free MPSC-style ring: one global atomic serial counter, indexed
// into a fixed-size array. Writers fetch_add to claim a slot, then
// publish the entry; readers (dump) walk the serial range modulo
// capacity to emit oldest-to-newest. Capacity is set on first init
// (defaults to 1024 on first record if init wasn't called). The buffer
// lives in dynamically-allocated memory so capacity isn't a compile-time
// constant -- tests reconfigure it with smaller caps.
namespace {

constexpr usize kFlightDefaultCap = 1024;
constexpr usize kFlightMaxCap = 1 << 16;  // 64K entries upper bound

struct FlightRing {
    FlightEntry* buf = nullptr;
    std::atomic<u64> serial{0};
    usize capacity = 0;
    usize mask = 0;
};

FlightRing& flight_ring() {
    static FlightRing r;
    return r;
}

std::mutex& flight_init_mutex() {
    static std::mutex m;
    return m;
}

usize round_down_pow2(usize v) {
    if (v == 0)
        return 0;
    usize r = 1;
    while ((r << 1) <= v && (r << 1) != 0)
        r <<= 1;
    return r;
}

void flight_init_locked(usize capacity) {
    auto& r = flight_ring();
    if (r.buf)
        return;  // already initialised
    if (capacity == 0)
        capacity = kFlightDefaultCap;
    if (capacity > kFlightMaxCap)
        capacity = kFlightMaxCap;
    capacity = round_down_pow2(capacity);
    if (capacity == 0)
        capacity = kFlightDefaultCap;
    // PageAllocator is overkill for a 32 KiB buffer; std::malloc is fine
    // here because the recorder is *outside* the engine's hot frame loop
    // (init-time only, debug-only consumer).
    r.buf = static_cast<FlightEntry*>(std::calloc(capacity, sizeof(FlightEntry)));
    r.capacity = capacity;
    r.mask = capacity - 1;
    r.serial.store(0, std::memory_order_relaxed);
}

}  // namespace

void flight_recorder_init(usize capacity) noexcept {
    std::lock_guard<std::mutex> g(flight_init_mutex());
    auto& r = flight_ring();
    if (r.buf) {
        std::free(r.buf);
        r.buf = nullptr;
        r.capacity = 0;
        r.mask = 0;
    }
    flight_init_locked(capacity);
}

void flight_recorder_record(u32 site, i32 op, u32 size, Tag tag) noexcept {
    auto& r = flight_ring();
    if (!r.buf) {
        std::lock_guard<std::mutex> g(flight_init_mutex());
        flight_init_locked(kFlightDefaultCap);
    }
    // After the (possible) init above, r.buf / r.mask are stable for the
    // lifetime of the recorder (init clears and rebuilds atomically under
    // the init mutex; we accept that a record racing with re-init may
    // land in the old buffer just before it's freed -- the test path
    // doesn't re-init while recording, and the engine boot path calls
    // init() once at startup before any allocator is hit).
    const u64 serial = r.serial.fetch_add(1, std::memory_order_relaxed);
    const usize slot = static_cast<usize>(serial) & r.mask;
    FlightEntry e;
    e.serial = serial;
    e.site = site;
    e.op = op;
    e.size = size;
    e.tag = tag;
    r.buf[slot] = e;
}

u64 flight_recorder_count() noexcept {
    return flight_ring().serial.load(std::memory_order_relaxed);
}

usize flight_recorder_capacity() noexcept {
    return flight_ring().capacity;
}

void flight_recorder_clear() noexcept {
    std::lock_guard<std::mutex> g(flight_init_mutex());
    auto& r = flight_ring();
    if (r.buf && r.capacity) {
        std::memset(r.buf, 0, r.capacity * sizeof(FlightEntry));
    }
    r.serial.store(0, std::memory_order_relaxed);
}

usize flight_recorder_dump(const char* path) noexcept {
    if (!path)
        return 0;
    auto& r = flight_ring();
    if (!r.buf || r.capacity == 0)
        return 0;
    std::FILE* f = std::fopen(path, "w");
    if (!f)
        return 0;
    const u64 total = r.serial.load(std::memory_order_relaxed);
    const u64 start = (total > r.capacity) ? (total - r.capacity) : 0;
    const u64 start_idx = start & r.mask;
    usize written = 0;
    std::fprintf(f, "# psynder flight recorder dump\n");
    std::fprintf(f, "# serial,site,op,size,tag\n");
    for (u64 s = start; s < total; ++s) {
        const usize slot = static_cast<usize>(s) & r.mask;
        const FlightEntry& e = r.buf[slot];
        // Defensive: only emit entries whose serial actually matches.
        // If we raced with a writer mid-record this filters out stale
        // half-overwritten slots from earlier wraps.
        if (e.serial != s)
            continue;
        std::fprintf(f,
                     "%llu,%u,%d,%u,%u\n",
                     static_cast<unsigned long long>(e.serial),
                     e.site,
                     e.op,
                     e.size,
                     static_cast<unsigned>(e.tag));
        ++written;
    }
    (void)start_idx;
    std::fclose(f);
    return written;
}

}  // namespace psynder::mem
