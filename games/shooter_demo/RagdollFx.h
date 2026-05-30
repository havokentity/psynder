// SPDX-License-Identifier: MIT
// Psynder shooter_demo -- W12-4 death-ragdoll pool (games-only).
//
// A thin, pooled wrapper around the engine's physics ragdoll primitive
// (engine/physics/Ragdoll.h) for the shooter demo's "enemy dies -> corpse flops"
// payoff. It is deliberately split into its OWN translation unit so that the
// heavy engine header physics/Ragdoll.h (which transitively pulls
// physics/Joints.h -> physics/Shape.h) is NEVER compiled in the same TU as
// scene/SceneEcs.h. Shape.h declares physics::detail::quat_rotate and calls it
// unqualified at namespace scope; scene/SceneEcs.h drags in math/MathExt.h's
// math::quat_rotate; with BOTH visible those two unqualified calls in Shape.h
// become an ADL-ambiguous overload set and the TU fails to compile. main.cpp
// needs SceneEcs.h (it owns the scene), so the ragdoll code that needs Ragdoll.h
// lives here instead, behind a Shape.h-free interface. See the report's
// "missing engine hook" note: Shape.h:71/75 are not ADL-safe (lines 52/53 ARE
// qualified, those two are not) -- a latent engine header bug worked around here
// without touching engine/*.
//
// The pool pre-sizes to a fixed corpse count; building a corpse reuses an idle
// slot (the underlying physics::Ragdoll keeps its vector capacity across reuse),
// so steady-state spawning is allocation-free. Determinism: spawn pose + impulse
// are pure functions of their inputs (no RNG / no time), inheriting the engine
// ragdoll's bit-for-bit -fno-fast-math guarantee.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "physics/Physics.h"  // physics::World / BodyId (NOT Joints/Shape/Ragdoll)

#include <array>
#include <memory>
#include <span>

namespace shooter {

using ::psynder::f32;
using ::psynder::u32;
using ::psynder::usize;

// Max simultaneous corpses + bodies per corpse (engine default_humanoid == 7).
inline constexpr u32 kMaxCorpses = 3u;
inline constexpr u32 kCorpseBodies = 7u;

// Per-body render transform handed back to the caller so it can drive a scene
// mesh instance per limb. Pure data -- no engine physics headers leak through.
struct LimbTransform {
    psynder::math::Vec3 position{0.0f, 0.0f, 0.0f};
    psynder::math::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    psynder::math::Vec3 box_scale{0.1f, 0.1f, 0.1f};  // render box size for this limb
    bool valid = false;
};

// Result of stepping one corpse: enough for the caller's render + settle gate.
struct CorpseState {
    bool active = false;
    bool finite = true;       // no NaN/Inf in any body
    bool grounded = true;     // every body above the floor
    bool settled = false;     // came to rest (debounced)
    f32 min_y = 0.0f;         // lowest body-centre Y
    u32 body_count = 0u;
};

// The pooled corpse manager. Owns a shared humanoid template, a static floor slab
// in the supplied World, and kMaxCorpses physics ragdolls. PIMPL so this header
// stays free of physics/Ragdoll.h (and thus Shape.h). Construct one, call
// init(world, floor_y, room half-extents) once at load, then spawn/step per
// frame and read back limb transforms to render.
class RagdollPool {
   public:
    RagdollPool();
    ~RagdollPool();
    RagdollPool(const RagdollPool&) = delete;
    RagdollPool& operator=(const RagdollPool&) = delete;

    // Build the shared template + a static floor box centred at (cx,floor_y,cz)
    // spanning (sx,sz) so corpses settle on it. Idempotent-safe to call once.
    void init(psynder::physics::World& world,
              f32 floor_y,
              f32 cx, f32 cz, f32 sx, f32 sz);

    // Spawn a corpse at `pose` (root translation + rotation) with `impulse` seeded
    // on every limb. Returns the slot index [0,kMaxCorpses) or kNoSlot if full.
    // The spawn is deterministic in its arguments.
    static constexpr u32 kNoSlot = 0xFFFFFFFFu;
    u32 spawn(psynder::physics::World& world,
              const psynder::math::Vec3& root_position,
              const psynder::math::Quat& root_rotation,
              const psynder::math::Vec3& impulse);

    // Advance every active corpse's joint solve (call AFTER world.step(dt)) and
    // update each one's settle/grounded/finite state. dt in seconds.
    void step(psynder::physics::World& world, f32 dt);

    // Read back the per-limb render transforms for slot `i`. Fills up to
    // out.size() entries; the rest are left invalid. No-op for an idle slot.
    void limb_transforms(psynder::physics::World& world,
                         u32 slot,
                         std::span<LimbTransform> out) const;

    // Current state of slot `i` (active/finite/grounded/settled/min_y).
    CorpseState state(u32 slot) const;

    u32 spawned_total() const noexcept { return spawned_total_; }
    static constexpr u32 max_corpses() noexcept { return kMaxCorpses; }
    static constexpr u32 body_count() noexcept { return kCorpseBodies; }

    // Free every active corpse's physics bodies. Call at shutdown.
    void teardown(psynder::physics::World& world);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    u32 spawned_total_ = 0u;
};

}  // namespace shooter
