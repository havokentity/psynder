// SPDX-License-Identifier: MIT
// Psynder — runtime lightmap atlas. Lane 10 (world-bsp) owns.
//
// `lm_bake` (lane 24) emits per-surface 16-bit half-float RGB lightmaps and
// packs them into atlas pages on disk (DESIGN.md §8.1). At runtime, the BSP
// face → lightmap mapping is just `BspFace::lightmap` — an opaque page id.
// `LightmapAtlas` is the level-scope cache that converts (face_id) → page
// pointer, streaming pages lazily from the level allocator and evicting the
// least-recently-used page when the resident set exceeds `kLightmapMaxPages`.
//
// Allocator: per DESIGN.md §4.2 the canonical home for atlas pages is the
// level-scope `BuddyAllocator`, but Wave A only exposed `LinearArena` in the
// public allocator surface (lane 01 hasn't promoted Buddy to public yet).
// We slab pages out of a `LinearArena` instead — same lifetime semantics,
// O(1) bump, no fragmentation because every page is the same fixed size.
//
// Page size: a single lightmap page is a fixed-size half-float RGB block. We
// keep the layout self-describing in `LightmapPage` so consumers (rasterizer
// trilinear sampler) can address pixels without knowing the page came from
// the atlas. The page-payload pointer aliases into the slab; the slab outlives
// every page that points into it (level scope).

#pragma once

#include <vector>  // Bsp.h uses std::vector without <vector>; see Bsp.cpp.
#include "Bsp.h"
#include "core/Types.h"
#include "core/alloc/Allocator.h"

namespace psynder::world::bsp {

// Page size constants. DESIGN.md §8.1 says "per-surface resolution" but the
// atlas packs surfaces into fixed-size tiles; 128×128 RGB16F is a reasonable
// default page that matches the rasterizer's lightmap sampler stride and
// keeps a single page at exactly 96 KiB (128 * 128 * 3 * 2 bytes per pixel).
inline constexpr u32 kLightmapPageWidth = 128u;
inline constexpr u32 kLightmapPageHeight = 128u;
// Half-float RGB → 6 bytes per texel. Stored as plain bytes; lane 12 raster
// reinterpret-casts to `u16[3]` for sampling.
inline constexpr u32 kLightmapPageBytes = kLightmapPageWidth * kLightmapPageHeight * 6u;

// LRU resident-set cap. ~256 pages × 96 KiB ≈ 24 MiB upper bound on atlas RAM.
inline constexpr u32 kLightmapMaxPages = 256u;

// A single resident page. `pixels` points into the slab; null means the page
// has been evicted and a later `atlas_page_for_surface` will need to reload
// (the level cooker is the source of truth for content, so reload is a
// memcpy from disk — Wave B keeps the bytes zero-filled and lets the
// rasterizer fall back to its untextured path).
struct LightmapPage {
    u32 page_id = 0u;  // BspFace::lightmap value
    u32 width = 0u;
    u32 height = 0u;
    u32 byte_stride = 0u;  // bytes per row
    u8* pixels = nullptr;
};

// The atlas itself. One per BspMap.
class LightmapAtlas {
   public:
    LightmapAtlas() = default;
    ~LightmapAtlas() = default;

    LightmapAtlas(const LightmapAtlas&) = delete;
    LightmapAtlas& operator=(const LightmapAtlas&) = delete;

    // Bind a backing arena. The arena MUST outlive the atlas. `init` may be
    // called multiple times (handy for re-binding after a level swap); each
    // call clears the resident set and starts fresh. If `arena` is null the
    // atlas falls into a degraded mode where `atlas_page_for_surface` always
    // returns a zero-initialised page header — the renderer's "no lightmap"
    // path picks that up and shades unlit.
    void init(mem::LinearArena* arena) noexcept;

    // Resolve a BSP face's lightmap page. Lazily allocates the page on first
    // request; on subsequent requests returns the same page (LRU-touched).
    // When the resident set is full, the LRU page is evicted before the new
    // one is allocated.
    //
    // Returns nullptr only if `face_id` exceeds u32 sanity bounds AND the
    // arena allocation fails. The renderer treats nullptr as "no lightmap".
    LightmapPage* atlas_page_for_surface(u32 face_id) noexcept;

    // Number of pages currently resident in the cache. Bounded by
    // kLightmapMaxPages.
    u32 resident_page_count() const noexcept { return static_cast<u32>(entries_.size()); }

    // Total bytes allocated from the bound arena (sum of resident page sizes).
    usize allocated_bytes() const noexcept { return allocated_bytes_; }

   private:
    struct Entry {
        u32 page_id;
        u32 lru_clock;
        LightmapPage page;
    };

    // Find a resident entry by page id, returning a pointer to the slot or
    // nullptr if absent. O(N) over the resident set — N <= kLightmapMaxPages,
    // so we keep it linear-search-simple instead of paying for a hash map.
    Entry* find_entry(u32 page_id) noexcept;

    // Evict the least-recently-used entry from `entries_`. The arena memory
    // can't be reclaimed (LinearArena is bump-only), so the eviction just
    // forgets the slot — its bytes stay claimed in the slab. That's fine
    // because the resident set is bounded; the slab is sized for the cap.
    void evict_lru() noexcept;

    mem::LinearArena* arena_ = nullptr;
    u32 lru_tick_ = 0u;
    usize allocated_bytes_ = 0u;
    std::vector<Entry> entries_;
};

}  // namespace psynder::world::bsp
