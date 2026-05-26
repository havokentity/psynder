// SPDX-License-Identifier: MIT
// Psynder physics — joint solver, World-facing glue (DESIGN.md §10.8).
//
// The algorithm is header-only in Joints.h (so unit tests run it against a
// locally-staged Body array without linking psynder_physics). This TU is the
// only one that reaches into the world singleton: it maps the JointSet's body
// INDICES onto the live body vector and forwards to detail::joints::solve().
//
// Determinism: this file inherits the lane-wide -fno-fast-math gate from
// engine/physics/CMakeLists.txt, so a scripted call sequence reproduces
// bit-for-bit across hosts (same contract as the contact solver).

#include "physics/Joints.h"

#include "physics/FpControl.h"
#include "physics/WorldImpl.h"

#include <span>

namespace psynder::physics {

u32 body_index_of(BodyId id) noexcept {
    auto& w = detail::world_state();
    u32 idx = id.raw & 0x00FFFFFFu;
    if (idx >= w.bodies.size() || w.bodies[idx].gen == 0)
        return 0xFFFFFFFFu;
    return idx;
}

void solve_joints(World& world,
                  const JointSet& joints,
                  const JointSolverParams& params,
                  f32 dt,
                  u32 iterations) {
    (void)world;  // single global world; reached via world_state()
    if (joints.joints.empty() || dt <= 0.0f)
        return;

    auto& w = detail::world_state();
    detail::FpGuard fp_guard;  // pin round-to-nearest for the pass, like step()

    std::span<detail::Body> bodies{w.bodies.data(), w.bodies.size()};
    std::span<const Joint> js{joints.joints.data(), joints.joints.size()};
    detail::joints::solve(bodies, js, params, dt, iterations);
}

void solve_joints(World& world, const JointSet& joints, f32 dt, u32 iterations) {
    solve_joints(world, joints, JointSolverParams{}, dt, iterations);
}

}  // namespace psynder::physics
