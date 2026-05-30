// SPDX-License-Identifier: MIT
// Psynder physics — island detection + sequential-impulse PGS solver
// (DESIGN.md §10.1). One solver job per island; islands are independent so
// the job graph parallelism is embarrassing.
//
// Projected Gauss-Seidel (Erin Catto's GDC 2006 / 2014 recipe). Per contact
// each velocity iteration we:
//   1. Compute relative velocity along normal, project a non-negative impulse
//      that brings the constraint to zero, accumulate into the warm-start
//      cache, apply the delta to body velocities.
//   2. Same for two friction directions, clipped to the cone bound by the
//      accumulated normal impulse * coefficient.
// 8 velocity iterations + 3 position-correction (split-impulse) iterations.
//
// As of ADR-013 (DESIGN.md §16) the per-island solve is GRAPH-COLORED so it can
// run a colour's disjoint-body contacts in parallel while staying Gauss-Seidel
// across colours. `solve_island` is the serial colored walk (tests + small
// islands); `solve_island_colored` takes a per-colour dispatcher (the job
// system's parallel_for for large islands) and is bit-identical to the serial
// walk by construction (disjoint bodies => order-free). See SolverColoring.h.

#pragma once

#include "core/Types.h"
#include "Body.h"
#include "Narrowphase.h"
#include "internal/SolverColoring.h"

#include <functional>
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

// Per-colour batch dispatcher type (defined in SolverColoring.h): the colored
// solver hands it a contact count + a body functor and it invokes the functor
// over sub-ranges of [0,count). World.cpp binds it to the job system's
// parallel_for for large islands, or the serial dispatcher for small ones.
using kernels::ColorBatchDispatch;
// Pooled per-island scratch for the colored solve (zero per-frame heap).
using kernels::ColoredIslandScratch;

// ADR-013 colored projected-Gauss-Seidel solve for one island. Builds a
// deterministic graph colouring (two constraints differ in colour iff they
// share a dynamic body), then solves colour-by-colour (sequential, Gauss-Seidel
// across colours) dispatching each colour's disjoint-body batch through
// `batch`. Bit-identical whether `batch` is serial or parallel_for-backed, and
// deterministic run-to-run. `scratch` is caller-pooled. See SolverColoring.h
// and DESIGN.md §16 ADR-013.
void solve_island_colored(const Island& island,
                          std::span<Contact> contacts,
                          std::span<const u32> body_indices,
                          std::span<Body> bodies,
                          const SolverParams& params,
                          f32 dt,
                          ColoredIslandScratch& scratch,
                          const ColorBatchDispatch& batch);

}  // namespace psynder::physics::detail
