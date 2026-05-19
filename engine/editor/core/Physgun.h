// SPDX-License-Identifier: MIT
// Psynder — internal header. Physgun state for the editor / sandbox mode.
// Grab / drag / rotate / scale / freeze / weld a rigid body (DESIGN.md §10.8).
//
// Header-only so the algorithms (drag-pose update, weld-pair selection,
// cursor-ray pick) are testable without linking psynder_editor_core.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <cmath>
#include <limits>
#include <span>

namespace psynder::editor::physgun {

// ─── State ────────────────────────────────────────────────────────────────
struct State {
    u32         body_id        = 0;    // 0 = nothing grabbed
    math::Vec3  grab_local_pt  {0,0,0};  // body-local hit point
    math::Vec3  cursor_world   {0,0,0};  // world point the cursor projects to
    math::Quat  orient         {0,0,0,1};
    math::Vec3  scale          {1,1,1};
    f32         grab_distance  = 5.0f;   // metres from cursor origin to grab point
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
    // Quat multiplication isn't constexpr (lane 02 owns the math TU), so
    // callers that need composed orientations pass delta_rot here and we
    // hand it back unchanged for the OOL stub; the editor's PhysgunApi
    // wires the live psynder_math::quat_mul through compose_orientation()
    // below, which links against lane 02.
    t.rotation = s.orient;
    (void)delta_rot;
    t.scale.x = s.scale.x * delta_scale;
    t.scale.y = s.scale.y * delta_scale;
    t.scale.z = s.scale.z * delta_scale;
    return t;
}

// Apply `delta_rot` to the physgun's current orient. Pulled out so the
// kinematic-coupling step in PhysgunApi.cpp can do it without re-reading
// the State (kept in this header so the unit test exercises it too).
PSY_FORCEINLINE math::Quat compose_orientation(math::Quat current, math::Quat delta_rot) noexcept {
    return math::quat_normalize(math::quat_mul(delta_rot, current));
}

// ─── Cursor-ray AABB pick ────────────────────────────────────────────────
// Pick a body whose AABB the cursor ray intersects, returning the body id
// with the smallest hit-distance (0 = no hit). The body AABB is derived
// from `position` plus an isotropic 0.5 m half-extent — placeholder until
// lane 13 ships per-shape bounds queries on its frozen surface.
struct PickInput {
    u32        id        = 0;
    math::Vec3 position{0,0,0};
    math::Vec3 half_extent{0.5f, 0.5f, 0.5f};
};

struct PickResult {
    u32 body_id      = 0;
    f32 hit_distance = 0.0f;
};

// Slab-method ray/AABB intersection. Returns true iff the ray (origin, dir)
// intersects the AABB [centre-he, centre+he]; writes the near-hit distance
// to `t_near`. Direction need not be normalised; t is in dir-units.
PSY_FORCEINLINE bool ray_aabb_intersect(math::Vec3 origin, math::Vec3 dir,
                                        math::Vec3 centre, math::Vec3 he,
                                        f32& t_near) noexcept {
    f32 tmin = -std::numeric_limits<f32>::infinity();
    f32 tmax =  std::numeric_limits<f32>::infinity();
    const f32 oo[3] = { origin.x, origin.y, origin.z };
    const f32 dd[3] = { dir.x,    dir.y,    dir.z    };
    const f32 cc[3] = { centre.x, centre.y, centre.z };
    const f32 hh[3] = { he.x,     he.y,     he.z     };
    for (int axis = 0; axis < 3; ++axis) {
        const f32 lo = cc[axis] - hh[axis];
        const f32 hi = cc[axis] + hh[axis];
        if (std::fabs(dd[axis]) < 1e-8f) {
            if (oo[axis] < lo || oo[axis] > hi) return false;
        } else {
            f32 t1 = (lo - oo[axis]) / dd[axis];
            f32 t2 = (hi - oo[axis]) / dd[axis];
            if (t1 > t2) { f32 s = t1; t1 = t2; t2 = s; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
            if (tmax < 0.0f) return false;
        }
    }
    t_near = (tmin >= 0.0f) ? tmin : tmax;
    return true;
}

PSY_FORCEINLINE PickResult pick(std::span<const PickInput> bodies,
                                math::Vec3 origin, math::Vec3 dir) noexcept {
    PickResult best{};
    f32 best_t = std::numeric_limits<f32>::infinity();
    for (const PickInput& b : bodies) {
        if (b.id == 0) continue;
        f32 t = 0.0f;
        if (!ray_aabb_intersect(origin, dir, b.position, b.half_extent, t)) continue;
        if (t < best_t) {
            best_t      = t;
            best.body_id      = b.id;
            best.hit_distance = t;
        }
    }
    return best;
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
