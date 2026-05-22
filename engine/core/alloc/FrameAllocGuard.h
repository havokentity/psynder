// SPDX-License-Identifier: MIT
// Psynder — lightweight frame heap-allocation guard / telemetry.
//
// This module tracks heap allocations that happen while an explicit frame
// scope is active. It is intentionally nonblocking: scope registration uses a
// fixed-size lock-free table, writers use relaxed atomics, and names are kept
// as stable caller-owned pointers (usually string literals).

#pragma once

#include "../Types.h"

namespace psynder::mem {

struct FrameAllocScopeId {
    u32 value = 0;
    constexpr bool valid() const noexcept { return value != 0; }
    constexpr bool operator==(const FrameAllocScopeId& o) const noexcept = default;
};

struct FrameAllocStats {
    u64 alloc_count = 0;
    u64 free_count = 0;
    u64 alloc_bytes = 0;
    u64 free_bytes = 0;
};

struct FrameAllocScopeInfo {
    FrameAllocScopeId id;
    const char* name = nullptr;  // stable caller-owned storage; never copied.
    FrameAllocStats totals;
};

struct FrameAllocScopeToken {
    FrameAllocScopeId id;
    u32 slot = 0;
    u32 depth = 0;
    bool active = false;
    FrameAllocStats start;
};

constexpr FrameAllocScopeId frame_alloc_scope_id(const char* stable_name) noexcept {
    u32 h = 0x811C9DC5u;
    for (const char* c = stable_name; c && *c; ++c) {
        h ^= static_cast<u8>(*c);
        h *= 0x01000193u;
    }
    return FrameAllocScopeId{h ? h : 1u};
}

void frame_alloc_guard_set_enabled(bool enabled) noexcept;
bool frame_alloc_guard_enabled() noexcept;

FrameAllocScopeToken begin_frame_alloc_scope(FrameAllocScopeId id, const char* stable_name) noexcept;
FrameAllocStats end_frame_alloc_scope(FrameAllocScopeToken& token) noexcept;

FrameAllocStats frame_alloc_scope_totals(FrameAllocScopeId id) noexcept;
FrameAllocScopeInfo frame_alloc_scope_info(FrameAllocScopeId id) noexcept;

void frame_alloc_guard_reset(FrameAllocScopeId id) noexcept;
void frame_alloc_guard_reset_all() noexcept;

// Internal hooks for heap-producing code paths. They are public so future
// allocator wrappers can participate without another API turn.
void frame_alloc_guard_record_alloc(usize bytes) noexcept;
void frame_alloc_guard_record_free(usize bytes) noexcept;

class FrameAllocScopeGuard {
   public:
    FrameAllocScopeGuard(FrameAllocScopeId id, const char* stable_name) noexcept
        : token_(begin_frame_alloc_scope(id, stable_name)) {}

    FrameAllocScopeGuard(const FrameAllocScopeGuard&) = delete;
    FrameAllocScopeGuard& operator=(const FrameAllocScopeGuard&) = delete;

    FrameAllocScopeGuard(FrameAllocScopeGuard&& o) noexcept : token_(o.token_) {
        o.token_.active = false;
    }

    FrameAllocScopeGuard& operator=(FrameAllocScopeGuard&& o) noexcept {
        if (this != &o) {
            close();
            token_ = o.token_;
            o.token_.active = false;
        }
        return *this;
    }

    ~FrameAllocScopeGuard() { close(); }

    FrameAllocStats close() noexcept { return end_frame_alloc_scope(token_); }

    bool active() const noexcept { return token_.active; }

   private:
    FrameAllocScopeToken token_;
};

}  // namespace psynder::mem
