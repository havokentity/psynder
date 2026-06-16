// SPDX-License-Identifier: MIT
// Psynder — lane-06 internal: declares the type-erased helpers the public
// templates in `EcsRegistry.h` forward to. NOT a public header — other lanes
// only ever include `EcsRegistry.h`.

#pragma once

#include "EcsRegistry.h"
#include "Archetype.h"
#include "Chunk.h"
#include "Registry.h"

#include "core/Types.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace psynder::scene::detail {

struct EcsEditorSelectionSnapshot;

// ─── Entity slot table ────────────────────────────────────────────────
// Each Entity::index() addresses a slot in this table. The slot knows
// which archetype the entity belongs to, which chunk and row within that
// archetype, and a generation counter so stale Entity handles fail
// `alive()` checks once the underlying entity has been destroyed.
struct EntitySlot {
    u32 archetype_id = 0;  // 0 = "empty archetype" (entity exists but has no components)
    u32 chunk_index = 0;
    u32 row_in_chunk = 0;
    u32 generation = 0;  // upper 8 bits replicated in Entity::raw
    bool alive = false;
};

// ─── Deferred structural-change command ─────────────────────────────
enum class StructuralOp : u8 {
    AddComponent,
    RemoveComponent,
    Destroy,
};

struct StructuralChange {
    StructuralOp op;
    Entity target;
    ComponentId component;
    // For Add, points into a side arena that owns the bytes. Owned by the
    // registry; freed when the change is applied.
    const std::byte* bytes;
    u32 byte_count;
};

// ─── EcsRegistry implementation singleton ─────────────────────────────
// Holds every piece of state the public `EcsRegistry` class is implemented on
// top of. The public `EcsRegistry` has no data members (header-frozen), so we
// own everything here.
class EcsRegistryImpl {
   public:
    static EcsRegistryImpl& Get();

    // Lifetime
    void shutdown() noexcept;

    // Entities
    Entity create();
    void destroy(Entity e);
    bool alive(Entity e) const noexcept;
    void reserve_entities(u32 count);
    void reserve_structural_changes(u32 op_count, u32 byte_count);
    void clear() noexcept { shutdown(); }

    // Type-erased component ops
    void add_raw(Entity e, ComponentId comp, const void* bytes, u32 byte_count);
    void* get_raw(Entity e, ComponentId comp) noexcept;
    void remove_raw(Entity e, ComponentId comp);

    // Structural-change queue helpers
    void defer_add(Entity e, ComponentId comp, const void* bytes, u32 byte_count);
    void defer_remove(Entity e, ComponentId comp);
    void defer_destroy(Entity e);
    void apply_structural_changes();
    void reserve_archetype_rows(std::span<const ComponentId> sorted_components, u32 row_count);

    // Toggle: when true, add/remove/destroy are queued; when false, they
    // happen immediately. Default is false (immediate) so unit tests are
    // straightforward; the main loop flips it on at frame start.
    void set_deferred_mode(bool on) noexcept { deferred_ = on; }
    bool deferred_mode() const noexcept { return deferred_; }

    // Query support — given sorted reads + writes, populate matching
    // archetype indices.
    void resolve_query(std::span<const ComponentId> required, std::vector<u32>& matched) const;
    u32 snapshot_live_entities(std::span<Entity> out) const;
    u32 snapshot_components(Entity e, std::span<ComponentId> out) const;
    u32 component_count(Entity e) const noexcept;
    void snapshot_selected_entities(std::span<const Entity> selected,
                                    EcsEditorSelectionSnapshot& out) const;

    // Public access for queries / tests.
    Archetype& archetype(u32 id) noexcept { return archetypes_[id]; }
    const Archetype& archetype(u32 id) const noexcept { return archetypes_[id]; }
    u32 archetype_count() const noexcept { return static_cast<u32>(archetypes_.size()); }

    // Stats
    u32 entity_count() const noexcept;
    u32 entity_capacity() const noexcept;
    u32 chunk_live_count() const noexcept;
    u32 pending_structural_change_count() const noexcept;

   private:
    EcsRegistryImpl();
    ~EcsRegistryImpl() = default;

    EcsRegistryImpl(const EcsRegistryImpl&) = delete;
    EcsRegistryImpl& operator=(const EcsRegistryImpl&) = delete;

    // Archetype lookup / creation.
    u32 ensure_archetype(std::span<const ComponentId> sorted_components);

    // Move an entity from one archetype to another, preserving any
    // overlapping components. Used by add/remove.
    void migrate_entity(u32 entity_idx,
                        u32 new_archetype_id,
                        ComponentId added_id,
                        const void* added_bytes,
                        u32 added_bytes_count,
                        ComponentId removed_id);

