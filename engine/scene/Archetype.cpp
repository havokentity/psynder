// SPDX-License-Identifier: MIT
// Psynder — archetype storage impl. Lane 06.

#include "World.h"      // full ComponentTypeInfo / ComponentId definitions
#include "Archetype.h"
#include "Registry.h"

#include <algorithm>
#include <cstring>

namespace psynder::scene::detail {

namespace {

// Round `x` up to the next multiple of `align` (assumed power-of-two).
constexpr usize align_up(usize x, usize align) noexcept {
    return (x + align - 1) & ~(align - 1);
}

}  // namespace

void Archetype::init(u32 id, std::span<const ComponentId> sorted_components) {
    id_ = id;
    components_.assign(sorted_components.begin(), sorted_components.end());
    column_sizes_.clear();
    column_offsets_.clear();

    auto& registry = ComponentRegistry::Get();

    // Sum per-row footprint to compute capacity. The first column inside the
    // chunk (right after the header + version array) is the entity sidecar
    // (a u32 per row). User columns follow, each rounded up to 64 bytes.
    column_sizes_.reserve(components_.size());
    for (ComponentId cid : components_) {
        auto rec = registry.lookup(cid);
        column_sizes_.push_back(rec.size);
    }

    // Version-stamps array: one u32 per user column, padded to a cache line.
    const usize version_bytes = align_up(
        sizeof(u32) * components_.size(), kColumnAlign);
    const usize version_offset = sizeof(ChunkHeader);  // immediately after header

    // Entity sidecar starts after version stamps, 64-byte aligned.
    const usize entity_offset = align_up(version_offset + version_bytes, kColumnAlign);

    // Try capacities from kMaxRowsPerChunk down until everything fits.
    u32 cap = kMaxRowsPerChunk;
    while (cap > 0) {
        usize cursor = entity_offset + align_up(sizeof(u32) * cap, kColumnAlign);
        bool ok = true;
        column_offsets_.assign(components_.size(), 0);
        for (usize i = 0; i < components_.size(); ++i) {
            const usize col_bytes = align_up(column_sizes_[i] * cap, kColumnAlign);
            column_offsets_[i] = static_cast<u32>(cursor);
            cursor += col_bytes;
        }
        if (cursor <= kChunkBytes) {
            ok = true;
        } else {
            ok = false;
        }
        if (ok) break;
        --cap;
    }

    // Lay out final offsets at the chosen capacity.
    {
        usize cursor = entity_offset + align_up(sizeof(u32) * cap, kColumnAlign);
        column_offsets_.assign(components_.size(), 0);
        for (usize i = 0; i < components_.size(); ++i) {
            const usize col_bytes = align_up(column_sizes_[i] * cap, kColumnAlign);
            column_offsets_[i] = static_cast<u32>(cursor);
            cursor += col_bytes;
        }
    }

    entity_column_offset_ = static_cast<u32>(entity_offset);
    capacity_per_chunk_   = cap;
    row_size_             = sizeof(u32);
    for (u32 sz : column_sizes_) row_size_ += sz;
    total_rows_           = 0;
}

u32 Archetype::column_index(ComponentId id) const noexcept {
    // components_ is sorted; binary search.
    auto it = std::lower_bound(components_.begin(), components_.end(), id);
    if (it == components_.end() || *it != id) return 0xFFFFFFFFu;
    return static_cast<u32>(it - components_.begin());
}

Chunk* Archetype::acquire_chunk_with_room(ChunkPool& pool) {
    if (!chunks_.empty()) {
        Chunk* c = chunks_.back();
        if (c->header.row_count < capacity_per_chunk_) return c;
    }
    Chunk* c = pool.acquire();
    if (!c) return nullptr;
    c->header.archetype_id    = id_;
    c->header.row_count       = 0;
    c->header.capacity        = capacity_per_chunk_;
    c->header.column_count    = static_cast<u32>(components_.size());
    c->header.dirty_mask      = 0;
    c->header.version_stamps_offset = static_cast<u32>(sizeof(ChunkHeader));
    c->header.first_column_offset   = entity_column_offset_;
    c->header.next_chunk_raw  = 0;
    // Zero the version-stamp array.
    std::memset(c->at(c->header.version_stamps_offset), 0,
                sizeof(u32) * c->header.column_count);
    chunks_.push_back(c);
    return c;
}

Archetype::RowRef Archetype::append_row(ChunkPool& pool) {
    Chunk* c = acquire_chunk_with_room(pool);
    if (!c) return RowRef{ 0xFFFFFFFFu, 0xFFFFFFFFu };
    const u32 ci  = static_cast<u32>(chunks_.size() - 1);
    const u32 row = c->header.row_count;
    ++c->header.row_count;
    ++total_rows_;
    // Zero the user columns for this row — components default to all-zero
    // bytes, matching the POD trivially-copyable contract.
    for (usize i = 0; i < components_.size(); ++i) {
        std::byte* col = c->at(column_offsets_[i]);
        std::memset(col + row * column_sizes_[i], 0, column_sizes_[i]);
    }
    // Initialize the entity sidecar slot to an invalid sentinel; the
    // caller patches in the real entity index immediately after.
    reinterpret_cast<u32*>(c->at(entity_column_offset_))[row] = kInvalidEntityIndex;
    return RowRef{ ci, row };
}

void Archetype::bump_version(Chunk* c, u32 col_index) noexcept {
    auto* v = reinterpret_cast<u32*>(
        c->at(c->header.version_stamps_offset));
    ++v[col_index];
    // Mark the corresponding dirty-mask bucket.
    constexpr u32 kBitsPerBucket = 4;
    const u32 bucket = col_index / kBitsPerBucket;
    if (bucket < 64) c->header.dirty_mask |= (u64{1} << bucket);
}

u32 Archetype::swap_pop(u32 chunk_index, u32 row,
                        std::span<const u32> /*entity_per_row_for_chunk*/) {
    Chunk* c = chunks_[chunk_index];

    // Find the last NON-EMPTY chunk. Earlier swap_pops may have left
    // trailing chunks at row_count=0; pulling the swap-from row off one
    // of those would underflow `row_count - 1` and segfault. The
    // last_chunk we care about is the one that still has rows.
    u32 last_chunk_idx = static_cast<u32>(chunks_.size() - 1);
    while (last_chunk_idx > chunk_index
           && chunks_[last_chunk_idx]->header.row_count == 0) {
        --last_chunk_idx;
    }
    Chunk* last_chunk = chunks_[last_chunk_idx];
    const u32 last_row = last_chunk->header.row_count - 1;

    u32 moved_entity_idx = kInvalidEntityIndex;

    // If we're not removing the literal last row, copy it down.
    const bool same_slot = (chunk_index == last_chunk_idx && row == last_row);
    if (!same_slot) {
        // Copy each user column.
        for (usize i = 0; i < components_.size(); ++i) {
            std::byte* dst = c->at(column_offsets_[i])
                           + static_cast<usize>(row) * column_sizes_[i];
            std::byte* src = last_chunk->at(column_offsets_[i])
                           + static_cast<usize>(last_row) * column_sizes_[i];
            std::memcpy(dst, src, column_sizes_[i]);
        }
        // Copy the entity sidecar slot.
        u32* dst_e = reinterpret_cast<u32*>(c->at(entity_column_offset_));
        u32* src_e = reinterpret_cast<u32*>(last_chunk->at(entity_column_offset_));
        moved_entity_idx = src_e[last_row];
        dst_e[row]       = moved_entity_idx;
    }

    // Trim the last row.
    --last_chunk->header.row_count;
    --total_rows_;

    // If the last chunk is now empty AND it isn't the only chunk left, we
    // could return it to the pool. We hold on to it instead so subsequent
    // additions don't immediately re-allocate. Wave B can add a high-water
    // policy.

    return moved_entity_idx;
}

void Archetype::release_all(ChunkPool& pool) noexcept {
    for (Chunk* c : chunks_) {
        if (c) pool.release(c);
    }
    chunks_.clear();
    total_rows_ = 0;
}

}  // namespace psynder::scene::detail
