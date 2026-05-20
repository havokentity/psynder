// SPDX-License-Identifier: MIT
// Psynder — shared integer / float / handle types used across every subsystem.
//
// This header is the load-bearing cross-cutting type contract. Lane agents
// freely depend on it; changing it requires an Issue against the orchestrator.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace psynder {

// ─── Sized integer / floating-point aliases ──────────────────────────────
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

static_assert(sizeof(f32) == 4 && sizeof(f64) == 8, "non-IEEE float on this target");

// ─── Cache-line + page constants ─────────────────────────────────────────
inline constexpr usize kCacheLine = 64;
inline constexpr usize kPage = 4096;
inline constexpr usize kHugePage = 2 * 1024 * 1024;

// ─── ECS handle types ────────────────────────────────────────────────────
// An Entity is an opaque 32-bit handle. Top 8 bits are a generation counter
// (to detect stale references); bottom 24 bits index the entity table.
// See engine/scene/World.h for the full ECS contract.
struct Entity {
    u32 raw = 0;
    constexpr bool valid() const noexcept { return raw != 0; }
    constexpr u32 index() const noexcept { return raw & 0x00FFFFFFu; }
    constexpr u32 gen() const noexcept { return raw >> 24; }
    constexpr bool operator==(const Entity& o) const noexcept = default;
};
inline constexpr Entity kInvalidEntity{};

// ─── Strongly-typed opaque handle ────────────────────────────────────────
// `Handle<Tag>` is a u32 newtype keyed by an empty Tag struct. Use it for
// asset / texture / mesh / sound IDs to avoid accidental conversions.
template <class Tag>
struct Handle {
    u32 raw = 0;
    constexpr bool valid() const noexcept { return raw != 0; }
    constexpr explicit operator bool() const noexcept { return valid(); }
    constexpr bool operator==(const Handle& o) const noexcept = default;
};

// ─── Build / platform compile-time switches ──────────────────────────────
#if defined(__clang__)
#define PSY_FORCEINLINE [[gnu::always_inline]] inline
#define PSY_NOINLINE [[gnu::noinline]]
#define PSY_LIKELY(x) __builtin_expect(!!(x), 1)
#define PSY_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
#define PSY_FORCEINLINE __forceinline
#define PSY_NOINLINE __declspec(noinline)
#define PSY_LIKELY(x) (x)
#define PSY_UNLIKELY(x) (x)
#else
#define PSY_FORCEINLINE inline
#define PSY_NOINLINE
#define PSY_LIKELY(x) (x)
#define PSY_UNLIKELY(x) (x)
#endif

#define PSY_CACHELINE_ALIGN alignas(::psynder::kCacheLine)

}  // namespace psynder
