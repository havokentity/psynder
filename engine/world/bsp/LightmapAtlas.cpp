// SPDX-License-Identifier: MIT
// Psynder — runtime lightmap atlas. Lane 10 owns. See LightmapAtlas.h.

#include <vector>  // Bsp.h convention (see Bsp.cpp).

#include "LightmapAtlas.h"

#include <algorithm>
#include <cstring>

namespace psynder::world::bsp {

void LightmapAtlas::init(mem::LinearArena* arena) noexcept {
    arena_           = arena;
    lru_tick_        = 0u;
    allocated_bytes_ = 0u;
    entries_.clear();
    entries_.reserve(kLightmapMaxPages);
}

LightmapAtlas::Entry* LightmapAtlas::find_entry(u32 page_id) noexcept {
    for (Entry& e : entries_) {
        if (e.page_id == page_id) return &e;
    }
    return nullptr;
}

void LightmapAtlas::evict_lru() noexcept {
    if (entries_.empty()) return;
    usize victim_idx = 0;
    u32   victim_clock = entries_[0].lru_clock;
    for (usize i = 1; i < entries_.size(); ++i) {
        if (entries_[i].lru_clock < victim_clock) {
            victim_clock = entries_[i].lru_clock;
            victim_idx   = i;
        }
    }
    // LinearArena is bump-only — we can't return the page bytes. The
    // resident-set cap keeps the slab footprint bounded; in practice a real
    // BuddyAllocator drop-in (when lane 01 exposes it) would free the page
    // here. We compensate by leaving allocated_bytes_ untouched so callers
    // can see the high-water mark.
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(victim_idx));
}

LightmapPage* LightmapAtlas::atlas_page_for_surface(u32 face_id) noexcept {
    // We use `face_id` directly as the page id. Real callers usually pass
    // `BspFace::lightmap` (a packed page id from lm_qbsp); the public contract
    // is "same id → same page", which holds either way.
    ++lru_tick_;

    if (Entry* existing = find_entry(face_id)) {
        existing->lru_clock = lru_tick_;
        return &existing->page;
    }

    if (entries_.size() >= kLightmapMaxPages) {
        evict_lru();
    }

    // Allocate page bytes from the bound arena. If no arena is bound we still
    // hand back a header-only page (pixels=nullptr) so callers can treat the
    // result uniformly — the rasterizer's "no lightmap" path keys off
    // pixels==nullptr.
    u8* pixels = nullptr;
    if (arena_ != nullptr) {
        pixels = static_cast<u8*>(arena_->alloc(kLightmapPageBytes, 64));
        if (pixels != nullptr) {
            // Lane 24 lm_qbsp will eventually stream the page bytes in from
            // the .psybsp blob; until that lands we zero-init so trilinear
            // samples don't read garbage when the rasterizer integrates.
            std::memset(pixels, 0, kLightmapPageBytes);
            allocated_bytes_ += kLightmapPageBytes;
        }
    }

    Entry e{};
    e.page_id           = face_id;
    e.lru_clock         = lru_tick_;
    e.page.page_id      = face_id;
    e.page.width        = kLightmapPageWidth;
    e.page.height       = kLightmapPageHeight;
    e.page.byte_stride  = kLightmapPageWidth * 6u;  // 6 bytes per RGB16F texel
    e.page.pixels       = pixels;
    entries_.push_back(e);
    return &entries_.back().page;
}

}  // namespace psynder::world::bsp
