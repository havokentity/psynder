// SPDX-License-Identifier: MIT
// Psynder — our own physics engine. Rigid bodies + SAP broadphase + GJK/EPA
// narrowphase + parallel island solver + vehicle/character modules. No
// third-party physics dependency (DESIGN.md §10.1). Lane 13 owns.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <memory>
#include <span>

namespace psynder::physics {

namespace detail {
struct WorldImpl;
}  // namespace detail

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
    // A `World` now OWNS all its physics state (PIMPL). Constructing one makes
    // an independent, empty world; multiple instances never share state. The
    // default ctor + out-of-line dtor (defined in World.cpp where WorldImpl is
    // complete) let the unique_ptr hold an incomplete type here. Movable so a
    // world can be relocated; non-copyable (the state is large + unique).
    World();
    ~World();
    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Legacy default-world accessor. Returns a lazily-constructed process-wide
    // default `World` so the reference samples (04/07/08/09/10) and the physics
    // tests/bench keep compiling and running UNCHANGED. New code (PlayRuntime,
    // per-scene sim) owns its OWN `World` instance instead of calling this.
    static World& Get();

    // Lane-internal: the owned state block. Declared here so the free vehicle/
    // character functions and the default-world accessors can reach a World's
    // sub-state; the type is opaque to public callers (forward-declared).
    [[nodiscard]] detail::WorldImpl& internal() noexcept { return *impl_; }
    [[nodiscard]] const detail::WorldImpl& internal() const noexcept { return *impl_; }

    BodyId create_body(const BodyDesc& desc);
    void destroy_body(BodyId id);

    // Fixed 120 Hz tick (DESIGN.md §10.1)
    void step(f32 dt_seconds);

    // Queries
    void set_gravity(math::Vec3 g);
    math::Vec3 get_position(BodyId id) const;
    math::Quat get_rotation(BodyId id) const;

    // --- Scene raycast (line-of-sight + bullet hits; DESIGN.md 10.1) ------
    // Closest live body struck by the ray, for gameplay/AI LOS and hit-scan.
    // `body` is a gen-safe BodyId (decodes through resolve_body); `t` is the
    // distance along the UNIT-normalised `dir` to the hit (in metres), so
    // point == origin + dir*t. `normal` is the outward surface normal at the
    // hit. `hit` is false when nothing is struck within `max_t`.
    struct RaycastHit {
        BodyId body{};
        f32 t = 0.0f;
        math::Vec3 point{0, 0, 0};
        math::Vec3 normal{0, 0, 0};
        bool hit = false;
    };

    // Cast a ray from `origin` along `dir` (any length; internally normalised)
    // up to `max_t` metres and return the NEAREST hit. Iterates live bodies,
    // skipping holes, the optional `ignore` body (the shooter), and bodies
    // whose broad AABB the ray misses, then runs the exact per-shape test:
    //   Sphere  -> ray-vs-sphere
    //   Capsule -> ray-vs-capsule (local +Y segment, swept-sphere)
    //   Box     -> ray-vs-OBB (oriented by the body rotation)
    //   ConvexHull / Compound / Heightfield / TriangleMesh -> ray-vs-OBB of the
    //       half_extent (documented Wave-A fallback until those shapes land).
    // Pure const read: no mutation, no heap allocation per call, no RNG. Safe
    // to call concurrently with OTHER const reads, but must not race step().
    [[nodiscard]] RaycastHit raycast(math::Vec3 origin,
                                     math::Vec3 dir,
                                     f32 max_t,
                                     BodyId ignore = {}) const noexcept;

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

   private:
    std::unique_ptr<detail::WorldImpl> impl_;
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
// Every vehicle function takes a trailing `World& world` that defaults to the
// legacy default world, so existing call sites compile unchanged while new
// callers (PlayRuntime) pass their OWN world. The vehicle's state lives in
// THAT world's vehicle sub-world — no shared global.
VehicleId create(const VehicleDesc& d, World& world = World::Get());
void destroy(VehicleId v, World& world = World::Get());
void set_throttle(VehicleId v, f32 t, World& world = World::Get());   // 0..1
void set_brake(VehicleId v, f32 b, World& world = World::Get());      // 0..1
void set_steer(VehicleId v, f32 angle, World& world = World::Get());  // radians
// Flat ground-plane height (world Y) the suspension rays contact against.
// Default 0. The oval test track in sample 04 is flat, so a single plane is
// sufficient; elevated terrain would need a per-wheel height probe instead.
void set_ground_plane(VehicleId v, f32 ground_y, World& world = World::Get());
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
// Same trailing-defaulted `World&` pattern as the vehicle functions: existing
// callers compile unchanged against the default world; new callers pass their
// own. The character's state + its collision queries operate on THAT world.
CharacterId create(const CharacterDesc& d, World& world = World::Get());
void destroy(CharacterId c, World& world = World::Get());
void move(CharacterId c, math::Vec3 delta, f32 dt, World& world = World::Get());
// Resolved capsule centre (world space) for a live character. Generation-
// checked: returns {0,0,0} for a stale / destroyed / invalid handle. Lets
// gameplay + the editor read a character's position without reaching into the
// internal character store (and without dragging the math/physics quat_rotate
// ADL ambiguity into their TU). Additive — the rest of the surface is frozen.
math::Vec3 get_position(CharacterId c, World& world = World::Get());
}  // namespace character

}  // namespace psynder::physics
