// SPDX-License-Identifier: MIT
// Psynder — archetype-chunked ECS registry impl. Lane 06.
//
// The public `EcsRegistry` class (frozen header) is a façade over a singleton
// `EcsRegistryImpl` owned by this TU. All actual storage — archetypes, chunks,
// the entity slot table, deferred-change queue — lives in `EcsRegistryImpl`.

#include "EcsRegistry.h"
#include "EcsRegistry_Internal.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace psynder::scene {

// ─── Public EcsRegistry façade ───────────────────────────────────────
EcsRegistry& EcsRegistry::Get() {
    static EcsRegistry registry;
    return registry;
}

Entity EcsRegistry::create() {
    return detail::EcsRegistryImpl::Get().create();
}
void EcsRegistry::destroy(Entity e) {
    detail::EcsRegistryImpl::Get().destroy(e);
}
bool EcsRegistry::alive(Entity e) const noexcept {
    return detail::EcsRegistryImpl::Get().alive(e);
}

void EcsRegistry::reserve_entities(u32 count) {
    detail::EcsRegistryImpl::Get().reserve_entities(count);
}

void EcsRegistry::reserve_structural_changes(u32 op_count, u32 byte_count) {
    detail::EcsRegistryImpl::Get().reserve_structural_changes(op_count, byte_count);
}

u32 EcsRegistry::entity_capacity() const noexcept {
    return detail::EcsRegistryImpl::Get().entity_capacity();
}

u32 EcsRegistry::chunk_live_count() const noexcept {
    return detail::EcsRegistryImpl::Get().chunk_live_count();
}

void EcsRegistry::set_structural_deferred(bool on) noexcept {
    detail::EcsRegistryImpl::Get().set_deferred_mode(on);
}

void EcsRegistry::apply_structural_changes() {
    detail::EcsRegistryImpl::Get().apply_structural_changes();
}

// ─── EcsRegistryImpl ──────────────────────────────────────────────────────
namespace detail {

EcsRegistryImpl::EcsRegistryImpl() {
    // Archetype 0 is the "empty" archetype — entities that exist but have
    // no components. It has zero columns, so it allocates no chunks.
    archetypes_.emplace_back();
    archetype_keys_.emplace_back();
    archetypes_[0].init(0, std::span<const ComponentId>{});
}

EcsRegistryImpl& EcsRegistryImpl::Get() {
    static EcsRegistryImpl impl;
    return impl;
}

void EcsRegistryImpl::shutdown() noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& a : archetypes_)
        a.release_all(pool_);
    archetypes_.clear();
    archetype_keys_.clear();
    entities_.clear();
    free_indices_.clear();
    pending_.clear();
    deferred_arena_.clear();
    live_entities_ = 0;
}

u32 EcsRegistryImpl::allocate_slot() {
    if (!free_indices_.empty()) {
        const u32 idx = free_indices_.back();
        free_indices_.pop_back();
        return idx;
    }
    entities_.emplace_back();
    return static_cast<u32>(entities_.size() - 1);
}

void EcsRegistryImpl::reserve_entities(u32 count) {
    std::lock_guard<std::mutex> lk(mutex_);
    entities_.reserve(count);
    free_indices_.reserve(count);
}

void EcsRegistryImpl::reserve_structural_changes(u32 op_count, u32 byte_count) {
    std::lock_guard<std::mutex> lk(mutex_);
    pending_.reserve(op_count);
    deferred_arena_.reserve(byte_count);
}

u32 EcsRegistryImpl::entity_capacity() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<u32>(entities_.capacity());
}

u32 EcsRegistryImpl::chunk_live_count() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<u32>(pool_.live_count());
}

Entity EcsRegistryImpl::create() {
    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = allocate_slot();
    auto& slot = entities_[idx];
    slot.archetype_id = 0;
    slot.chunk_index = 0;
    slot.row_in_chunk = 0;
    slot.alive = true;
    // Bump generation so a stale Entity stored from before destroy() will
    // fail `alive()`.
    slot.generation = (slot.generation + 1) & 0xFFu;
    if (slot.generation == 0)
        slot.generation = 1;
    ++live_entities_;

    // Pack index + generation into Entity::raw. Index in low 24 bits,
    // generation in the top 8. Reserved value 0 means invalid, so we use
    // (idx + 1) as the raw index to keep that sentinel out of bounds.
    Entity e;
    e.raw = ((idx + 1) & 0x00FFFFFFu) | (slot.generation << 24);
    return e;
}

namespace {
constexpr u32 entity_index_of(Entity e) noexcept {
    // Inverse of the packing in create() — subtract the +1 offset.
    return e.index() == 0 ? 0xFFFFFFFFu : e.index() - 1u;
}
}  // namespace

