// SPDX-License-Identifier: MIT
// Psynder — surface cache (lane 07 internal). Implements the classic
// Quake-style pre-multiplied base*lightmap chunk that auto-engages per
// surface, per frame, when eligible. DESIGN.md §7.6 / ADR-001.
//
// The cache is keyed by `(surface_id, lightmap_version, mip_level)`,
// LRU-evicted, with a 4-frame hysteresis on eligibility so a passing
// muzzle flash doesn't thrash entries. Entries are not evicted when a
// surface flips to OnTheFly — they sit warm in case the surface becomes
// eligible again next frame.
//
// Storage is a fixed-size slab carved from `.bss` (no per-frame
// new/delete; no shared_ptr). The slab is sized for the 4–8 MiB budget
// from DESIGN.md §7.6 (lane 01 will replace this with a level-scope
// allocator carve-out once it lands).

#pragma once

#include "core/Types.h"

namespace psynder::render::raster {

// ─── Shading path tag ────────────────────────────────────────────────────
// One byte on each DrawCmd after eligibility. Selected per-draw — never
// per-pixel — so SIMD lanes stay coherent (DESIGN.md §7.6).
enum class ShadingPath : u8 {
    OnTheFly = 0,       // sample base texture, sample lightmap, multiply
    SurfaceCached = 1,  // sample pre-multiplied combined chunk
};

// ─── DrawItem.flags bits (lane-07 internal contract) ─────────────────────
// Bits 0 / 1 are pre-existing (alpha-test / affine). Bits 2–6 are
// eligibility hints set by the lane 18 / lane 10 caller; the rasterizer
// reads them in classify_surface().
//
// Public-header layout is unchanged — only bit semantics within the
// existing one-byte `flags` field are added.
namespace draw_flags {
inline constexpr u8 kAlphaTest = 1u << 0;
inline constexpr u8 kAffine = 1u << 1;
inline constexpr u8 kLightmapNearest = 1u << 2;
inline constexpr u8 kNoDynamicLights = 1u << 3;
inline constexpr u8 kNoNormalMap = 1u << 4;
inline constexpr u8 kLdrLightmap = 1u << 5;
inline constexpr u8 kStableMip = 1u << 6;
inline constexpr u8 kForceShadingPath = 1u << 7;  // used by r_force_shading_path

// All eligibility hints set ⇒ candidate for surface cache.
inline constexpr u8 kEligibleMask =
    kLightmapNearest | kNoDynamicLights | kNoNormalMap | kLdrLightmap | kStableMip;
}  // namespace draw_flags

// ─── Surface-cache entry header ──────────────────────────────────────────
// One slot in the slab. The payload (pre-multiplied texels) lives right
// after this header inside the same arena range, 16-byte aligned.
//
// `base_used = 0` ⇒ empty slot. The LRU list is intrusive: every live
// entry has prev/next indices forming a doubly-linked ring; the head
// `lru_head` points at the MRU entry.
struct PSY_CACHELINE_ALIGN SurfaceCacheEntry {
    // Key
    u32 surface_id = 0;
    u32 lightmap_version = 0;
    u32 mip_level = 0;
    // Payload metadata
    u32 width = 0;
    u32 height = 0;
    u32 pitch = 0;        // texels per row (= width)
    u32 byte_offset = 0;  // into the slab
    u32 byte_size = 0;
    // Hysteresis: rolling count of consecutive eligible frames for this
    // surface. Re-classification gated until count >= 4 (DESIGN.md §7.6 /
    // ADR-001).
    u32 eligible_streak = 0;
    // Frame the entry was last touched (for LRU + staleness).
    u32 last_frame = 0;
    // LRU doubly-linked-list indices; kInvalid means "not in list".
    u32 lru_prev = 0;
    u32 lru_next = 0;
    // Slot is occupied iff byte_size != 0 (or eligible_streak > 0; either
    // ⇒ we want to retain the surface_id entry across frames).
    bool occupied = false;
    u8 _pad[7] = {};
};

// ─── Surface cache (singleton, lane-07 internal) ─────────────────────────
// Lives in static .bss; zero-initialized at process start. Size is fixed
// at 4 MiB for the slab + 1024 entries for the hash/LRU table (the table
// is over-provisioned vs the slab so we can carry warm keys across frames
// even when the slab evicts).
class SurfaceCache {
   public:
    static SurfaceCache& Get() noexcept;

    static constexpr u32 kInvalid = 0xFFFFFFFFu;
    static constexpr u32 kMaxEntries = 1024;
    static constexpr u32 kSlabBytes = 4 * 1024 * 1024;  // 4 MiB
    static constexpr u32 kHysteresis = 4;               // ADR-001

