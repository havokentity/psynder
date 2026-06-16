// SPDX-License-Identifier: MIT
// Psynder physics — internal world state (DESIGN.md §10.1).
//
// The public `World` is now an INSTANCE-OWNED object (Physics.h holds a
// std::unique_ptr<detail::WorldImpl>). `WorldImpl` aggregates ALL physics
// state for one world: the rigid-body `WorldState` plus the per-world vehicle
// and character sub-worlds that used to be separate file-static singletons.
// Multiple `World` instances are therefore fully independent — no shared
// static state — which is the whole point of the refactor (multi-scene,
// deterministic test isolation, isolated Play sessions, future networking).
//
// Internal layout is the hot path's responsibility — bodies live in a
// contiguous std::vector with a free-list of holes so BodyId stays valid
// across removals.

#pragma once

#include "core/Types.h"
#include "math/Math.h"
#include "Body.h"
#include "Narrowphase.h"
#include "Broadphase.h"
#include "Solver.h"
#include "Vehicle.h"
#include "Character.h"

#include "jobs/JobSystem.h"

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

    // Island-solve dispatch scratch (Fix 2: zero-garbage). One job per island
    // is submitted every sub-tick at 120 Hz; these two vectors used to be
    // allocated fresh inside run_island_solve each tick. They now live here so
    // the loop only clears (never frees) them — capacity is retained, no
    // behaviour change. IslandJobCtx is defined where run_island_solve lives.
    struct IslandJobCtx {
        const Island* island;
        Contact* contacts_base;
        const u32* body_index_base;
        Body* bodies_base;
        usize bodies_count;
        const SolverParams* params;
        f32 dt;
        // ADR-013 colored solve: each concurrently-solved island owns one
        // pooled scratch (graph colouring buckets + constraint cache) so the
        // hot path heap-allocates nothing per frame. Indexed by island.
        kernels::ColoredIslandScratch* solver_scratch;
    };
    std::vector<IslandJobCtx> island_ctx_scratch;
    std::vector<jobs::JobHandle> island_handle_scratch;
    // One colored-solve scratch per island, pooled across frames (grown, never
    // shrunk). island_ctx_scratch[i].solver_scratch points into this.
    std::vector<kernels::ColoredIslandScratch> island_solver_scratch;

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

// All physics state for ONE world instance. The public `World` owns exactly
// one of these via a unique_ptr (PIMPL). The rigid-body state, the vehicle
// sub-world, and the character sub-world all live HERE — none of them are
// file-static singletons anymore, so two `World` instances never alias.
struct WorldImpl {
    WorldState state;
    VehicleWorld vehicles;
    CharacterWorld characters;
};

// Default-world sub-state accessors. These return the lazily-constructed
// default `World::Get()`'s sub-state, so legacy callers that reached for the
// global (the bench's world_state(), sample 09's character_world()) keep
// working UNCHANGED. They are NOT a separate global — they forward into the
// one default World instance. Defined in World.cpp (needs the full World type).
WorldState& world_state();
VehicleWorld& vehicle_world();
CharacterWorld& character_world();

}  // namespace psynder::physics::detail