void EcsRegistryImpl::destroy(Entity e) {
    if (deferred_) {
        defer_destroy(e);
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = entity_index_of(e);
    if (idx == 0xFFFFFFFFu || idx >= entities_.size())
        return;
    auto& slot = entities_[idx];
    if (!slot.alive || slot.generation != e.gen())
        return;

    // Swap-pop the entity's row from its archetype, if it had any.
    if (slot.archetype_id != 0) {
        auto& arche = archetypes_[slot.archetype_id];
        Chunk* c = arche.chunk(slot.chunk_index);
        const u32 moved_entity_idx =
            arche.swap_pop(slot.chunk_index,
                           slot.row_in_chunk,
                           std::span<const u32>{arche.entity_index_column(c), c->header.row_count + 1});
        if (moved_entity_idx != Archetype::kInvalidEntityIndex && moved_entity_idx != idx) {
            // The row that filled the gap belonged to `moved_entity_idx`.
            // Patch its slot to point at the new row coords.
            auto& moved = entities_[moved_entity_idx];
            moved.chunk_index = slot.chunk_index;
            moved.row_in_chunk = slot.row_in_chunk;
        }
    }

    slot.alive = false;
    slot.archetype_id = 0;
    free_indices_.push_back(idx);
    --live_entities_;
}

bool EcsRegistryImpl::alive(Entity e) const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = entity_index_of(e);
    if (idx == 0xFFFFFFFFu || idx >= entities_.size())
        return false;
    const auto& slot = entities_[idx];
    return slot.alive && slot.generation == e.gen();
}

std::vector<ComponentId> EcsRegistryImpl::merged_components(std::span<const ComponentId> base,
                                                      ComponentId added,
                                                      ComponentId removed) {
    std::vector<ComponentId> out;
    out.reserve(base.size() + 1);
    for (ComponentId c : base) {
        if (c == removed)
            continue;
        out.push_back(c);
    }
    if (added != 0) {
        // Insert in sorted position; dedup.
        auto it = std::lower_bound(out.begin(), out.end(), added);
        if (it == out.end() || *it != added)
            out.insert(it, added);
    }
    return out;
}

u32 EcsRegistryImpl::ensure_archetype(std::span<const ComponentId> sorted_components) {
    // Linear search — typical games have hundreds of archetypes, not
    // millions. This is only on structural change. Wave B can swap in a
    // hash if it ever shows up in profiling.
    for (u32 i = 0; i < archetype_keys_.size(); ++i) {
        const auto& k = archetype_keys_[i];
        if (k.size() != sorted_components.size())
            continue;
        if (std::equal(k.begin(), k.end(), sorted_components.begin()))
            return i;
    }
    const u32 id = static_cast<u32>(archetypes_.size());
    archetypes_.emplace_back();
    archetype_keys_.emplace_back(sorted_components.begin(), sorted_components.end());
    archetypes_.back().init(id, sorted_components);
    ++archetype_topology_version_;
    return id;
}

void EcsRegistryImpl::reserve_archetype_rows(std::span<const ComponentId> sorted_components, u32 row_count) {
    if (row_count == 0u)
        return;
    std::lock_guard<std::mutex> lk(mutex_);
    const u32 archetype_id = ensure_archetype(sorted_components);
    archetypes_[archetype_id].reserve_rows(pool_, row_count);
}

