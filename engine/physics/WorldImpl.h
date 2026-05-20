// SPDX-License-Identifier: MIT
// Psynder physics — internal world state (DESIGN.md §10.1).
//
// One global World singleton mirrors the public API. Internal layout is the
// hot path's responsibility — bodies live in a contiguous std::vector with a
// free-list of holes so BodyId stays valid across removals.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "Body.h"
#include "Narrowphase.h"
#include "Broadphase.h"
#include "Solver.h"

#include <mutex>
#include <vector>

namespace psynder::physics::detail {

struct WorldState {
    std::vector<Body> bodies;
    std::vector<u32> free_slots;
    math::Vec3 gravity{0.0f, -9.81f, 0.0f};

    // Per-step scratch — preserved across frames so we don't re-allocate.
    std::vector<AabbEntry> aabb_scratch;
    std::vector<CandidatePair> pair_scratch;
    std::vector<Contact> contact_scratch;
    std::vector<u32> island_body_indices;
    std::vector<Island> islands;

    SolverParams solver{};

    // Fixed-timestep accumulator (DESIGN.md §10.1: 120 Hz sim, interpolated
    // render). step(dt) advances the sim by N fixed sub-ticks; the residual
    // alpha is exposed via interpolation_alpha() for render lerping.
    f32 accumulator = 0.0f;
    f32 alpha = 0.0f;
    static constexpr f32 kFixedDt = 1.0f / 120.0f;

    // Mutation guard. The public API is documented as single-thread; this
    // mutex catches accidental cross-thread misuse without slowing the hot
    // path (we only take it from create_body / destroy_body, not step).
    std::mutex mutate;
};

// Singleton accessor — used by World::Get(), the character module, the
// vehicle module, and tests. Header-only so no ABI surface beyond the lane.
WorldState& world_state();

}  // namespace psynder::physics::detail
