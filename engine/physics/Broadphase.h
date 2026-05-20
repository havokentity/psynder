// SPDX-License-Identifier: MIT
// Psynder physics — sweep-and-prune broadphase (DESIGN.md §10.1).
//
// Three parallel axis-pass jobs (one per cardinal axis) each return a list of
// overlapping pairs along their axis. A pair survives only if it overlaps on
// ALL three axes — we intersect the three lists into a global candidate list
// for the narrowphase. The 3-way intersection is the SAP trick that keeps the
// candidate-pair count near-linear in body count even when AABBs are dense.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::physics::detail {

struct AabbEntry {
    math::Vec3 min;
    math::Vec3 max;
    u32 body_index;
};

struct CandidatePair {
    u32 a;
    u32 b;
    constexpr bool operator==(const CandidatePair& o) const noexcept = default;
};

// Run the 3 axis-pass jobs in parallel via the job system. The output vector
// is sorted, unique, and contains exactly the pairs whose AABBs overlap on
// all three axes. Pairs are normalised so a < b.
void broadphase_sap(std::vector<AabbEntry>& aabbs, std::vector<CandidatePair>& out_pairs);

}  // namespace psynder::physics::detail
