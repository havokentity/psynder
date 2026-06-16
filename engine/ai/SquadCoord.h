// SPDX-License-Identifier: MIT
// Psynder — M-AI squad / flanking coordination (DOTS, deterministic, alloc-free).
//
// THE PROBLEM (W13-3): today each agent paths to its target INDEPENDENTLY, so a
// group engaging the SAME entity stacks up and approaches single-file down one
// corridor. Real squads SPREAD OUT and surround — one or two pin the target from
// the front while the rest flank from the sides. This module adds a lightweight
// coordination layer ON TOP of the existing perceive -> think -> navigate -> act
// pipeline: agents sharing a target are handed DISTINCT approach SLOTS arranged
// around it, and each then routes / steers to its slot's offset position near the
// target rather than to the target's exact cell. The result: the squad fans out
// and flanks instead of bunching.
//
// WHERE IT PLUGS IN: `navigate` (and the no-grid `act` fallback) resolves an
// agent's Chase goal as the target's last-seen position. With the squad layer ON
// (AiContext::squad_flanking), that goal is replaced by squad_flank_goal(...),
// which offsets it to the agent's assigned slot. EVERYTHING ELSE — perception,
// the FSM, A* routing, path following, separation, the host hooks — is unchanged.
// With the flag OFF (the default) this module is never consulted and behaviour is
// bit-for-bit the old single-agent path (so every existing host + test regresses
// to nothing).
//
// DETERMINISM (no RNG, no clock):
//   * Two agents belong to the same SQUAD iff they are both Chase/Attack and hold
//     the SAME target_entity. The engaging set is discovered per-tick by a nested
//     read-only query (re-entrant + alloc-free, exactly the pattern perceive's
//     nearest-hostile + act's separation_nudge already use), so it recomputes
//     automatically as agents acquire / lose / change the target.
//   * Within a squad the agents are RANKED by their stable Entity id
//     (Entity::index(), the generation-free low bits — stable across a frame and
//     re-acquisition). Rank r of group size N maps to an evenly-spaced angular
//     slot; equal ids cannot occur (one id per entity), so the rank — and hence
//     the slot — is a pure function of the engaging id set. Same ids in => same
//     slots out, byte-for-byte, independent of thread scheduling.
//   * The slot ring is anchored to the APPROACH AXIS: the direction from the
//     target back toward the squad's id-lowest member (a deterministic anchor, NOT
//     a centroid that would depend on live positions another worker is mutating).
//     Slot 0 sits on that axis (the "pin" lane); the rest alternate left/right of
//     it at growing angles, so a 3-agent squad gets centre + left + right.
//
// ALLOC-FREE / PARALLEL-SAFE: no owned containers; the engaging-set scan reduces
// onto the calling body's stack. Each agent computes ONLY its own goal offset
// (per-row), so there is no cross-row mutation to guard — same contract as the
// rest of engine/ai. Host-agnostic: depends only on scene / math / the AI
// components, never render / host / physics / net.
//
// (See DESIGN.md ADR-023 for the scheme rationale + the suppress-vs-flank role
// split.)

#pragma once

#include "ai/AiComponents.h"
#include "core/Types.h"
#include "math/Math.h"
#include "scene/EcsRegistry.h"

namespace psynder::ai {

using ::psynder::Entity;
using ::psynder::f32;
using ::psynder::u32;

// ─── SquadConfig ────────────────────────────────────────────────────────────
// Tuning for the flank ring. Lives on the AiContext (set once, like the hooks);
// all fields have sane defaults so a host only flips `enabled`.
struct SquadConfig {
    // Master switch. OFF (default) => the whole layer is a no-op and goals are the
    // raw target position, i.e. the unchanged single-agent behaviour. ON => Chase
    // goals are offset to per-agent flank slots.
    bool enabled = false;
    u8 _pad[3] = {};

    // Radius (metres) of the slot ring around the target: each agent aims for a
    // point this far out from the target, on its slot's bearing, so the squad
    // forms a loose arc/circle around the target instead of a point.
    f32 flank_radius = 6.0f;

    // Half-angle (radians) of the widest flank. The N slots are spread across
    // [-spread, +spread] off the approach axis. ~70 deg by default: slot 0 pins
    // head-on, the outermost slots come in near the target's flanks.
    f32 spread = 1.22f;  // ~70 degrees

    // Maximum squad size the ring is laid out for. A squad larger than this still
    // works (ranks past the cap wrap), but the angular spacing is computed against
    // this cap so spacing stays stable as members drop in/out near the cap. Small
    // fixed bound keeps the layout O(1) and the math branch-free.
    u32 max_slots = 8u;

