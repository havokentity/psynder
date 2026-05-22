// SPDX-License-Identifier: MIT
// Psynder — chunk pool. Backs 16 KiB chunks with page-aligned blocks
// from `psynder::mem::page_alloc`. Reuses freed chunks via a free list
// threaded through `ChunkHeader::next_chunk_raw`. App-scope lifetime,
// owned by `EcsRegistryImpl`.

#include "Chunk.h"

#include "core/alloc/Allocator.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace psynder::scene::detail {

ChunkPool::~ChunkPool() {
    for (usize i = 0; i < block_len_; ++i) {
        if (blocks_[i]) {
            mem::page_free(mem::PageBlock{blocks_[i], kChunkBytes});
        }
    }
    std::free(blocks_);
}

void ChunkPool::grow_block_table_() {
    const usize new_cap = block_cap_ == 0 ? 16 : block_cap_ * 2;
    void** new_table = static_cast<void**>(std::realloc(blocks_, new_cap * sizeof(void*)));
    blocks_ = new_table;
    block_cap_ = new_cap;
}

Chunk* ChunkPool::acquire() {
    if (free_head_) {
        Chunk* c = free_head_;
        free_head_ = reinterpret_cast<Chunk*>(c->header.next_chunk_raw);
        // Zero the header so callers get a clean slate.
        std::memset(&c->header, 0, sizeof(ChunkHeader));
        ++live_;
        return c;
    }

    // No reusable chunk — allocate a fresh page-aligned block.
    mem::PageBlock pb = mem::page_alloc(kChunkBytes, /*prefer_hugepage*/ true);
    if (!pb.ptr)
        return nullptr;

    // The page allocator may overshoot to page size; we placement-construct
    // a Chunk in the first 16 KiB regardless.
    Chunk* c = new (pb.ptr) Chunk{};
    std::memset(&c->header, 0, sizeof(ChunkHeader));

    if (block_len_ == block_cap_)
        grow_block_table_();
    blocks_[block_len_++] = pb.ptr;

    ++live_;
    ++total_;
    return c;
}

void ChunkPool::release(Chunk* c) noexcept {
    if (!c)
        return;
    c->header.next_chunk_raw = reinterpret_cast<u64>(free_head_);
    free_head_ = c;
    if (live_ > 0)
        --live_;
}

}  // namespace psynder::scene::detail