void EcsRegistryImpl::migrate_entity(u32 entity_idx,
                               u32 new_archetype_id,
                               ComponentId added_id,
                               const void* added_bytes,
                               u32 added_bytes_count,
                               ComponentId removed_id) {
    auto& slot = entities_[entity_idx];
    Archetype& old_arche = archetypes_[slot.archetype_id];
    Archetype& new_arche = archetypes_[new_archetype_id];

    auto ref = new_arche.append_row(pool_);
    Chunk* new_chunk = new_arche.chunk(ref.chunk_index);

    // Copy overlapping component values from old → new chunk.
    if (slot.archetype_id != 0) {
        Chunk* old_chunk = old_arche.chunk(slot.chunk_index);
        const u32 old_row = slot.row_in_chunk;

        for (u32 ci = 0; ci < old_arche.components().size(); ++ci) {
            ComponentId cid = old_arche.components()[ci];
            if (cid == removed_id)
                continue;
            const u32 new_col = new_arche.column_index(cid);
            if (new_col == 0xFFFFFFFFu)
                continue;
            const u32 sz = old_arche.column_sizes()[ci];
            const std::byte* src =
                old_arche.column_base(old_chunk, ci) + static_cast<usize>(old_row) * sz;
            std::byte* dst =
                new_arche.column_base(new_chunk, new_col) + static_cast<usize>(ref.row) * sz;
            std::memcpy(dst, src, sz);
        }
    }

    // Write the added component bytes, if any.
    if (added_id != 0 && added_bytes && added_bytes_count > 0) {
        const u32 new_col = new_arche.column_index(added_id);
        if (new_col != 0xFFFFFFFFu) {
            const u32 sz = new_arche.column_sizes()[new_col];
            std::byte* dst =
                new_arche.column_base(new_chunk, new_col) + static_cast<usize>(ref.row) * sz;
            const u32 to_copy = std::min(sz, added_bytes_count);
            std::memcpy(dst, added_bytes, to_copy);
        }
    }

    // Patch the new chunk's entity sidecar.
    new_arche.entity_index_column(new_chunk)[ref.row] = entity_idx;

    // Remove the row from the old archetype and patch the moved row's slot.
    if (slot.archetype_id != 0) {
        const u32 old_chunk_idx = slot.chunk_index;
        const u32 old_row = slot.row_in_chunk;
        Chunk* old_chunk = old_arche.chunk(old_chunk_idx);
        const u32 moved =
            old_arche.swap_pop(old_chunk_idx,
                               old_row,
                               std::span<const u32>{old_arche.entity_index_column(old_chunk),
                                                    old_chunk->header.row_count + 1});
        if (moved != Archetype::kInvalidEntityIndex && moved != entity_idx) {
            auto& moved_slot = entities_[moved];
            moved_slot.chunk_index = old_chunk_idx;
            moved_slot.row_in_chunk = old_row;
        }
    }

    // Point the slot at its new home.
    slot.archetype_id = new_archetype_id;
    slot.chunk_index = ref.chunk_index;
    slot.row_in_chunk = ref.row;
}

void EcsRegistryImpl::add_raw(Entity e, ComponentId comp, const void* bytes, u32 byte_count) {
    if (deferred_) {
        defer_add(e, comp, bytes, byte_count);
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = entity_index_of(e);
    if (idx == 0xFFFFFFFFu || idx >= entities_.size())
        return;
    auto& slot = entities_[idx];
    if (!slot.alive || slot.generation != e.gen())
        return;

    Archetype& cur = archetypes_[slot.archetype_id];
    auto merged = merged_components(cur.components(), comp, 0u);

    // If the entity already had `comp`, this is an overwrite — write bytes
    // in place, bump version, done.
    const u32 same_col = cur.column_index(comp);
    if (same_col != 0xFFFFFFFFu) {
        Chunk* c = cur.chunk(slot.chunk_index);
        const u32 sz = cur.column_sizes()[same_col];
        std::byte* dst = cur.column_base(c, same_col) + static_cast<usize>(slot.row_in_chunk) * sz;
        std::memcpy(dst, bytes, std::min(sz, byte_count));
        cur.bump_version(c, same_col);
        return;
    }

    const u32 new_arche = ensure_archetype(merged);
    migrate_entity(idx, new_arche, comp, bytes, byte_count, 0u);
}

void* EcsRegistryImpl::get_raw(Entity e, ComponentId comp) noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = entity_index_of(e);
    if (idx == 0xFFFFFFFFu || idx >= entities_.size())
        return nullptr;
    auto& slot = entities_[idx];
    if (!slot.alive || slot.generation != e.gen())
        return nullptr;

    Archetype& arche = archetypes_[slot.archetype_id];
    const u32 col = arche.column_index(comp);
    if (col == 0xFFFFFFFFu)
        return nullptr;
    Chunk* c = arche.chunk(slot.chunk_index);
    const u32 sz = arche.column_sizes()[col];
    return arche.column_base(c, col) + static_cast<usize>(slot.row_in_chunk) * sz;
}

