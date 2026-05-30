// SPDX-License-Identifier: MIT
// Psynder — M-AI squad / flanking coordination implementation.

#include "ai/SquadCoord.h"

#include "scene/SceneEcs.h"

#include <cmath>
#include <span>

namespace psynder::ai {

using namespace ::psynder::scene;

namespace {

// An agent is ENGAGING when its FSM is closing on / firing at a known target.
[[nodiscard]] bool is_engaging(const AiAgentComponent& a) noexcept {
    return (a.state == AiState::Chase || a.state == AiState::Attack) &&
           a.target_entity.valid();
}

// One pass over every agent that shares `target`: count the engaging set, find
// this agent's id-sorted rank (members with a STRICTLY smaller stable id), and
// capture the id-lowest member as the deterministic squad ANCHOR (with its
// position). All reduced onto the caller's stack — alloc-free, re-entrant,
// thread-safe (read-only nested query, same pattern perceive/separation use).
struct SquadScan {
    u32 group_size = 0u;
    u32 rank = 0u;
    Entity anchor{};            // id-lowest engaging member
    math::Vec3 anchor_pos{};    // anchor's world position
    bool anchor_has_pos = false;
};

[[nodiscard]] SquadScan scan_squad(EcsRegistry& registry, Entity self, Entity target) {
    SquadScan out{};
    const u32 self_id = self.index();
    u32 best_anchor_id = 0xFFFFFFFFu;
    registry.query<reads<SceneNodeComponent, TransformComponent, AiAgentComponent>,
                   writes<>>(
        [&](std::span<const SceneNodeComponent> nodes,
            std::span<const TransformComponent> xforms,
            std::span<const AiAgentComponent> agents) {
            const usize n = std::min({nodes.size(), xforms.size(), agents.size()});
            for (usize i = 0; i < n; ++i) {
                const AiAgentComponent& a = agents[i];
                if (!is_engaging(a) || a.target_entity != target)
                    continue;
                const Entity e = nodes[i].entity;
                const u32 id = e.index();
                ++out.group_size;
                if (id < self_id)
                    ++out.rank;
                if (id < best_anchor_id) {
                    best_anchor_id = id;
                    out.anchor = e;
                    out.anchor_pos = xforms[i].local.translation;
                    out.anchor_has_pos = true;
                }
            }
        });
    return out;
}

}  // namespace

SquadAssignment assign_slot(EcsRegistry& registry,
                            const SquadConfig& cfg,
                            Entity self,
                            const AiAgentComponent& agent) {
    SquadAssignment out{};
    if (!is_engaging(agent)) {
        // Not closing on anything: a lone head-on agent (no flank offset).
        out.group_size = 1u;
        out.rank = 0u;
        out.slot = 0u;
        out.suppressor = true;
        return out;
    }
    const SquadScan scan = scan_squad(registry, self, agent.target_entity);
    out.group_size = scan.group_size > 0u ? scan.group_size : 1u;
    out.rank = scan.rank;
    // Slot index == rank, clamped into the ring cap so a squad larger than the
    // cap still lays members out across the configured spread (wrap, not overflow).
    const u32 cap = cfg.max_slots > 0u ? cfg.max_slots : 1u;
    out.slot = (out.rank < cap) ? out.rank : (out.rank % cap);
    // Suppressor role: the `suppressors` id-lowest members (lowest ranks) hold the
    // head-on line; the rest flank. With suppressors==0 every member flanks.
    out.suppressor = out.rank < cfg.suppressors;
    return out;
}

f32 slot_bearing(u32 slot, u32 group_size, const SquadConfig& cfg) noexcept {
    if (slot == 0u || group_size <= 1u)
        return 0.0f;
    // Alternate left/right outward from the approach axis:
    //   slot 1 -> +1 step, slot 2 -> -1 step, slot 3 -> +2 steps, ...
    // `mag` is the ring distance (1,1,2,2,3,3,...) and `sign` the side.
    const u32 mag = (slot + 1u) / 2u;          // 1,1,2,2,3,3,...
    const f32 sign = (slot % 2u == 1u) ? 1.0f : -1.0f;
    // Number of distinct magnitudes the group needs on EACH side, against the
    // ring cap so spacing is stable as members come and go. For a group of G the
    // outermost rank is G-1 -> magnitude ceil((G-1)/2). We space those magnitudes
    // evenly across [0, spread], so the outermost member sits at +/-spread.
    const u32 span_group = (group_size > 1u) ? group_size : 2u;
    const u32 max_mag_group = (span_group - 1u + 1u) / 2u;  // ceil((G-1)/2)
    const u32 cap = cfg.max_slots > 1u ? cfg.max_slots : 2u;
    const u32 max_mag_cap = (cap - 1u + 1u) / 2u;
    // Use the smaller of the two so a small group still fans to the full spread
    // (members pack the spread) while never exceeding the cap layout.
    u32 max_mag = max_mag_group < max_mag_cap ? max_mag_group : max_mag_cap;
    if (max_mag == 0u)
        max_mag = 1u;
    const f32 step = cfg.spread / static_cast<f32>(max_mag);
    f32 ang = sign * static_cast<f32>(mag) * step;
    if (ang > cfg.spread) ang = cfg.spread;
    if (ang < -cfg.spread) ang = -cfg.spread;
    return ang;
}

math::Vec3 squad_flank_goal(EcsRegistry& registry,
                            const SquadConfig& cfg,
                            Entity self,
                            const AiAgentComponent& agent,
                            math::Vec3 target_pos,
                            math::Vec3 self_pos) {
    if (!cfg.enabled || !is_engaging(agent))
        return target_pos;  // layer off / not engaging => raw target (unchanged).

    const SquadScan scan = scan_squad(registry, self, agent.target_entity);
    const u32 group_size = scan.group_size > 0u ? scan.group_size : 1u;
    if (group_size <= 1u)
        return target_pos;  // lone engager: no one to flank around, hold the line.

    const u32 cap = cfg.max_slots > 0u ? cfg.max_slots : 1u;
    const u32 slot = (scan.rank < cap) ? scan.rank : (scan.rank % cap);

    // Suppressor role: the lowest-rank members pin head-on (raw target goal).
    if (scan.rank < cfg.suppressors)
        return target_pos;

    // Approach axis: target -> squad anchor (deterministic, id-lowest member).
    // Fall back to target -> self when the anchor coincides with the target (or is
    // self, the anchor's own axis). Horizontal only (the ring lies in the XZ plane,
    // matching the 2.5D nav grid + the ground-plane separation).
    math::Vec3 axis = math::sub(scan.anchor_has_pos ? scan.anchor_pos : self_pos,
                                target_pos);
    axis.y = 0.0f;
    f32 alen = math::length(axis);
    if (!(alen > 1e-4f)) {
        // Anchor on top of the target: use target -> self instead.
        axis = math::sub(self_pos, target_pos);
        axis.y = 0.0f;
        alen = math::length(axis);
    }
    if (!(alen > 1e-4f)) {
        // Degenerate (self also on the target): pick a stable world axis so the
        // ring is still well-defined and deterministic.
        axis = math::Vec3{0.0f, 0.0f, 1.0f};
        alen = 1.0f;
    }
    const math::Vec3 adir = math::mul(axis, 1.0f / alen);

    // Rotate the unit approach axis about world +Y by the slot bearing, then push
    // out by the flank radius from the target. Rotation about +Y in the XZ plane:
    //   x' =  x*cos + z*sin ;  z' = -x*sin + z*cos
    const f32 ang = slot_bearing(slot, group_size, cfg);
    const f32 c = std::cos(ang);
    const f32 s = std::sin(ang);
    const math::Vec3 bearing{
        adir.x * c + adir.z * s,
        0.0f,
        -adir.x * s + adir.z * c,
    };
    return math::Vec3{
        target_pos.x + bearing.x * cfg.flank_radius,
        target_pos.y,
        target_pos.z + bearing.z * cfg.flank_radius,
    };
}

}  // namespace psynder::ai
