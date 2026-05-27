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
#include <vector>

namespace psynder::physics {

namespace {
// World-aware resolve: validate a BodyId against the GIVEN world's body store
// (full generation check). The public body_index_of() delegates to the default
// world so its frozen signature is preserved; solve_joints() uses the world it
// was handed so a joint set is solved against the right world's bodies.
u32 body_index_in(detail::WorldState& w, BodyId id) noexcept {
    const u32 idx = detail::handle_index(id.raw);
    // Validate the FULL generation: a stale handle whose slot was recycled
    // must NOT silently resolve to the new occupant (UAF-class aliasing).
    if (idx >= w.bodies.size() || w.bodies[idx].alive == 0 ||
        w.bodies[idx].gen != detail::handle_gen(id.raw))
        return 0xFFFFFFFFu;
    return idx;
}
}  // namespace

u32 body_index_of(BodyId id) noexcept {
    return body_index_in(detail::world_state(), id);
}

void solve_joints(World& world,
                  const JointSet& joints,
                  const JointSolverParams& params,
                  f32 dt,
                  u32 iterations) {
    if (joints.joints.empty() || dt <= 0.0f)
        return;

    // Operate on the world we were handed — NOT a global. This is what lets a
    // per-scene / per-Play World solve its own joints in isolation.
    auto& w = world.internal().state;
    detail::FpGuard fp_guard;  // pin round-to-nearest for the pass, like step()

    // Resolve gen-checked handles to live indices each solve. A joint that
    // carries valid BodyIds (joint_with_handles) is re-bound to the slot its
    // handle currently names — and SKIPPED entirely if either handle is stale,
    // so a destroyed-and-recycled body never gets pulled by a dangling joint.
    // Joints authored with raw indices (body_id_* invalid) pass through as-is.
    std::vector<Joint> resolved;
    resolved.reserve(joints.joints.size());
    for (const Joint& j : joints.joints) {
        const bool has_handles = j.body_id_a.valid() || j.body_id_b.valid();
        if (!has_handles) {
            resolved.push_back(j);
            continue;
        }
        const u32 ia = body_index_in(w, j.body_id_a);
        const u32 ib = body_index_in(w, j.body_id_b);
        if (ia == 0xFFFFFFFFu || ib == 0xFFFFFFFFu)
            continue;  // stale handle on either end — drop this joint
        Joint r = j;
        r.body_a = ia;
        r.body_b = ib;
        resolved.push_back(r);
    }
    if (resolved.empty())
        return;

    std::span<detail::Body> bodies{w.bodies.data(), w.bodies.size()};
    std::span<const Joint> js{resolved.data(), resolved.size()};
    detail::joints::solve(bodies, js, params, dt, iterations);
}

void solve_joints(World& world, const JointSet& joints, f32 dt, u32 iterations) {
    solve_joints(world, joints, JointSolverParams{}, dt, iterations);
}

}  // namespace psynder::physics
