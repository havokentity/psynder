// SPDX-License-Identifier: MIT
// Psynder — internal header. Constraint graph types serialised with each
// contraption (.psyc) and each level (.psylevel). DESIGN.md §10.8 lists
// six kinds: weld, axis, slider, ball-socket, rope, elastic.
//
// The actual physics is owned by lane 13; this header is just the
// editor-side authoring model (what saves to disk, what undo/redo
// records, what the inspector renders).

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include <vector>

namespace psynder::editor::constraints {

enum class Kind : u8 {
    Weld       = 0,    // rigid attachment
    Axis       = 1,    // 1-DoF rotation along an axis
    Slider     = 2,    // 1-DoF translation along an axis
    BallSocket = 3,    // 3-DoF rotation about a point
    Rope       = 4,    // length-limited distance constraint
    Elastic    = 5,    // length-relaxing spring
};

struct Constraint {
    u32        id        = 0;
    Kind       kind      = Kind::Weld;
    u32        body_a    = 0;
    u32        body_b    = 0;
    math::Vec3 anchor_a  {0,0,0};
    math::Vec3 anchor_b  {0,0,0};
    math::Vec3 axis      {0,1,0};            // axis / slider direction; ignored for weld/ball
    f32        rest_length = 0.0f;           // rope / elastic
    f32        stiffness   = 0.0f;           // elastic only (N/m)
    f32        damping     = 0.0f;           // elastic only
    f32        min_limit   = 0.0f;           // axis / slider
    f32        max_limit   = 0.0f;
};

class Graph {
public:
    PSY_FORCEINLINE u32 add(const Constraint& c) {
        Constraint copy = c;
        copy.id = next_id_++;
        list_.push_back(copy);
        return copy.id;
    }
    PSY_FORCEINLINE bool remove(u32 id) {
        for (auto it = list_.begin(); it != list_.end(); ++it) {
            if (it->id == id) { list_.erase(it); return true; }
        }
        return false;
    }
    PSY_FORCEINLINE void clear() noexcept { list_.clear(); }
    PSY_FORCEINLINE usize size() const noexcept { return list_.size(); }
    PSY_FORCEINLINE const Constraint& at(usize i) const noexcept { return list_[i]; }
    PSY_FORCEINLINE std::vector<Constraint>& mutable_list() noexcept { return list_; }
    PSY_FORCEINLINE const std::vector<Constraint>& list() const noexcept { return list_; }

private:
    std::vector<Constraint> list_;
    u32                     next_id_ = 1;
};

// ─── Constructors (a constraint per kind) ────────────────────────────────
PSY_FORCEINLINE Constraint make_weld(u32 a, u32 b, math::Vec3 anchor) noexcept {
    Constraint c;
    c.kind = Kind::Weld;
    c.body_a = a; c.body_b = b;
    c.anchor_a = anchor; c.anchor_b = anchor;
    return c;
}

PSY_FORCEINLINE Constraint make_axis(u32 a, u32 b, math::Vec3 anchor, math::Vec3 axis,
                                     f32 min_angle, f32 max_angle) noexcept {
    Constraint c;
    c.kind = Kind::Axis;
    c.body_a = a; c.body_b = b;
    c.anchor_a = anchor; c.anchor_b = anchor;
    c.axis = math::normalize(axis);
    c.min_limit = min_angle; c.max_limit = max_angle;
    return c;
}

PSY_FORCEINLINE Constraint make_slider(u32 a, u32 b, math::Vec3 anchor, math::Vec3 axis,
                                       f32 min_t, f32 max_t) noexcept {
    Constraint c;
    c.kind = Kind::Slider;
    c.body_a = a; c.body_b = b;
    c.anchor_a = anchor; c.anchor_b = anchor;
    c.axis = math::normalize(axis);
    c.min_limit = min_t; c.max_limit = max_t;
    return c;
}

PSY_FORCEINLINE Constraint make_ball_socket(u32 a, u32 b, math::Vec3 anchor) noexcept {
    Constraint c;
    c.kind = Kind::BallSocket;
    c.body_a = a; c.body_b = b;
    c.anchor_a = anchor; c.anchor_b = anchor;
    return c;
}

PSY_FORCEINLINE Constraint make_rope(u32 a, u32 b, math::Vec3 ax, math::Vec3 bx, f32 max_length) noexcept {
    Constraint c;
    c.kind = Kind::Rope;
    c.body_a = a; c.body_b = b;
    c.anchor_a = ax; c.anchor_b = bx;
    c.rest_length = max_length;
    return c;
}

PSY_FORCEINLINE Constraint make_elastic(u32 a, u32 b, math::Vec3 ax, math::Vec3 bx,
                                        f32 rest_length, f32 stiffness, f32 damping) noexcept {
    Constraint c;
    c.kind = Kind::Elastic;
    c.body_a = a; c.body_b = b;
    c.anchor_a = ax; c.anchor_b = bx;
    c.rest_length = rest_length;
    c.stiffness   = stiffness;
    c.damping     = damping;
    return c;
}

}  // namespace psynder::editor::constraints
