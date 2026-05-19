// SPDX-License-Identifier: MIT
// Psynder — archetype-chunked ECS world. Lane 06 owns the full ECS impl
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
    ComponentId  id;
    u32          size;
    u32          align;
    const char*  name;
};

ComponentId register_component(const ComponentTypeInfo& info);

template <class T>
ComponentId component_id() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Psynder components must be trivially copyable POD");
    static const ComponentId id = register_component(
        ComponentTypeInfo{ 0, sizeof(T), alignof(T), "" });
    return id;
}

#define PSYNDER_COMPONENT(Name) \
    struct Name; \
    /* clang-format off */ \
    inline auto Psynder_Register_##Name = ::psynder::scene::component_id<Name>(); \
    /* clang-format on */ \
    struct Name

// ─── World ───────────────────────────────────────────────────────────────
class World {
public:
    static World& Get();

    Entity create();
    void   destroy(Entity e);
    bool   alive(Entity e) const noexcept;

    template <class T>
    void add(Entity e, const T& component);

    template <class T>
    T* get(Entity e);

    template <class T>
    void remove(Entity e);

    // Query construction is library-provided; lane 06 implements.
    // Users register systems via the script-side API (see engine/script).
};

// ─── Tag traits for read/write declarations ──────────────────────────────
template <class... Ts> struct reads  {};
template <class... Ts> struct writes {};

}  // namespace psynder::scene
