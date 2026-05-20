// SPDX-License-Identifier: MIT
// Psynder — archetype storage. An archetype is "the set of components an
// entity has." Within an archetype we lay out SoA columns inside 16 KiB
// chunks; rows of an entity live at the same index across every column of
// the archetype's current chunk. Lane 06 internal.

#pragma once

#include "Chunk.h"

#include "core/Types.h"

#include <span>
#include <vector>

namespace psynder::scene {
// `ComponentId` is defined in `World.h`; we forward-declare a compatible
// alias here to break the include cycle (`World.h` pulls in
// `World_Internal.h` which pulls in `Archetype.h`).
using ComponentId = u32;
}  // namespace psynder::scene

namespace psynder::scene::detail {

// One archetype = a sorted set of ComponentIds + a linked list of chunks.
// The `column_offsets` array stores the byte offset (from the chunk base)
// of column `i`. Stored in the same order as `components` so binary search
// over `components` resolves the column index.
class Archetype {
   public:
    Archetype() = default;
    ~Archetype() = default;

    Archetype(const Archetype&) = delete;
    Archetype& operator=(const Archetype&) = delete;
    Archetype(Archetype&&) noexcept = default;
    Archetype& operator=(Archetype&&) noexcept = default;

    // Build column layout from a sorted component id list. Called once at
    // archetype creation; recomputes capacity for the chunk size.
    void init(u32 id, std::span<const ComponentId> sorted_components);

    u32 id() const noexcept { return id_; }
    std::span<const ComponentId> components() const noexcept { return components_; }
    std::span<const u32> column_offsets() const noexcept { return column_offsets_; }
    std::span<const u32> column_sizes() const noexcept { return column_sizes_; }
    u32 capacity() const noexcept { return capacity_per_chunk_; }
    u32 row_size() const noexcept { return row_size_; }
    bool empty() const noexcept { return total_rows_ == 0; }
    u32 total_rows() const noexcept { return total_rows_; }
    u32 chunk_count() const noexcept { return static_cast<u32>(chunks_.size()); }

    // Returns the index in `components()` for the given id, or UINT32_MAX
    // if this archetype does not have that component.
    u32 column_index(ComponentId id) const noexcept;

    // Acquire a chunk with at least one free row. Allocates from the pool
    // if all current chunks are full.
    Chunk* acquire_chunk_with_room(ChunkPool& pool);

    // Append a default-zeroed row to the archetype. Returns (chunk_index,
    // row_in_chunk).
    struct RowRef {
        u32 chunk_index;
        u32 row;
    };
    RowRef append_row(ChunkPool& pool);

    // Move the last row of the last chunk into (chunk_index, row), then
    // pop the trailing row. Returns the entity index previously stored at
    // the moved-from position (or kInvalidIndex if no swap happened, i.e.
    // we removed the last row).
    static constexpr u32 kInvalidEntityIndex = 0xFFFFFFFFu;
    u32 swap_pop(u32 chunk_index, u32 row, std::span<const u32> entity_per_row_for_chunk);

    // ─── Per-row entity-index column ──────────────────────────────────
    // The archetype stores, alongside the user-visible columns, a sidecar
    // u32 column with the entity index for each row. The World uses it to
    // patch entity slots on swap_pop. The sidecar is **the first column
    // inside the chunk**, occupying the slot reserved by `init`.
    u32* entity_index_column(Chunk* c) const noexcept {
        return reinterpret_cast<u32*>(c->at(entity_column_offset_));
    }
    const u32* entity_index_column(const Chunk* c) const noexcept {
        return reinterpret_cast<const u32*>(c->at(entity_column_offset_));
    }

    // Column base pointer for the i-th user component within a given chunk.
    std::byte* column_base(Chunk* c, u32 col_index) const noexcept {
        return c->at(column_offsets_[col_index]);
    }
    const std::byte* column_base(const Chunk* c, u32 col_index) const noexcept {
        return c->at(column_offsets_[col_index]);
    }

    Chunk* chunk(u32 index) noexcept { return chunks_[index]; }
    const Chunk* chunk(u32 index) const noexcept { return chunks_[index]; }
    std::span<Chunk* const> chunks() const noexcept { return chunks_; }

    // Bump per-column version stamp (called when a system writes the column).
    void bump_version(Chunk* c, u32 col_index) noexcept;

    // Tear down chunks and return them to the pool.
    void release_all(ChunkPool& pool) noexcept;

   private:
    u32 id_ = 0;
    u32 row_size_ = 0;  // total bytes per row across all user columns + entity sidecar
    u32 capacity_per_chunk_ = 0;
    u32 entity_column_offset_ = 0;
    u32 total_rows_ = 0;

    std::vector<ComponentId> components_;  // sorted ascending
    std::vector<u32> column_offsets_;      // byte offset from chunk base
    std::vector<u32> column_sizes_;        // bytes per element

    std::vector<Chunk*> chunks_;
};

}  // namespace psynder::scene::detail
