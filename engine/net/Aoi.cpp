// SPDX-License-Identifier: MIT
// Psynder — AOI filter impl. Lane 14 internal.

#include "Aoi.h"

namespace psynder::net {

void AoiFilter::set_peer(PeerId p, math::Vec3 centre, f32 radius) noexcept {
    AoiPeerState& s = peers_[p.raw];
    s.centre    = centre;
    s.radius    = radius;
    s.radius_sq = radius * radius;
}

void AoiFilter::remove_peer(PeerId p) noexcept {
    peers_.erase(p.raw);
}

bool AoiFilter::visible(PeerId p, math::Vec3 pos) const noexcept {
    auto it = peers_.find(p.raw);
    if (it == peers_.end()) return false;
    const AoiPeerState& s = it->second;
    const f32 dx = pos.x - s.centre.x;
    const f32 dy = pos.y - s.centre.y;
    const f32 dz = pos.z - s.centre.z;
    const f32 d2 = dx*dx + dy*dy + dz*dz;
    // Inclusive: on-boundary == visible.
    return d2 <= s.radius_sq;
}

}  // namespace psynder::net
