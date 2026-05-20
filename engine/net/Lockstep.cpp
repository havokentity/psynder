// SPDX-License-Identifier: MIT
// Psynder — lockstep coordinator impl. Lane 14 internal.

#include "Lockstep.h"

#include <algorithm>
#include <utility>

namespace psynder::net {

LockstepCoordinator::LockstepCoordinator(u32 expected_peers) noexcept
    : expected_peers_(expected_peers) {}

bool LockstepCoordinator::submit(LockstepInput input) noexcept {
    LockstepBundle& b = buckets_[input.tick];
    b.tick = input.tick;
    // Dedup by peer_index — replace if a peer re-submits for the same tick.
    auto it = std::find_if(b.inputs.begin(), b.inputs.end(), [&](const LockstepInput& x) {
        return x.peer_index == input.peer_index;
    });
    if (it != b.inputs.end()) {
        *it = std::move(input);
    } else {
        b.inputs.push_back(std::move(input));
    }
    b.complete = (b.inputs.size() == expected_peers_);
    if (b.complete) {
        // Sort by peer_index for deterministic playback order.
        std::sort(b.inputs.begin(), b.inputs.end(), [](const LockstepInput& a, const LockstepInput& c) {
            return a.peer_index < c.peer_index;
        });
    }
    return b.complete;
}

bool LockstepCoordinator::is_ready(u32 tick) const noexcept {
    auto it = buckets_.find(tick);
    return it != buckets_.end() && it->second.complete;
}

LockstepBundle LockstepCoordinator::take_bundle(u32 tick) noexcept {
    auto it = buckets_.find(tick);
    if (it == buckets_.end() || !it->second.complete) {
        return {};
    }
    LockstepBundle out = std::move(it->second);
    buckets_.erase(it);
    return out;
}

}  // namespace psynder::net
