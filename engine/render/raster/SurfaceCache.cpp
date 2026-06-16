// SPDX-License-Identifier: MIT
// Psynder — surface cache implementation. See header for design notes.
// DESIGN.md §7.6 / ADR-001.

#include "SurfaceCache.h"

#include "core/Types.h"
#include "core/console/Console.h"

#include <cstring>

namespace psynder::render::raster {

namespace {

// 16-byte align helper.
PSY_FORCEINLINE u32 align_up(u32 x, u32 a) noexcept {
    return (x + a - 1) & ~(a - 1);
}

// `r_force_shading_path` cvar: 0 = auto, 1 = force OnTheFly, 2 = force
// SurfaceCached. Lazily registered on first use to keep the singleton
// constructor exception-free.
struct SurfaceCacheCvars {
    console::CVar* r_force = nullptr;
    SurfaceCacheCvars() {
        r_force = console::Console::Get().RegisterCVar(
            "r_force_shading_path",
            "0",
            "0=auto, 1=force OnTheFly, 2=force SurfaceCached. Debug A/B only.",
            console::CVarFlags::Archive);
    }
};
PSY_FORCEINLINE SurfaceCacheCvars& cache_cvars() noexcept {
    static SurfaceCacheCvars c;
    return c;
}

}  // namespace

// ─── Singleton ───────────────────────────────────────────────────────────
SurfaceCache& SurfaceCache::Get() noexcept {
    static SurfaceCache s;
    return s;
}

SurfaceCache::SurfaceCache() noexcept {
    // .bss zero-init handles entries_/slab_/counters. Mark every entry
    // as not-in-LRU explicitly so the prev/next == 0 default doesn't
    // false-positive as "linked at head".
    for (u32 i = 0; i < kMaxEntries; ++i) {
        entries_[i].lru_prev = kInvalid;
        entries_[i].lru_next = kInvalid;
    }
}

// ─── Frame lifecycle ─────────────────────────────────────────────────────
void SurfaceCache::begin_frame() noexcept {
    ++frame_index_;
}

void SurfaceCache::clear() noexcept {
    for (u32 i = 0; i < kMaxEntries; ++i) {
        entries_[i] = SurfaceCacheEntry{};
        entries_[i].lru_prev = kInvalid;
        entries_[i].lru_next = kInvalid;
    }
    lru_head_ = kInvalid;
    lru_tail_ = kInvalid;
    slab_used_ = 0;
    entries_live_ = 0;
    stats_ = {};
    // Don't reset frame_index_ — tests verify it increments monotonically.
}

// ─── Lookup helpers ──────────────────────────────────────────────────────
u32 SurfaceCache::lookup_slot(u32 surface_id, u32 lightmap_version, u32 mip_level) const noexcept {
    // Linear probe — kMaxEntries is small (1024) and the access pattern
    // walks recent entries first via LRU touches. Real impl will swap in
    // open-addressing hash when miss rates climb.
    for (u32 i = 0; i < kMaxEntries; ++i) {
        const SurfaceCacheEntry& e = entries_[i];
        if (!e.occupied)
            continue;
        if (e.byte_size == 0)
            continue;  // streak-only slot, not a payload
        if (e.surface_id == surface_id && e.lightmap_version == lightmap_version &&
            e.mip_level == mip_level) {
            return i;
        }
    }
    return kInvalid;
}

u32 SurfaceCache::lookup_streak_slot(u32 surface_id) const noexcept {
    for (u32 i = 0; i < kMaxEntries; ++i) {
        const SurfaceCacheEntry& e = entries_[i];
        if (!e.occupied)
            continue;
        if (e.surface_id == surface_id)
            return i;
    }
    return kInvalid;
}

u32 SurfaceCache::find(u32 surface_id, u32 lightmap_version, u32 mip_level) noexcept {
    const u32 slot = lookup_slot(surface_id, lightmap_version, mip_level);
    if (slot != kInvalid) {
        entries_[slot].last_frame = frame_index_;
        lru_touch(slot);
        ++stats_.hits;
        return slot;
    }
    ++stats_.misses;
    return kInvalid;
}

// ─── Streak / hysteresis ─────────────────────────────────────────────────
u32 SurfaceCache::acquire_streak_slot(u32 surface_id) noexcept {
    u32 slot = lookup_streak_slot(surface_id);
    if (slot != kInvalid)
        return slot;
    // Find first empty.
    for (u32 i = 0; i < kMaxEntries; ++i) {
        if (!entries_[i].occupied) {
            entries_[i] = SurfaceCacheEntry{};
            entries_[i].lru_prev = kInvalid;
            entries_[i].lru_next = kInvalid;
            entries_[i].surface_id = surface_id;
            entries_[i].occupied = true;
            entries_[i].last_frame = frame_index_;
            ++entries_live_;
            return i;
        }
    }
    // Table full — evict LRU tail's slot. We pick a slot that has a
    // payload (not streak-only) so we keep streak counters around.
    if (lru_tail_ != kInvalid) {
        const u32 victim = lru_tail_;
        lru_detach(victim);
        if (entries_[victim].byte_size != 0) {
            slab_used_ -= entries_[victim].byte_size;
            ++stats_.evictions;
        }
        entries_[victim] = SurfaceCacheEntry{};
        entries_[victim].lru_prev = kInvalid;
        entries_[victim].lru_next = kInvalid;
        entries_[victim].surface_id = surface_id;
        entries_[victim].occupied = true;
        entries_[victim].last_frame = frame_index_;
        return victim;
    }
    return kInvalid;
}

void SurfaceCache::mark_ineligible(u32 surface_id) noexcept {
    const u32 slot = lookup_streak_slot(surface_id);
    if (slot == kInvalid)
        return;
    entries_[slot].eligible_streak = 0;
}

u32 SurfaceCache::bump_streak(u32 surface_id) noexcept {
    const u32 slot = acquire_streak_slot(surface_id);
    if (slot == kInvalid)
        return 0;
    if (entries_[slot].eligible_streak < 0xFFFFu) {
        ++entries_[slot].eligible_streak;
    }
    // Keep the diagnostic counter in sync even for streak-only entries.
    stats_.entries_live = entries_live_;
    return entries_[slot].eligible_streak;
}

bool SurfaceCache::past_hysteresis(u32 surface_id) const noexcept {
    const u32 slot = lookup_streak_slot(surface_id);
    if (slot == kInvalid)
        return false;
    return entries_[slot].eligible_streak >= kHysteresis;
}

// ─── Acquire (with eviction) ─────────────────────────────────────────────
u32 SurfaceCache::acquire(
    u32 surface_id, u32 lightmap_version, u32 mip_level, u32 width, u32 height, u32 byte_size) noexcept {
    byte_size = align_up(byte_size, 16);

    // Re-use existing payload slot if the key already matches.
    const u32 existing = lookup_slot(surface_id, lightmap_version, mip_level);
    if (existing != kInvalid) {
        entries_[existing].last_frame = frame_index_;
        lru_touch(existing);
        return existing;
    }

    if (byte_size > kSlabBytes)
        return kInvalid;

    // Evict if needed.
    if (slab_used_ + byte_size > kSlabBytes) {
        if (!evict_for(byte_size))
            return kInvalid;
    }

    // Find a slot — prefer the streak slot for this surface if it exists
    // (so we keep the eligible_streak counter).
    u32 slot = lookup_streak_slot(surface_id);
    if (slot == kInvalid) {
        for (u32 i = 0; i < kMaxEntries; ++i) {
            if (!entries_[i].occupied) {
                slot = i;
                break;
            }
        }
    }
    if (slot == kInvalid) {
        // No empty slot — evict the LRU tail.
        if (lru_tail_ == kInvalid)
            return kInvalid;
        slot = lru_tail_;
        lru_detach(slot);
        if (entries_[slot].byte_size != 0) {
            slab_used_ -= entries_[slot].byte_size;
            ++stats_.evictions;
        }
        entries_[slot] = SurfaceCacheEntry{};
        entries_[slot].lru_prev = kInvalid;
        entries_[slot].lru_next = kInvalid;
    } else if (!entries_[slot].occupied) {
        ++entries_live_;
    }

    // Carry forward the streak counter if the slot was already attached
    // to this surface.
    u32 existing_streak = 0;
    if (entries_[slot].occupied && entries_[slot].surface_id == surface_id) {
        existing_streak = entries_[slot].eligible_streak;
    }

    SurfaceCacheEntry& e = entries_[slot];
    e.surface_id = surface_id;
    e.lightmap_version = lightmap_version;
    e.mip_level = mip_level;
    e.width = width;
    e.height = height;
    e.pitch = width;
    e.byte_offset = slab_used_;
    e.byte_size = byte_size;
    e.eligible_streak = existing_streak;
    e.last_frame = frame_index_;
    e.lru_prev = kInvalid;
    e.lru_next = kInvalid;
    e.occupied = true;
    slab_used_ += byte_size;
    lru_push_front(slot);
    stats_.entries_live = entries_live_;
    stats_.slab_bytes_used = slab_used_;
    return slot;
}

// ─── LRU plumbing ────────────────────────────────────────────────────────
void SurfaceCache::lru_detach(u32 slot) noexcept {
    SurfaceCacheEntry& e = entries_[slot];
    if (e.lru_prev != kInvalid)
        entries_[e.lru_prev].lru_next = e.lru_next;
    if (e.lru_next != kInvalid)
        entries_[e.lru_next].lru_prev = e.lru_prev;
    if (lru_head_ == slot)
        lru_head_ = e.lru_next;
    if (lru_tail_ == slot)
        lru_tail_ = e.lru_prev;
    e.lru_prev = kInvalid;
    e.lru_next = kInvalid;
}

void SurfaceCache::lru_push_front(u32 slot) noexcept {
    SurfaceCacheEntry& e = entries_[slot];
    e.lru_prev = kInvalid;
    e.lru_next = lru_head_;
    if (lru_head_ != kInvalid)
        entries_[lru_head_].lru_prev = slot;
    lru_head_ = slot;
    if (lru_tail_ == kInvalid)
        lru_tail_ = slot;
}

void SurfaceCache::lru_touch(u32 slot) noexcept {
    if (lru_head_ == slot)
        return;
    lru_detach(slot);
    lru_push_front(slot);
}

bool SurfaceCache::evict_for(u32 bytes_needed) noexcept {
    // First pass: prefer evicting entries that weren't touched this
    // frame (those are stale by definition — their owner won't read
    // them mid-frame). If that's not enough, second pass evicts the
    // LRU tail unconditionally — the caller is asking for capacity and
    // the slab is bounded, so we have to release *something*.
    auto evict_one = [&](u32 victim) noexcept {
        SurfaceCacheEntry& e = entries_[victim];
        lru_detach(victim);
        if (e.byte_size != 0) {
            slab_used_ -= e.byte_size;
            ++stats_.evictions;
        }
        // Drop the payload but keep the slot occupied so the streak
        // counter survives — DESIGN.md §7.6 ("entries are not evicted
        // when a surface flips to OnTheFly — they sit warm").
        e.byte_size = 0;
        e.byte_offset = 0;
        e.width = e.height = e.pitch = 0;
        e.lightmap_version = 0;
        e.mip_level = 0;
    };

    // Pass 1: skip current-frame entries.
    while (slab_used_ + bytes_needed > kSlabBytes) {
        u32 victim = lru_tail_;
        bool found = false;
        while (victim != kInvalid) {
            if (entries_[victim].byte_size != 0 && entries_[victim].last_frame != frame_index_) {
                found = true;
                break;
            }
            victim = entries_[victim].lru_prev;
        }
        if (!found)
            break;
        evict_one(victim);
    }
    // Pass 2: fall back to LRU tail unconditionally — only matters when
    // every payload entry is from this frame (rare; means the frame is
    // already over budget).
    while (slab_used_ + bytes_needed > kSlabBytes) {
        if (lru_tail_ == kInvalid)
            return false;
        u32 victim = lru_tail_;
        // Walk back until we find a payload-bearing entry.
        while (victim != kInvalid && entries_[victim].byte_size == 0) {
            victim = entries_[victim].lru_prev;
        }
        if (victim == kInvalid)
            return false;
        evict_one(victim);
    }
    stats_.slab_bytes_used = slab_used_;
    return true;
}

const SurfaceCacheEntry* SurfaceCache::entry(u32 slot) const noexcept {
    if (slot >= kMaxEntries)
        return nullptr;
    if (!entries_[slot].occupied)
        return nullptr;
    return &entries_[slot];
}
SurfaceCacheEntry* SurfaceCache::entry(u32 slot) noexcept {
    if (slot >= kMaxEntries)
        return nullptr;
    if (!entries_[slot].occupied)
        return nullptr;
    return &entries_[slot];
}

// ─── Eligibility classifier ──────────────────────────────────────────────
ShadingPath classify_surface(const SurfaceDesc& s) noexcept {
    auto& cv = cache_cvars();
    // Debug override — `r_force_shading_path`.
    if (cv.r_force) {
        const i32 v = cv.r_force->GetInt();
        if (v == 1)
            return ShadingPath::OnTheFly;
        if (v == 2)
            return ShadingPath::SurfaceCached;
    }
    auto& cache = SurfaceCache::Get();
    const bool eligible = draw_flags_has_all(s.flags, DrawFlags::EligibleMask);
    if (!eligible) {
        cache.mark_ineligible(s.surface_id);
        return ShadingPath::OnTheFly;
    }
    // Eligible — bump the streak. Only return SurfaceCached once the
    // 4-frame hysteresis bar is cleared.
    const u32 streak = cache.bump_streak(s.surface_id);
    return streak >= SurfaceCache::kHysteresis ? ShadingPath::SurfaceCached : ShadingPath::OnTheFly;
}

}  // namespace psynder::render::raster
