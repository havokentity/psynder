// SPDX-License-Identifier: MIT
// Psynder physics — internal rigid body storage (DESIGN.md §10.1, §3.4).
// SoA chunked storage to keep the integrator and solver cache-friendly.
//
// All units are SI: kilograms, metres, seconds, newtons. Mass = 0 marks a
// static body (infinite mass, ignored by the integrator, still collided).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::physics::detail {

// ─── Generation-safe handle codec ─────────────────────────────────────────
// Every physics handle (BodyId / CharacterId / VehicleId / joint body ref)
// packs a 24-bit slot index in the low bits and an 8-bit generation counter
// in the high bits, mirroring the scene-lane scheme (EcsRegistry.cpp,
// SceneGraph.cpp). A live slot always carries gen >= 1, so the top byte is
// non-zero and raw == 0 is therefore an unambiguous "invalid" sentinel.
//
// On slot reuse the generation is bumped; the OLD handle's stale generation
// then fails the per-decode equality check, so a freed-then-recreated slot
// never aliases the previous owner's handle (UAF-class bug). Generation wraps
// 255 -> 1 (skipping 0 so a live slot's gen never reads as "freed"). A slot's
// gen is preserved across destroy (the `alive` flag marks the hole) so the
// next reuse advances from the last value.
inline constexpr u32 kHandleIndexMask = 0x00FFFFFFu;
inline constexpr u32 kHandleGenShift = 24u;
inline constexpr u32 kHandleMaxGen = 0xFFu;

[[nodiscard]] constexpr u32 handle_index(u32 raw) noexcept {
    return raw & kHandleIndexMask;
}
[[nodiscard]] constexpr u32 handle_gen(u32 raw) noexcept {
    return (raw >> kHandleGenShift) & 0xFFu;
}
[[nodiscard]] constexpr u32 handle_encode(u32 gen, u32 index) noexcept {
    return ((gen & 0xFFu) << kHandleGenShift) | (index & kHandleIndexMask);
}
// Advance a slot generation on reuse: 1..255 then wrap to 1 (never 0).
[[nodiscard]] constexpr u32 handle_next_gen(u32 gen) noexcept {
    u32 g = (gen + 1u) & 0xFFu;
    return (g == 0u) ? 1u : g;
}

enum class BodyFlags : u8 {
    None = 0,
    Kinematic = 1u << 0,
    Sleeping = 1u << 1,
    Static = 1u << 2,
};

[[nodiscard]] constexpr u8 body_flags_bits(BodyFlags flags) noexcept {
    return static_cast<u8>(flags);
}

[[nodiscard]] constexpr BodyFlags operator|(BodyFlags a, BodyFlags b) noexcept {
    return static_cast<BodyFlags>(body_flags_bits(a) | body_flags_bits(b));
}

[[nodiscard]] constexpr u8 operator&(BodyFlags a, BodyFlags b) noexcept {
    return static_cast<u8>(body_flags_bits(a) & body_flags_bits(b));
}

constexpr BodyFlags& operator|=(BodyFlags& a, BodyFlags b) noexcept {
    a = a | b;
    return a;
}

// Inertia tensor in body-local space. Stored as a diagonal — Wave-A shapes
// (sphere, capsule, box, convex hull) all have well-defined principal axes
// aligned with the body's local frame after construction. Off-diagonal
// terms re-enter only for compound shapes (Wave B).
struct Inertia {
    math::Vec3 local{1, 1, 1};  // I_xx, I_yy, I_zz
    math::Vec3 inv_local{1, 1, 1};
};

struct Body {
    // ─── Kinematic state (read every solver iteration) ─────────────────
    math::Vec3 position{0, 0, 0};
    math::Quat rotation{0, 0, 0, 1};
    math::Vec3 linear_velocity{0, 0, 0};
    math::Vec3 angular_velocity{0, 0, 0};

    // Snapshot from the start of the current sim step. The render side
    // interpolates between (prev_*, position/rotation) by alpha.
    math::Vec3 prev_position{0, 0, 0};
    math::Quat prev_rotation{0, 0, 0, 1};

    // ─── Accumulators (cleared every step before forces apply) ─────────
    math::Vec3 force{0, 0, 0};
    math::Vec3 torque{0, 0, 0};

    // ─── Mass / inertia ───────────────────────────────────────────────
    f32 mass = 1.0f;
    f32 inv_mass = 1.0f;  // 0 for static
    Inertia inertia;

    // ─── Material ─────────────────────────────────────────────────────
    f32 friction = 0.5f;
    f32 restitution = 0.0f;

    // ─── Shape (Wave A: sphere/capsule/box/convex hull) ───────────────
    u8 shape = 0;  // matches public Shape enum
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};

    // ─── Bookkeeping ──────────────────────────────────────────────────
    // `gen` is the slot's CURRENT generation (1..255, never 0). It is bumped
    // on slot REUSE and is preserved across destroy so the next reuse can
    // advance it; a BodyId decodes successfully only when its packed gen
    // equals this. `alive` marks whether the slot currently holds a live body
    // (a destroyed slot keeps its gen for the bump but sets alive = 0). Hole
    // checks throughout the world iterate on `alive`, not on gen.
    u32 gen = 1;   // current slot generation, for stale-handle detection
    BodyFlags flags = BodyFlags::None;
    u8 alive = 0;  // 1 while the slot holds a live body, 0 for a hole
    u8 _pad[2] = {};
};

}  // namespace psynder::physics::detail
