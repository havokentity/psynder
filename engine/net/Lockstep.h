// SPDX-License-Identifier: MIT
// Psynder — lockstep tick coordinator. Lane 14 internal.
//
// Lockstep is the racing-game mode (8 players, perfect determinism — see
// DESIGN.md §10.4 and lane 13 for the deterministic physics that backs it).
// The contract:
//
//   1. Each peer posts its inputs for tick T into its outbound queue.
//   2. The host gathers inputs from ALL peers (including itself) for T.
//   3. Once every peer's tick-T inputs are present, the host advances to T;
//      the gathered inputs are broadcast to every peer as one bundle.
//   4. Clients only execute simulation tick T when they've received the
//      bundle for T. No bundle => sim stalls => "waiting for player N".
//
// This module owns ONLY the input-bundle coordination; physics determinism
// is lane 13's responsibility. We hand callers the input bundle for a tick
// and they feed it to the sim.

#pragma once

#include "core/Types.h"

#include <unordered_map>
#include <vector>

namespace psynder::net {

struct LockstepInput {
    u32              peer_index = 0;   // 0..max_peers-1 (NOT PeerId.raw)
    u32              tick       = 0;
    std::vector<u8>  payload;          // game-defined input blob
};

struct LockstepBundle {
    u32                          tick = 0;
    std::vector<LockstepInput>   inputs;   // one per peer, sorted by peer_index
    bool                         complete = false;
};

class LockstepCoordinator {
public:
    explicit LockstepCoordinator(u32 expected_peers) noexcept;

    // Register a peer's input for tick T. Returns true if the bundle for
    // T is now complete (i.e. all expected peers have posted).
    bool submit(LockstepInput input) noexcept;

    // True if every peer's input for `tick` has been received.
    bool is_ready(u32 tick) const noexcept;

    // Pull out the complete bundle for `tick`. Returns nullptr if not ready
    // (or already consumed). After this returns, the bundle is removed.
    LockstepBundle take_bundle(u32 tick) noexcept;

    u32 expected_peers() const noexcept { return expected_peers_; }

    // For tests.
    usize pending_buckets() const noexcept { return buckets_.size(); }

private:
    u32 expected_peers_;
    std::unordered_map<u32, LockstepBundle> buckets_;  // keyed by tick
};

}  // namespace psynder::net
