// SPDX-License-Identifier: MIT
// Psynder — Area-Of-Interest filter for snapshot streaming. Lane 14 internal.
//
// For 16-32 player FPS / tactical FPS on large outdoor maps we can't ship
// every entity's state to every peer. The AOI filter holds each peer's
// "interest sphere" (centre + radius) and answers "should this entity get
// included in peer P's next snapshot?". Boundary inclusion is INCLUSIVE on
// the radius so the test at the edge is deterministic.
//
// AOI is intentionally peer-centric, not entity-centric: callers usually
// iterate "for each peer, for each entity, ask aoi.visible(...)". The peer
// state is keyed by PeerId.

#pragma once

#include "math/Math.h"
#include "core/Types.h"
#include "Net.h"  // PeerId

#include <unordered_map>

namespace psynder::net {

struct AoiPeerState {
    math::Vec3 centre{0.f, 0.f, 0.f};
    f32        radius   = 0.f;
    f32        radius_sq = 0.f;  // cached squared radius for the hot loop.
};

class AoiFilter {
public:
    AoiFilter() noexcept = default;

    // Register / update the peer's interest sphere.
    void set_peer(PeerId p, math::Vec3 centre, f32 radius) noexcept;
    void remove_peer(PeerId p) noexcept;

    // Hot path: is entity at `pos` visible to peer `p`? Boundary is
    // inclusive: pos exactly on the sphere -> visible.
    bool visible(PeerId p, math::Vec3 pos) const noexcept;

    // For tests / introspection.
    bool has_peer(PeerId p) const noexcept { return peers_.count(p.raw) != 0; }
    usize peer_count() const noexcept { return peers_.size(); }

private:
    // Keyed by raw u32 from the Handle so we don't need a std::hash override.
    std::unordered_map<u32, AoiPeerState> peers_;
};

}  // namespace psynder::net
