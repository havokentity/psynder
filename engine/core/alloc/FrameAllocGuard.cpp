// SPDX-License-Identifier: MIT
// Psynder — frame heap-allocation guard / telemetry implementation.

#include "FrameAllocGuard.h"

#include <array>
#include <atomic>

namespace psynder::mem {

namespace {

constexpr usize kMaxScopes = 128;
constexpr u32 kInvalidSlot = 0xFFFF'FFFFu;
constexpr u32 kMaxScopeDepth = 16;

static_assert((kMaxScopes & (kMaxScopes - 1)) == 0, "scope table must be power-of-two");

struct alignas(kCacheLine) ScopeSlot {
    std::atomic<u32> id{0};
    std::atomic<const char*> name{nullptr};
    std::atomic<u64> alloc_count{0};
    std::atomic<u64> free_count{0};
    std::atomic<u64> alloc_bytes{0};
    std::atomic<u64> free_bytes{0};
};

std::array<ScopeSlot, kMaxScopes>& scope_slots() {
    static std::array<ScopeSlot, kMaxScopes> slots;
    return slots;
}

std::atomic<bool>& guard_enabled() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

thread_local u32 tls_scope_slots[kMaxScopeDepth];
thread_local u32 tls_scope_depth = 0;

FrameAllocStats load_stats(const ScopeSlot& slot) noexcept {
    FrameAllocStats s;
    s.alloc_count = slot.alloc_count.load(std::memory_order_relaxed);
    s.free_count = slot.free_count.load(std::memory_order_relaxed);
    s.alloc_bytes = slot.alloc_bytes.load(std::memory_order_relaxed);
    s.free_bytes = slot.free_bytes.load(std::memory_order_relaxed);
    return s;
}

FrameAllocStats diff_stats(FrameAllocStats end, FrameAllocStats start) noexcept {
    end.alloc_count -= start.alloc_count;
    end.free_count -= start.free_count;
    end.alloc_bytes -= start.alloc_bytes;
    end.free_bytes -= start.free_bytes;
    return end;
}

u32 find_slot(FrameAllocScopeId id) noexcept {
    if (!id.valid())
        return kInvalidSlot;

    auto& slots = scope_slots();
    const u32 base = id.value & static_cast<u32>(kMaxScopes - 1);
    for (u32 probe = 0; probe < kMaxScopes; ++probe) {
        const u32 idx = (base + probe) & static_cast<u32>(kMaxScopes - 1);
        const u32 current = slots[idx].id.load(std::memory_order_relaxed);
        if (current == id.value)
            return idx;
        if (current == 0)
            return kInvalidSlot;
    }
    return kInvalidSlot;
}

u32 find_or_claim_slot(FrameAllocScopeId id, const char* stable_name) noexcept {
    if (!id.valid())
        return kInvalidSlot;

    auto& slots = scope_slots();
    const u32 base = id.value & static_cast<u32>(kMaxScopes - 1);
    for (u32 probe = 0; probe < kMaxScopes; ++probe) {
        const u32 idx = (base + probe) & static_cast<u32>(kMaxScopes - 1);
        u32 current = slots[idx].id.load(std::memory_order_relaxed);
        if (current == id.value) {
            const char* expected = nullptr;
            slots[idx].name.compare_exchange_strong(expected, stable_name, std::memory_order_relaxed);
            return idx;
        }
        if (current == 0 &&
            slots[idx].id.compare_exchange_strong(current, id.value, std::memory_order_relaxed)) {
            slots[idx].name.store(stable_name, std::memory_order_relaxed);
            return idx;
        }
    }
    return kInvalidSlot;
}

void add_alloc_to_slot(u32 idx, usize bytes) noexcept {
    auto& slot = scope_slots()[idx];
    slot.alloc_count.fetch_add(1, std::memory_order_relaxed);
    slot.alloc_bytes.fetch_add(static_cast<u64>(bytes), std::memory_order_relaxed);
}

void add_free_to_slot(u32 idx, usize bytes) noexcept {
    auto& slot = scope_slots()[idx];
    slot.free_count.fetch_add(1, std::memory_order_relaxed);
    slot.free_bytes.fetch_add(static_cast<u64>(bytes), std::memory_order_relaxed);
}

void reset_slot(ScopeSlot& slot) noexcept {
    slot.alloc_count.store(0, std::memory_order_relaxed);
    slot.free_count.store(0, std::memory_order_relaxed);
    slot.alloc_bytes.store(0, std::memory_order_relaxed);
    slot.free_bytes.store(0, std::memory_order_relaxed);
}

}  // namespace

void frame_alloc_guard_set_enabled(bool enabled) noexcept {
    guard_enabled().store(enabled, std::memory_order_relaxed);
}

bool frame_alloc_guard_enabled() noexcept {
    return guard_enabled().load(std::memory_order_relaxed);
}

FrameAllocScopeToken begin_frame_alloc_scope(FrameAllocScopeId id, const char* stable_name) noexcept {
    FrameAllocScopeToken token;
    token.id = id;
    if (!frame_alloc_guard_enabled() || !id.valid() || tls_scope_depth >= kMaxScopeDepth)
        return token;

    const u32 slot = find_or_claim_slot(id, stable_name);
    if (slot == kInvalidSlot)
        return token;

    tls_scope_slots[tls_scope_depth++] = slot;
    token.slot = slot;
    token.depth = tls_scope_depth;
    token.active = true;
    token.start = load_stats(scope_slots()[slot]);
    return token;
}

FrameAllocStats end_frame_alloc_scope(FrameAllocScopeToken& token) noexcept {
    if (!token.active)
        return {};

    token.active = false;
    if (token.depth > 0 && tls_scope_depth >= token.depth) {
        tls_scope_depth = token.depth - 1;
    }

    return diff_stats(load_stats(scope_slots()[token.slot]), token.start);
}

FrameAllocStats frame_alloc_scope_totals(FrameAllocScopeId id) noexcept {
    const u32 slot = find_slot(id);
    return slot == kInvalidSlot ? FrameAllocStats{} : load_stats(scope_slots()[slot]);
}

FrameAllocScopeInfo frame_alloc_scope_info(FrameAllocScopeId id) noexcept {
    FrameAllocScopeInfo info;
    info.id = id;
    const u32 slot = find_slot(id);
    if (slot == kInvalidSlot)
        return info;
    const auto& s = scope_slots()[slot];
    info.name = s.name.load(std::memory_order_relaxed);
    info.totals = load_stats(s);
    return info;
}

void frame_alloc_guard_reset(FrameAllocScopeId id) noexcept {
    const u32 slot = find_slot(id);
    if (slot != kInvalidSlot)
        reset_slot(scope_slots()[slot]);
}

void frame_alloc_guard_reset_all() noexcept {
    for (auto& slot : scope_slots()) {
        reset_slot(slot);
    }
}

void frame_alloc_guard_record_alloc(usize bytes) noexcept {
    if (PSY_LIKELY(!frame_alloc_guard_enabled()) || tls_scope_depth == 0)
        return;

    for (u32 i = 0; i < tls_scope_depth; ++i) {
        add_alloc_to_slot(tls_scope_slots[i], bytes);
    }
}

void frame_alloc_guard_record_free(usize bytes) noexcept {
    if (PSY_LIKELY(!frame_alloc_guard_enabled()) || tls_scope_depth == 0)
        return;

    for (u32 i = 0; i < tls_scope_depth; ++i) {
        add_free_to_slot(tls_scope_slots[i], bytes);
    }
}

}  // namespace psynder::mem