    // Begin a new frame. Bumps the frame counter; entries get age info.
    void begin_frame() noexcept;

    // Look up a key. If present, returns the entry index (and bumps LRU).
    // Otherwise returns kInvalid.
    u32 find(u32 surface_id, u32 lightmap_version, u32 mip_level) noexcept;

    // Acquire a slot for (key, byte_size). Returns kInvalid on
    // out-of-slab (caller must fall back to OnTheFly for this frame).
    // The returned slot's `byte_offset` points into payload_base().
    u32 acquire(u32 surface_id, u32 lightmap_version, u32 mip_level, u32 width, u32 height, u32 byte_size) noexcept;

    // Mark a surface as ineligible *this frame*. Resets the streak so the
    // hysteresis counter restarts on the next eligible run.
    void mark_ineligible(u32 surface_id) noexcept;

    // Bump and read the surface's eligible-streak counter. Returns the
    // *new* count after the bump.
    u32 bump_streak(u32 surface_id) noexcept;

    // Has this surface cleared the hysteresis bar this frame? (counter
    // >= kHysteresis). Convenience over bump_streak().
    bool past_hysteresis(u32 surface_id) const noexcept;

    // Access raw payload storage. Caller indexes via entry.byte_offset.
    u8* payload_base() noexcept { return slab_; }
    const u8* payload_base() const noexcept { return slab_; }

    // Diagnostics — exposed for tests + the r_surface_cache_stats cvar.
    struct Stats {
        u32 entries_live = 0;
        u32 slab_bytes_used = 0;
        u32 hits = 0;
        u32 misses = 0;
        u32 evictions = 0;
    };
    Stats stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }

    // Forget everything — used by tests + on level transitions. Does NOT
    // free the .bss slab (which is fixed-size).
    void clear() noexcept;

    // Read the active frame index (debug + tests).
    u32 frame_index() const noexcept { return frame_index_; }

    // Direct slot read. Returns nullptr for kInvalid.
    const SurfaceCacheEntry* entry(u32 slot) const noexcept;
    SurfaceCacheEntry* entry(u32 slot) noexcept;

   private:
    SurfaceCache() noexcept;

    // Find the slot index in entries_[] for a key. kInvalid if absent.
    u32 lookup_slot(u32 surface_id, u32 lightmap_version, u32 mip_level) const noexcept;

    // Find or insert a slot for a surface_id alone (streak tracking).
    // The slot may have byte_size==0 if no payload has been materialized
    // yet (this is the "warm key only" state).
    u32 lookup_streak_slot(u32 surface_id) const noexcept;
    u32 acquire_streak_slot(u32 surface_id) noexcept;

    // Bump `slot` to the head of the LRU list.
    void lru_touch(u32 slot) noexcept;
    void lru_detach(u32 slot) noexcept;
    void lru_push_front(u32 slot) noexcept;
    // Evict from LRU tail until at least `bytes_needed` bytes are free
    // in the slab. Returns false if eviction couldn't satisfy the
    // request (e.g. a single entry larger than the whole slab).
    bool evict_for(u32 bytes_needed) noexcept;

    // Storage. All static-sized so no per-frame new/delete.
    alignas(16) u8 slab_[kSlabBytes];
    SurfaceCacheEntry entries_[kMaxEntries];
    u32 lru_head_ = kInvalid;
    u32 lru_tail_ = kInvalid;
    u32 slab_used_ = 0;
    u32 frame_index_ = 0;
    u32 entries_live_ = 0;
    Stats stats_{};
};

// ─── Surface description (lane-07 internal) ──────────────────────────────
// Lightweight aggregate the rasterizer carries through binning/dispatch.
// Filled per-DrawItem from the DrawItem's material + lightmap inputs.
// Not a public-header concept — Lane 18 (editor) writes this via lane 06
// (scene) when it builds the per-frame draw list, but the conversion lives
// here.
struct SurfaceDesc {
    u32 surface_id = 0;        // typically = MaterialId.raw
    u32 lightmap_version = 0;  // bumped by lane 10 on bake / atlas swap
    u32 mip_level = 0;         // current stable mip for this surface
    u8 flags = 0;              // DrawItem.flags
};

// ─── Eligibility & dispatch ──────────────────────────────────────────────
// Returns SurfaceCached when ALL of the §7.6 conditions hold AND the
// surface has been eligible for kHysteresis consecutive frames. Updates
// the streak counter as a side-effect. Honors the r_force_shading_path
// override when SurfaceDesc.flags has kForceShadingPath set.
//
// Pure; only state mutated is the SurfaceCache singleton's streak table.
ShadingPath classify_surface(const SurfaceDesc& s) noexcept;

}  // namespace psynder::render::raster