    // Allocate an unused slot or grow the table.
    u32 allocate_slot();

    void apply_one(const StructuralChange& sc);

    // Build the sorted superset of (archetype components ∪ {added}) \ {removed}.
    static std::vector<ComponentId> merged_components(std::span<const ComponentId> base,
                                                      ComponentId added,
                                                      ComponentId removed);

    // ─── State ─────────────────────────────────────────────────────
    mutable std::mutex mutex_;

    ChunkPool pool_;

    std::vector<Archetype> archetypes_;
    // Hash map archetype-id-vector → archetype index. We do linear search
    // for simplicity (Wave A) — typical games have hundreds of archetypes,
    // not millions; this is not a hot path (only on structural change).
    std::vector<std::vector<ComponentId>> archetype_keys_;

    std::vector<EntitySlot> entities_;
    std::vector<u32> free_indices_;
    u32 live_entities_ = 0;

    // Deferred structural-change queue
    std::vector<StructuralChange> pending_;
    bool deferred_ = false;

    // Side buffer for byte payloads of deferred add ops. Append-only;
    // drained on apply_structural_changes().
    std::vector<std::byte> deferred_arena_;

    // Bumped every time we add or remove an archetype. Query caches
    // compare their last-seen value against this.
    u64 archetype_topology_version_ = 0;
};

// ─── Query helpers used by the public templates ───────────────────────
template <class... Ts>
inline std::array<ComponentId, sizeof...(Ts)> component_ids_array() {
    return {component_id<Ts>()...};
}

}  // namespace psynder::scene::detail

// ─── Public-template definitions ──────────────────────────────────────
// These provide bodies for the templates declared in the (frozen) public
// `EcsRegistry.h`. Lane 06 includes this file via the small append at the
// bottom of `EcsRegistry.h` (kept text-identical to the bootstrap header except
// for an `#include "EcsRegistry_Internal.h"`-equivalent that downstream lanes
// already see through the normal include chain).
namespace psynder::scene {

template <class T>
void EcsRegistry::add(Entity e, const T& component) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    detail::EcsRegistryImpl::Get().add_raw(e, component_id<T>(), &component, sizeof(T));
}

template <class... Ts>
void EcsRegistry::reserve_archetype(u32 row_count) {
    auto ids = detail::component_ids_array<Ts...>();
    std::vector<ComponentId> sorted;
    sorted.reserve(ids.size());
    for (ComponentId id : ids)
        sorted.push_back(id);
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    detail::EcsRegistryImpl::Get().reserve_archetype_rows(sorted, row_count);
}

template <class T>
T* EcsRegistry::get(Entity e) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    return static_cast<T*>(detail::EcsRegistryImpl::Get().get_raw(e, component_id<T>()));
}

template <class T>
void EcsRegistry::remove(Entity e) {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    detail::EcsRegistryImpl::Get().remove_raw(e, component_id<T>());
}

// ─── query<reads<R...>, writes<W...>>(body) ─────────────────────────
// `body` receives one std::span<T> per reads/writes argument, in order:
// reads first, then writes. Each span covers a contiguous run of rows
// inside ONE chunk; the framework calls `body` once per chunk. This
// makes the column-at-a-time walk explicit in the API.
//
// Body signature: void(std::span<const R0>, std::span<const R1>, ...,
//                      std::span<W0>, std::span<W1>, ...)
template <class Reads, class Writes>
class QueryBuilder;

