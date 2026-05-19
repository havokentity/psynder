// SPDX-License-Identifier: MIT
// Psynder — public physgun_* API impl. Backed by editor::physgun::State.
//
// Wave B wires the physgun fully to the editor's body table (which is the
// authoritative editor-side mirror of lane 13 physics bodies — see
// EditorState.h). The flow:
//
//   1. physgun_grab(body_id)              cold-pick by id (UI / IPC entry)
//   1'. ops::physgun_pick(origin, dir)    cursor-ray pick → grab the hit
//   2. ops::physgun_drag(origin, dir)     follow the cursor each tick
//   3. ops::physgun_rotate(delta_quat)    accumulate orient about world
//   4. ops::physgun_scale(delta_scale)    accumulate uniform scale
//   5. physgun_freeze()                   pin body in place
//   6. ops::physgun_weld(target_id)       drop a Weld constraint on the
//                                          editor's constraint graph; when
//                                          lane 13 exposes a constraint
//                                          API, the editor reflects this
//                                          graph through that adapter.
//   7. physgun_drop()                     release, commit pose, push undo
//
// All paths apply edits to BodyRec (the editor-side cache). The physics
// world is driven from this cache by the editor's per-tick sync (lane 19
// wires the IPC; lane 18 produces the deltas).

#include "Editor.h"
#include "EditorState.h"
#include "EntityOps.h"
#include "Physgun.h"
#include "Undo.h"

#include <vector>

