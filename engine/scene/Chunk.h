// SPDX-License-Identifier: MIT
// Psynder — ECS chunk: 16 KiB, page-aligned, SoA columns 64-byte aligned.
// Cache-line header carries archetype id + row count + per-column version
// stamps + per-row dirty mask. Lane 06 internal — not a public contract.

#pragma once

#include "core/Types.h"

#include <cstddef>
#include <cstring>

namespace psynder::scene::detail {

// 16 KB chunks — fits in L1 with system locals (DESIGN.md §4.4).
inline constexpr usize kChunkBytes = 16ull * 1024ull;
// 16 KB ÷ 64-byte cache line ⇒ 256-row hard ceiling per chunk. In practice the
// per-row footprint pulls the live capacity well below this.
inline constexpr u32 kMaxRowsPerChunk = 256;
// 64-byte alignment for every SoA column start within a chunk.
inline constexpr usize kColumnAlign = kCacheLine;

// One cache line. Plenty of room for the metadata an ECS chunk needs.
struct alignas(kCacheLine) ChunkHeader {
    u32 archetype_id;           // index into ArchetypeStore
    u32 row_count;              // live rows currently in the chunk
    u32 capacity;               // max rows the columns can hold
    u32 column_count;           // number of SoA columns
    u64 dirty_mask;             // 1 bit per group-of-rows; bit 0 = bucket 0
    u32 version_stamps_offset;  // byte offset of per-column version array
    u32 first_column_offset;    // byte offset of column 0 within the chunk
    // 8 bytes reserved for the linked-list "next sibling" pointer (set up by
    // the archetype). We keep this inside the chunk header so traversal stays
    // a single cache-line touch.
    u64 next_chunk_raw;
    // 16 bytes spare — leaves us room to grow without breaking layout asserts.
    u32 pad0;
    u32 pad1;
    u32 pad2;
    u32 pad3;
};

static_assert(sizeof(ChunkHeader) == 64, "ChunkHeader must be exactly one cache line");
static_assert(alignof(ChunkHeader) == 64, "ChunkHeader must be cache-line aligned");

// A chunk is a 16 KB blob with the header in the first cache line. The rest
// of the bytes are split into: (a) a small array of u32 version stamps —
// one per column — and (b) the SoA columns themselves, each rounded up to
// the next 64-byte boundary. `Archetype` owns the layout.
struct alignas(kCacheLine) Chunk {
    ChunkHeader header;
    // The remaining bytes serve as raw storage for version stamps + columns.
    // We address them via raw byte offsets stored in `Archetype`.
    alignas(kCacheLine) std::byte storage[kChunkBytes - sizeof(ChunkHeader)];

    static constexpr usize storage_bytes() noexcept { return kChunkBytes - sizeof(ChunkHeader); }

    // Pointer to the raw byte that begins at `byte_offset` measured from the
    // start of the chunk (NOT from `storage`). All callers translate via the
    // offsets `Archetype` precomputes.
    std::byte* at(usize byte_offset) noexcept {
        return reinterpret_cast<std::byte*>(this) + byte_offset;
    }
    const std::byte* at(usize byte_offset) const noexcept {
        return reinterpret_cast<const std::byte*>(this) + byte_offset;
    }
};

static_assert(sizeof(Chunk) == kChunkBytes, "Chunk must be exactly 16 KiB");
static_assert(alignof(Chunk) == kCacheLine, "Chunk must be cache-line aligned");

// ─── Chunk pool ────────────────────────────────────────────────────────
// A poor-man's `TypedPool<Chunk>` until lane 01 fills in the real one.
// Backs all chunks with page-aligned blocks. Reuses returned chunks via a
// free list. App-scope lifetime — owned by the World.
class ChunkPool {
   public:
    ChunkPool() = default;
    ~ChunkPool();

    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;

    Chunk* acquire();
    void release(Chunk* c) noexcept;

    usize live_count() const noexcept { return live_; }
    usize total_count() const noexcept { return total_; }

   private:
    // The chunks themselves are large (16 KiB) so we slab-allocate one chunk
    // per `page_alloc`. The free list threads through the chunks via the
    // `next_chunk_raw` field of the header when a chunk is parked here.
    Chunk* free_head_ = nullptr;
    // All chunks ever allocated, kept so we can free on shutdown.
    void** blocks_ = nullptr;
    usize block_cap_ = 0;
    usize block_len_ = 0;
    usize live_ = 0;
    usize total_ = 0;

    void grow_block_table_();
};

}  // namespace psynder::scene::detail