template <class... R, class... W>
class QueryBuilder<reads<R...>, writes<W...>> {
   public:
    template <class Body>
    static void run(Body&& body) {
        const auto reads_ids = detail::component_ids_array<R...>();
        const auto writes_ids = detail::component_ids_array<W...>();

        // `required` is the sorted/deduped set of component ids the query
        // needs. Its upper bound is a COMPILE-TIME constant — the number of
        // reads plus writes template arguments — so we build it in a fixed
        // on-stack array (SBO) instead of a per-call std::vector. This removes
        // the allocation entirely with zero race risk: the array is a private
        // automatic on this call's stack frame, so nested / concurrent queries
        // each own their own copy. We pass a std::span view to resolve_query,
        // matching its frozen signature (owned by EcsRegistry.cpp).
        constexpr usize kRequiredMax = sizeof...(R) + sizeof...(W);
        std::array<ComponentId, kRequiredMax> required_buf{};
        usize required_n = 0;
        for (auto id : reads_ids)
            required_buf[required_n++] = id;
        for (auto id : writes_ids)
            required_buf[required_n++] = id;
        std::sort(required_buf.begin(), required_buf.begin() + required_n);
        required_n = static_cast<usize>(
            std::unique(required_buf.begin(), required_buf.begin() + required_n) -
            required_buf.begin());
        std::span<const ComponentId> required(required_buf.data(), required_n);

        // `matched` (the list of matching archetype indices) is intentionally
        // LEFT as a per-call std::vector: resolve_query's signature is owned by
        // EcsRegistry.cpp (out of this lane's scope) and writes into it via
        // .clear()/.push_back(), so its element count cannot be bounded here.
        // It is a fresh per-call local, so it is re-entrancy/thread safe; only
        // the heap allocation remains, and it is the cold path (archetype
        // count, not chunk/entity count).
        std::vector<u32> matched;
        detail::EcsRegistryImpl::Get().resolve_query(required, matched);

        // Flatten the per-archetype chunks into a single per-chunk work list so
        // `parallel_for` can spread them across workers. With lane 04's Wave-A
        // Chase-Lev backend, distinct chunks land on distinct workers — each
        // worker walks its chunks column-at-a-time. The Phase-0 job-system stub
        // runs everything synchronously, which the body must already tolerate
        // (queries do not mutate the archetype topology — structural changes
        // require defer + apply).
        //
        // KERNELIZATION (Wave 8 perf pass): the per-chunk column resolution is
        // archetype-invariant, not chunk-invariant. The previous walk re-ran,
        // FOR EVERY CHUNK, a std::lower_bound binary search per component
        // (`arche.column_index(component_id<T>())`), a function-local-static
        // guard per `component_id<T>()`, and a singleton fetch +
        // vector-index (`EcsRegistryImpl::Get().archetype(...)`). With hundreds
        // of chunks per frame across render/physics/AI queries that dispatch
        // overhead is pure waste. We hoist all of it OUT of the per-chunk loop:
        //   * component ids resolved ONCE per query (line below),
        //   * column byte-offsets + write column indices resolved ONCE per
        //     matched archetype,
        //   * each work entry caches the resolved chunk base pointer + the
        //     per-column byte offsets, so the inner walk is pure pointer
        //     arithmetic (`base + offset`) with zero binary search / zero
        //     singleton fetch.
        // The computed component VALUES are untouched — same column pointers,
        // same `n`, same `body` arguments, same `bump_version` calls in the
        // same order — so results stay bit-identical; only the iteration is
        // faster + more cache-coherent.
        //
        // The work-list previously allocated a std::vector on EVERY multi-chunk
        // query (render gather, physics writeback, save) — the hot per-frame
        // allocation flagged by review. We keep the small-buffer optimization:
        // a fixed on-stack array for the common small case, with a C-heap
        // fallback only when the chunk count overflows kWorkSbo.
        //
        // Re-entrancy / concurrency: a query body can issue another query
        // (nested), and queries run from many worker threads concurrently. The
        // SBO array `work_sbo` is a plain automatic on this call's stack frame,
        // so each (possibly nested, possibly cross-thread) invocation owns a
        // private copy. No shared mutable scratch is touched — identical
        // thread-safety to the old per-call vector, minus the allocation.
        constexpr usize kReadCols = sizeof...(R);
        constexpr usize kWriteCols = sizeof...(W);

        // Component ids are resolved exactly once for the whole query (was once
        // per chunk via the function-local-static guard inside component_id).
        const std::array<ComponentId, (kReadCols ? kReadCols : 1)> read_ids =
            {component_id<R>()...};
        const std::array<ComponentId, (kWriteCols ? kWriteCols : 1)> write_ids =
            {component_id<W>()...};

        // One work entry per populated chunk. It caches the resolved chunk base
        // pointer plus the per-column byte offsets (read columns then write
        // columns) and the write column indices needed by bump_version. All of
        // these are archetype-invariant, computed once per archetype below.
        struct Job {
            detail::Chunk* chunk;
            detail::Archetype* arche;  // only needed for bump_version (version array)
            u32 row_count;
            u32 read_off[kReadCols ? kReadCols : 1];
            u32 write_off[kWriteCols ? kWriteCols : 1];
            u32 write_col[kWriteCols ? kWriteCols : 1];
        };
        constexpr usize kWorkSbo = 64;
        Job work_sbo[kWorkSbo];
        Job* work_heap = nullptr;  // owns the spill allocation, if any
        usize work_cap = kWorkSbo;
        usize work_n = 0;
        Job* work = work_sbo;
        for (u32 archetype_idx : matched) {
            auto& arche = detail::EcsRegistryImpl::Get().archetype(archetype_idx);
            // Resolve column indices + byte offsets ONCE per archetype. These
            // are identical for every chunk of this archetype, so the binary
            // search that used to run per chunk now runs per archetype.
            const std::span<const u32> offsets = arche.column_offsets();
            u32 read_off[kReadCols ? kReadCols : 1];
            u32 write_off[kWriteCols ? kWriteCols : 1];
            u32 write_col[kWriteCols ? kWriteCols : 1];
            [&]<usize... I>(std::index_sequence<I...>) {
                ((read_off[I] = offsets[arche.column_index(read_ids[I])]), ...);
            }(std::make_index_sequence<kReadCols>{});
            [&]<usize... I>(std::index_sequence<I...>) {
                ((write_col[I] = arche.column_index(write_ids[I]),
                  write_off[I] = offsets[write_col[I]]),
                 ...);
            }(std::make_index_sequence<kWriteCols>{});

            for (u32 ci = 0; ci < arche.chunk_count(); ++ci) {
                detail::Chunk* c = arche.chunk(ci);
                if (!c || c->header.row_count == 0)
                    continue;
                if (work_n == work_cap) {
                    // Grow onto the heap (cold path). Double the capacity and
                    // copy the existing entries over.
                    usize new_cap = work_cap * 2;
                    Job* grown = static_cast<Job*>(std::malloc(sizeof(Job) * new_cap));
                    std::memcpy(grown, work, sizeof(Job) * work_n);
                    if (work_heap)
                        std::free(work_heap);
                    work_heap = grown;
                    work = grown;
                    work_cap = new_cap;
                }
                Job& j = work[work_n++];
                j.chunk = c;
                j.arche = &arche;
                j.row_count = c->header.row_count;
                for (usize k = 0; k < kReadCols; ++k)
                    j.read_off[k] = read_off[k];
                for (usize k = 0; k < kWriteCols; ++k) {
                    j.write_off[k] = write_off[k];
                    j.write_col[k] = write_col[k];
                }
            }
        }

        auto walk_one = [&](usize begin, usize end) {
            for (usize w_i = begin; w_i < end; ++w_i) {
                const Job& j = work[w_i];
                detail::Chunk* c = j.chunk;
                const u32 n = j.row_count;
                // Resolved-once chunk base. Columns are addressed by raw byte
                // offset from the chunk base (see Chunk::at) — no per-element
                // bounds check, no binary search.
                std::byte* const base = c->at(0);

                // Build per-column std::span<T>s from the cached offsets. Pure
                // pointer arithmetic: base + precomputed offset. Same pointers
                // the old path produced via column_base()/column_index(). The
                // index_sequence pairs each pack element R_i / W_i with its
                // compile-time slot in read_off[] / write_off[].
                auto reads_spans = [&]<usize... I>(std::index_sequence<I...>) {
                    return std::tuple<std::span<const R>...>{std::span<const R>(
                        reinterpret_cast<const R*>(base + j.read_off[I]), n)...};
                }(std::make_index_sequence<kReadCols>{});
                auto writes_spans = [&]<usize... I>(std::index_sequence<I...>) {
                    return std::tuple<std::span<W>...>{
                        std::span<W>(reinterpret_cast<W*>(base + j.write_off[I]), n)...};
                }(std::make_index_sequence<kWriteCols>{});

                // Bump version for written columns. Same calls, same order as
                // before — just using the cached column indices.
                for (usize k = 0; k < kWriteCols; ++k)
                    j.arche->bump_version(c, j.write_col[k]);

                std::apply(
                    [&](auto... rs) {
                        std::apply([&](auto... ws) { body(rs..., ws...); }, writes_spans);
                    },
                    reads_spans);
            }
        };

        if (work_n == 0) {
            // No work-list spill could have happened (work_n == 0), so there
            // is nothing to free here.
            return;
        }
        jobs::JobSystem::Get().parallel_for(usize{0}, work_n, /*grain*/ usize{1}, walk_one);
        if (work_heap)
            std::free(work_heap);
    }
};

// Member-template body for the public `EcsRegistry::query<Reads, Writes>(body)`
// API. Defined here so the class declaration in the public header stays a
// pure surface contract.
template <class Reads, class Writes, class Body>
void EcsRegistry::query(Body&& body) {
    QueryBuilder<Reads, Writes>::run(std::forward<Body>(body));
}

// Free-function variant. Equivalent to `EcsRegistry::Get().query<R,W>(body)` for
// callers that prefer the namespaced form.
template <class Reads, class Writes, class Body>
inline void query(Body&& body) {
    QueryBuilder<Reads, Writes>::run(std::forward<Body>(body));
}

}  // namespace psynder::scene
