// SPDX-License-Identifier: MIT
// Psynder — our own physics engine. Rigid bodies + SAP broadphase + GJK/EPA
// narrowphase + parallel island solver + vehicle/character modules. No
// third-party physics dependency (DESIGN.md §10.1). Lane 13 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <span>

namespace psynder::physics {

struct BodyTag {};
using BodyId = Handle<BodyTag>;

enum class Shape : u8 { Sphere, Capsule, Box, ConvexHull, Compound, Heightfield, TriangleMesh };

struct BodyDesc {
    Shape shape = Shape::Sphere;
    f32 mass = 1.0f;  // kg; 0 = static
    math::Vec3 position{0, 0, 0};
    math::Quat rotation{0, 0, 0, 1};
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};  // shape-dependent
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
};

class World {
   public:
    static World& Get();

    BodyId create_body(const BodyDesc& desc);
    void destroy_body(BodyId id);

    // Fixed 120 Hz tick (DESIGN.md §10.1)
    void step(f32 dt_seconds);

    // Queries
    void set_gravity(math::Vec3 g);
    math::Vec3 get_position(BodyId id) const;
    math::Quat get_rotation(BodyId id) const;

    // ─── Body mutation (sample 10 physgun; DESIGN.md §10.8) ───────────────
    // Minimal deterministic writers so an external "physgun" (or any gameplay
    // code) can grab and fling a body without owning the internal Body struct.
    // All three are no-ops on a stale handle or a static body (inv_mass == 0).
    // Determinism: each is a plain field write — no RNG, no time dependence,
    // -fno-fast-math friendly — so a scripted call sequence reproduces bit-for-
    // bit across hosts.
    //
    // Teleport the body's centre to `p`. Also snaps prev_position to `p` so the
    // render interpolation does not lerp across the jump, and zeroes linear +
    // angular velocity so a held body stays put rather than carrying momentum
    // from before the grab. Use this each frame to hold a grabbed body.
    void set_body_position(BodyId id, math::Vec3 p);

    // Overwrite the linear velocity (m/s). Use on release to fling a body.
    void set_body_velocity(BodyId id, math::Vec3 v);

    // Apply an instantaneous linear impulse J (kg·m/s): v += J * inv_mass.
    // Alternative throw path that scales the resulting speed by 1/mass.
    void apply_impulse(BodyId id, math::Vec3 impulse);

    // ─── Angular writers (flight controllers, custom vehicles; DESIGN.md §10.1) ─
    // The integrator already carries per-body torque + a (world-space diagonal)
    // inertia tensor; these expose it so gameplay code can rotate a body without
    // owning the internal Body. All no-op on a stale handle or static body.
    //
    // Accumulate torque (N·m) consumed on the next step: w += I^-1 * torque * dt.
    void apply_torque(BodyId id, math::Vec3 torque);
    // Instantaneous angular impulse (N·m·s): w += I^-1 * impulse.
    void apply_angular_impulse(BodyId id, math::Vec3 impulse);
    // Overwrite angular velocity (rad/s) about the world axes.
    void set_angular_velocity(BodyId id, math::Vec3 w);
};

// Vehicle module (DESIGN.md §10.1 vehicle specialization)
namespace vehicle {
struct WheelDesc {
    math::Vec3 local_position;
    f32 radius = 0.32f;
    f32 suspension = 0.3f;
    f32 stiffness = 35000.0f;  // N/m
    f32 damping = 4500.0f;
};
struct VehicleDesc {
    BodyId chassis;
    std::span<const WheelDesc> wheels;
    f32 engine_max_torque = 400.0f;  // N·m
    f32 drag_coefficient = 0.30f;
    f32 downforce_coefficient = 0.0f;
};
struct VehicleTag {};
using VehicleId = Handle<VehicleTag>;
VehicleId create(const VehicleDesc& d);
void destroy(VehicleId v);
void set_throttle(VehicleId v, f32 t);   // 0..1
void set_brake(VehicleId v, f32 b);      // 0..1
void set_steer(VehicleId v, f32 angle);  // radians
// Flat ground-plane height (world Y) the suspension rays contact against.
// Default 0. The oval test track in sample 04 is flat, so a single plane is
// sufficient; elevated terrain would need a per-wheel height probe instead.
void set_ground_plane(VehicleId v, f32 ground_y);
}  // namespace vehicle

// Character controller (DESIGN.md §10.1 character specialization)
namespace character {
struct CharacterDesc {
    math::Vec3 position;
    f32 height = 1.8f;
    f32 radius = 0.35f;
};
struct CharacterTag {};
using CharacterId = Handle<CharacterTag>;
CharacterId create(const CharacterDesc& d);
void destroy(CharacterId c);
void move(CharacterId c, math::Vec3 delta, f32 dt);
}  // namespace character

}  // namespace psynder::physics
