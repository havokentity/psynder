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

        std::vector<ComponentId> required;
        required.reserve(reads_ids.size() + writes_ids.size());
        for (auto id : reads_ids)
            required.push_back(id);
        for (auto id : writes_ids)
            required.push_back(id);
        std::sort(required.begin(), required.end());
        required.erase(std::unique(required.begin(), required.end()), required.end());

        std::vector<u32> matched;
        detail::EcsRegistryImpl::Get().resolve_query(required, matched);

        // Flatten the per-archetype chunks into a single (archetype_idx,
        // chunk_idx) list so `parallel_for` can spread them across
        // workers. With lane 04's Wave-A Chase-Lev backend, distinct
        // chunks land on distinct workers — each worker walks its chunks
        // column-at-a-time. The Phase-0 job-system stub runs everything
        // synchronously, which the body must already tolerate (queries
        // do not mutate the archetype topology — structural changes
        // require defer + apply).
        struct Job {
            u32 arche;
            u32 chunk;
        };
        std::vector<Job> work;
        work.reserve(matched.size());
        for (u32 archetype_idx : matched) {
            auto& arche = detail::EcsRegistryImpl::Get().archetype(archetype_idx);
            for (u32 ci = 0; ci < arche.chunk_count(); ++ci) {
                detail::Chunk* c = arche.chunk(ci);
                if (!c || c->header.row_count == 0)
                    continue;
                work.push_back({archetype_idx, ci});
            }
        }

        auto walk_one = [&](usize begin, usize end) {
            for (usize w_i = begin; w_i < end; ++w_i) {
                auto& arche = detail::EcsRegistryImpl::Get().archetype(work[w_i].arche);
                detail::Chunk* c = arche.chunk(work[w_i].chunk);
                const u32 n = c->header.row_count;

                // Build per-column std::span<T>s. We resolve each
                // component's column index once per chunk; the inner
                // iteration is column-at-a-time inside `body`.
                auto reads_spans = std::tuple<std::span<const R>...>{
                    std::span<const R>(reinterpret_cast<const R*>(
                                           arche.column_base(c, arche.column_index(component_id<R>()))),
                                       n)...};
                auto writes_spans = std::tuple<std::span<W>...>{std::span<W>(
                    reinterpret_cast<W*>(arche.column_base(c, arche.column_index(component_id<W>()))),
                    n)...};

                // Bump version for written columns. Useful for change
                // detection (Wave B will key reactive systems off this).
                (arche.bump_version(c, arche.column_index(component_id<W>())), ...);

                std::apply(
                    [&](auto... rs) {
                        std::apply([&](auto... ws) { body(rs..., ws...); }, writes_spans);
                    },
                    reads_spans);
            }
        };

        if (work.empty())
            return;
        jobs::JobSystem::Get().parallel_for(usize{0}, work.size(), /*grain*/ usize{1}, walk_one);
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