    // ── Suppress-vs-flank role split ─────────────────────────────────────────
    // When >0, the `suppressors` id-lowest members of a squad are NOT pushed onto
    // the flank ring: they hold their head-on line (goal = the raw target pos) and
    // pin/fire while the remaining members flank. 1 is the classic "one holds,
    // the rest flank". 0 disables the split (every member flanks to a ring slot).
    u32 suppressors = 1u;
};

[[nodiscard]] inline SquadConfig sanitize_squad_config(SquadConfig c) noexcept {
    c._pad[0] = c._pad[1] = c._pad[2] = 0u;
    if (!(c.flank_radius >= 0.0f)) c.flank_radius = 0.0f;
    if (!(c.spread >= 0.0f)) c.spread = 0.0f;
    if (c.spread > math::kPi) c.spread = math::kPi;
    if (c.max_slots == 0u) c.max_slots = 1u;
    return c;
}

// ─── SquadAssignment ────────────────────────────────────────────────────────
// The result of placing one agent in its squad: the agent's rank within the
// engaging set (0-based, id-sorted), the engaging set size, the agent's slot
// index, and whether it is a SUPPRESSOR (holds head-on) vs a FLANKER (rings out).
// Returned by assign_slot(); a plain POD for the tests to assert on.
struct SquadAssignment {
    u32 group_size = 1u;  // number of agents engaging the same target (>=1)
    u32 rank = 0u;        // this agent's id-sorted rank in that group (0..group_size-1)
    u32 slot = 0u;        // slot index on the ring (== rank, clamped to the cap)
    bool suppressor = false;  // true => holds head-on (no flank offset)
    u8 _pad[3] = {};
};

// ─── count_engaging / assign_slot ───────────────────────────────────────────
// Scan the engaging set for `self`'s target and compute `self`'s rank + group
// size. An agent ENGAGES `target` when it is Chase or Attack and its
// target_entity == target. Only `self`'s own target is considered the squad key;
// agents on a different target form a different squad. Deterministic: rank = the
// count of engaging members whose stable id is STRICTLY LESS than self's id (a
// total order on a unique-id set), so the same id set always yields the same
// ranks. Alloc-free (nested read-only query, reduces onto the caller's stack).
//
// Returns a SquadAssignment. When `self` is not actually engaging (no valid
// target, or not Chase/Attack), group_size is 1 / rank 0 / suppressor true — i.e.
// it is treated as a lone head-on agent (no flank offset), so a caller can apply
// the offset unconditionally and a non-engaging agent is simply unaffected.
[[nodiscard]] SquadAssignment assign_slot(scene::EcsRegistry& registry,
                                          const SquadConfig& cfg,
                                          Entity self,
                                          const AiAgentComponent& agent);

// ─── slot_bearing ───────────────────────────────────────────────────────────
// Map a slot index (0..group_size-1) to its signed angular offset (radians) off
// the approach axis. Slot 0 -> 0 (head-on). Then it alternates outward:
// 1 -> +a, 2 -> -a, 3 -> +2a, 4 -> -2a, ... so the ring fills symmetrically
// left/right. The step `a` is spread / max(1, half-span) where half-span is the
// number of distinct magnitudes the group needs — clamped so a squad never folds
// two members onto the same bearing. Pure function of (slot, group_size, cfg).
[[nodiscard]] f32 slot_bearing(u32 slot, u32 group_size, const SquadConfig& cfg) noexcept;

// ─── squad_flank_goal ───────────────────────────────────────────────────────
// The whole layer in one call: given the agent, its target's world position, and
// the agent's own world position, return the world GOAL the agent should head for
// — the flank-slot offset of the target when the squad layer is engaged, or the
// raw target position when it is not (flag off / lone agent / suppressor role).
//
// The slot offset is built around the APPROACH AXIS = the horizontal direction
// from the target toward the SQUAD ANCHOR (the id-lowest engaging member's
// position), rotated by the slot's bearing about world +Y and pushed out by
// flank_radius. Anchoring to the id-lowest member (not a live centroid) keeps it
// deterministic + race-free: every member computes the SAME axis from the SAME
// anchor id, regardless of which worker runs which row. The anchor position is
// read from the anchor's snapshot/transform via the nested query; for the anchor
// itself the axis falls back to (target -> self).
//
// `self_pos` is only used as the axis fallback when target and anchor coincide.
// Alloc-free; one nested read-only query (the assign_slot scan also yields the
// anchor id + position in the same pass, so this is a single sweep).
[[nodiscard]] math::Vec3 squad_flank_goal(scene::EcsRegistry& registry,
                                          const SquadConfig& cfg,
                                          Entity self,
                                          const AiAgentComponent& agent,
                                          math::Vec3 target_pos,
                                          math::Vec3 self_pos);

}  // namespace psynder::ai