void EcsRegistryImpl::remove_raw(Entity e, ComponentId comp) {
    if (deferred_) {
        defer_remove(e, comp);
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    const u32 idx = entity_index_of(e);
    if (idx == 0xFFFFFFFFu || idx >= entities_.size())
        return;
    auto& slot = entities_[idx];
    if (!slot.alive || slot.generation != e.gen())
        return;

    Archetype& cur = archetypes_[slot.archetype_id];
    if (cur.column_index(comp) == 0xFFFFFFFFu)
        return;

    auto merged = merged_components(cur.components(), 0u, comp);
    const u32 new_arche = ensure_archetype(merged);
    migrate_entity(idx, new_arche, 0u, nullptr, 0u, comp);
}

void EcsRegistryImpl::defer_add(Entity e, ComponentId comp, const void* bytes, u32 byte_count) {
    std::lock_guard<std::mutex> lk(mutex_);
    const usize offset = deferred_arena_.size();
    deferred_arena_.insert(deferred_arena_.end(),
                           static_cast<const std::byte*>(bytes),
                           static_cast<const std::byte*>(bytes) + byte_count);
    StructuralChange sc{
        StructuralOp::AddComponent,
        e,
        comp,
        // Note: we can't take the address of deferred_arena_[offset] yet
        // because subsequent inserts may reallocate. Resolve on apply().
        reinterpret_cast<const std::byte*>(offset),
        byte_count,
    };
    pending_.push_back(sc);
}

void EcsRegistryImpl::defer_remove(Entity e, ComponentId comp) {
    std::lock_guard<std::mutex> lk(mutex_);
    pending_.push_back({StructuralOp::RemoveComponent, e, comp, nullptr, 0});
}

void EcsRegistryImpl::defer_destroy(Entity e) {
    std::lock_guard<std::mutex> lk(mutex_);
    pending_.push_back({StructuralOp::Destroy, e, 0u, nullptr, 0});
}

void EcsRegistryImpl::apply_one(const StructuralChange& sc) {
    // Caller holds mutex_.
    const u32 idx = entity_index_of(sc.target);
    if (idx == 0xFFFFFFFFu || idx >= entities_.size())
        return;
    auto& slot = entities_[idx];
    if (!slot.alive || slot.generation != sc.target.gen())
        return;

    switch (sc.op) {
        case StructuralOp::AddComponent: {
            Archetype& cur = archetypes_[slot.archetype_id];
            const u32 same_col = cur.column_index(sc.component);
            // Resolve byte pointer through the deferred arena offset.
            const std::byte* bytes = deferred_arena_.data() + reinterpret_cast<usize>(sc.bytes);
            if (same_col != 0xFFFFFFFFu) {
                Chunk* c = cur.chunk(slot.chunk_index);
                const u32 sz = cur.column_sizes()[same_col];
                std::memcpy(cur.column_base(c, same_col) + static_cast<usize>(slot.row_in_chunk) * sz,
                            bytes,
                            std::min(sz, sc.byte_count));
                cur.bump_version(c, same_col);
            } else {
                auto merged = merged_components(cur.components(), sc.component, 0u);
                const u32 new_arche = ensure_archetype(merged);
                migrate_entity(idx, new_arche, sc.component, bytes, sc.byte_count, 0u);
            }
            break;
        }
        case StructuralOp::RemoveComponent: {
            Archetype& cur = archetypes_[slot.archetype_id];
            if (cur.column_index(sc.component) == 0xFFFFFFFFu)
                break;
            auto merged = merged_components(cur.components(), 0u, sc.component);
            const u32 new_arche = ensure_archetype(merged);
            migrate_entity(idx, new_arche, 0u, nullptr, 0u, sc.component);
            break;
        }
        case StructuralOp::Destroy: {
            // Swap-pop + free slot, mirroring `destroy()` immediate path.
            if (slot.archetype_id != 0) {
                auto& arche = archetypes_[slot.archetype_id];
                Chunk* c = arche.chunk(slot.chunk_index);
                const u32 moved = arche.swap_pop(slot.chunk_index,
                                                 slot.row_in_chunk,
                                                 std::span<const u32>{arche.entity_index_column(c),
                                                                      c->header.row_count + 1});
                if (moved != Archetype::kInvalidEntityIndex && moved != idx) {
                    auto& moved_slot = entities_[moved];
                    moved_slot.chunk_index = slot.chunk_index;
                    moved_slot.row_in_chunk = slot.row_in_chunk;
                }
            }
            slot.alive = false;
            slot.archetype_id = 0;
            free_indices_.push_back(idx);
            --live_entities_;
            break;
        }
    }
}

void EcsRegistryImpl::apply_structural_changes() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (pending_.empty())
        return;

    // Snapshot + drain. We disable deferred mode during apply so any
    // recursive structural ops issued by post-apply hooks land
    // immediately. (Wave A has no such hooks, but the discipline is
    // cheap and matches DESIGN.md §3.5.)
    auto snapshot = std::move(pending_);
    pending_.clear();
    const bool was_deferred = deferred_;
    deferred_ = false;

    for (const auto& sc : snapshot)
        apply_one(sc);

    deferred_ = was_deferred;
    deferred_arena_.clear();
    ++archetype_topology_version_;
}

void EcsRegistryImpl::resolve_query(std::span<const ComponentId> required, std::vector<u32>& matched) const {
    std::lock_guard<std::mutex> lk(mutex_);
    matched.clear();
    for (u32 i = 1; i < archetypes_.size(); ++i) {
        const auto& comps = archetype_keys_[i];
        // Check that every required id is present in `comps`. comps is
        // sorted, so we can scan once.
        bool all_present = true;
        usize j = 0;
        for (ComponentId need : required) {
            while (j < comps.size() && comps[j] < need)
                ++j;
            if (j == comps.size() || comps[j] != need) {
                all_present = false;
                break;
            }
        }
        if (all_present)
            matched.push_back(i);
    }
}

}  // namespace detail

}  // namespace psynder::scene