namespace psynder::editor {

namespace {

PSY_FORCEINLINE math::Vec3 ray_point(math::Vec3 origin, math::Vec3 dir, f32 t) noexcept {
    return math::add(origin, math::mul(math::normalize(dir), t));
}

// Apply the gun state to its currently-grabbed body. Kinematic coupling:
// position/rotation/scale follow the gun, frozen flag mirrors the gun's.
PSY_FORCEINLINE void sync_body_from_gun(detail::State& s) noexcept {
    if (!physgun::is_active(s.gun)) return;
    if (auto* body = detail::find_body(s.gun.body_id)) {
        body->position = s.gun.cursor_world;
        body->rotation = s.gun.orient;
        body->scale    = s.gun.scale;
        body->frozen   = s.gun.frozen;
    }
}

}  // namespace

// ─── Public Editor.h facade ──────────────────────────────────────────────
void physgun_grab(u32 body_id) {
    auto& s = detail::get_state();
    auto* body = detail::find_body(body_id);
    if (!body) return;

    s.gun.body_id       = body_id;
    s.gun.grab_local_pt = {0,0,0};
    s.gun.cursor_world  = body->position;
    s.gun.orient        = body->rotation;
    s.gun.scale         = body->scale;
    s.gun.grab_distance = 5.0f;
    s.gun.frozen        = body->frozen;
    s.gun.active        = true;

    // Record so undo restores prior pose.
    undo::Delta d;
    d.op        = undo::Op::PhysgunGrab;
    d.target_id = body_id;
    d.before    = body->position;
    d.after     = body->position;
    s.undo.push(d);
}

void physgun_drop() {
    auto& s = detail::get_state();
    if (!s.gun.active) return;

    auto* body = detail::find_body(s.gun.body_id);
    if (body) {
        const math::Vec3 prev = body->position;
        body->position = s.gun.cursor_world;
        body->rotation = s.gun.orient;
        body->scale    = s.gun.scale;

        undo::Delta d;
        d.op        = undo::Op::PhysgunDrop;
        d.target_id = body->id;
        d.before    = prev;
        d.after     = body->position;
        s.undo.push(d);
    }
    s.gun.active  = false;
    s.gun.body_id = 0;
}

void physgun_freeze() {
    auto& s = detail::get_state();
    if (!s.gun.active) return;
    if (auto* body = detail::find_body(s.gun.body_id)) {
        body->frozen = !body->frozen;     // toggle, per DESIGN.md §10.8
        s.gun.frozen = body->frozen;
    }
}

// ─── Internal ops namespace (Wave B wiring) ──────────────────────────────
namespace ops {

// Cursor-ray pick → grab. Returns the grabbed body id, or 0 on no hit.
u32 physgun_pick(math::Vec3 cursor_origin, math::Vec3 cursor_dir) {
    auto& s = detail::get_state();

    // Build the pick set from live (non-deleted) editor bodies. The half-
    // extent is the placeholder isotropic 0.5 m box — the real per-shape
    // bounds will come through lane 13's frozen surface once it exposes
    // them.
    std::vector<physgun::PickInput> picks;
    picks.reserve(s.bodies.size());
    for (const auto& b : s.bodies) {
        if (!b.alive) continue;
        physgun::PickInput p;
        p.id          = b.id;
        p.position    = b.position;
        p.half_extent = { 0.5f, 0.5f, 0.5f };
        picks.push_back(p);
    }

    const physgun::PickResult r = physgun::pick(picks, cursor_origin, cursor_dir);
    if (r.body_id == 0) return 0;

    physgun_grab(r.body_id);
    s.gun.grab_distance = r.hit_distance;
    return r.body_id;
}

// Drag through 3D. The body's grab point projects to a point on the cursor
// ray at the stored grab_distance; orientation/scale come from prior
// rotate/scale calls.
void physgun_drag(math::Vec3 cursor_origin, math::Vec3 cursor_dir) {
    auto& s = detail::get_state();
    if (!physgun::is_active(s.gun)) return;
    if (s.gun.frozen) return;     // frozen bodies don't follow the cursor

    s.gun.cursor_world = ray_point(cursor_origin, cursor_dir, s.gun.grab_distance);
    sync_body_from_gun(s);
}

// Adjust grab distance (mouse-wheel: pull body closer or push it further).
void physgun_set_grab_distance(f32 metres) {
    auto& s = detail::get_state();
    if (!physgun::is_active(s.gun)) return;
    if (metres < 0.5f)  metres = 0.5f;
    if (metres > 80.0f) metres = 80.0f;
    s.gun.grab_distance = metres;
}

// Rotate the grabbed body about its centre by `delta_rot` (world-space).
void physgun_rotate(math::Quat delta_rot) {
    auto& s = detail::get_state();
    if (!physgun::is_active(s.gun)) return;
    if (s.gun.frozen) return;
    s.gun.orient = physgun::compose_orientation(s.gun.orient, delta_rot);
    sync_body_from_gun(s);
}

// Uniform-scale the grabbed body.
void physgun_scale(f32 delta_scale) {
    auto& s = detail::get_state();
    if (!physgun::is_active(s.gun)) return;
    if (s.gun.frozen) return;
    if (delta_scale <= 0.0f) return;
    s.gun.scale.x *= delta_scale;
    s.gun.scale.y *= delta_scale;
    s.gun.scale.z *= delta_scale;
    sync_body_from_gun(s);
}

// Drop a Weld constraint between the grabbed body and `target_id`. The
// anchor is the AABB-midpoint heuristic from physgun::make_weld. Returns
// the new constraint's id, or 0 on failure (no grab / missing target /
// self-weld).
u32 physgun_weld(u32 target_body_id) {
    auto& s = detail::get_state();
    if (!physgun::is_active(s.gun))               return 0;
    if (target_body_id == 0)                      return 0;
    if (target_body_id == s.gun.body_id)          return 0;

    auto* a = detail::find_body(s.gun.body_id);
    auto* b = detail::find_body(target_body_id);
    if (!a || !b) return 0;

    const physgun::WeldRequest w = physgun::make_weld(a->id, b->id, a->position, b->position);

    constraints::Constraint c = constraints::make_weld(w.body_a, w.body_b, w.anchor);
    // Store the anchor in each body's local frame: editor cache treats the
    // body as a rigid frame at its position, so local = world - position.
    c.anchor_a = math::sub(w.anchor, a->position);
    c.anchor_b = math::sub(w.anchor, b->position);
    return ops::add_constraint(c);
}

}  // namespace ops

}  // namespace psynder::editor
