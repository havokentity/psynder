// SPDX-License-Identifier: MIT
// Psynder — AOI filter impl. Lane 14 internal.

#include "Aoi.h"

#include <algorithm>

namespace psynder::net {

void AoiFilter::set_peer(PeerId p, math::Vec3 centre, f32 radius) noexcept {
    AoiPeerState& s = peers_[p.raw];
    s.centre = centre;
    s.radius = radius;
    s.radius_sq = radius * radius;
}

void AoiFilter::remove_peer(PeerId p) noexcept {
    peers_.erase(p.raw);
}

bool AoiFilter::visible(PeerId p, math::Vec3 pos) const noexcept {
    auto it = peers_.find(p.raw);
    if (it == peers_.end())
        return false;
    const AoiPeerState& s = it->second;
    const f32 dx = pos.x - s.centre.x;
    const f32 dy = pos.y - s.centre.y;
    const f32 dz = pos.z - s.centre.z;
    const f32 d2 = dx * dx + dy * dy + dz * dz;
    // Inclusive: on-boundary == visible.
    return d2 <= s.radius_sq;
}

math::Vec3 AoiFilter::peer_centre(PeerId p) const noexcept {
    auto it = peers_.find(p.raw);
    if (it == peers_.end())
        return math::Vec3{0.f, 0.f, 0.f};
    return it->second.centre;
}

void AoiFilter::set_channel_priority(u8 channel, u8 prio) noexcept {
    if (channel >= kAoiPriorityChannels)
        return;
    // Clamp to [1, 255]; a priority of 0 would zero out the channel
    // entirely, which is almost certainly a caller bug.
    if (prio == 0)
        prio = 1;
    channel_priorities_[channel] = prio;
}

u8 AoiFilter::channel_priority(u8 channel) const noexcept {
    if (channel >= kAoiPriorityChannels)
        return kDefaultChannelPriority;
    return channel_priorities_[channel];
}

u32 AoiFilter::bytes_for_channel(u8 channel, u32 base_budget_bytes) const noexcept {
    // Scale by priority / default. Integer math: priority 1 ⇒ /2; 2 ⇒ ×1;
    // 4 ⇒ ×2. Multiplying first preserves precision for small budgets.
    const u32 prio = channel_priority(channel);
    return u32((u64(base_budget_bytes) * prio) / kDefaultChannelPriority);
}

}  // namespace psynder::net
