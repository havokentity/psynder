// SPDX-License-Identifier: MIT
// Psynder — public physgun_* API impl. Backed by editor::physgun::State.

#include "Editor.h"
#include "EditorState.h"
#include "Physgun.h"
#include "Undo.h"

namespace psynder::editor {

void physgun_grab(u32 body_id) {
    auto& s = detail::get_state();
    auto* body = detail::find_body(body_id);
    if (!body) return;

    s.gun.body_id       = body_id;
    s.gun.grab_local_pt = {0,0,0};
    s.gun.cursor_world  = body->position;
    s.gun.orient        = body->rotation;
    s.gun.scale         = body->scale;
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
        body->frozen = true;
        s.gun.frozen = true;
    }
}

}  // namespace psynder::editor
