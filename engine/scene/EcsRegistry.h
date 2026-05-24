// SPDX-License-Identifier: MIT
// Psynder — archetype-chunked ECS registry. Lane 06 owns the full ECS impl
// (chunks, archetypes, queries, structural batching, spatial indices).
//
// This header freezes the contract used by every other subsystem.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <string_view>
#include <type_traits>

namespace psynder::scene {

// ─── Component registration ──────────────────────────────────────────────
// Components are POD structs. Use PSYNDER_COMPONENT(Name) to declare; the
// macro asserts triviality at compile time and registers the type at static
// init. The lane 06 implementation provides the registry; this header makes
// the macro available everywhere.

using ComponentId = u32;

struct ComponentTypeInfo {
    ComponentId id;
    u32 size;
    u32 align;
    const char* name;
};

ComponentId register_component(const ComponentTypeInfo& info);
ComponentTypeInfo component_type_info(ComponentId id);

namespace detail {
template <class T>
const char* component_type_name() noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#else
    return "unknown_component";
#endif
}
}  // namespace detail

template <class T>
ComponentId component_id() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    static const ComponentId id =
        register_component(ComponentTypeInfo{0, sizeof(T), alignof(T), detail::component_type_name<T>()});
    return id;
}

#define PSYNDER_COMPONENT(Name) \
    struct Name;                \
    /* clang-format off */ \
    inline auto Psynder_Register_##Name = ::psynder::scene::component_id<Name>();    \
    /* clang-format on */       \
    struct Name

// ─── EcsRegistry ───────────────────────────────────────────────────────────────
class EcsRegistry {
   public:
    static EcsRegistry& Get();

    Entity create();
    void destroy(Entity e);
    bool alive(Entity e) const noexcept;

    void reserve_entities(u32 count);
    void reserve_structural_changes(u32 op_count, u32 byte_count);

    template <class... Ts>
    void reserve_archetype(u32 row_count);

    void clear() noexcept;
    u32 entity_count() const noexcept;
    u32 entity_capacity() const noexcept;
    u32 chunk_live_count() const noexcept;
    u32 pending_structural_change_count() const noexcept;

    // Snapshot surfaces for editor/script source-of-truth reads. Return the
    // total count available; copy up to out.size() entries into `out`.
    u32 snapshot_live_entities(std::span<Entity> out) const;
    u32 snapshot_components(Entity e, std::span<ComponentId> out) const;
    u32 component_count(Entity e) const noexcept;

    template <class T>
    void add(Entity e, const T& component);

    template <class T>
    T* get(Entity e);

    template <class T>
    void remove(Entity e);

    // Query construction is library-provided; lane 06 implements.
    // Users register systems via the script-side API (see engine/script).
    //
    // Call site: `registry.query<reads<A>, writes<B>>(body)` — see the brief
    // for Lane 06 (Issue #6). The body receives one `std::span<const R>`
    // per Reads type then one `std::span<W>` per Writes type, fired once
    // per chunk so iteration stays column-at-a-time.
    template <class Reads, class Writes, class Body>
    void query(Body&& body);

    // Defer / immediate-mode toggle. The engine main loop flips deferred
    // mode on at frame start; tests and tools that want immediate
    // mutation leave it off. Structural changes queue when on.
    void set_structural_deferred(bool on) noexcept;
    [[nodiscard]] bool structural_deferred() const noexcept;
    void apply_structural_changes();
};

// ─── Tag traits for read/write declarations ──────────────────────────────
template <class... Ts>
struct reads {};
template <class... Ts>
struct writes {};

}  // namespace psynder::scene

// Lane 06 internal: template definitions for `add`, `get`, `remove`,
// `query` live in the impl header, which is pulled in here so users of
// `EcsRegistry.h` see the full template bodies. The header above stays
// declarations-only for everything an external lane should grep against.
#include "EcsRegistry_Internal.h"
