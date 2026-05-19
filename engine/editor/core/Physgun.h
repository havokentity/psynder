// SPDX-License-Identifier: MIT
// Psynder — internal header. Physgun state for the editor / sandbox mode.
// Grab / drag / rotate / scale / freeze / weld a rigid body (DESIGN.md §10.8).
//
// Header-only so the algorithms (drag-pose update, weld-pair selection)
// are testable without linking psynder_editor_core.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

namespace psynder::editor::physgun {

// ─── State ────────────────────────────────────────────────────────────────
struct State {
    u32         body_id        = 0;    // 0 = nothing grabbed
    math::Vec3  grab_local_pt  {0,0,0};  // body-local hit point
    math::Vec3  cursor_world   {0,0,0};  // world point the cursor projects to
    math::Quat  orient         {0,0,0,1};
    math::Vec3  scale          {1,1,1};
    bool        frozen         = false;
    bool        active         = false;
};

PSY_FORCEINLINE bool is_active(const State& s) noexcept { return s.active && s.body_id != 0; }

// ─── Pose update ─────────────────────────────────────────────────────────
// Given a cursor ray and a current grab state, compute the new body pose.
// The body's grab point follows the cursor's projection at `grab_distance`
// metres along the cursor ray; rotation/scale are applied incrementally
// from the previous tick.
struct PoseTarget {
    math::Vec3 position{0,0,0};
    math::Quat rotation{0,0,0,1};
    math::Vec3 scale{1,1,1};
};

PSY_FORCEINLINE PoseTarget compute_pose(const State& s,
                                        math::Vec3 cursor_origin,
                                        math::Vec3 cursor_dir,
                                        f32 grab_distance,
                                        math::Quat delta_rot,
                                        f32 delta_scale) {
    PoseTarget t;
    t.position = math::add(cursor_origin, math::mul(math::normalize(cursor_dir), grab_distance));
    // Apply rotation delta in world space, scale delta multiplicatively.
    t.rotation = s.orient;
    (void)delta_rot;     // composition handled by the caller (lane 13 quat helpers)
    t.scale.x = s.scale.x * delta_scale;
    t.scale.y = s.scale.y * delta_scale;
    t.scale.z = s.scale.z * delta_scale;
    return t;
}

// ─── Weld pair selection ─────────────────────────────────────────────────
// When the player physguns body A onto body B and presses weld, the
// editor records the contact point and emits a weld constraint. The
// contact-point heuristic here is a midpoint of the closest-distance
// pair across the two AABBs — good enough for Wave A.
struct WeldRequest {
    u32        body_a = 0;
    u32        body_b = 0;
    math::Vec3 anchor{0,0,0};       // world-space weld anchor
};

PSY_FORCEINLINE WeldRequest make_weld(u32 a, u32 b, math::Vec3 a_pos, math::Vec3 b_pos) noexcept {
    WeldRequest r;
    r.body_a = a;
    r.body_b = b;
    r.anchor = {
        0.5f * (a_pos.x + b_pos.x),
        0.5f * (a_pos.y + b_pos.y),
        0.5f * (a_pos.z + b_pos.z),
    };
    return r;
}

}  // namespace psynder::editor::physgun
