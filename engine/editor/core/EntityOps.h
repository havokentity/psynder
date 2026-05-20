// SPDX-License-Identifier: MIT
// Psynder — internal header. Entity spawn / move / delete + constraint
// authoring helpers. The IPC layer (lane 19) and the in-engine input
// dispatch call into these; they're not on the frozen public Editor.h
// surface because we don't want to lock the wire format this early.

#pragma once

#include "core/Types.h"
#include "math/Math.h"

#include "editor/core/Constraints.h"
#include "editor/core/EditorState.h"
#include "editor/core/Undo.h"

namespace psynder::editor::ops {

// ─── Entities ─────────────────────────────────────────────────────────────
// Spawn an entity at `pos` with optional `prefab_id`. Returns the new
// editor-stable id.
inline u32 spawn_entity(math::Vec3 pos, u32 prefab_id = 0) {
    auto& s = detail::get_state();
    detail::EntityRec rec;
    rec.id = s.next_entity_id++;
    rec.position = pos;
    rec.prefab_id = prefab_id;
    rec.alive = true;
    s.entities.push_back(rec);
    s.undo.push(undo::make_spawn(rec.id, pos));
    return rec.id;
}

inline bool move_entity(u32 id, math::Vec3 new_pos) {
    auto* e = detail::find_entity(id);
    if (!e)
        return false;
    auto& s = detail::get_state();
    s.undo.push(undo::make_move(id, e->position, new_pos));
    e->position = new_pos;
    return true;
}

inline bool delete_entity(u32 id) {
    auto* e = detail::find_entity(id);
    if (!e || !e->alive)
        return false;
    auto& s = detail::get_state();
    s.undo.push(undo::make_delete(id, e->position));
    e->alive = false;
    return true;
}

// ─── Bodies (physgun pick set) ───────────────────────────────────────────
inline u32 spawn_body(math::Vec3 pos) {
    auto& s = detail::get_state();
    detail::BodyRec rec;
    rec.id = s.next_body_id++;
    rec.position = pos;
    rec.alive = true;
    s.bodies.push_back(rec);
    return rec.id;
}

// ─── Constraints ─────────────────────────────────────────────────────────
inline u32 add_constraint(const constraints::Constraint& c) {
    auto& s = detail::get_state();
    const u32 id = s.constraint_graph.add(c);
    undo::Delta d;
    d.op = undo::Op::Constraint;
    d.target_id = id;
    s.undo.push(d);
    return id;
}

inline bool remove_constraint(u32 id) {
    auto& s = detail::get_state();
    if (!s.constraint_graph.remove(id))
        return false;
    undo::Delta d;
    d.op = undo::Op::Constraint;
    d.target_id = id;
    s.undo.push(d);
    return true;
}

// ─── Physgun (Wave-B wiring) ─────────────────────────────────────────────
// Out-of-line in PhysgunApi.cpp because they touch the live editor state
// and the constraint graph. Declared here so the IPC dispatcher (lane 19)
// and per-platform input plumbing (lanes 21-23) can route into them
// without including PhysgunApi.cpp's internals.
u32 physgun_pick(math::Vec3 cursor_origin, math::Vec3 cursor_dir);
void physgun_drag(math::Vec3 cursor_origin, math::Vec3 cursor_dir);
void physgun_set_grab_distance(f32 metres);
void physgun_rotate(math::Quat delta_rot);
void physgun_scale(f32 delta_scale);
u32 physgun_weld(u32 target_body_id);

// ─── Undo / redo dispatch ────────────────────────────────────────────────
inline bool undo_one() {
    auto& s = detail::get_state();
    undo::Delta d;
    if (!s.undo.undo(d))
        return false;
    detail::apply_delta(d, /*reverse=*/true);
    return true;
}

inline bool redo_one() {
    auto& s = detail::get_state();
    undo::Delta d;
    if (!s.undo.redo(d))
        return false;
    detail::apply_delta(d, /*reverse=*/false);
    return true;
}

}  // namespace psynder::editor::ops
