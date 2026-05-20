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
    u32 gen = 1;   // for stale-handle detection
    u8 flags = 0;  // bit 0: kinematic, bit 1: sleeping
    u8 _pad[3] = {};
};

inline constexpr u8 kFlagKinematic = 1u << 0;
inline constexpr u8 kFlagSleeping = 1u << 1;
inline constexpr u8 kFlagStatic = 1u << 2;

}  // namespace psynder::physics::detail
