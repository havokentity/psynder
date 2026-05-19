// SPDX-License-Identifier: MIT
// Psynder — snapshot streaming / interpolation. Lane 14 internal.
//
// Client-server snapshot interpolation for 16-32 player FPS / tactical FPS
// (DESIGN.md §10.4). The server composes a per-peer snapshot each tick,
// running every entity through the AOI filter and emitting only the visible
// subset. Clients buffer at least two snapshots and interpolate between
// them at render time.
//
// On the wire each snapshot is a small header + a packed array of entity
// records. We keep the record minimal in Wave A (id + position + 1 u32 of
// game-defined state). Lane 14's job is the framing + AOI; specific
// payload schemas are owned by the game's gameplay layer in Wave B.

#pragma once

#include "Aoi.h"
#include "core/Types.h"
#include "math/Math.h"

#include <span>
#include <vector>

namespace psynder::net {

struct SnapshotEntity {
    u32         entity_id = 0;
    math::Vec3  position{0.f, 0.f, 0.f};
    u32         state_bits = 0;
};

struct SnapshotFrame {
    u32                          tick = 0;
    std::vector<SnapshotEntity>  entities;
};

// Pack a SnapshotFrame into a flat byte buffer. Returns the size written.
// `out` must have room for kSnapshotHeaderBytes + entities.size()*kSnapshotEntityBytes.
inline constexpr usize kSnapshotHeaderBytes = 8;   // tick (4) + count (4)
inline constexpr usize kSnapshotEntityBytes = 20;  // id (4) + xyz (12) + state (4)

usize encode_snapshot(const SnapshotFrame& f, std::span<u8> out) noexcept;
bool  decode_snapshot(std::span<const u8> in, SnapshotFrame& out) noexcept;

// Compose a snapshot for peer `p` from `world` (all entities) by filtering
// through `aoi`. `world` is read-only; only visible entries make it into
// `out_frame`.
void compose_for_peer(const SnapshotFrame& world,
                      const AoiFilter&     aoi,
                      PeerId               p,
                      SnapshotFrame&       out_frame) noexcept;

// Linear-interpolate two snapshots into `out`. `t` in [0,1]. Entities not
// present in both frames pass through from whichever frame holds them.
void interpolate(const SnapshotFrame& a, const SnapshotFrame& b, f32 t,
                 SnapshotFrame& out) noexcept;

}  // namespace psynder::net
