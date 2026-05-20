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
//
// Wave-B adds **per-channel priorities** (DESIGN.md §10.4). Once the AOI
// has gated by sphere, the next concern is fitting the peer's per-tick
// byte budget across the engine's logical channels (default / lockstep /
// snapshot / app-defined). `set_channel_priority(channel, prio)` raises
// or lowers a channel's share of that budget. Priority 2 is the default
// (full share); priority 1 halves the channel's slice; priority 4
// doubles it. Channels never seen by `set_channel_priority` use the
// default — i.e. existing call sites are unaffected.

#pragma once

#include "math/Math.h"
#include "core/Types.h"
#include "Net.h"  // PeerId

#include <array>
#include <unordered_map>

namespace psynder::net {

struct AoiPeerState {
    math::Vec3 centre{0.f, 0.f, 0.f};
    f32 radius = 0.f;
    f32 radius_sq = 0.f;  // cached squared radius for the hot loop.
};

// Default per-channel priority. Priorities are unitless; `2` is the
// neutral value so halving (→1) and doubling (→4) are clean integer ops.
inline constexpr u8 kDefaultChannelPriority = 2;

// Number of channels we track priorities for. Matches the four reserved
// channel ids in Frame.h (0=default, 1=lockstep, 2=snapshot, 3=reserved).
inline constexpr usize kAoiPriorityChannels = 4;

class AoiFilter {
   public:
    AoiFilter() noexcept { channel_priorities_.fill(kDefaultChannelPriority); }

    // Register / update the peer's interest sphere.
    void set_peer(PeerId p, math::Vec3 centre, f32 radius) noexcept;
    void remove_peer(PeerId p) noexcept;

    // Hot path: is entity at `pos` visible to peer `p`? Boundary is
    // inclusive: pos exactly on the sphere -> visible.
    bool visible(PeerId p, math::Vec3 pos) const noexcept;

    // For tests / introspection.
    bool has_peer(PeerId p) const noexcept { return peers_.count(p.raw) != 0; }
    usize peer_count() const noexcept { return peers_.size(); }

    // ── Per-channel priorities (Wave-B) ─────────────────────────────────
    //
    // Set the priority for `channel`. Higher prio == bigger share of the
    // per-peer byte budget when callers invoke `bytes_for_channel`. Values
    // outside [1, 255] are clamped. Channels >= kAoiPriorityChannels are
    // silently ignored — the engine only routes through the first four
    // channel ids (see Frame.h).
    void set_channel_priority(u8 channel, u8 prio) noexcept;

    // Read the priority for `channel`. Returns `kDefaultChannelPriority`
    // for never-set / out-of-range channels.
    u8 channel_priority(u8 channel) const noexcept;

    // Given a per-tick `base_budget_bytes` (the peer's overall cap), return
    // how many bytes this channel may consume.
    //
    //   bytes = base_budget * channel_priority / kDefaultChannelPriority
    //
    // So at the default (2), a channel gets the full budget. Setting
    // a channel to 1 halves it; 4 doubles it. The semantics give callers
    // a per-channel scaling factor while keeping the legacy single-budget
    // path simple.
    u32 bytes_for_channel(u8 channel, u32 base_budget_bytes) const noexcept;

   private:
    // Keyed by raw u32 from the Handle so we don't need a std::hash override.
    std::unordered_map<u32, AoiPeerState> peers_;
    std::array<u8, kAoiPriorityChannels> channel_priorities_{};
};

}  // namespace psynder::net
