// SPDX-License-Identifier: MIT
// Psynder physics — solver TU. Thin forwarder: union-find island detection
// and per-island sequential-impulse PGS both live header-only in
// `internal/Kernels.h` so they're testable without linking the lane lib.

#include "Solver.h"
#include "internal/Kernels.h"

#include <vector>

namespace psynder::physics::detail {

void detect_islands(std::vector<Contact>& contacts,
                    std::span<const Body> bodies,
                    std::vector<u32>& body_indices,
                    std::vector<Island>& islands) {
    kernels::kernel_detect_islands(contacts, bodies, body_indices, islands);
}

void solve_island(const Island& island,
                  std::span<Contact> contacts,
                  std::span<const u32> body_indices,
                  std::span<Body> bodies,
                  const SolverParams& params,
                  f32 dt) noexcept {
    kernels::kernel_solve_island(island, contacts, body_indices, bodies, params, dt);
}

}  // namespace psynder::physics::detail
