// SPDX-License-Identifier: MIT
// Psynder physics — island detection + sequential-impulse PGS solver
// (DESIGN.md §10.1). One solver job per island; islands are independent so
// the job graph parallelism is embarrassing.
//
// Sequential-impulse / projected Gauss-Seidel is Erin Catto's GDC 2006 / 2014
// recipe. Per contact each iteration we:
//   1. Compute relative velocity along normal, project a non-negative impulse
//      that brings the constraint to zero, accumulate into the warm-start
//      cache, apply the delta to body velocities.
//   2. Same for two friction directions, clipped to the cone bound by the
//      accumulated normal impulse * coefficient.
// 8 velocity iterations + 3 position-correction (split-impulse) iterations.

#pragma once

#include "core/Types.h"
#include "Body.h"
#include "Narrowphase.h"

#include <span>
#include <vector>

namespace psynder::physics::detail {

struct Island {
    u32 first_contact = 0;  // index into solver's contact vector
    u32 contact_count = 0;
    u32 first_body = 0;  // index into solver's body-id vector
    u32 body_count = 0;
};

// Union-find over the contact graph; populates `islands` and partitions the
// `contacts` slice / `body_indices` slice contiguously by island. Static
// bodies do not propagate union (they connect at most one dynamic island at
// a time to themselves).
void detect_islands(std::vector<Contact>& contacts,
                    std::span<const Body> bodies,
                    std::vector<u32>& body_indices,
                    std::vector<Island>& islands);

// Solver parameters; tweakable per-world but never per-island.
struct SolverParams {
    u32 velocity_iterations = 8;
    u32 position_iterations = 3;
    f32 gravity_y = -9.81f;            // m/s^2
    f32 baumgarte = 0.20f;             // position-correction blend [0..1]
    f32 slop = 0.005f;                 // 5 mm penetration tolerance
    f32 restitution_threshold = 1.0f;  // m/s — below this, restitution=0
};

// Solve one island in place. Reads body velocities, writes corrected
// velocities + (optional) position deltas via split-impulse. Designed for
// invocation as a single job per island in parallel.
void solve_island(const Island& island,
                  std::span<Contact> contacts,
                  std::span<const u32> body_indices,
                  std::span<Body> bodies,
                  const SolverParams& params,
                  f32 dt) noexcept;

}  // namespace psynder::physics::detail
